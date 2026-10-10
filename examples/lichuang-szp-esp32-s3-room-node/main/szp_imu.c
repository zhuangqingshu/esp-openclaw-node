#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_openclaw_room_node.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "szp_board.h"

#define TAG "szp_imu"

/* QMI8658 six-axis IMU; SA0 selects 0x6A or 0x6B. */
#define QMI8658_ADDR_A 0x6A
#define QMI8658_ADDR_B 0x6B
#define QMI8658_REG_WHO_AM_I 0x00
#define QMI8658_WHO_AM_I 0x05
#define QMI8658_REG_CTRL1 0x02
#define QMI8658_REG_CTRL2 0x03
#define QMI8658_REG_CTRL7 0x08
#define QMI8658_REG_TEMP_L 0x33
#define QMI8658_REG_AX_L 0x35
#define QMI8658_REG_RESET 0x60
#define QMI8658_RESET_CMD 0xB0
/* CTRL2: +/-4 g full scale, 125 Hz output data rate. */
#define QMI8658_CTRL2_4G_125HZ 0x16
#define QMI8658_LSB_PER_G 8192.0f

#define IMU_POLL_MS 20
/* Gravity low-pass for orientation; the residual is the shock signal. */
#define IMU_GRAVITY_ALPHA 0.08f
#define IMU_KNOCK_G 0.55f
#define IMU_KNOCK_REFRACTORY_MS 90
#define IMU_DOUBLE_KNOCK_MIN_MS 120
#define IMU_DOUBLE_KNOCK_MAX_MS 550
/* Ignore knocks right after a Talk transition: speaker and handling shake the board. */
#define IMU_KNOCK_COOLDOWN_MS 1500
#define IMU_SETTLE_MS 2000
#define IMU_FLAT_G 0.8f
#define IMU_FACE_DOWN_HOLD_MS 1500

typedef enum {
    IMU_ORIENT_UNKNOWN = 0,
    IMU_ORIENT_FACE_UP,
    IMU_ORIENT_FACE_DOWN,
    IMU_ORIENT_UPRIGHT,
    IMU_ORIENT_TILTED,
} imu_orientation_t;

static const char *const ORIENTATION_NAMES[] = {"unknown", "face_up", "face_down", "upright", "tilted"};

static i2c_master_dev_handle_t imu;
static portMUX_TYPE imu_mux = portMUX_INITIALIZER_UNLOCKED;
static float gravity[3];
static float temperature_c;
static imu_orientation_t orientation;
static uint32_t knock_count;
static bool flip_dnd;
/* Console-injected knock, consumed by the IMU task so tests exercise the same
 * task context and stack as a real gesture. */
static volatile bool simulated_knock;
/* Polling waits until the board has configured the audio codecs: IMU traffic
 * during that init burst coincided with ES8311 write failures that left the
 * node without a microphone path. */
static SemaphoreHandle_t audio_ready;
static StaticSemaphore_t audio_ready_storage;
#define IMU_AUDIO_WAIT_MS 30000

static esp_err_t imu_write(uint8_t reg, uint8_t value)
{
    const uint8_t data[] = {reg, value};
    return i2c_master_transmit(imu, data, sizeof(data), 50);
}

static esp_err_t imu_read(uint8_t reg, uint8_t *out, size_t len)
{
    return i2c_master_transmit_receive(imu, &reg, 1, out, len, 50);
}

static imu_orientation_t classify(const float g[3])
{
    /* Board axes: Z is the PCB normal (screen side positive), so a board lying
     * screen-up reads +1 g on Z. */
    if (g[2] > IMU_FLAT_G) return IMU_ORIENT_FACE_UP;
    if (g[2] < -IMU_FLAT_G) return IMU_ORIENT_FACE_DOWN;
    if (fabsf(g[2]) < 0.35f) return IMU_ORIENT_UPRIGHT;
    return IMU_ORIENT_TILTED;
}

static void knock_action(void)
{
    taskENTER_CRITICAL(&imu_mux);
    knock_count++;
    taskEXIT_CRITICAL(&imu_mux);
    if (esp_openclaw_room_node_talk_busy()) {
        ESP_LOGI(TAG, "double knock: stop Talk");
        (void)esp_openclaw_room_node_request_talk_stop();
    } else {
        ESP_LOGI(TAG, "double knock: start Talk");
        (void)esp_openclaw_room_node_request_talk();
    }
}

