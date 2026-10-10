#include "room_desk.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "esp_openclaw_room_node.h"
#include "room_board.h"

#define TAG "room_desk"

#define DESK_MAX_TIMERS 8
#define DESK_TIMER_LABEL_MAX 32
#define DESK_NOTICE_TITLE_MAX 64
#define DESK_NOTICE_TEXT_MAX 240
#define DESK_TICKER_MAX 96
#define DESK_NOTICE_DEFAULT_MS 15000
#define DESK_NOTICE_MAX_MS 600000
#define DESK_TIMER_NOTICE_MS 60000
#define DESK_TIMER_MAX_SECONDS 86400
#define DESK_TICK_MS 250
#define DESK_NIGHT_START_HOUR 22
#define DESK_NIGHT_END_HOUR 7
/* Before SNTP the RTC starts at 1970; anything before 2024 is unsynced. */
#define DESK_MIN_VALID_EPOCH 1704067200

typedef struct {
    uint32_t id;
    int64_t due_us;
    char label[DESK_TIMER_LABEL_MAX + 1];
} desk_timer_t;

/* Command handlers (node task) write these facts; the LVGL tick reads them.
 * The spinlock only guards copies, never LVGL calls. */
static portMUX_TYPE desk_mux = portMUX_INITIALIZER_UNLOCKED;
static desk_timer_t timers[DESK_MAX_TIMERS];
static uint32_t next_timer_id = 1;
static char notice_title[DESK_NOTICE_TITLE_MAX + 1];
static char notice_text[DESK_NOTICE_TEXT_MAX + 1];
static uint32_t notice_duration_ms;
static bool notice_chime;
static bool notice_pending;
static char ticker_text[DESK_TICKER_MAX + 1];
static int64_t ticker_until_us;
static bool ticker_dirty;

static bool enabled;
static bool chinese;
static const lv_font_t *text_font;
static uint8_t day_brightness;
static uint8_t night_brightness;

/* LVGL-owned; touched only with the display lock held. */
static lv_obj_t *clock_root;
static lv_obj_t *status_left;
static lv_obj_t *status_right;
static lv_obj_t *time_label;
static lv_obj_t *date_label;
static lv_obj_t *ticker_label;
static lv_obj_t *timer_label;
static lv_obj_t *notice_card;
static lv_obj_t *notice_title_label;
static lv_obj_t *notice_text_label;
static lv_timer_t *desk_tick;
static int64_t notice_until_us;
static bool notice_visible;
static bool clock_visible;
static bool last_night;
static bool last_synced;
static int last_minute = -1;
static esp_timer_handle_t repaint_timer;
static bool time_sync_started;

static const char *const WEEKDAYS_ZH[] = {"日", "一", "二", "三", "四", "五", "六"};

static void repaint_expired(void *arg)
{
    (void)arg;
    room_ui_refresh();
}

/* Brightness belongs to the UI controller's paint; ask it to repaint from a
 * context that may take the display lock. */
static void request_repaint(void)
{
    if (repaint_timer != NULL) (void)esp_timer_start_once(repaint_timer, 1000);
}

static bool time_synced(time_t now)
{
    return now >= DESK_MIN_VALID_EPOCH;
}

static bool is_night(const struct tm *local)
{
    return local->tm_hour >= DESK_NIGHT_START_HOUR || local->tm_hour < DESK_NIGHT_END_HOUR;
}

static const lv_font_t *time_font(void)
{
#if LV_FONT_MONTSERRAT_48
    return &lv_font_montserrat_48;
#elif LV_FONT_MONTSERRAT_28
    return &lv_font_montserrat_28;
#else
    return &lv_font_montserrat_14;
#endif
}

static lv_obj_t *desk_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_label_set_text(label, "");
    return label;
}

static void notice_clicked(lv_event_t *event)
{
    (void)event;
    notice_until_us = 0;
}

static void notice_hide_locked(void)
{
    if (notice_card != NULL) lv_obj_add_flag(notice_card, LV_OBJ_FLAG_HIDDEN);
    if (notice_visible) {
        notice_visible = false;
        request_repaint();
    }
}

