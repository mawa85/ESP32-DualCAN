#pragma once

/* Dorky Commander — ESP32-C6-SuperMini, two on-chip TWAI controllers.
 * Pin numbers match the previous firmware/src/main.c constants so this
 * target behaves the same as before the board split.
 *
 *   bus 0  vehicle CAN   TX GPIO2   RX GPIO1   STB GPIO19  (TCAN1044, active-low)
 *   bus 1  chassis CAN   TX GPIO4   RX GPIO3   STB GPIO18
 *   status LED           GPIO15 (active high)
 *   RGB WS2812           GPIO8
 *   BOOT button          GPIO9  (active low, external pull-up on the module)
 */

#define BOARD_NAME              "Dorky Commander (ESP32-C6)"
#define BOARD_STATUS_LED_GPIO   GPIO_NUM_15
#define BOARD_BOOT_BTN_GPIO     GPIO_NUM_9
#define BOARD_HAS_RGB_LED       1
#define BOARD_RGB_LED_GPIO      GPIO_NUM_8
#define BOARD_RGB_LED_COUNT     1

static inline void board_can_config(int bus_id, uint32_t bitrate_kbps, can_bus_config_t *out)
{
    can_bus_config_t c = {
        .bus_id = bus_id,
        .backend = CAN_BACKEND_TWAI,
        .twai_controller = bus_id,
        .bitrate_kbps = bitrate_kbps,
        .stb_pin = GPIO_NUM_NC,
        .tx_pin = GPIO_NUM_NC,
        .rx_pin = GPIO_NUM_NC,
        .spi_sclk = GPIO_NUM_NC,
        .spi_mosi = GPIO_NUM_NC,
        .spi_miso = GPIO_NUM_NC,
        .spi_cs = GPIO_NUM_NC,
        .int_pin = GPIO_NUM_NC,
        .int0_pin = GPIO_NUM_NC,
        .int1_pin = GPIO_NUM_NC,
        .osc_hz = 0,
    };
    if (bus_id == 0) {
        c.tx_pin = GPIO_NUM_2;
        c.rx_pin = GPIO_NUM_1;
        c.stb_pin = GPIO_NUM_19;
    } else {
        c.tx_pin = GPIO_NUM_4;
        c.rx_pin = GPIO_NUM_3;
        c.stb_pin = GPIO_NUM_18;
    }
    *out = c;
}
