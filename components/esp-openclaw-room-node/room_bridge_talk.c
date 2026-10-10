#include "room_bridge_talk.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_transport.h"
#include "esp_transport_tcp.h"
#include "esp_transport_ws.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "room_board.h"
#include "room_media.h"

#define TAG "room_bridge"

enum {
    BRIDGE_URL_MAX = 128,
    BRIDGE_HOST_MAX = 64,
    BRIDGE_CONNECT_TIMEOUT_MS = 5000,
    BRIDGE_IO_TIMEOUT_MS = 2000,
    BRIDGE_POLL_MS = 10,
    /* Text frames carry transcripts and replies; larger ones are skipped. */
    BRIDGE_RX_BYTES = 4096,
    BRIDGE_TX_BYTES = 2048,
    /* PSRAM stack: this task only frames socket I/O and parses small JSON. */
    BRIDGE_TASK_STACK = 8192,
    BRIDGE_TASK_PRIORITY = 10,
};

static const char *NVS_NAMESPACE = "room_bridge";
static const char *NVS_KEY_URL = "url";

static portMUX_TYPE url_mux = portMUX_INITIALIZER_UNLOCKED;
static char bridge_url[BRIDGE_URL_MAX];
static bool bridge_url_from_nvs;

struct room_bridge_call {
    room_bridge_callbacks_t cb;
    char url[BRIDGE_URL_MAX];
    atomic_bool cancel;
    bool started;
    StaticSemaphore_t done_storage;
    SemaphoreHandle_t done;
    esp_transport_handle_t ws;
    room_bridge_state_t state;
    uint8_t *tx;
    bool io_failed;
    uint32_t mic_bytes;
    uint32_t speaker_bytes;
};

static void copy_url(char *dst)
{
    taskENTER_CRITICAL(&url_mux);
    memcpy(dst, bridge_url, BRIDGE_URL_MAX);
    taskEXIT_CRITICAL(&url_mux);
}

static void set_url(const char *url, bool from_nvs)
{
    taskENTER_CRITICAL(&url_mux);
    strlcpy(bridge_url, url, sizeof(bridge_url));
    bridge_url_from_nvs = from_nvs;
    taskEXIT_CRITICAL(&url_mux);
}

/* ws://host[:port][/path]; only plain ws:// is supported (LAN bridge). */
static bool parse_url(const char *url, char *host, int *port, const char **path)
{
    if (strncmp(url, "ws://", 5) != 0) return false;
    const char *authority = url + 5;
    const char *slash = strchr(authority, '/');
    size_t authority_len = slash != NULL ? (size_t)(slash - authority) : strlen(authority);
    const char *colon = memchr(authority, ':', authority_len);
    size_t host_len = colon != NULL ? (size_t)(colon - authority) : authority_len;
    if (host_len == 0 || host_len >= BRIDGE_HOST_MAX) return false;
    memcpy(host, authority, host_len);
    host[host_len] = '\0';
    *port = 80;
    if (colon != NULL) {
        char *end = NULL;
        long value = strtol(colon + 1, &end, 10);
        if (end != authority + authority_len || value <= 0 || value > 65535) return false;
        *port = (int)value;
    }
    *path = slash != NULL ? slash : "/";
    return true;
}

bool room_bridge_configured(void)
{
    char url[BRIDGE_URL_MAX];
    char host[BRIDGE_HOST_MAX];
    int port;
    const char *path;
    copy_url(url);
    return parse_url(url, host, &port, &path);
}

/* ------------------------------------------------------------------ call */

