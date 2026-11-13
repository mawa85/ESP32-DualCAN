/* Thin ESP-IDF wrapper around ACAN2517FD. The chip driver is vendored;
 * this file only connects it to can_bus_config_t and twai_message_t.
 * The C6 build gets stubs so the unused driver is not linked into the
 * 1.75 MB image. */

#include "can/mcp2518fd.h"

#if !defined(BOARD_T2CAN_FD)

extern "C" esp_err_t mcp2518fd_install(const can_bus_config_t *cfg)
{
    (void)cfg;
    return ESP_ERR_NOT_SUPPORTED;
}
extern "C" esp_err_t mcp2518fd_send(int bus_id, uint32_t id, const uint8_t *data, uint8_t len, uint32_t timeout_ms)
{
    (void)bus_id; (void)id; (void)data; (void)len; (void)timeout_ms;
    return ESP_ERR_NOT_SUPPORTED;
}
extern "C" esp_err_t mcp2518fd_receive(int bus_id, twai_message_t *out, uint32_t timeout_ms)
{
    (void)bus_id; (void)out; (void)timeout_ms;
    return ESP_ERR_NOT_SUPPORTED;
}
extern "C" esp_err_t mcp2518fd_status(int bus_id, twai_status_info_t *out)
{
    (void)bus_id; (void)out;
    return ESP_ERR_NOT_SUPPORTED;
}
extern "C" esp_err_t mcp2518fd_set_bitrate(int bus_id, uint32_t kbps)
{
    (void)bus_id; (void)kbps;
    return ESP_ERR_NOT_SUPPORTED;
}
extern "C" void mcp2518fd_bus_off_recover(int bus_id) { (void)bus_id; }
extern "C" bool mcp2518fd_take_activity(int bus_id) { (void)bus_id; return false; }
extern "C" bool mcp2518fd_is_up(int bus_id) { (void)bus_id; return false; }

#else

#define ARDUINO_ARCH_ESP32 1

#include <string.h>

#include "ACAN2517FD.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "mcp2518";

/* From can/can.h (C header without extern "C" guard). */
extern "C" int can_filter_get(int bus_id, uint32_t *ids, int max);
#define MCP_HW_FILTER_MAX 32

static SPIClass s_spi;
static ACAN2517FD *s_can = nullptr;
static can_bus_config_t s_cfg;
static bool s_up = false;
static volatile bool s_activity = false;
static uint32_t s_off_since_ms = 0;

static void mcp_isr(void)
{
    s_activity = true;
    if (s_can) s_can->isr();
}

static ACAN2517FDSettings::Oscillator oscillator_for(uint32_t hz)
{
    if (hz == 40000000u) return ACAN2517FDSettings::OSC_40MHz;
    if (hz == 20000000u) return ACAN2517FDSettings::OSC_20MHz;
    if (hz == 4000000u) return ACAN2517FDSettings::OSC_4MHz;
    return ACAN2517FDSettings::OSC_40MHz;
}

static uint8_t clamp_fifo(uint32_t n, uint8_t fallback, uint8_t lo, uint8_t hi)
{
    if (n == 0) n = fallback;
    if (n < lo) n = lo;
    if (n > hi) n = hi;
    return static_cast<uint8_t>(n);
}

static ACAN2517FDSettings make_settings(uint32_t kbps)
{
    ACAN2517FDSettings settings(oscillator_for(s_cfg.osc_hz),
                                kbps * 1000u,
                                DataBitRateFactor::x1);
    /* Classic CAN on the wire. x1 still fills DBTCFG, so FD mode is not
     * blocked on an unprogrammed data phase. */
    settings.mRequestedMode = ACAN2517FDSettings::Normal20B;
    settings.mControllerTransmitFIFOPayload = ACAN2517FDSettings::PAYLOAD_8;
    settings.mControllerReceiveFIFOPayload = ACAN2517FDSettings::PAYLOAD_8;
    settings.mControllerTXQBufferPayload = ACAN2517FDSettings::PAYLOAD_8;
    settings.mControllerTransmitFIFOSize = clamp_fifo(s_cfg.tx_queue_len, 8, 1, 32);
    settings.mControllerReceiveFIFOSize = clamp_fifo(s_cfg.rx_queue_len, 16, 1, 32);
    settings.mDriverTransmitFIFOSize = clamp_fifo(s_cfg.tx_queue_len, 8, 1, 32);
    settings.mDriverReceiveFIFOSize = clamp_fifo(s_cfg.rx_queue_len, 16, 1, 32);
    settings.mControllerTXQSize = 0;
    return settings;
}

static void pull_idle(gpio_num_t pin)
{
    if (pin == GPIO_NUM_NC) return;
    gpio_reset_pin(pin);
    gpio_set_direction(pin, GPIO_MODE_INPUT);
    gpio_set_pull_mode(pin, GPIO_PULLUP_ONLY);
}

