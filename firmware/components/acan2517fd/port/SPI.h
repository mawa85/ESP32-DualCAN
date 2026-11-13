#pragma once

#include "Arduino.h"

#include "driver/spi_master.h"

#define MSBFIRST  1
#define SPI_MODE0 0

class SPISettings {
  public:
    uint32_t clockHz;
    uint8_t bitOrder;
    uint8_t dataMode;
    SPISettings(uint32_t clock = 1000000, uint8_t order = MSBFIRST, uint8_t mode = SPI_MODE0)
        : clockHz(clock), bitOrder(order), dataMode(mode) {}
};

/* Bit-banged chip-select stays in ACAN2517FD (digitalWrite). This device
 * is added with CS = -1. beginTransaction / endTransaction hold a mutex,
 * which is the lock the library expects from Arduino's SPI class. */
class SPIClass {
  public:
    SPIClass() : m_host(SPI2_HOST), m_dev(nullptr), m_lock(nullptr), m_hz(1000000) {}

    esp_err_t begin(int sclk, int miso, int mosi);
    void beginTransaction(SPISettings settings);
    void endTransaction();
    void transfer(uint8_t *buf, size_t len);
    void transfer(void *buf, size_t len) { transfer(static_cast<uint8_t *>(buf), len); }
    uint8_t transfer(uint8_t value);
    uint16_t transfer16(uint16_t value);
    void usingInterrupt(int) {}

  private:
    spi_host_device_t m_host;
    spi_device_handle_t m_dev;
    SemaphoreHandle_t m_lock;
    uint32_t m_hz;
    bool m_bus_up = false;
};
