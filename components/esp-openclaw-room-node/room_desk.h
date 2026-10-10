#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_openclaw_node.h"
#include "lvgl.h"
#include "room_ui_controller.h"

/* Desk-assistant surface. Every function except the command registration and
 * time-sync start must run with the display lock held. */

/** Builds the hidden clock view and notice layer. No-op when desk is disabled. */
esp_err_t room_desk_init(lv_obj_t *screen);
bool room_desk_enabled(void);

/** Shows the clock for the idle/error view; returns false when unavailable. */
bool room_desk_clock_show(room_ui_state_t state, const char *detail, const room_ui_facts_t *facts);
void room_desk_clock_hide(void);

/** Backlight for the idle clock (night-dimmed) and while a notice is up. */
int room_desk_idle_brightness(void);
bool room_desk_notice_active(void);

/** Starts SNTP once the network stack is up. Safe to call repeatedly. */
void room_desk_start_time_sync(void);
esp_err_t room_desk_register_node_commands(esp_openclaw_node_handle_t node);
