#include "esp_openclaw_room_node.h"
#include "szp_board.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_commands.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_touch_ft5x06.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define TAG "lichuang_room"

/* 78/xiaozhi-fonts: Alibaba PuHuiTi 16 px, ~7400 common CJK glyphs. */
LV_FONT_DECLARE(font_puhui_16_4);

/* LCSC (Lichuang) "SZP" ESP32-S3 board. Pin map matches the vendor schematic
 * as used by xiaozhi-esp32's lichuang-dev board. */
#define LICHUANG_I2C_SDA GPIO_NUM_1
#define LICHUANG_I2C_SCL GPIO_NUM_2

#define LICHUANG_PCA9557_ADDR 0x19
#define PCA9557_REG_OUTPUT 0x01
#define PCA9557_REG_CONFIG 0x03
#define PCA9557_LCD_CS_BIT 0      /* Active low; the panel has no other CS. */
#define PCA9557_PA_EN_BIT 1       /* NS4150 speaker amplifier enable. */

#define LICHUANG_I2S_NUM I2S_NUM_0
#define LICHUANG_I2S_MCLK GPIO_NUM_38
#define LICHUANG_I2S_BCLK GPIO_NUM_14
#define LICHUANG_I2S_WS GPIO_NUM_13
#define LICHUANG_I2S_DOUT GPIO_NUM_45
#define LICHUANG_I2S_DIN GPIO_NUM_12
#define LICHUANG_ES7210_ADDR 0x82

#define LICHUANG_LCD_SPI_HOST SPI3_HOST
#define LICHUANG_LCD_MOSI GPIO_NUM_40
#define LICHUANG_LCD_SCLK GPIO_NUM_41
#define LICHUANG_LCD_DC GPIO_NUM_39
#define LICHUANG_LCD_BACKLIGHT GPIO_NUM_42
#define LICHUANG_LCD_H_RES 320
#define LICHUANG_LCD_V_RES 240
#define LICHUANG_DRAW_ROWS 8

#define LICHUANG_BACKLIGHT_TIMER LEDC_TIMER_1
#define LICHUANG_BACKLIGHT_CHANNEL LEDC_CHANNEL_1

static i2c_master_bus_handle_t i2c_bus;
static i2c_master_dev_handle_t pca9557;
/* Output-register shadow. The camera toggles its power bit at runtime while
 * the audio path may still be latching the amplifier, so writes serialize. */
static uint8_t pca9557_output;
static SemaphoreHandle_t pca9557_lock;
static StaticSemaphore_t pca9557_lock_storage;
static void (*toggle_room_view)(void);

i2c_master_bus_handle_t szp_i2c_bus(void)
{
    return i2c_bus;
}

esp_err_t szp_pca9557_set_bit(uint8_t bit, bool level)
{
    ESP_RETURN_ON_FALSE(pca9557_lock != NULL, ESP_ERR_INVALID_STATE, TAG, "PCA9557 not prepared");
    xSemaphoreTake(pca9557_lock, portMAX_DELAY);
    uint8_t output = level
        ? (uint8_t)(pca9557_output | (1U << bit))
        : (uint8_t)(pca9557_output & ~(1U << bit));
    const uint8_t write[] = {PCA9557_REG_OUTPUT, output};
    esp_err_t err = i2c_master_transmit(pca9557, write, sizeof(write), 100);
    if (err == ESP_OK) pca9557_output = output;
    xSemaphoreGive(pca9557_lock);
    ESP_RETURN_ON_ERROR(err, TAG, "PCA9557 output");
    return ESP_OK;
}

