#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_camera.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_openclaw_room_node.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "img_converters.h"
#include "mbedtls/base64.h"
#include "szp_board.h"

#define TAG "szp_camera"

/* DVP camera wiring on the LCSC SZP board (vendor schematic / xiaozhi). Board
 * batches ship either a GC0308 (VGA) or a GC2145 (UXGA); esp32-camera probes
 * which one answers on the shared SCCB bus. */
#define CAMERA_PIN_XCLK 5
#define CAMERA_PIN_PCLK 7
#define CAMERA_PIN_VSYNC 3
#define CAMERA_PIN_HREF 46
#define CAMERA_PIN_D0 16
#define CAMERA_PIN_D1 18
#define CAMERA_PIN_D2 8
#define CAMERA_PIN_D3 17
#define CAMERA_PIN_D4 15
#define CAMERA_PIN_D5 6
#define CAMERA_PIN_D6 4
#define CAMERA_PIN_D7 9
#define CAMERA_XCLK_HZ 10000000
#define CAMERA_DEVICE_ID "szp-front"
/* Auto-exposure needs a few frames after power-up before the image is usable. */
#define CAMERA_WARMUP_FRAMES 4

static framesize_t frame_size_for_width(int max_width, int *out_width, int *out_height)
{
    if (max_width >= 1600) {
        *out_width = 1600;
        *out_height = 1200;
        return FRAMESIZE_UXGA;
    }
    if (max_width >= 1024) {
        *out_width = 1024;
        *out_height = 768;
        return FRAMESIZE_XGA;
    }
    if (max_width >= 800) {
        *out_width = 800;
        *out_height = 600;
        return FRAMESIZE_SVGA;
    }
    if (max_width >= 640) {
        *out_width = 640;
        *out_height = 480;
        return FRAMESIZE_VGA;
    }
    if (max_width >= 320) {
        *out_width = 320;
        *out_height = 240;
        return FRAMESIZE_QVGA;
    }
    *out_width = 160;
    *out_height = 120;
    return FRAMESIZE_QQVGA;
}

/* The driver bounces each DMA chunk through internal RAM (capped at 8 KiB by
 * CONFIG_CAMERA_DMA_BUFFER_SIZE_MAX). Streaming straight into PSRAM drops bytes
 * while the wake-word AFE loads the PSRAM bus, and by the time a capture runs
 * the internal heap is too fragmented for even 8 KiB contiguous. So the block
 * is reserved at boot and handed to the driver for the length of a capture.
 * Talk's audio tasks need that same internal RAM (holding it made AFE task
 * creation fail), and the camera lease keeps the camera idle during Talk, so
 * the block is also lent to Talk for the length of each call. */
#define CAMERA_DMA_RESERVE_BYTES (12 * 1024)
static void *dma_reserve;
/* While Talk owns the audio pipeline the reserve stays lent out; a mid-call
 * capture allocates its ~8 KiB directly and returns it on close. */
static volatile bool talk_owns_media;

