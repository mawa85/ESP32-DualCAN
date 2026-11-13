#pragma once
#include <stdint.h>
#include "esp_err.h"
typedef struct { uint32_t flags; uint32_t identifier; uint8_t data_length_code; uint8_t data[8]; } twai_message_t;
typedef struct { int state; uint32_t tx_error_counter, rx_error_counter; } twai_status_info_t;