static esp_err_t start_controller(uint32_t kbps)
{
    ACAN2517FDSettings settings = make_settings(kbps);
    if (!settings.mArbitrationBitRateClosedToDesiredRate || !settings.exactArbitrationBitRate()) {
        ESP_LOGE(TAG, "no exact bit timing for %" PRIu32 " kbit/s at %" PRIu32 " Hz",
                 kbps, s_cfg.osc_hz);
        return ESP_ERR_INVALID_ARG;
    }
    /* Hardware acceptance filters: up to 32 exact standard IDs from the
     * bus' can_filter list. More IDs (or an extended ID) -> pass-all, the
     * software filter in can_poll() still applies. */
    uint32_t ids[64];
    int n = can_filter_get(s_cfg.bus_id, ids, 64);
    bool exact = n > 0 && n <= MCP_HW_FILTER_MAX;
    for (int i = 0; exact && i < n; i++) if (ids[i] > 0x7FF) exact = false;
    uint32_t err;
    if (exact) {
        ACAN2517FDFilters filters;
        for (int i = 0; i < n; i++) filters.appendFrameFilter(kStandard, ids[i], nullptr);
        if (filters.filterStatus() != ACAN2517FDFilters::kFiltersOk) {
            ESP_LOGW(TAG, "bus%d: filter list rejected (status %d), accepting all",
                     s_cfg.bus_id, (int)filters.filterStatus());
            err = s_can->begin(settings, mcp_isr);
        } else {
            err = s_can->begin(settings, mcp_isr, filters);
            if (err == 0) ESP_LOGI(TAG, "bus%d: %d MCP2518FD hw filters active", s_cfg.bus_id, n);
        }
    } else {
        err = s_can->begin(settings, mcp_isr);
    }
    if (err != 0) {
        ESP_LOGE(TAG, "ACAN2517FD begin failed: 0x%08" PRIx32, err);
        s_can->end();
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "bus%d MCP2518FD up via ACAN2517FD (cs=%d sclk=%d mosi=%d miso=%d int=%d, %" PRIu32
             " kbit/s, osc=%" PRIu32 " Hz, actual=%" PRIu32 ")",
             s_cfg.bus_id, s_cfg.spi_cs, s_cfg.spi_sclk, s_cfg.spi_mosi, s_cfg.spi_miso,
             s_cfg.int_pin, kbps, s_cfg.osc_hz, settings.actualArbitrationBitRate());
    return ESP_OK;
}

extern "C" esp_err_t mcp2518fd_install(const can_bus_config_t *cfg)
{
    if (!cfg || cfg->bus_id < 0 || cfg->bus_id >= CAN_BUS_COUNT) return ESP_ERR_INVALID_ARG;
    if (s_up) return ESP_ERR_INVALID_STATE;
    if (cfg->osc_hz == 0 || cfg->int_pin == GPIO_NUM_NC || cfg->spi_cs == GPIO_NUM_NC) {
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->osc_hz != 40000000u && cfg->osc_hz != 20000000u && cfg->osc_hz != 4000000u) {
        return ESP_ERR_INVALID_ARG;
    }

    s_cfg = *cfg;
    if (cfg->stb_pin != GPIO_NUM_NC) {
        gpio_reset_pin(cfg->stb_pin);
        gpio_set_direction(cfg->stb_pin, GPIO_MODE_OUTPUT);
        gpio_set_level(cfg->stb_pin, 0);
    }
    pull_idle(cfg->int0_pin);
    pull_idle(cfg->int1_pin);

    esp_err_t err = s_spi.begin(cfg->spi_sclk, cfg->spi_miso, cfg->spi_mosi);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI begin: %s", esp_err_to_name(err));
        return err;
    }

    static ACAN2517FD can(static_cast<uint8_t>(cfg->spi_cs), s_spi,
                          static_cast<uint8_t>(cfg->int_pin));
    s_can = &can;

    err = start_controller(cfg->bitrate_kbps);
    if (err != ESP_OK) {
        s_can = nullptr;
        return err;
    }
    s_up = true;
    return ESP_OK;
}