static void notice_show_locked(const char *title, const char *text, uint32_t duration_ms)
{
    if (notice_card == NULL) return;
    lv_label_set_text(notice_title_label, title);
    if (title[0] == '\0') {
        lv_obj_add_flag(notice_title_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(notice_title_label, LV_OBJ_FLAG_HIDDEN);
    }
    lv_label_set_text(notice_text_label, text);
    lv_obj_clear_flag(notice_card, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(notice_card);
    notice_until_us = esp_timer_get_time() + (int64_t)duration_ms * 1000;
    if (!notice_visible) {
        notice_visible = true;
        request_repaint();
    }
}

static void format_remaining(char *out, size_t out_size, int64_t remaining_us)
{
    int64_t seconds = remaining_us <= 0 ? 0 : (remaining_us + 999999) / 1000000;
    int hours = (int)(seconds / 3600);
    int minutes = (int)((seconds % 3600) / 60);
    int secs = (int)(seconds % 60);
    if (hours > 0) {
        snprintf(out, out_size, "%d:%02d:%02d", hours, minutes, secs);
    } else {
        snprintf(out, out_size, "%02d:%02d", minutes, secs);
    }
}

static void update_clock_locked(time_t now, const struct tm *local, int64_t now_us)
{
    bool synced = time_synced(now);
    if (synced) {
        /* Blink the colon so a glance shows the clock is alive. */
        lv_label_set_text_fmt(
            time_label, (now % 2) == 0 ? "%02d:%02d" : "%02d %02d", local->tm_hour, local->tm_min);
        if (local->tm_min != last_minute || synced != last_synced) {
            if (chinese) {
                lv_label_set_text_fmt(date_label, "%d月%d日  星期%s",
                    local->tm_mon + 1, local->tm_mday, WEEKDAYS_ZH[local->tm_wday % 7]);
            } else {
                char date[32];
                strftime(date, sizeof(date), "%a %d %b", local);
                lv_label_set_text(date_label, date);
            }
            last_minute = local->tm_min;
        }
    } else {
        lv_label_set_text(time_label, "--:--");
        lv_label_set_text(date_label, chinese ? "等待网络校时" : "Syncing time");
        last_minute = -1;
    }
    last_synced = synced;

    /* Nearest countdown, if any. */
    desk_timer_t nearest = {0};
    int active = 0;
    taskENTER_CRITICAL(&desk_mux);
    for (size_t i = 0; i < DESK_MAX_TIMERS; ++i) {
        if (timers[i].id == 0) continue;
        if (active == 0 || timers[i].due_us < nearest.due_us) nearest = timers[i];
        active++;
    }
    taskEXIT_CRITICAL(&desk_mux);
    if (active == 0) {
        lv_obj_add_flag(timer_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        char remaining[16];
        format_remaining(remaining, sizeof(remaining), nearest.due_us - now_us);
        const char *prefix = chinese ? "计时" : "Timer";
        if (active > 1) {
            lv_label_set_text_fmt(timer_label, "%s %s  %s  (+%d)", prefix, remaining, nearest.label, active - 1);
        } else {
            lv_label_set_text_fmt(timer_label, "%s %s  %s", prefix, remaining, nearest.label);
        }
        lv_obj_clear_flag(timer_label, LV_OBJ_FLAG_HIDDEN);
    }
}

static void fire_due_timers_locked(int64_t now_us)
{
    desk_timer_t fired = {0};
    bool any = false;
    taskENTER_CRITICAL(&desk_mux);
    for (size_t i = 0; i < DESK_MAX_TIMERS; ++i) {
        if (timers[i].id != 0 && timers[i].due_us <= now_us) {
            if (!any) fired = timers[i];
            any = true;
            memset(&timers[i], 0, sizeof(timers[i]));
        }
    }
    taskEXIT_CRITICAL(&desk_mux);
    if (!any) return;
    ESP_LOGI(TAG, "timer %" PRIu32 " fired: %s", fired.id, fired.label);
    notice_show_locked(
        chinese ? "时间到" : "Time's up",
        fired.label[0] != '\0' ? fired.label : (chinese ? "计时结束" : "Timer finished"),
        DESK_TIMER_NOTICE_MS);
    (void)esp_openclaw_room_node_play_chime();
}

static void desk_tick_cb(lv_timer_t *timer)
{
    (void)timer;
    int64_t now_us = esp_timer_get_time();
    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);

    fire_due_timers_locked(now_us);

    char title[DESK_NOTICE_TITLE_MAX + 1];
    char text[DESK_NOTICE_TEXT_MAX + 1];
    uint32_t duration_ms = 0;
    bool show_notice = false;
    bool chime = false;
    char ticker[DESK_TICKER_MAX + 1];
    bool ticker_changed = false;
    taskENTER_CRITICAL(&desk_mux);
    if (notice_pending) {
        memcpy(title, notice_title, sizeof(title));
        memcpy(text, notice_text, sizeof(text));
        duration_ms = notice_duration_ms;
        chime = notice_chime;
        notice_pending = false;
        show_notice = true;
    }
    if (ticker_until_us != 0 && now_us >= ticker_until_us) {
        ticker_text[0] = '\0';
        ticker_until_us = 0;
        ticker_dirty = true;
    }
    if (ticker_dirty) {
        memcpy(ticker, ticker_text, sizeof(ticker));
        ticker_dirty = false;
        ticker_changed = true;
    }
    taskEXIT_CRITICAL(&desk_mux);

    if (show_notice) {
        notice_show_locked(title, text, duration_ms);
        if (chime) (void)esp_openclaw_room_node_play_chime();
    } else if (notice_visible && now_us >= notice_until_us) {
        notice_hide_locked();
    }
    if (ticker_changed) lv_label_set_text(ticker_label, ticker);

    update_clock_locked(now, &local, now_us);

    bool night = time_synced(now) && is_night(&local);
    if (night != last_night) {
        last_night = night;
        request_repaint();
    }
}

esp_err_t room_desk_init(lv_obj_t *screen)
{
    const esp_openclaw_room_node_config_t *board = room_board_config();
    if (board == NULL || !board->desk.enabled) return ESP_OK;
    enabled = true;
    text_font = board->desk.text_font;
    chinese = text_font != NULL;
    if (text_font == NULL) text_font = &lv_font_montserrat_14;
    day_brightness = board->desk.brightness != 0 ? board->desk.brightness : 30;
    night_brightness = board->desk.night_brightness != 0 ? board->desk.night_brightness : 6;
    if (board->desk.timezone != NULL) {
        setenv("TZ", board->desk.timezone, 1);
        tzset();
    }
    const esp_timer_create_args_t repaint_args = {
        .callback = repaint_expired,
        .name = "desk_repaint",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&repaint_args, &repaint_timer), TAG, "repaint timer");

    int32_t inset = board->display.safe_inset;
    clock_root = lv_obj_create(screen);
    lv_obj_remove_style_all(clock_root);
    lv_obj_set_size(clock_root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(clock_root, inset, 0);
    lv_obj_remove_flag(clock_root, LV_OBJ_FLAG_SCROLLABLE);
    /* Taps fall through to the screen handler (canvas toggle, diagnostics). */
    lv_obj_remove_flag(clock_root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(clock_root, LV_OBJ_FLAG_EVENT_BUBBLE);

    status_left = desk_label(clock_root, text_font, 0x7a7a7a);
    lv_obj_align(status_left, LV_ALIGN_TOP_LEFT, 0, 0);
    status_right = desk_label(clock_root, text_font, 0xd08a2a);
    lv_obj_align(status_right, LV_ALIGN_TOP_RIGHT, 0, 0);

    time_label = desk_label(clock_root, time_font(), 0xffffff);
    lv_obj_align(time_label, LV_ALIGN_CENTER, 0, -34);
    date_label = desk_label(clock_root, text_font, 0xb8b8b8);
    lv_obj_align(date_label, LV_ALIGN_CENTER, 0, 10);

    ticker_label = desk_label(clock_root, text_font, 0x6fb7ff);
    lv_obj_set_width(ticker_label, LV_PCT(100));
    lv_label_set_long_mode(ticker_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(ticker_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(ticker_label, LV_ALIGN_CENTER, 0, 40);

    timer_label = desk_label(clock_root, text_font, 0xffc35a);
    lv_obj_set_width(timer_label, LV_PCT(100));
    lv_label_set_long_mode(timer_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(timer_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(timer_label, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(timer_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(clock_root, LV_OBJ_FLAG_HIDDEN);

    /* Notices ride on the top layer so they cover the face and Canvas too. */
    notice_card = lv_obj_create(lv_layer_top());
    lv_obj_set_width(notice_card, LV_PCT(92));
    lv_obj_set_height(notice_card, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(notice_card, LV_PCT(90), 0);
    lv_obj_set_style_bg_color(notice_card, lv_color_hex(0x15304f), 0);
    lv_obj_set_style_bg_opa(notice_card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(notice_card, lv_color_hex(0x3f7fc0), 0);
    lv_obj_set_style_border_width(notice_card, 2, 0);
    lv_obj_set_style_radius(notice_card, 14, 0);
    lv_obj_set_style_pad_all(notice_card, 12, 0);
    lv_obj_set_style_pad_row(notice_card, 6, 0);
    lv_obj_set_flex_flow(notice_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(notice_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(notice_card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(notice_card, notice_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_center(notice_card);
    notice_title_label = desk_label(notice_card, text_font, 0xffd27a);
    lv_obj_set_width(notice_title_label, LV_PCT(100));
    notice_text_label = desk_label(notice_card, text_font, 0xffffff);
    lv_obj_set_width(notice_text_label, LV_PCT(100));
    lv_label_set_long_mode(notice_text_label, LV_LABEL_LONG_WRAP);
    lv_obj_add_flag(notice_card, LV_OBJ_FLAG_HIDDEN);

    desk_tick = lv_timer_create(desk_tick_cb, DESK_TICK_MS, NULL);
    ESP_RETURN_ON_FALSE(desk_tick != NULL, ESP_ERR_NO_MEM, TAG, "desk tick");
    ESP_LOGI(TAG, "desk clock enabled (TZ=%s)", board->desk.timezone != NULL ? board->desk.timezone : "UTC");
    return ESP_OK;
}

bool room_desk_enabled(void)
{
    return enabled && clock_root != NULL;
}

bool room_desk_clock_show(room_ui_state_t state, const char *detail, const room_ui_facts_t *facts)
{
    if (!room_desk_enabled()) return false;
    bool online = facts != NULL && facts->gateway == ROOM_UI_GATEWAY_CONNECTED;
    if (state == ROOM_UI_ERROR) {
        lv_obj_set_style_text_color(status_left, lv_color_hex(0xe0605a), 0);
        lv_label_set_text(status_left, detail != NULL && detail[0] != '\0' ? detail : "Error");
    } else {
        lv_obj_set_style_text_color(status_left, lv_color_hex(online ? 0x5fbf6a : 0x7a7a7a), 0);
        lv_label_set_text(status_left,
            online ? (chinese ? "已连接" : "Online") : (chinese ? "离线" : "Offline"));
    }
    lv_label_set_text(status_right,
        esp_openclaw_room_node_wake_muted() ? (chinese ? "勿扰" : "DND") : "");
    lv_obj_clear_flag(clock_root, LV_OBJ_FLAG_HIDDEN);
    if (notice_card != NULL) lv_obj_move_foreground(notice_card);
    if (!clock_visible) {
        /* Repaint the time immediately instead of waiting for the next tick. */
        clock_visible = true;
        last_minute = -1;
        time_t now = time(NULL);
        struct tm local;
        localtime_r(&now, &local);
        update_clock_locked(now, &local, esp_timer_get_time());
    }
    return true;
}

void room_desk_clock_hide(void)
{
    if (clock_root != NULL) lv_obj_add_flag(clock_root, LV_OBJ_FLAG_HIDDEN);
    clock_visible = false;
}

int room_desk_idle_brightness(void)
{
    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    return time_synced(now) && is_night(&local) ? night_brightness : day_brightness;
}

bool room_desk_notice_active(void)
{
    return notice_visible;
}

void room_desk_start_time_sync(void)
{
    if (!enabled || time_sync_started) return;
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_OPENCLAW_ROOM_DESK_NTP_SERVER);
    config.start = true;
    esp_err_t err = esp_netif_sntp_init(&config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SNTP start failed: %s", esp_err_to_name(err));
        return;
    }
    time_sync_started = true;
    ESP_LOGI(TAG, "SNTP started (%s)", CONFIG_OPENCLAW_ROOM_DESK_NTP_SERVER);
}

/* ---- node commands ---- */

static esp_err_t fail(esp_openclaw_node_error_t *error, const char *code, const char *message, esp_err_t err)
{
    error->code = code;
    error->message = message;
    return err;
}

static esp_err_t reply(cJSON *root, char **out, esp_openclaw_node_error_t *error)
{
    *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return *out != NULL ? ESP_OK
        : fail(error, "INTERNAL", "not enough memory for the command result", ESP_ERR_NO_MEM);
}

/* Accepts only the listed keys so typos surface instead of being ignored. */
static bool keys_allowed(const cJSON *params, const char *const *keys, size_t count)
{
    for (const cJSON *field = params->child; field != NULL; field = field->next) {
        bool known = false;
        for (size_t i = 0; i < count && !known; ++i) {
            known = field->string != NULL && strcmp(field->string, keys[i]) == 0;
        }
        if (!known) return false;
    }
    return true;
}

static bool optional_string(const cJSON *params, const char *key, size_t max_len, const char **out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(params, key);
    if (item == NULL) {
        *out = NULL;
        return true;
    }
    if (!cJSON_IsString(item) || item->valuestring == NULL || strlen(item->valuestring) > max_len) return false;
    *out = item->valuestring;
    return true;
}

static bool optional_int(const cJSON *params, const char *key, double min, double max, int64_t *out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(params, key);
    if (item == NULL) return true;
    if (!cJSON_IsNumber(item) || item->valuedouble < min || item->valuedouble > max) return false;
    *out = (int64_t)item->valuedouble;
    return true;
}

static void add_timers(cJSON *root, int64_t now_us)
{
    cJSON *list = cJSON_AddArrayToObject(root, "timers");
    desk_timer_t copy[DESK_MAX_TIMERS];
    taskENTER_CRITICAL(&desk_mux);
    memcpy(copy, timers, sizeof(copy));
    taskEXIT_CRITICAL(&desk_mux);
    for (size_t i = 0; list != NULL && i < DESK_MAX_TIMERS; ++i) {
        if (copy[i].id == 0) continue;
        cJSON *entry = cJSON_CreateObject();
        if (entry == NULL) break;
        cJSON_AddNumberToObject(entry, "id", copy[i].id);
        cJSON_AddStringToObject(entry, "label", copy[i].label);
        int64_t remaining = copy[i].due_us - now_us;
        cJSON_AddNumberToObject(entry, "remainingSeconds", remaining > 0 ? (double)((remaining + 999999) / 1000000) : 0);
        cJSON_AddItemToArray(list, entry);
    }
}

static esp_err_t handle_status(
    esp_openclaw_node_handle_t node, void *context, const char *params_json,
    size_t params_len, char **out, esp_openclaw_node_error_t *error)
{
    (void)node;
    (void)context;
    cJSON *params = cJSON_ParseWithLength(params_json, params_len);
    bool valid = cJSON_IsObject(params) && params->child == NULL;
    cJSON_Delete(params);
    if (!valid) return fail(error, "INVALID_PARAMS", "desk.status accepts only {}", ESP_ERR_INVALID_ARG);

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) return fail(error, "INTERNAL", "out of memory", ESP_ERR_NO_MEM);
    time_t now = time(NULL);
    bool synced = time_synced(now);
    cJSON_AddBoolToObject(root, "timeSynced", synced);
    if (synced) {
        struct tm local;
        localtime_r(&now, &local);
        char iso[40];
        strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%S%z", &local);
        cJSON_AddStringToObject(root, "localTime", iso);
    }
    const char *tz = getenv("TZ");
    cJSON_AddStringToObject(root, "timezone", tz != NULL ? tz : "UTC");
    cJSON_AddBoolToObject(root, "dnd", esp_openclaw_room_node_wake_muted());
    cJSON_AddBoolToObject(root, "talkActive", esp_openclaw_room_node_talk_busy());
    taskENTER_CRITICAL(&desk_mux);
    char ticker[DESK_TICKER_MAX + 1];
    memcpy(ticker, ticker_text, sizeof(ticker));
    taskEXIT_CRITICAL(&desk_mux);
    cJSON_AddStringToObject(root, "ticker", ticker);
    cJSON_AddBoolToObject(root, "noticeVisible", notice_visible);
    add_timers(root, esp_timer_get_time());
    return reply(root, out, error);
}

static esp_err_t handle_notify(
    esp_openclaw_node_handle_t node, void *context, const char *params_json,
    size_t params_len, char **out, esp_openclaw_node_error_t *error)
{
    (void)node;
    (void)context;
    static const char *const KEYS[] = {"title", "text", "durationMs", "chime"};
    cJSON *params = cJSON_ParseWithLength(params_json, params_len);
    const char *title = NULL;
    const char *text = NULL;
    int64_t duration_ms = DESK_NOTICE_DEFAULT_MS;
    const cJSON *chime_item = cJSON_IsObject(params)
        ? cJSON_GetObjectItemCaseSensitive(params, "chime") : NULL;
    bool valid = cJSON_IsObject(params) && keys_allowed(params, KEYS, 4) &&
        optional_string(params, "title", DESK_NOTICE_TITLE_MAX, &title) &&
        optional_string(params, "text", DESK_NOTICE_TEXT_MAX, &text) && text != NULL && text[0] != '\0' &&
        optional_int(params, "durationMs", 1000, DESK_NOTICE_MAX_MS, &duration_ms) &&
        (chime_item == NULL || cJSON_IsBool(chime_item));
    if (!valid) {
        cJSON_Delete(params);
        return fail(error, "INVALID_PARAMS",
            "desk.notify requires text (<=240 bytes), optional title (<=64), durationMs 1000..600000, chime bool",
            ESP_ERR_INVALID_ARG);
    }
    taskENTER_CRITICAL(&desk_mux);
    strlcpy(notice_title, title != NULL ? title : "", sizeof(notice_title));
    strlcpy(notice_text, text, sizeof(notice_text));
    notice_duration_ms = (uint32_t)duration_ms;
    notice_chime = chime_item == NULL || cJSON_IsTrue(chime_item);
    notice_pending = true;
    taskEXIT_CRITICAL(&desk_mux);
    cJSON_Delete(params);
    *out = strdup("{\"queued\":true}");
    return *out != NULL ? ESP_OK : fail(error, "INTERNAL", "out of memory", ESP_ERR_NO_MEM);
}

static esp_err_t handle_ticker(
    esp_openclaw_node_handle_t node, void *context, const char *params_json,
    size_t params_len, char **out, esp_openclaw_node_error_t *error)
{
    (void)node;
    (void)context;
    static const char *const KEYS[] = {"text", "ttlSeconds"};
    cJSON *params = cJSON_ParseWithLength(params_json, params_len);
    const char *text = NULL;
    int64_t ttl = 0;
    bool valid = cJSON_IsObject(params) && keys_allowed(params, KEYS, 2) &&
        optional_string(params, "text", DESK_TICKER_MAX, &text) && text != NULL &&
        optional_int(params, "ttlSeconds", 0, 7 * 86400, &ttl);
    if (!valid) {
        cJSON_Delete(params);
        return fail(error, "INVALID_PARAMS",
            "desk.ticker requires text (<=96 bytes, empty clears) and optional ttlSeconds 0..604800 (0 = keep)",
            ESP_ERR_INVALID_ARG);
    }
    taskENTER_CRITICAL(&desk_mux);
    strlcpy(ticker_text, text, sizeof(ticker_text));
    ticker_until_us = ttl > 0 && text[0] != '\0' ? esp_timer_get_time() + ttl * 1000000 : 0;
    ticker_dirty = true;
    taskEXIT_CRITICAL(&desk_mux);
    cJSON_Delete(params);
    *out = strdup("{\"set\":true}");
    return *out != NULL ? ESP_OK : fail(error, "INTERNAL", "out of memory", ESP_ERR_NO_MEM);
}

static esp_err_t handle_timer_set(
    esp_openclaw_node_handle_t node, void *context, const char *params_json,
    size_t params_len, char **out, esp_openclaw_node_error_t *error)
{
    (void)node;
    (void)context;
    static const char *const KEYS[] = {"seconds", "label"};
    cJSON *params = cJSON_ParseWithLength(params_json, params_len);
    const char *label = NULL;
    int64_t seconds = 0;
    bool valid = cJSON_IsObject(params) && keys_allowed(params, KEYS, 2) &&
        cJSON_GetObjectItemCaseSensitive(params, "seconds") != NULL &&
        optional_int(params, "seconds", 1, DESK_TIMER_MAX_SECONDS, &seconds) &&
        optional_string(params, "label", DESK_TIMER_LABEL_MAX, &label);
    if (!valid) {
        cJSON_Delete(params);
        return fail(error, "INVALID_PARAMS",
            "desk.timer.set requires seconds 1..86400 and optional label (<=32 bytes)", ESP_ERR_INVALID_ARG);
    }
    uint32_t id = 0;
    taskENTER_CRITICAL(&desk_mux);
    for (size_t i = 0; i < DESK_MAX_TIMERS; ++i) {
        if (timers[i].id != 0) continue;
        id = next_timer_id++;
        if (next_timer_id == 0) next_timer_id = 1;
        timers[i].id = id;
        timers[i].due_us = esp_timer_get_time() + seconds * 1000000;
        strlcpy(timers[i].label, label != NULL ? label : "", sizeof(timers[i].label));
        break;
    }
    taskEXIT_CRITICAL(&desk_mux);
    cJSON_Delete(params);
    if (id == 0) return fail(error, "BUSY", "all 8 desk timers are in use; cancel one first", ESP_ERR_NO_MEM);
    ESP_LOGI(TAG, "timer %" PRIu32 " set for %" PRId64 " s", id, seconds);
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) return fail(error, "INTERNAL", "out of memory", ESP_ERR_NO_MEM);
    cJSON_AddNumberToObject(root, "id", id);
    cJSON_AddNumberToObject(root, "seconds", (double)seconds);
    return reply(root, out, error);
}

static esp_err_t handle_timer_cancel(
    esp_openclaw_node_handle_t node, void *context, const char *params_json,
    size_t params_len, char **out, esp_openclaw_node_error_t *error)
{
    (void)node;
    (void)context;
    static const char *const KEYS[] = {"id"};
    cJSON *params = cJSON_ParseWithLength(params_json, params_len);
    int64_t id = 0;
    bool all = cJSON_IsObject(params) && params->child == NULL;
    bool valid = all || (cJSON_IsObject(params) && keys_allowed(params, KEYS, 1) &&
        optional_int(params, "id", 1, UINT32_MAX, &id));
    cJSON_Delete(params);
    if (!valid) {
        return fail(error, "INVALID_PARAMS", "desk.timer.cancel accepts {id} or {} to cancel all", ESP_ERR_INVALID_ARG);
    }
    int cancelled = 0;
    taskENTER_CRITICAL(&desk_mux);
    for (size_t i = 0; i < DESK_MAX_TIMERS; ++i) {
        if (timers[i].id != 0 && (all || timers[i].id == (uint32_t)id)) {
            memset(&timers[i], 0, sizeof(timers[i]));
            cancelled++;
        }
    }
    taskEXIT_CRITICAL(&desk_mux);
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) return fail(error, "INTERNAL", "out of memory", ESP_ERR_NO_MEM);
    cJSON_AddNumberToObject(root, "cancelled", cancelled);
    return reply(root, out, error);
}

static esp_err_t handle_dnd(
    esp_openclaw_node_handle_t node, void *context, const char *params_json,
    size_t params_len, char **out, esp_openclaw_node_error_t *error)
{
    (void)node;
    (void)context;
    cJSON *params = cJSON_ParseWithLength(params_json, params_len);
    const cJSON *enabled_item = cJSON_IsObject(params)
        ? cJSON_GetObjectItemCaseSensitive(params, "enabled") : NULL;
    static const char *const KEYS[] = {"enabled"};
    bool valid = cJSON_IsBool(enabled_item) && keys_allowed(params, KEYS, 1);
    bool muted = cJSON_IsTrue(enabled_item);
    cJSON_Delete(params);
    if (!valid) return fail(error, "INVALID_PARAMS", "desk.dnd requires {enabled: bool}", ESP_ERR_INVALID_ARG);
    esp_openclaw_room_node_set_wake_muted(muted);
    request_repaint();
    *out = strdup(muted ? "{\"dnd\":true}" : "{\"dnd\":false}");
    return *out != NULL ? ESP_OK : fail(error, "INTERNAL", "out of memory", ESP_ERR_NO_MEM);
}

esp_err_t room_desk_register_node_commands(esp_openclaw_node_handle_t node)
{
    if (!enabled) return ESP_OK;
    static const esp_openclaw_node_command_t COMMANDS[] = {
        {.name = "desk.status", .handler = handle_status},
        {.name = "desk.notify", .handler = handle_notify},
        {.name = "desk.ticker", .handler = handle_ticker},
        {.name = "desk.timer.set", .handler = handle_timer_set},
        {.name = "desk.timer.cancel", .handler = handle_timer_cancel},
        {.name = "desk.dnd", .handler = handle_dnd},
    };
    ESP_RETURN_ON_ERROR(esp_openclaw_node_register_capability(node, "desk"), TAG, "desk capability");
    for (size_t i = 0; i < sizeof(COMMANDS) / sizeof(COMMANDS[0]); ++i) {
        ESP_RETURN_ON_ERROR(
            esp_openclaw_node_register_command(node, &COMMANDS[i]), TAG, "register %s", COMMANDS[i].name);
    }
    return ESP_OK;
}
