#pragma once
/* DORKY_EXT_BSS: place a large, task-only static buffer in PSRAM .bss
 * (CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY). Expands to nothing on
 * targets without PSRAM and in host builds, so it is always safe to use.
 * Do NOT use for data touched from ISRs, while the flash cache is disabled,
 * or for FreeRTOS objects/spinlocks - those must stay in internal RAM. */
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#define DORKY_EXT_BSS EXT_RAM_BSS_ATTR
#else
#define DORKY_EXT_BSS
#endif
