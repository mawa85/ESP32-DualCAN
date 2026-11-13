#pragma once

#include "driver/gpio.h"
#include "can/can_driver.h"

/* Selected by -DBOARD_T2CAN_FD or -DBOARD_DORKY_C6. The C6 firmware
 * build passes BOARD_DORKY_C6; if neither flag is set, the C6 board
 * is used so an unflagged build keeps the original target. */

#if defined(BOARD_T2CAN_FD) && defined(BOARD_DORKY_C6)
#error "define only one of BOARD_T2CAN_FD and BOARD_DORKY_C6"
#elif defined(BOARD_T2CAN_FD)
#include "board_t2can_fd.h"
#else
#include "board_dorky_c6.h"
#endif