static esp_err_t lichuang_prepare_runtime(void *ctx)
{
    (void)ctx;
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = LICHUANG_I2C_PORT,
        .sda_io_num = LICHUANG_I2C_SDA,
        .scl_io_num = LICHUANG_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &i2c_bus), TAG, "I2C bus");
    const i2c_device_config_t pca_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = LICHUANG_PCA9557_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(
        i2c_master_bus_add_device(i2c_bus, &pca_cfg, &pca9557), TAG, "PCA9557 device");
    pca9557_lock = xSemaphoreCreateMutexStatic(&pca9557_lock_storage);

    /* Latch safe levels before switching the pins to outputs: LCD deselected,
     * amplifier off until audio is open, camera powered down (unused). */
    pca9557_output = (1U << PCA9557_LCD_CS_BIT) | (1U << PCA9557_CAMERA_PWDN_BIT);
    const uint8_t output[] = {PCA9557_REG_OUTPUT, pca9557_output};
    ESP_RETURN_ON_ERROR(
        i2c_master_transmit(pca9557, output, sizeof(output), 100), TAG, "PCA9557 output");
    const uint8_t config[] = {PCA9557_REG_CONFIG, 0xf8};
    ESP_RETURN_ON_ERROR(
        i2c_master_transmit(pca9557, config, sizeof(config), 100), TAG, "PCA9557 direction");

    if (szp_camera_reserve() != ESP_OK) {
        ESP_LOGW(TAG, "camera DMA reserve failed; camera.snap may fail");
    }

    /* Motion gestures are optional: a missing IMU only logs a warning. */
    esp_err_t imu_err = szp_imu_start();
    if (imu_err != ESP_OK) {
        ESP_LOGW(TAG, "IMU start failed: %s", esp_err_to_name(imu_err));
    }

    const ledc_timer_config_t timer_cfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LICHUANG_BACKLIGHT_TIMER,
        .freq_hz = 20000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer_cfg), TAG, "backlight timer");
    const ledc_channel_config_t channel_cfg = {
        .gpio_num = LICHUANG_LCD_BACKLIGHT,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LICHUANG_BACKLIGHT_CHANNEL,
        .timer_sel = LICHUANG_BACKLIGHT_TIMER,
        .duty = 0,
        /* The backlight driver is active low. */
        .flags.output_invert = 1,
    };
    return ledc_channel_config(&channel_cfg);
}

static void boot_button_task(void *arg)
{
    (void)arg;
    bool was_pressed = false;
    for (;;) {
        bool pressed = gpio_get_level(GPIO_NUM_0) == 0;
        if (pressed && !was_pressed && toggle_room_view != NULL) {
            toggle_room_view();
        }
        was_pressed = pressed;
        vTaskDelay(pdMS_TO_TICKS(60));
    }
}

static esp_err_t lichuang_setup_local_input(void *ctx, void (*toggle_view)(void))
{
    (void)ctx;
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << GPIO_NUM_0,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "BOOT button GPIO");
    toggle_room_view = toggle_view;
    return xTaskCreateWithCaps(
        boot_button_task,
        "boot_btn",
        2560,
        NULL,
        4,
        NULL,
        MALLOC_CAP_SPIRAM) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

#define RETURN_NULL_ON_ERROR(expr, what) do { \
        esp_err_t err_ = (expr); \
        if (err_ != ESP_OK) { \
            ESP_LOGE(TAG, "%s: %s", what, esp_err_to_name(err_)); \
            return NULL; \
        } \
    } while (0)