esp_err_t room_bridge_call_prepare(const room_bridge_callbacks_t *callbacks, room_bridge_call_t **out_call)
{
    ESP_RETURN_ON_FALSE(callbacks != NULL && out_call != NULL, ESP_ERR_INVALID_ARG, TAG, "args");
    room_bridge_call_t *call = heap_caps_calloc(1, sizeof(*call), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (call == NULL) return ESP_ERR_NO_MEM;
    call->cb = *callbacks;
    copy_url(call->url);
    atomic_init(&call->cancel, false);
    call->done = xSemaphoreCreateBinaryStatic(&call->done_storage);
    *out_call = call;
    return ESP_OK;
}

static int send_frame(room_bridge_call_t *call, ws_transport_opcodes_t opcode, const void *data, size_t len)
{
    /* The ws transport masks the payload in place, so it must be writable. */
    const uint8_t *src = data;
    do {
        size_t n = len < BRIDGE_TX_BYTES ? len : BRIDGE_TX_BYTES;
        if (n > 0) memcpy(call->tx, src, n);
        int written = esp_transport_ws_send_raw(call->ws,
            opcode | WS_TRANSPORT_OPCODES_FIN, (const char *)call->tx, (int)n, BRIDGE_IO_TIMEOUT_MS);
        if (written != (int)n) {
            call->io_failed = true;
            return -1;
        }
        src += n;
        len -= n;
    } while (len > 0);
    return 0;
}

static int send_json(room_bridge_call_t *call, const char *json)
{
    return send_frame(call, WS_TRANSPORT_OPCODES_TEXT, json, strlen(json));
}

static void mic_frame(const uint8_t *pcm, size_t size, void *ctx)
{
    room_bridge_call_t *call = ctx;
    /* Half duplex: the bridge drops uplink outside listening anyway, and not
     * sending keeps Wi-Fi airtime free for the reply audio. */
    if (call->state != ROOM_BRIDGE_STATE_LISTENING || call->io_failed) return;
    if (send_frame(call, WS_TRANSPORT_OPCODES_BINARY, pcm, size) == 0) {
        call->mic_bytes += size;
    }
}

static void set_state(room_bridge_call_t *call, room_bridge_state_t state)
{
    if (call->state == state) return;
    call->state = state;
    if (call->cb.state_cb != NULL) call->cb.state_cb(state, call->cb.ctx);
}

/* Returns true when the bridge ended the session; *error is set for failures. */
static bool handle_json(room_bridge_call_t *call, const char *text, size_t len, const char **error)
{
    cJSON *msg = cJSON_ParseWithLength(text, len);
    const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "type"));
    bool ended = false;
    if (type == NULL) {
        ESP_LOGW(TAG, "ignoring malformed text frame (%u bytes)", (unsigned)len);
    } else if (strcmp(type, "state") == 0) {
        const char *state = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "state"));
        if (state != NULL && strcmp(state, "listening") == 0) {
            set_state(call, ROOM_BRIDGE_STATE_LISTENING);
        } else if (state != NULL && strcmp(state, "thinking") == 0) {
            set_state(call, ROOM_BRIDGE_STATE_THINKING);
        } else if (state != NULL && strcmp(state, "speaking") == 0) {
            set_state(call, ROOM_BRIDGE_STATE_SPEAKING);
        }
    } else if (strcmp(type, "transcript") == 0 || strcmp(type, "reply") == 0) {
        const char *value = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "text"));
        ESP_LOGI(TAG, "%s: %s", type, value != NULL ? value : "");
    } else if (strcmp(type, "end") == 0) {
        const char *reason = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "reason"));
        ESP_LOGI(TAG, "bridge ended the session: %s", reason != NULL ? reason : "?");
        if (reason != NULL && strcmp(reason, "error") == 0) *error = "Talk failed";
        ended = true;
    }
    cJSON_Delete(msg);
    return ended;
}

/*
 * Read one complete data frame. Returns 1 with the frame in rx, 0 when nothing
 * usable arrived (control frame handled by the transport, or an oversized or
 * fragmented frame that was skipped), and -1 when the connection is gone.
 */
static int read_frame(room_bridge_call_t *call, uint8_t *rx, ws_transport_opcodes_t *opcode, size_t *len)
{
    int got = esp_transport_read(call->ws, (char *)rx, BRIDGE_RX_BYTES, BRIDGE_IO_TIMEOUT_MS);
    if (got < 0) return -1;
    ws_transport_opcodes_t op = esp_transport_ws_get_read_opcode(call->ws);
    if (op == WS_TRANSPORT_OPCODES_CLOSE) return -1;
    if (got == 0) return 0;
    int total = esp_transport_ws_get_read_payload_len(call->ws);
    bool fits = total <= BRIDGE_RX_BYTES;
    while (got < total) {
        /* Oversized frames are drained into the tail of rx and dropped. */
        size_t offset = fits ? (size_t)got : 0;
        size_t want = (size_t)(total - got);
        if (want > BRIDGE_RX_BYTES - offset) want = BRIDGE_RX_BYTES - offset;
        int n = esp_transport_read(call->ws, (char *)rx + offset, (int)want, BRIDGE_IO_TIMEOUT_MS);
        if (n <= 0) return -1;
        got += n;
    }
    if (!fits || (op != WS_TRANSPORT_OPCODES_TEXT && op != WS_TRANSPORT_OPCODES_BINARY) ||
        !esp_transport_ws_get_fin_flag(call->ws)) {
        ESP_LOGW(TAG, "skipped frame opcode=%d len=%d", (int)op, total);
        return 0;
    }
    *opcode = op;
    *len = (size_t)total;
    return 1;
}

