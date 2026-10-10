#pragma once

#include <stdbool.h>

#include "esp_err.h"

/*
 * Local voice bridge Talk backend (CONFIG_OPENCLAW_ROOM_TALK_BACKEND_BRIDGE).
 *
 * One call is one WebSocket session to the bridge. JSON text frames carry
 * control messages; binary frames carry PCM16LE mono 16 kHz both ways:
 *   board -> bridge: {"type":"hello",...}, mic PCM, {"type":"stop"}
 *   bridge -> board: {"type":"state","state":"listening|thinking|speaking"},
 *                    {"type":"transcript"|"reply","text":...}, TTS PCM,
 *                    {"type":"end","reason":"idle|bye|error"}
 * The bridge is half duplex, so the board only uploads while it listens.
 */

typedef enum {
    ROOM_BRIDGE_STATE_CONNECTING = 0,
    ROOM_BRIDGE_STATE_LISTENING,
    ROOM_BRIDGE_STATE_THINKING,
    ROOM_BRIDGE_STATE_SPEAKING,
} room_bridge_state_t;

typedef struct {
    /** Called from the bridge task on every state change reported by the bridge. */
    void (*state_cb)(room_bridge_state_t state, void *ctx);
    /**
     * Called exactly once per started call, from the bridge task, after its
     * media paths are released. NULL means a normal end (idle, bye, or cancel).
     */
    void (*closed_cb)(const char *error_message, void *ctx);
    void *ctx;
} room_bridge_callbacks_t;

typedef struct room_bridge_call room_bridge_call_t;

/** Load the NVS URL override and register the `bridge` console command. */
esp_err_t room_bridge_init(void);
/** True when a bridge URL is configured (NVS override or Kconfig default). */
bool room_bridge_configured(void);

/** Allocate a call bound to the current URL. Cheap; safe under the caller's state lock. */
esp_err_t room_bridge_call_prepare(const room_bridge_callbacks_t *callbacks, room_bridge_call_t **out_call);
/** Start the session task. The caller must own Talk media until the call is quiesced. */
esp_err_t room_bridge_call_start(room_bridge_call_t *call);
/** Ask the session to end. Idempotent, non-blocking, callable from any task. */
void room_bridge_call_cancel(room_bridge_call_t *call);
/** Cancel and wait until the session task has exited; no callback runs afterwards. */
void room_bridge_call_quiesce(room_bridge_call_t *call);
void room_bridge_call_release(room_bridge_call_t *call);
