/*
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_codec_dev.h"
#include "esp_err.h"
#include "esp_openclaw_node.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    lv_display_t *(*start)(void *ctx);
    esp_err_t (*setup_local_input)(void *ctx, void (*toggle_view)(void));
    bool (*lock)(void *ctx, uint32_t timeout_ms);
    void (*unlock)(void *ctx);
    esp_err_t (*set_brightness)(void *ctx, int percent);
    uint16_t native_width;
    uint16_t native_height;
    uint16_t safe_inset;
    bool animated_face; /**< Enable the procedural face when the display pipeline can sustain it. */
    uint16_t animation_frame_ms; /**< Sustainable face cadence for this display pipeline; 0 uses 16 ms. */
    void *ctx;
    uint8_t idle_brightness; /**< Idle backlight percentage, 0..100; omitted/0 preserves display sleep. */
} esp_openclaw_room_display_port_t;

typedef struct {
    esp_codec_dev_handle_t record;
    esp_codec_dev_handle_t playback;
} esp_openclaw_room_audio_handles_t;

#define ESP_OPENCLAW_ROOM_PLAYBACK_GAIN_MAX_DB 12.0f

typedef struct {
    esp_err_t (*open)(void *ctx, esp_openclaw_room_audio_handles_t *handles);
    const char *afe_layout;
    uint8_t record_channels;
    uint8_t channel_mask;
    uint8_t playback_volume;
    bool configure_input_gain; /**< Apply input_gain_db instead of preserving the codec/board default. */
    float input_gain_db;
    void *ctx;
    /* Append optional policy so existing positional board initializers retain their meaning. */
    float playback_gain_db; /**< Post-decode PCM16 boost, 0..12 dB; 0 preserves samples exactly. */
} esp_openclaw_room_audio_port_t;

typedef struct {
    esp_err_t (*prepare_runtime)(void *ctx);
    esp_err_t (*prepare_network)(void *ctx);
    esp_err_t (*register_commands)(void *ctx, esp_openclaw_node_handle_t node);
    void *ctx;
    /** Optional: called on the Talk worker right before Talk acquires the audio
     *  pipeline (true) and after it releases it (false). Boards that park
     *  internal RAM for other peripherals (e.g. a camera DMA buffer, which the
     *  camera lease keeps idle during Talk) lend it to the audio tasks here. */
    void (*talk_media_changed)(void *ctx, bool talk_owns_media);
} esp_openclaw_room_services_port_t;

/** Optional board-owned file namespace. Omit it to advertise no file commands. */
typedef struct {
    const char *public_root;
    bool (*is_available)(void *ctx);
    esp_err_t (*get_metrics)(void *ctx, uint64_t *total_bytes, uint64_t *free_bytes);
    void *ctx;
} esp_openclaw_room_storage_port_t;

/** Optional desk-assistant surface: an idle clock plus desk.* node commands
 *  (notices, a ticker line, countdown timers, do-not-disturb). */
typedef struct {
    bool enabled;
    const char *timezone;       /**< POSIX TZ such as "CST-8"; NULL keeps UTC. */
    const lv_font_t *text_font; /**< Notice/ticker/date font (CJK); NULL uses Montserrat and English. */
    uint8_t brightness;         /**< Idle clock backlight 1..100; 0 uses 30. */
    uint8_t night_brightness;   /**< 22:00-07:00 idle backlight; 0 uses 6. */
} esp_openclaw_room_desk_port_t;

typedef struct {
    const char *display_name;
    const char *model_identifier;
    esp_openclaw_room_display_port_t display;
    esp_openclaw_room_audio_port_t audio;
    esp_openclaw_room_services_port_t services;
    esp_openclaw_room_storage_port_t storage;
    esp_openclaw_room_desk_port_t desk;
} esp_openclaw_room_node_config_t;

/** Start the canonical room-node product. This call owns the product lifecycle. */
esp_err_t esp_openclaw_room_node_start(const esp_openclaw_room_node_config_t *config);

/** Board camera adapters use this to serialize capture against Talk media
 *  (unless CONFIG_OPENCLAW_ROOM_CAMERA_DURING_TALK lets bridge calls overlap). */
bool esp_openclaw_room_node_try_acquire_camera(void);
void esp_openclaw_room_node_release_camera(void);

/** Arm/disarm the shared, visibly flushed camera privacy indicator. */
esp_err_t esp_openclaw_room_node_camera_indicator_begin(void);
void esp_openclaw_room_node_camera_indicator_end(void);

/** Local board triggers (buttons, motion): same admission as the wake word.
 *  Returns true when a new Talk session was admitted. */
bool esp_openclaw_room_node_request_talk(void);
/** Ends the active Talk session, if any. Returns true when one was stopping. */
bool esp_openclaw_room_node_request_talk_stop(void);
/** Mutes or unmutes wake-word admission (do-not-disturb); Talk commands still work. */
void esp_openclaw_room_node_set_wake_muted(bool muted);
bool esp_openclaw_room_node_wake_muted(void);
/** True while a Talk session is admitted or running. */
bool esp_openclaw_room_node_talk_busy(void);
/** Plays the short diagnostics chime; refused while Talk owns the speaker. */
esp_err_t esp_openclaw_room_node_play_chime(void);

#ifdef __cplusplus
}
#endif