static void imu_task(void *arg)
{
    (void)arg;
    bool primed = false;
    float g[3] = {0};
    int64_t last_knock_us = 0;
    int64_t first_knock_us = 0;
    int64_t cooldown_until_us = 0;
    int64_t face_down_since_us = 0;
    bool was_busy = false;
    int temp_divider = 0;
    int64_t settle_until_us = 0;
    if (xSemaphoreTake(audio_ready, pdMS_TO_TICKS(IMU_AUDIO_WAIT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "audio init did not report in %d ms; polling anyway", IMU_AUDIO_WAIT_MS);
    }
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(IMU_POLL_MS));
        uint8_t raw[6];
        if (imu_read(QMI8658_REG_AX_L, raw, sizeof(raw)) != ESP_OK) continue;
        float a[3];
        for (int i = 0; i < 3; ++i) {
            a[i] = (float)(int16_t)(raw[2 * i] | (raw[2 * i + 1] << 8)) / QMI8658_LSB_PER_G;
        }
        /* The first conversions after reset read as zero; priming on them
         * would make gravity itself look like a pair of knocks. */
        float magnitude = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
        if (!primed) {
            if (magnitude < 0.5f || magnitude > 1.5f) continue;
            memcpy(g, a, sizeof(g));
            primed = true;
            settle_until_us = esp_timer_get_time() + (int64_t)IMU_SETTLE_MS * 1000;
        }
        for (int i = 0; i < 3; ++i) g[i] += IMU_GRAVITY_ALPHA * (a[i] - g[i]);
        float dx = a[0] - g[0];
        float dy = a[1] - g[1];
        float dz = a[2] - g[2];
        float shock = sqrtf(dx * dx + dy * dy + dz * dz);
        int64_t now_us = esp_timer_get_time();
        if (simulated_knock) {
            simulated_knock = false;
            knock_action();
            ESP_LOGI(TAG, "stack high-water mark: %u bytes", (unsigned)uxTaskGetStackHighWaterMark(NULL));
        }

        bool busy = esp_openclaw_room_node_talk_busy();
        if (busy != was_busy) {
            cooldown_until_us = now_us + (int64_t)IMU_KNOCK_COOLDOWN_MS * 1000;
            was_busy = busy;
            first_knock_us = 0;
        }

        imu_orientation_t current = classify(g);
        if (shock > IMU_KNOCK_G && now_us >= cooldown_until_us && now_us >= settle_until_us &&
            now_us - last_knock_us > (int64_t)IMU_KNOCK_REFRACTORY_MS * 1000 &&
            current != IMU_ORIENT_FACE_DOWN) {
            last_knock_us = now_us;
            int64_t gap_ms = (now_us - first_knock_us) / 1000;
            if (first_knock_us != 0 && gap_ms >= IMU_DOUBLE_KNOCK_MIN_MS && gap_ms <= IMU_DOUBLE_KNOCK_MAX_MS) {
                first_knock_us = 0;
                cooldown_until_us = now_us + (int64_t)IMU_KNOCK_COOLDOWN_MS * 1000;
                knock_action();
            } else {
                first_knock_us = now_us;
            }
        } else if (first_knock_us != 0 && (now_us - first_knock_us) / 1000 > IMU_DOUBLE_KNOCK_MAX_MS) {
            first_knock_us = 0;
        }

        /* Face-down is do-not-disturb: end any call and mute the wake word.
         * Only an IMU-initiated mute is lifted by turning the board back over. */
        if (current == IMU_ORIENT_FACE_DOWN) {
            if (face_down_since_us == 0) face_down_since_us = now_us;
            if (!flip_dnd && now_us - face_down_since_us > (int64_t)IMU_FACE_DOWN_HOLD_MS * 1000 &&
                !esp_openclaw_room_node_wake_muted()) {
                ESP_LOGI(TAG, "face down: do-not-disturb on");
                flip_dnd = true;
                (void)esp_openclaw_room_node_request_talk_stop();
                esp_openclaw_room_node_set_wake_muted(true);
            }
        } else {
            face_down_since_us = 0;
            if (flip_dnd && current != IMU_ORIENT_TILTED) {
                ESP_LOGI(TAG, "turned over: do-not-disturb off");
                flip_dnd = false;
                esp_openclaw_room_node_set_wake_muted(false);
            }
        }

        float temp = temperature_c;
        if (++temp_divider >= 50) {
            temp_divider = 0;
            uint8_t t[2];
            if (imu_read(QMI8658_REG_TEMP_L, t, sizeof(t)) == ESP_OK) {
                temp = (float)(int16_t)(t[0] | (t[1] << 8)) / 256.0f;
            }
        }
        taskENTER_CRITICAL(&imu_mux);
        memcpy(gravity, g, sizeof(gravity));
        orientation = current;
        temperature_c = temp;
        taskEXIT_CRITICAL(&imu_mux);
    }
}

void szp_imu_audio_ready(void)
{
    if (audio_ready != NULL) xSemaphoreGive(audio_ready);
}