static void reserve_dma(void)
{
    if (talk_owns_media) return;
    if (dma_reserve == NULL) {
        dma_reserve = heap_caps_malloc(CAMERA_DMA_RESERVE_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    }
    /* Right after Talk the audio tasks may still hold it; the next capture
     * allocates directly and re-reserves on close. */
    if (dma_reserve == NULL) ESP_LOGI(TAG, "camera DMA reserve deferred to the next capture");
}

esp_err_t szp_camera_reserve(void)
{
    reserve_dma();
    return dma_reserve != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

void szp_camera_talk_media_changed(bool owns)
{
    talk_owns_media = owns;
    if (owns) {
        heap_caps_free(dma_reserve);
        dma_reserve = NULL;
    } else {
        reserve_dma();
    }
}

/* The sensor is powered only for the duration of one capture. */
static esp_err_t camera_open(framesize_t frame_size)
{
    ESP_RETURN_ON_ERROR(szp_pca9557_set_bit(PCA9557_CAMERA_PWDN_BIT, false), TAG, "camera power");
    vTaskDelay(pdMS_TO_TICKS(10));
    const camera_config_t config = {
        .pin_pwdn = -1,
        .pin_reset = -1,
        .pin_xclk = CAMERA_PIN_XCLK,
        /* SCCB reuses the board's already-installed I2C master bus. */
        .pin_sccb_sda = -1,
        .pin_sccb_scl = -1,
        .sccb_i2c_port = LICHUANG_I2C_PORT,
        .pin_d7 = CAMERA_PIN_D7,
        .pin_d6 = CAMERA_PIN_D6,
        .pin_d5 = CAMERA_PIN_D5,
        .pin_d4 = CAMERA_PIN_D4,
        .pin_d3 = CAMERA_PIN_D3,
        .pin_d2 = CAMERA_PIN_D2,
        .pin_d1 = CAMERA_PIN_D1,
        .pin_d0 = CAMERA_PIN_D0,
        .pin_vsync = CAMERA_PIN_VSYNC,
        .pin_href = CAMERA_PIN_HREF,
        .pin_pclk = CAMERA_PIN_PCLK,
        .xclk_freq_hz = CAMERA_XCLK_HZ,
        /* The backlight owns LEDC timer/channel 1. */
        .ledc_timer = LEDC_TIMER_2,
        .ledc_channel = LEDC_CHANNEL_2,
        .pixel_format = PIXFORMAT_RGB565,
        .frame_size = frame_size,
        .jpeg_quality = 12,
        .fb_count = 1,
        .fb_location = CAMERA_FB_IN_PSRAM,
        /* One buffer: LATEST would let DMA overwrite the frame mid-encode. */
        .grab_mode = CAMERA_GRAB_WHEN_EMPTY,
    };
    heap_caps_free(dma_reserve);
    dma_reserve = NULL;
    ESP_LOGI(TAG, "internal DMA free=%u largest=%u",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        reserve_dma();
        (void)szp_pca9557_set_bit(PCA9557_CAMERA_PWDN_BIT, true);
        ESP_LOGE(TAG, "camera init failed: %s", esp_err_to_name(err));
    }
    return err;
}

static const char *sensor_name(void)
{
    sensor_t *sensor = esp_camera_sensor_get();
    if (sensor == NULL) return "unknown";
    switch (sensor->id.PID) {
    case GC0308_PID: return "GC0308";
    case GC2145_PID: return "GC2145";
    default: return "unknown";
    }
}

static void camera_close(void)
{
    (void)esp_camera_deinit();
    reserve_dma();
    (void)szp_pca9557_set_bit(PCA9557_CAMERA_PWDN_BIT, true);
}

static esp_err_t camera_list(
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
        error->message = "camera.list accepts only {}";
        return ESP_ERR_INVALID_ARG;
    }
    *out = strdup("{\"devices\":[{\"id\":\"" CAMERA_DEVICE_ID "\",\"name\":\"SZP camera\","
                  "\"position\":\"front\",\"deviceType\":\"camera\"}]}");
    return *out != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t camera_snap(
    esp_openclaw_node_handle_t node, void *context, const char *params_json,
    size_t params_len, char **out, esp_openclaw_node_error_t *error)
{
    (void)node;
    (void)context;
    static const char *const KEYS[] = {"facing", "deviceId", "maxWidth", "quality", "format", "delayMs"};
    cJSON *params = cJSON_ParseWithLength(params_json, params_len);
    bool closed = cJSON_IsObject(params);
    for (cJSON *field = closed ? params->child : NULL; field != NULL && closed; field = field->next) {
        bool known = false;
        for (size_t i = 0; i < sizeof(KEYS) / sizeof(KEYS[0]) && !known; ++i) {
            known = field->string != NULL && strcmp(field->string, KEYS[i]) == 0;
        }
        closed = known;
    }
    cJSON *facing = cJSON_GetObjectItemCaseSensitive(params, "facing");
    cJSON *device_id = cJSON_GetObjectItemCaseSensitive(params, "deviceId");
    cJSON *width_item = cJSON_GetObjectItemCaseSensitive(params, "maxWidth");
    cJSON *quality_item = cJSON_GetObjectItemCaseSensitive(params, "quality");
    cJSON *format_item = cJSON_GetObjectItemCaseSensitive(params, "format");
    cJSON *delay_item = cJSON_GetObjectItemCaseSensitive(params, "delayMs");
    int max_width = cJSON_IsNumber(width_item) ? width_item->valueint : 640;
    double quality = cJSON_IsNumber(quality_item) ? quality_item->valuedouble : 0.8;
    int delay_ms = cJSON_IsNumber(delay_item) ? delay_item->valueint : 0;
    const char *format = cJSON_IsString(format_item) ? format_item->valuestring : "jpg";
    bool valid = closed &&
        (facing == NULL || (cJSON_IsString(facing) && strcmp(facing->valuestring, "front") == 0)) &&
        (device_id == NULL || (cJSON_IsString(device_id) && strcmp(device_id->valuestring, CAMERA_DEVICE_ID) == 0)) &&
        (width_item == NULL || cJSON_IsNumber(width_item)) &&
        (quality_item == NULL || cJSON_IsNumber(quality_item)) &&
        (delay_item == NULL || cJSON_IsNumber(delay_item)) &&
        (format_item == NULL || cJSON_IsString(format_item)) &&
        max_width >= 64 && max_width <= 2048 && quality >= 0.01 && quality <= 1.0 &&
        delay_ms >= 0 && delay_ms <= 10000 &&
        (strcmp(format, "jpg") == 0 || strcmp(format, "jpeg") == 0);
    cJSON_Delete(params);
    if (!valid) {
        error->code = "INVALID_PARAMS";
        error->message = "camera.snap accepts front/" CAMERA_DEVICE_ID
            " JPEG, maxWidth 64..2048 (GC2145 max 1600, GC0308 max 640), quality 0.01..1, delayMs 0..10000";
        return ESP_ERR_INVALID_ARG;
    }

    if (!esp_openclaw_room_node_try_acquire_camera()) {
        error->code = "BUSY";
        error->message = "another capture is in progress, or Talk is starting";
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t result = esp_openclaw_room_node_camera_indicator_begin();
    if (result != ESP_OK) {
        esp_openclaw_room_node_release_camera();
        error->code = "PRIVACY_INDICATOR_UNAVAILABLE";
        error->message = "capture refused because the visible camera indicator could not be armed";
        return result;
    }

    int width = 0;
    int height = 0;
    int64_t started_us = esp_timer_get_time();
    result = camera_open(frame_size_for_width(max_width, &width, &height));
    uint8_t *jpeg = NULL;
    size_t jpeg_size = 0;
    const char *sensor = "unknown";
    if (result == ESP_OK) {
        sensor = sensor_name();
        if (delay_ms > 0) vTaskDelay(pdMS_TO_TICKS(delay_ms));
        for (int i = 0; i < CAMERA_WARMUP_FRAMES; ++i) {
            camera_fb_t *warmup = esp_camera_fb_get();
            if (warmup != NULL) esp_camera_fb_return(warmup);
        }
        camera_fb_t *frame = esp_camera_fb_get();
        if (frame == NULL) {
            result = ESP_ERR_TIMEOUT;
        } else {
            width = (int)frame->width;
            height = (int)frame->height;
            uint8_t q = (uint8_t)(quality * 100.0);
            if (!frame2jpg(frame, q < 1 ? 1 : q, &jpeg, &jpeg_size)) result = ESP_FAIL;
            esp_camera_fb_return(frame);
        }
        camera_close();
    }
    esp_openclaw_room_node_camera_indicator_end();
    esp_openclaw_room_node_release_camera();
    ESP_LOGI(TAG, "snap %s %dx%d result=%s jpeg=%u bytes in %lld ms",
        sensor, width, height, esp_err_to_name(result), (unsigned)jpeg_size,
        (long long)((esp_timer_get_time() - started_us) / 1000));
    if (result != ESP_OK) {
        free(jpeg);
        error->code = "UNAVAILABLE";
        error->message = "camera capture or JPEG encode failed";
        return result;
    }

    size_t base64_size = 4 * ((jpeg_size + 2) / 3);
    size_t payload_size = base64_size + 96;
    char *payload = malloc(payload_size);
    if (payload == NULL) {
        free(jpeg);
        return ESP_ERR_NO_MEM;
    }
    int prefix = snprintf(payload, payload_size, "{\"format\":\"jpg\",\"base64\":\"");
    size_t written = 0;
    int b64 = mbedtls_base64_encode(
        (unsigned char *)payload + prefix, payload_size - (size_t)prefix, &written, jpeg, jpeg_size);
    free(jpeg);
    if (b64 != 0) {
        free(payload);
        return ESP_FAIL;
    }
    snprintf(payload + prefix + written, payload_size - (size_t)prefix - written,
        "\",\"width\":%d,\"height\":%d,\"sensor\":\"%s\"}", width, height, sensor);
    *out = payload;
    return ESP_OK;
}

esp_err_t szp_camera_register_commands(esp_openclaw_node_handle_t node)
{
    static const esp_openclaw_node_command_t COMMANDS[] = {
        {.name = "camera.list", .handler = camera_list},
        {.name = "camera.snap", .handler = camera_snap},
    };
    ESP_RETURN_ON_ERROR(esp_openclaw_node_register_capability(node, "camera"), TAG, "camera capability");
    for (size_t i = 0; i < sizeof(COMMANDS) / sizeof(COMMANDS[0]); ++i) {
        ESP_RETURN_ON_ERROR(
            esp_openclaw_node_register_command(node, &COMMANDS[i]), TAG, "register %s", COMMANDS[i].name);
    }
    return ESP_OK;
}