static lv_display_t *lichuang_display_start(void *ctx)
{
    (void)ctx;
    const spi_bus_config_t bus_cfg = {
        .mosi_io_num = LICHUANG_LCD_MOSI,
        .miso_io_num = GPIO_NUM_NC,
        .sclk_io_num = LICHUANG_LCD_SCLK,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = LICHUANG_LCD_H_RES * LICHUANG_DRAW_ROWS * sizeof(uint16_t),
    };
    RETURN_NULL_ON_ERROR(
        spi_bus_initialize(LICHUANG_LCD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO), "LCD SPI bus");

    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = GPIO_NUM_NC,
        .dc_gpio_num = LICHUANG_LCD_DC,
        /* 40 MHz stays inside the ST7789's ~62 MHz write-clock limit. */
        .spi_mode = 2,
        .pclk_hz = 40 * 1000 * 1000,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    esp_lcd_panel_io_handle_t io = NULL;
    RETURN_NULL_ON_ERROR(
        esp_lcd_new_panel_io_spi(LICHUANG_LCD_SPI_HOST, &io_cfg, &io), "LCD panel IO");
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = GPIO_NUM_NC,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    esp_lcd_panel_handle_t panel = NULL;
    RETURN_NULL_ON_ERROR(esp_lcd_new_panel_st7789(io, &panel_cfg, &panel), "ST7789 panel");
    /* SCLK sits low until the bus runs its first mode-2 transaction. Selecting
     * the panel first turns that low->high step into a phantom clock edge and
     * shifts every following byte by one bit, so the panel never wakes. Clock
     * one NOP while deselected to park SCLK high, then select the panel before
     * the software reset so it actually receives it. */
    RETURN_NULL_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_NOP, NULL, 0), "LCD idle clock");
    RETURN_NULL_ON_ERROR(szp_pca9557_set_bit(PCA9557_LCD_CS_BIT, false), "LCD chip select");
    RETURN_NULL_ON_ERROR(esp_lcd_panel_reset(panel), "panel reset");
    /* The panel stays powered across a chip reset, so SWRESET can land while
     * it is awake; ST7789 then ignores SLPOUT for 120 ms. The driver waits
     * only 20 ms, which leaves the backlit panel asleep and black. */
    vTaskDelay(pdMS_TO_TICKS(120));
    RETURN_NULL_ON_ERROR(esp_lcd_panel_init(panel), "panel init");
    RETURN_NULL_ON_ERROR(esp_lcd_panel_invert_color(panel, true), "panel invert");
    /* The panel is mounted in portrait; hardware-rotate it to landscape. */
    RETURN_NULL_ON_ERROR(esp_lcd_panel_swap_xy(panel, true), "panel swap");
    RETURN_NULL_ON_ERROR(esp_lcd_panel_mirror(panel, true, false), "panel mirror");
    RETURN_NULL_ON_ERROR(esp_lcd_panel_disp_on_off(panel, true), "panel on");

    /* Internal RAM is the scarce pool: once the node session is up, the
     * operator client still needs two internal 8 KiB task stacks. Keep the
     * LVGL stack in PSRAM (as room-node does for its own supervisors) and the
     * internal DMA flush chunks at 8 rows (2 x 5 KiB). */
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_stack_caps = MALLOC_CAP_SPIRAM;
    RETURN_NULL_ON_ERROR(lvgl_port_init(&port_cfg), "LVGL port");
    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = io,
        .panel_handle = panel,
        .buffer_size = LICHUANG_LCD_H_RES * LICHUANG_DRAW_ROWS,
        .double_buffer = true,
        .hres = LICHUANG_LCD_H_RES,
        .vres = LICHUANG_LCD_V_RES,
        .monochrome = false,
        .color_format = LV_COLOR_FORMAT_RGB565,
        /* Must match the panel orientation set above. */
        .rotation = {.swap_xy = true, .mirror_x = true, .mirror_y = false},
        .flags = {.buff_dma = true, .buff_spiram = false, .swap_bytes = true},
    };
    lv_display_t *display = lvgl_port_add_disp(&display_cfg);
    if (display == NULL) {
        return NULL;
    }

    esp_lcd_panel_io_handle_t touch_io = NULL;
    esp_lcd_panel_io_i2c_config_t touch_io_cfg = ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG();
    touch_io_cfg.scl_speed_hz = 400000;
    if (esp_lcd_new_panel_io_i2c(i2c_bus, &touch_io_cfg, &touch_io) == ESP_OK) {
        const esp_lcd_touch_config_t touch_cfg = {
            .x_max = LICHUANG_LCD_V_RES,
            .y_max = LICHUANG_LCD_H_RES,
            .rst_gpio_num = GPIO_NUM_NC,
            .int_gpio_num = GPIO_NUM_NC,
            .levels = {.reset = 0, .interrupt = 0},
            .flags = {.swap_xy = 1, .mirror_x = 1, .mirror_y = 0},
        };
        esp_lcd_touch_handle_t touch = NULL;
        if (esp_lcd_touch_new_i2c_ft5x06(touch_io, &touch_cfg, &touch) == ESP_OK) {
            const lvgl_port_touch_cfg_t lv_touch_cfg = {.disp = display, .handle = touch};
            if (lvgl_port_add_touch(&lv_touch_cfg) == NULL) {
                ESP_LOGW(TAG, "FT6336 touch could not be attached to LVGL");
            }
        } else {
            ESP_LOGW(TAG, "FT6336 touch controller not detected");
        }
    }
    return display;
}

