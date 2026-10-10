#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_openclaw_node.h"

#define LICHUANG_I2C_PORT I2C_NUM_1
#define PCA9557_CAMERA_PWDN_BIT 2 /* High powers the GC0308 down. */

/** Shared I2C bus (PCA9557, codecs, touch, camera SCCB, IMU). */
i2c_master_bus_handle_t szp_i2c_bus(void);
/** Thread-safe PCA9557 output write; runtime users (camera) race startup ones. */
esp_err_t szp_pca9557_set_bit(uint8_t bit, bool level);

/** Reserves the camera's internal DMA block; call early, before the heap fragments. */
esp_err_t szp_camera_reserve(void);
/** Lends the reserved block to Talk while it owns the audio pipeline. */
void szp_camera_talk_media_changed(bool talk_owns_media);
esp_err_t szp_camera_register_commands(esp_openclaw_node_handle_t node);

/** Probes the QMI8658 and starts the motion task; absent hardware is not an error. */
esp_err_t szp_imu_start(void);
/** Releases IMU polling once the audio codecs are configured (or failed). */
void szp_imu_audio_ready(void);
esp_err_t szp_imu_register_commands(esp_openclaw_node_handle_t node);
