#pragma once

/* LilyGo T-2CAN FD (ESP32-S3-WROOM-1, one TWAI + MCP2518FD).
 *
 * Sources:
 *   pin_config.h in Xinyuan-LilyGO/T-2Can (T_2Can_Fd):
 *     CAN_TX 7, CAN_RX 6, SPI 12/11/13, CS 10, INT 8, INT0 9, INT1 3, BOOT 0
 *   project/T-2Can-Fd_V1.0.pdf text next to the MCP2518FD: crystal "40MHZ±10ppm"
 *   TD501MCAN pinout has no standby/enable pin (RXD, TXD, GND, VCC, CANG, CANL, CANH)
 *
 * Bus map — change BOARD_T2CAN_MCP_BUS to swap which Dorky bus uses the
 * external controller. Bus 0 is Tesla vehicle CAN, bus 1 is chassis CAN.
 * The ESP32-S3 has a single TWAI controller, so the other bus must stay
 * on the MCP2518FD and twai_controller stays 0.
 *
 *   default: bus 0 (vehicle) -> MCP2518FD CAN-A
 *            bus 1 (chassis) -> TWAI CAN-B (TX GPIO7, RX GPIO6)
 *
 * pin_config.h and the V1.0 schematic BOM do not define a user LED.
 * GPIO8 is the MCP2518FD interrupt, so the C6 WS2812 must not be driven.
 */

#define BOARD_NAME              "LilyGo T-2CAN FD (ESP32-S3)"
#define BOARD_STATUS_LED_GPIO   GPIO_NUM_NC
#define BOARD_BOOT_BTN_GPIO     GPIO_NUM_0
#define BOARD_HAS_RGB_LED       0
#define BOARD_RGB_LED_GPIO      GPIO_NUM_NC
#define BOARD_RGB_LED_COUNT     0

/* Which logical bus is the MCP2518FD. 0 = vehicle CAN on CAN-A. */
#define BOARD_T2CAN_MCP_BUS     0

#define BOARD_MCP_OSC_HZ        40000000u

static inline void board_can_config(int bus_id, uint32_t bitrate_kbps, can_bus_config_t *out)
{
    can_bus_config_t c = {
        .bus_id = bus_id,
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
        .twai_controller = 0,
    };
    if (bus_id == BOARD_T2CAN_MCP_BUS) {
        c.backend = CAN_BACKEND_MCP2518FD;
        c.spi_sclk = GPIO_NUM_12;
        c.spi_mosi = GPIO_NUM_11;
        c.spi_miso = GPIO_NUM_13;
        c.spi_cs   = GPIO_NUM_10;
        c.int_pin  = GPIO_NUM_8;
        c.int0_pin = GPIO_NUM_9;
        c.int1_pin = GPIO_NUM_3;   /* S3 strapping pin; held as input with pull-up */
        c.osc_hz   = BOARD_MCP_OSC_HZ;
    } else {
        c.backend = CAN_BACKEND_TWAI;
        c.twai_controller = 0;
        c.tx_pin = GPIO_NUM_7;
        c.rx_pin = GPIO_NUM_6;
    }
    *out = c;
}