extern "C" esp_err_t mcp2518fd_send(int bus_id, uint32_t id,
                                    const uint8_t *data, uint8_t len,
                                    uint32_t timeout_ms)
{
    if (len > 8) return ESP_ERR_INVALID_ARG;
    if (!s_up || !s_can || bus_id != s_cfg.bus_id) return ESP_ERR_INVALID_STATE;

    CANFDMessage msg;
    msg.id = id;
    msg.ext = id > 0x7FFu;
    msg.type = CANFDMessage::CAN_DATA;
    msg.len = len;
    if (data && len) memcpy(msg.data, data, len);

    uint32_t start = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    for (;;) {
        if (s_can->tryToSend(msg)) return ESP_OK;
        uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000);
        if ((now - start) >= timeout_ms) return ESP_ERR_TIMEOUT;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

extern "C" esp_err_t mcp2518fd_receive(int bus_id, twai_message_t *out, uint32_t timeout_ms)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    if (!s_up || !s_can || bus_id != s_cfg.bus_id) return ESP_ERR_INVALID_STATE;

    uint32_t start = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    for (;;) {
        if (s_can->available()) {
            CANFDMessage msg;
            if (s_can->receive(msg)) {
                memset(out, 0, sizeof(*out));
                out->identifier = msg.id;
                out->extd = msg.ext ? 1 : 0;
                out->rtr = (msg.type == CANFDMessage::CAN_REMOTE) ? 1 : 0;
                uint8_t n = msg.len > 8 ? 8 : msg.len;
                out->data_length_code = n;
                if (!out->rtr && n) memcpy(out->data, msg.data, n);
                if (msg.type == CANFDMessage::CANFD_NO_BIT_RATE_SWITCH ||
                    msg.type == CANFDMessage::CANFD_WITH_BIT_RATE_SWITCH) {
                    ESP_LOGD(TAG, "bus%d FD frame id=0x%" PRIx32 " len=%u kept first 8 bytes",
                             bus_id, msg.id, msg.len);
                }
                return ESP_OK;
            }
        }
        uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000);
        if ((now - start) >= timeout_ms) return ESP_ERR_TIMEOUT;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

extern "C" esp_err_t mcp2518fd_status(int bus_id, twai_status_info_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    if (!s_up || !s_can || bus_id != s_cfg.bus_id) return ESP_ERR_INVALID_STATE;

    uint32_t trec = s_can->errorCounters();
    ACAN2517FDSettings::OperationMode mode = s_can->currentOperationMode();
    bool txbo = (trec & (1u << 21)) != 0;

    memset(out, 0, sizeof(*out));
    if (txbo || mode == ACAN2517FDSettings::RestrictedOperation) out->state = TWAI_STATE_BUS_OFF;
    else if (mode == ACAN2517FDSettings::Normal20B || mode == ACAN2517FDSettings::NormalFD) out->state = TWAI_STATE_RUNNING;
    else out->state = TWAI_STATE_STOPPED;

    out->tx_error_counter = (trec >> 8) & 0xFF;
    out->rx_error_counter = trec & 0xFF;
    out->rx_overrun_count = s_can->hardwareReceiveBufferOverflowCount();
    out->msgs_to_tx = s_can->driverTransmitBufferCount();
    out->msgs_to_rx = s_can->available() ? 1 : 0;
    return ESP_OK;
}

extern "C" esp_err_t mcp2518fd_set_bitrate(int bus_id, uint32_t kbps)
{
    if (!s_up || !s_can || bus_id != s_cfg.bus_id) return ESP_ERR_INVALID_STATE;
    if (kbps != 125 && kbps != 250 && kbps != 500 && kbps != 1000) return ESP_ERR_INVALID_ARG;
    s_can->end();
    esp_err_t err = start_controller(kbps);
    if (err == ESP_OK) s_cfg.bitrate_kbps = kbps;
    else s_up = false;
    return err;
}

extern "C" void mcp2518fd_bus_off_recover(int bus_id)
{
    if (!s_up || !s_can || bus_id != s_cfg.bus_id) return;
    /* This used to run every main-loop pass (1 ms): two SPI register reads,
     * each with interrupts masked inside ACAN2517FD. Bus-off recovery only
     * needs to react within ~1 s, so poll every 100 ms. */
    static uint32_t s_last_check_ms = 0;
    uint32_t now0 = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    if ((now0 - s_last_check_ms) < 100) return;
    s_last_check_ms = now0;
    uint32_t trec = s_can->errorCounters();
    bool txbo = (trec & (1u << 21)) != 0;
    ACAN2517FDSettings::OperationMode mode = s_can->currentOperationMode();
    uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    if (txbo || mode == ACAN2517FDSettings::RestrictedOperation) {
        s_activity = true;
        if (!s_off_since_ms) s_off_since_ms = now;
        else if ((now - s_off_since_ms) >= 1000) {
            ESP_LOGW(TAG, "bus%d bus-off, requesting CAN 2.0 mode", bus_id);
            s_can->recoverFromRestrictedOperationMode();
            if (s_can->currentOperationMode() != ACAN2517FDSettings::Normal20B) {
                s_can->setOperationMode(ACAN2517FDSettings::Normal20B);
            }
            s_off_since_ms = now;
        }
    } else {
        s_off_since_ms = 0;
    }
}

extern "C" bool mcp2518fd_take_activity(int bus_id)
{
    if (!s_up || bus_id != s_cfg.bus_id) return false;
    bool v = s_activity;
    s_activity = false;
    return v;
}

extern "C" bool mcp2518fd_is_up(int bus_id)
{
    return s_up && bus_id == s_cfg.bus_id;
}

#endif /* BOARD_T2CAN_FD */
