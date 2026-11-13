#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Install the GPIO ISR service once. ESP_ERR_INVALID_STATE (already
 * installed) is treated as success and does not get logged, because the
 * caller skips the IDF function on subsequent calls. */
esp_err_t dorky_gpio_isr_service_ensure(void);

#ifdef __cplusplus
}
#endif