esp_err_t szp_imu_start(void)
{
    i2c_master_bus_handle_t bus = szp_i2c_bus();
    ESP_RETURN_ON_FALSE(bus != NULL, ESP_ERR_INVALID_STATE, TAG, "I2C bus not prepared");
    uint16_t address = 0;
    if (i2c_master_probe(bus, QMI8658_ADDR_A, 50) == ESP_OK) {
        address = QMI8658_ADDR_A;
    } else if (i2c_master_probe(bus, QMI8658_ADDR_B, 50) == ESP_OK) {
        address = QMI8658_ADDR_B;
    } else {
        ESP_LOGW(TAG, "QMI8658 not found; motion gestures disabled");
        return ESP_OK;
    }
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &cfg, &imu), TAG, "IMU device");
    uint8_t who = 0;
    if (imu_read(QMI8658_REG_WHO_AM_I, &who, 1) != ESP_OK || who != QMI8658_WHO_AM_I) {
        ESP_LOGW(TAG, "device at 0x%02x is not a QMI8658 (id 0x%02x)", address, who);
        (void)i2c_master_bus_rm_device(imu);
        imu = NULL;
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(imu_write(QMI8658_REG_RESET, QMI8658_RESET_CMD), TAG, "IMU reset");
    vTaskDelay(pdMS_TO_TICKS(20));
    /* Register address auto-increment for burst reads. */
    ESP_RETURN_ON_ERROR(imu_write(QMI8658_REG_CTRL1, 0x40), TAG, "IMU CTRL1");
    ESP_RETURN_ON_ERROR(imu_write(QMI8658_REG_CTRL2, QMI8658_CTRL2_4G_125HZ), TAG, "IMU CTRL2");
    ESP_RETURN_ON_ERROR(imu_write(QMI8658_REG_CTRL7, 0x01), TAG, "IMU enable accel");
    audio_ready = xSemaphoreCreateBinaryStatic(&audio_ready_storage);
    ESP_LOGI(TAG, "QMI8658 at 0x%02x: double-knock toggles Talk, face-down mutes wake", address);
    /* A gesture admits Talk from this task (logging, timer creation), which
     * overflowed a 3 KiB stack and rebooted the board. PSRAM keeps it cheap. */
    ESP_RETURN_ON_FALSE(
        xTaskCreateWithCaps(imu_task, "imu", 8192, NULL, 3, NULL, MALLOC_CAP_SPIRAM) == pdPASS,
        ESP_ERR_NO_MEM, TAG, "IMU task");
    return ESP_OK;
}

static esp_err_t imu_status(
    esp_openclaw_node_handle_t node, void *context, const char *params_json,
    size_t params_len, char **out, esp_openclaw_node_error_t *error)
{
    (void)node;
    (void)context;
    cJSON *params = cJSON_ParseWithLength(params_json, params_len);
    bool valid = cJSON_IsObject(params) && params->child == NULL;
    cJSON_Delete(params);
    if (!valid) {
        error->code = "INVALID_PARAMS";
        error->message = "imu.status accepts only {}";
        return ESP_ERR_INVALID_ARG;
    }
    if (imu == NULL) {
        *out = strdup("{\"available\":false}");
        return *out != NULL ? ESP_OK : ESP_ERR_NO_MEM;
    }
    float g[3];
    taskENTER_CRITICAL(&imu_mux);
    memcpy(g, gravity, sizeof(g));
    imu_orientation_t current = orientation;
    float temp = temperature_c;
    uint32_t knocks = knock_count;
    taskEXIT_CRITICAL(&imu_mux);
    char buffer[224];
    snprintf(buffer, sizeof(buffer),
        "{\"available\":true,\"gravityG\":{\"x\":%.2f,\"y\":%.2f,\"z\":%.2f},"
        "\"orientation\":\"%s\",\"temperatureC\":%.1f,\"doubleKnocks\":%lu,\"faceDownDnd\":%s}",
        g[0], g[1], g[2], ORIENTATION_NAMES[current], temp, (unsigned long)knocks,
        flip_dnd ? "true" : "false");
    *out = strdup(buffer);
    return *out != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

static int imu_console_command(int argc, char **argv)
{
    if (argc != 2 || strcmp(argv[1], "knock") != 0) {
        printf("usage: imu knock\n");
        return 1;
    }
    if (imu == NULL) {
        printf("no IMU\n");
        return 1;
    }
    simulated_knock = true;
    printf("double knock queued\n");
    return 0;
}

esp_err_t szp_imu_register_commands(esp_openclaw_node_handle_t node)
{
    if (imu == NULL) return ESP_OK;
    static const esp_console_cmd_t CONSOLE = {
        .command = "imu",
        .help = "Simulate an IMU gesture",
        .hint = "knock",
        .func = imu_console_command,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&CONSOLE), TAG, "imu console command");
    static const esp_openclaw_node_command_t COMMAND = {.name = "imu.status", .handler = imu_status};
    ESP_RETURN_ON_ERROR(esp_openclaw_node_register_capability(node, "imu"), TAG, "imu capability");
    return esp_openclaw_node_register_command(node, &COMMAND);
}
