#pragma once

#include <stdbool.h>

#include "can/can_driver.h"

/* MCP2518FD behind can_bus_*. The register-level work is Pierre Molinaro's
 * ACAN2517FD (MIT, firmware/components/acan2517fd), the same driver
 * Battery-Emulator runs on the LilyGo T-2CAN FD. This wrapper only maps
 * pins, bitrate, and twai_message_t. The controller is left in Normal
 * CAN 2.0B; the data-phase timing is still programmed at 1x so CAN FD is
 * a mode change, not a missing clock setup.
 */

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t mcp2518fd_install(const can_bus_config_t *cfg);

esp_err_t mcp2518fd_send(int bus_id, uint32_t id,
                         const uint8_t *data, uint8_t len,
                         uint32_t timeout_ms);

esp_err_t mcp2518fd_receive(int bus_id, twai_message_t *out,
                            uint32_t timeout_ms);

esp_err_t mcp2518fd_status(int bus_id, twai_status_info_t *out);

esp_err_t mcp2518fd_set_bitrate(int bus_id, uint32_t kbps);

void mcp2518fd_bus_off_recover(int bus_id);

bool mcp2518fd_take_activity(int bus_id);

bool mcp2518fd_is_up(int bus_id);

#ifdef __cplusplus
}
#endif
