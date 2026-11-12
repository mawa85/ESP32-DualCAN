#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/twai.h"
#include "esp_err.h"

#define CAN_BUS_COUNT 2

/* Which controller sits behind a logical bus. TWAI is the on-chip
 * controller (two of them on ESP32-C6, one on ESP32-S3). MCP2518FD is
 * an external SPI controller. The frame type above this layer stays
 * twai_message_t either way. */
typedef enum {
    CAN_BACKEND_TWAI = 0,
    CAN_BACKEND_MCP2518FD = 1,
} can_backend_t;

typedef struct {
    int            bus_id;          // 0 or 1
    can_backend_t  backend;         // 0 == TWAI, so a zeroed config stays on-chip
    int            twai_controller; // TWAI index. C6: same as bus_id. S3: always 0.
    gpio_num_t     tx_pin;          // TWAI TX. Unused for MCP2518FD.
    gpio_num_t     rx_pin;          // TWAI RX. Unused for MCP2518FD.
    gpio_num_t     stb_pin;         // transceiver STB (active-low enable); GPIO_NUM_NC to skip
    uint32_t       bitrate_kbps;    // 125, 250, 500, 1000
    uint32_t       tx_queue_len;    // 0 -> default 8
    uint32_t       rx_queue_len;    // 0 -> default 16
    /* MCP2518FD. Leave at GPIO_NUM_NC / 0 on a TWAI bus. */
    gpio_num_t     spi_sclk;
    gpio_num_t     spi_mosi;
    gpio_num_t     spi_miso;
    gpio_num_t     spi_cs;
    gpio_num_t     int_pin;         // MCP2518FD INT, active low
    gpio_num_t     int0_pin;        // INT0, pulled up and otherwise unused
    gpio_num_t     int1_pin;        // INT1, pulled up and otherwise unused
    uint32_t       osc_hz;          // crystal frequency; 0 if this bus is not MCP2518FD
} can_bus_config_t;

esp_err_t can_bus_install(const can_bus_config_t *cfg);

esp_err_t can_bus_send(int bus_id, uint32_t id,
                       const uint8_t *data, uint8_t len,
                       uint32_t timeout_ms);

esp_err_t can_bus_receive(int bus_id, twai_message_t *out,
                          uint32_t timeout_ms);

esp_err_t can_bus_status(int bus_id, twai_status_info_t *out);

uint32_t  can_bus_get_bitrate(int bus_id);
esp_err_t can_bus_set_bitrate(int bus_id, uint32_t kbps);

typedef enum {
    BUS_IDLE    = 0,  /* no electrical activity */
    BUS_GOOD    = 1,  /* TWAI frames received or TX ACKed */
    BUS_TX_ERR  = 2,  /* TX errors present — we're the bus source but nobody ACKs */
    BUS_RX_ERR  = 3,  /* GPIO edges seen but no valid TWAI frames (wrong bitrate, noise, …) */
    BUS_ERROR   = 4,  /* TWAI bus-off */
} bus_status_t;

/* If bus is in bus-off state, initiate the 128-recessive-bit recovery sequence.
 * Call from can_poll() every tick so recovery is not gated on BLE connectivity. */
void can_bus_off_recover(int bus_id);

/* Compute current bus health. Drains the RX-edge flag internally.
 * `last_rx_ms` — timestamp of the last successfully received TWAI frame (0 = never).
 * `now_ms`     — current time in milliseconds. */
bus_status_t can_bus_health(int bus_id, uint32_t now_ms, uint32_t last_rx_ms);