static bool lichuang_display_lock(void *ctx, uint32_t timeout_ms)
{
    (void)ctx;
    return lvgl_port_lock(timeout_ms);
}

static void lichuang_display_unlock(void *ctx)
{
    (void)ctx;
    lvgl_port_unlock();
}

static esp_err_t lichuang_set_brightness(void *ctx, int percent)
{
    (void)ctx;
    if (percent < 0) {
        percent = 0;
    } else if (percent > 100) {
        percent = 100;
    }
    uint32_t duty = (uint32_t)percent * ((1U << LEDC_TIMER_10_BIT) - 1U) / 100U;
    ESP_RETURN_ON_ERROR(
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LICHUANG_BACKLIGHT_CHANNEL, duty), TAG, "backlight duty");
    return ledc_update_duty(LEDC_LOW_SPEED_MODE, LICHUANG_BACKLIGHT_CHANNEL);
}

static esp_err_t lichuang_audio_open_codecs(
    void *ctx,
    esp_openclaw_room_audio_handles_t *handles)
{
    (void)ctx;
    ESP_RETURN_ON_FALSE(handles != NULL, ESP_ERR_INVALID_ARG, TAG, "audio handles required");
    ESP_RETURN_ON_FALSE(i2c_bus != NULL, ESP_ERR_INVALID_STATE, TAG, "I2C bus not prepared");

    i2s_chan_handle_t tx = NULL;
    i2s_chan_handle_t rx = NULL;
    i2s_chan_config_t channel_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(LICHUANG_I2S_NUM, I2S_ROLE_MASTER);
    channel_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&channel_cfg, &tx, &rx), TAG, "I2S channel create");

    const i2s_std_config_t i2s_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(24000),
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = LICHUANG_I2S_MCLK,
            .bclk = LICHUANG_I2S_BCLK,
            .ws = LICHUANG_I2S_WS,
            .dout = LICHUANG_I2S_DOUT,
            .din = LICHUANG_I2S_DIN,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx, &i2s_cfg), TAG, "I2S TX init");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(rx, &i2s_cfg), TAG, "I2S RX init");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(tx), TAG, "I2S TX enable");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(rx), TAG, "I2S RX enable");

    audio_codec_i2s_cfg_t data_cfg = {
        .port = LICHUANG_I2S_NUM,
        .rx_handle = rx,
        .tx_handle = tx,
    };
    const audio_codec_data_if_t *data = audio_codec_new_i2s_data(&data_cfg);
    ESP_RETURN_ON_FALSE(data != NULL, ESP_ERR_NO_MEM, TAG, "I2S codec data interface");

    audio_codec_i2c_cfg_t speaker_ctrl_cfg = {
        .port = LICHUANG_I2C_PORT,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = i2c_bus,
    };
    const audio_codec_ctrl_if_t *speaker_ctrl = audio_codec_new_i2c_ctrl(&speaker_ctrl_cfg);
    ESP_RETURN_ON_FALSE(
        speaker_ctrl != NULL, ESP_ERR_NO_MEM, TAG, "ES8311 control interface");
    const audio_codec_gpio_if_t *gpio = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(gpio != NULL, ESP_ERR_NO_MEM, TAG, "ES8311 GPIO interface");
    esp_codec_dev_hw_gain_t gain = {.pa_voltage = 5.0, .codec_dac_voltage = 3.3};
    es8311_codec_cfg_t speaker_cfg = {
        .ctrl_if = speaker_ctrl,
        .gpio_if = gpio,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        /* The amplifier enable sits behind the PCA9557, not a GPIO. */
        .pa_pin = -1,
        .use_mclk = true,
        .hw_gain = gain,
    };
    const audio_codec_if_t *speaker = es8311_codec_new(&speaker_cfg);
    ESP_RETURN_ON_FALSE(speaker != NULL, ESP_ERR_NO_MEM, TAG, "ES8311 codec interface");
    esp_codec_dev_cfg_t speaker_dev = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = speaker,
        .data_if = data,
    };
    handles->playback = esp_codec_dev_new(&speaker_dev);
    ESP_RETURN_ON_FALSE(
        handles->playback != NULL, ESP_ERR_NO_MEM, TAG, "ES8311 playback device");

    audio_codec_i2c_cfg_t microphone_ctrl_cfg = {
        .port = LICHUANG_I2C_PORT,
        .addr = LICHUANG_ES7210_ADDR,
        .bus_handle = i2c_bus,
    };
    const audio_codec_ctrl_if_t *microphone_ctrl =
        audio_codec_new_i2c_ctrl(&microphone_ctrl_cfg);
    ESP_RETURN_ON_FALSE(
        microphone_ctrl != NULL, ESP_ERR_NO_MEM, TAG, "ES7210 control interface");
    es7210_codec_cfg_t microphone_cfg = {
        .ctrl_if = microphone_ctrl,
        /* Near-end MIC1 plus the speaker-reference loopback on MIC3. */
        .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC3,
    };
    const audio_codec_if_t *microphone = es7210_codec_new(&microphone_cfg);
    ESP_RETURN_ON_FALSE(
        microphone != NULL, ESP_ERR_NO_MEM, TAG, "ES7210 codec interface");
    esp_codec_dev_cfg_t microphone_dev = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = microphone,
        .data_if = data,
    };
    handles->record = esp_codec_dev_new(&microphone_dev);
    ESP_RETURN_ON_FALSE(
        handles->record != NULL, ESP_ERR_NO_MEM, TAG, "ES7210 capture device");

    /* Talk and the diagnostics tone share one always-open renderer, so the
     * amplifier stays enabled for the life of the audio path. */
    return szp_pca9557_set_bit(PCA9557_PA_EN_BIT, true);
}