static void bridge_task(void *arg)
{
    room_bridge_call_t *call = arg;
    const char *error = NULL;
    bool connected = false;
    bool media = false;
    uint8_t *rx = heap_caps_malloc(BRIDGE_RX_BYTES + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    call->tx = heap_caps_malloc(BRIDGE_TX_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    esp_transport_list_handle_t transports = esp_transport_list_init();
    esp_transport_handle_t tcp = esp_transport_tcp_init();
    if (transports != NULL && tcp != NULL) {
        esp_transport_list_add(transports, tcp, "_tcp");
        call->ws = esp_transport_ws_init(tcp);
        if (call->ws != NULL) esp_transport_list_add(transports, call->ws, "ws");
    }
    if (rx == NULL || call->tx == NULL || transports == NULL || tcp == NULL || call->ws == NULL) {
        if (transports == NULL && tcp != NULL) esp_transport_destroy(tcp);
        error = "Talk failed";
        goto out;
    }

    char host[BRIDGE_HOST_MAX];
    int port = 0;
    const char *path = NULL;
    if (!parse_url(call->url, host, &port, &path)) {
        error = "Bridge URL";
        goto out;
    }
    esp_transport_ws_set_path(call->ws, path);
    ESP_LOGI(TAG, "connecting to %s", call->url);
    if (esp_transport_connect(call->ws, host, port, BRIDGE_CONNECT_TIMEOUT_MS) < 0) {
        ESP_LOGW(TAG, "bridge %s unreachable", call->url);
        error = "Bridge offline";
        goto out;
    }
    connected = true;
    const esp_openclaw_room_node_config_t *board = room_board_config();
    char hello[160];
    snprintf(hello, sizeof(hello), "{\"type\":\"hello\",\"device\":\"%s\",\"rate\":16000}",
        board != NULL && board->model_identifier != NULL ? board->model_identifier : "room-node");
    if (atomic_load(&call->cancel)) goto out;
    if (send_json(call, hello) != 0) {
        error = "Bridge lost";
        goto out;
    }
    if (room_media_bridge_begin() != ESP_OK) {
        error = "Talk failed";
        goto out;
    }
    media = true;

    while (!atomic_load(&call->cancel)) {
        room_media_bridge_drain_mic(mic_frame, call);
        if (call->io_failed) {
            error = "Bridge lost";
            break;
        }
        int ready = esp_transport_poll_read(call->ws, BRIDGE_POLL_MS);
        if (ready == 0) continue;
        ws_transport_opcodes_t opcode = WS_TRANSPORT_OPCODES_NONE;
        size_t len = 0;
        int result = ready < 0 ? -1 : read_frame(call, rx, &opcode, &len);
        if (result < 0) {
            if (!atomic_load(&call->cancel)) error = "Bridge lost";
            break;
        }
        if (result == 0) continue;
        if (opcode == WS_TRANSPORT_OPCODES_BINARY) {
            call->speaker_bytes += len;
            if (room_media_bridge_play((const int16_t *)rx, len / 2) != ESP_OK) {
                error = "Talk failed";
                break;
            }
        } else {
            rx[len] = '\0';
            if (handle_json(call, (const char *)rx, len, &error)) break;
        }
    }
    if (atomic_load(&call->cancel) && connected && !call->io_failed) {
        /* Best effort: tell the bridge to drop the turn, then close cleanly. */
        send_json(call, "{\"type\":\"stop\"}");
        esp_transport_ws_send_raw(call->ws, WS_TRANSPORT_OPCODES_CLOSE | WS_TRANSPORT_OPCODES_FIN,
            NULL, 0, 200);
    }

out:
    ESP_LOGI(TAG, "session closed (%s): mic %" PRIu32 " B up, speaker %" PRIu32 " B down",
        error != NULL ? error : "ok", call->mic_bytes, call->speaker_bytes);
    if (media) room_media_bridge_end();
    if (call->ws != NULL) esp_transport_close(call->ws);
    if (transports != NULL) esp_transport_list_destroy(transports);
    call->ws = NULL;
    heap_caps_free(rx);
    heap_caps_free(call->tx);
    call->tx = NULL;
    if (call->cb.closed_cb != NULL) {
        call->cb.closed_cb(atomic_load(&call->cancel) ? NULL : error, call->cb.ctx);
    }
    xSemaphoreGive(call->done);
    vTaskDeleteWithCaps(NULL);
}

esp_err_t room_bridge_call_start(room_bridge_call_t *call)
{
    ESP_RETURN_ON_FALSE(call != NULL && !call->started, ESP_ERR_INVALID_STATE, TAG, "call state");
    call->state = ROOM_BRIDGE_STATE_CONNECTING;
    if (xTaskCreateWithCaps(bridge_task, "room_bridge", BRIDGE_TASK_STACK, call,
            BRIDGE_TASK_PRIORITY, NULL, MALLOC_CAP_SPIRAM) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    call->started = true;
    return ESP_OK;
}

void room_bridge_call_cancel(room_bridge_call_t *call)
{
    if (call != NULL) atomic_store(&call->cancel, true);
}

void room_bridge_call_quiesce(room_bridge_call_t *call)
{
    if (call == NULL) return;
    room_bridge_call_cancel(call);
    if (call->started) {
        xSemaphoreTake(call->done, portMAX_DELAY);
        /* Keep the token for any later quiesce of the same call. */
        xSemaphoreGive(call->done);
    }
}

void room_bridge_call_release(room_bridge_call_t *call)
{
    heap_caps_free(call);
}

/* --------------------------------------------------------------- console */

static void load_url(void)
{
    char url[BRIDGE_URL_MAX] = {0};
    size_t len = sizeof(url);
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        esp_err_t err = nvs_get_str(nvs, NVS_KEY_URL, url, &len);
        nvs_close(nvs);
        if (err == ESP_OK && url[0] != '\0') {
            set_url(url, true);
            return;
        }
    }
    set_url(CONFIG_OPENCLAW_ROOM_BRIDGE_URL, false);
}

static esp_err_t store_url(const char *url)
{
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs), TAG, "nvs open");
    esp_err_t err = url != NULL ? nvs_set_str(nvs, NVS_KEY_URL, url) : nvs_erase_key(nvs, NVS_KEY_URL);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

static int bridge_console_command(int argc, char **argv)
{
    if (argc == 1 || (argc == 2 && strcmp(argv[1], "url") == 0)) {
        char url[BRIDGE_URL_MAX];
        copy_url(url);
        printf("bridge url=%s (%s)%s\n", url[0] != '\0' ? url : "<unset>",
            bridge_url_from_nvs ? "nvs" : "kconfig default",
            room_bridge_configured() ? "" : " - not a valid ws:// URL");
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "url") == 0) {
        bool reset = strcmp(argv[2], "default") == 0;
        char host[BRIDGE_HOST_MAX];
        int port;
        const char *path;
        if (!reset && (strlen(argv[2]) >= BRIDGE_URL_MAX || !parse_url(argv[2], host, &port, &path))) {
            printf("expected ws://host:port[/path] (max %d chars)\n", BRIDGE_URL_MAX - 1);
            return 1;
        }
        esp_err_t err = store_url(reset ? NULL : argv[2]);
        if (err != ESP_OK) {
            printf("saving bridge url failed: %s\n", esp_err_to_name(err));
            return 1;
        }
        load_url();
        char url[BRIDGE_URL_MAX];
        copy_url(url);
        printf("bridge url=%s; used from the next Talk session\n", url[0] != '\0' ? url : "<unset>");
        return 0;
    }
    printf("usage: bridge url\n");
    printf("       bridge url ws://host:port[/path]\n");
    printf("       bridge url default\n");
    return 1;
}

esp_err_t room_bridge_init(void)
{
    load_url();
    char url[BRIDGE_URL_MAX];
    copy_url(url);
    ESP_LOGI(TAG, "voice bridge %s (%s)", url[0] != '\0' ? url : "<unset>",
        bridge_url_from_nvs ? "nvs" : "kconfig default");
    static const esp_console_cmd_t command = {
        .command = "bridge",
        .help = "Show or set the local voice bridge URL used by Talk",
        .hint = "url [ws://host:port[/path] | default]",
        .func = bridge_console_command,
    };
    return esp_console_cmd_register(&command);
}
