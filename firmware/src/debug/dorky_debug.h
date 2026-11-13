#pragma once

#include <stdbool.h>
#include "esp_err.h"

/* Persistent runtime debug flag (NVS namespace "dorky", key "debug").
 * Toggle without reflashing via the protocol (`log.set_debug` /
 * `log.get_debug`), Berry (`dorky_debug(true|false)`), or the USB-Serial-JTAG
 * console (`debug on`, `debug off`, `debug status`).
 *
 * Compile-time fallback when the NVS key is missing: define
 * DORKY_DEBUG_DEFAULT=1 in build_flags. */

#ifndef DORKY_DEBUG_DEFAULT
#define DORKY_DEBUG_DEFAULT 0
#endif

void dorky_debug_log_reset_reason(void); /* always, at boot, before NVS */

void dorky_debug_init(void);             /* load NVS + apply; after state_nvs_init */
void dorky_debug_console_init(void);     /* non-blocking stdin on the console */

bool dorky_debug_enabled(void);
esp_err_t dorky_debug_set(bool on);      /* persist + apply log levels */

/* Re-apply the ble-tag log level. Call after log.set_level on tag "*". */
void dorky_debug_apply_log_levels(void);

void dorky_debug_console_poll(void);     /* non-blocking line reader, main loop */
void dorky_debug_heartbeat(void);        /* periodic status line when enabled */