static esp_err_t lichuang_audio_open(
    void *ctx,
    esp_openclaw_room_audio_handles_t *handles)
{
    esp_err_t err = lichuang_audio_open_codecs(ctx, handles);
    /* Success or not, the codec init burst on the shared bus is over. */
    szp_imu_audio_ready();
    return err;
}

static void lichuang_talk_media_changed(void *ctx, bool talk_owns_media)
{
    (void)ctx;
    szp_camera_talk_media_changed(talk_owns_media);
}

static esp_err_t lichuang_register_commands(void *ctx, esp_openclaw_node_handle_t node)
{
    (void)ctx;
    ESP_RETURN_ON_ERROR(szp_camera_register_commands(node), TAG, "camera commands");
    return szp_imu_register_commands(node);
}

void app_main(void)
{
    const esp_openclaw_room_node_config_t config = {
        .display_name = "OpenClaw SZP Room Node",
        .model_identifier = "lichuang-szp-esp32-s3",
        .display = {
            .start = lichuang_display_start,
            .setup_local_input = lichuang_setup_local_input,
            .lock = lichuang_display_lock,
            .unlock = lichuang_display_unlock,
            .set_brightness = lichuang_set_brightness,
            .native_width = LICHUANG_LCD_H_RES,
            .native_height = LICHUANG_LCD_V_RES,
            .safe_inset = 8,
            .animated_face = true,
            .animation_frame_ms = 33,
        },
        .audio = {
            .open = lichuang_audio_open,
            .afe_layout = "MR",
            .record_channels = 2,
            .channel_mask = 0x3,
            .playback_volume = 90,
            .configure_input_gain = true,
            .input_gain_db = 30.0f,
        },
        .services = {
            .prepare_runtime = lichuang_prepare_runtime,
            .register_commands = lichuang_register_commands,
            .talk_media_changed = lichuang_talk_media_changed,
        },
        .desk = {
            .enabled = true,
            .timezone = CONFIG_LICHUANG_DESK_TIMEZONE,
            .text_font = &font_puhui_16_4,
            .brightness = 35,
            .night_brightness = 6,
        },
    };
    ESP_ERROR_CHECK(esp_openclaw_room_node_start(&config));
}
