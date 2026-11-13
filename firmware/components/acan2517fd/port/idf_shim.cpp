#include "Arduino.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_timer.h"
#include "dorky_gpio_isr.h"

static void (*s_gpio_isr)(void) = nullptr;

static bool s_isr_service_up = false;

extern "C" esp_err_t dorky_gpio_isr_service_ensure(void)
{
    if (s_isr_service_up) return ESP_OK;
    esp_err_t err = gpio_install_isr_service(0);
    /* Already-installed is success; skip the IDF call next time so the
     * "GPIO isr service already installed" error is not logged. */
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
        s_isr_service_up = true;
        return ESP_OK;
    }
    return err;
}


static void gpio_thunk(void *arg)
{
    (void)arg;
    if (s_gpio_isr) s_gpio_isr();
}

void pinMode(uint8_t pin, uint8_t mode)
{
    gpio_num_t gpio = static_cast<gpio_num_t>(pin);
    gpio_reset_pin(gpio);
    if (mode == OUTPUT) {
        gpio_set_direction(gpio, GPIO_MODE_OUTPUT);
        gpio_set_level(gpio, 1);
    } else if (mode == INPUT_PULLUP) {
        gpio_set_direction(gpio, GPIO_MODE_INPUT);
        gpio_set_pull_mode(gpio, GPIO_PULLUP_ONLY);
    } else {
        gpio_set_direction(gpio, GPIO_MODE_INPUT);
    }
}

void digitalWrite(uint8_t pin, uint8_t level)
{
    gpio_set_level(static_cast<gpio_num_t>(pin), level ? 1 : 0);
}

int digitalRead(uint8_t pin)
{
    return gpio_get_level(static_cast<gpio_num_t>(pin));
}

void attachInterrupt(uint8_t pin, void (*isr)(void), int mode)
{
    (void)mode;
    s_gpio_isr = isr;
    if (dorky_gpio_isr_service_ensure() != ESP_OK) return;
    gpio_set_intr_type(static_cast<gpio_num_t>(pin), GPIO_INTR_NEGEDGE);
    gpio_isr_handler_remove(static_cast<gpio_num_t>(pin));
    gpio_isr_handler_add(static_cast<gpio_num_t>(pin), gpio_thunk, nullptr);
}

void detachInterrupt(uint8_t pin)
{
    gpio_isr_handler_remove(static_cast<gpio_num_t>(pin));
    gpio_set_intr_type(static_cast<gpio_num_t>(pin), GPIO_INTR_DISABLE);
    s_gpio_isr = nullptr;
}

uint32_t millis(void)
{
    return static_cast<uint32_t>(esp_timer_get_time() / 1000);
}

#include "SPI.h"

esp_err_t SPIClass::begin(int sclk, int miso, int mosi)
{
    if (!m_lock) m_lock = xSemaphoreCreateMutex();
    if (!m_bus_up) {
        spi_bus_config_t bus = {};
        bus.sclk_io_num = sclk;
        bus.mosi_io_num = mosi;
        bus.miso_io_num = miso;
        bus.quadwp_io_num = -1;
        bus.quadhd_io_num = -1;
        bus.max_transfer_sz = 128;
        esp_err_t err = spi_bus_initialize(m_host, &bus, SPI_DMA_DISABLED);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
        spi_device_interface_config_t dev = {};
        dev.mode = 0;
        dev.clock_speed_hz = 1000000;
        dev.spics_io_num = -1;
        dev.queue_size = 1;
        err = spi_bus_add_device(m_host, &dev, &m_dev);
        if (err != ESP_OK) return err;
        m_bus_up = true;
    }
    return ESP_OK;
}

void SPIClass::beginTransaction(SPISettings settings)
{
    if (m_lock) xSemaphoreTake(m_lock, portMAX_DELAY);
    if (settings.clockHz) m_hz = settings.clockHz;
}

void SPIClass::endTransaction()
{
    if (m_lock) xSemaphoreGive(m_lock);
}

void SPIClass::transfer(uint8_t *buf, size_t len)
{
    if (!m_dev || !buf || !len) return;
    uint8_t rx[96];
    while (len) {
        size_t n = len > sizeof(rx) ? sizeof(rx) : len;
        spi_transaction_t t = {};
        t.length = n * 8;
        t.tx_buffer = buf;
        t.rx_buffer = rx;
        t.override_freq_hz = m_hz;
        if (spi_device_polling_transmit(m_dev, &t) != ESP_OK) return;
        memcpy(buf, rx, n);
        buf += n;
        len -= n;
    }
}

uint8_t SPIClass::transfer(uint8_t value)
{
    transfer(&value, 1);
    return value;
}

uint16_t SPIClass::transfer16(uint16_t value)
{
    uint8_t b[2] = { static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value) };
    transfer(b, 2);
    return static_cast<uint16_t>((b[0] << 8) | b[1]);
}
