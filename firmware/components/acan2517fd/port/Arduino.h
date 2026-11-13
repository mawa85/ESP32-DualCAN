#pragma once

/* Just enough of the Arduino core for the vendored ACAN2517FD sources.
 * This is not a general Arduino compatibility layer. */

#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* FreeRTOS already defines these as portDISABLE/ENABLE_INTERRUPTS().
 * ESP-IDF SPI polling takes a spinlock and deadlocks if interrupts are
 * masked for the whole transaction. The SPI mutex is the real lock. */
#undef taskDISABLE_INTERRUPTS
#undef taskENABLE_INTERRUPTS
#define taskDISABLE_INTERRUPTS() do { } while (0)
#define taskENABLE_INTERRUPTS()  do { } while (0)
#define noInterrupts() taskDISABLE_INTERRUPTS()
#define interrupts()   taskENABLE_INTERRUPTS()

#ifdef __cplusplus
extern "C" {
#endif

#define LOW  0
#define HIGH 1
#define INPUT        0x01
#define OUTPUT       0x03
#define INPUT_PULLUP 0x05
#define FALLING      0x02
#define NOT_AN_INTERRUPT (-1)

void pinMode(uint8_t pin, uint8_t mode);
void digitalWrite(uint8_t pin, uint8_t level);
int digitalRead(uint8_t pin);
void attachInterrupt(uint8_t pin, void (*isr)(void), int mode);
void detachInterrupt(uint8_t pin);
uint32_t millis(void);

static inline int digitalPinToInterrupt(uint8_t pin)
{
    return pin == 255 ? NOT_AN_INTERRUPT : (int)pin;
}

#ifdef __cplusplus
}
#endif
