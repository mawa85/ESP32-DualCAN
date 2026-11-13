#include "debug/dorky_debug.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "ble/ble_transport.h"
#include "can/can_driver.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "storage/state.h"

static const char *TAG = "debug";

#define DEBUG_NS  "dorky"
#define DEBUG_KEY "debug"

static bool s_enabled;
static bool s_stdin_nb;

static const char *reset_reason_str(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_UNKNOWN:   return "unknown";
    case ESP_RST_POWERON:   return "power-on";
    case ESP_RST_EXT:       return "external pin";
    case ESP_RST_SW:        return "software";
    case ESP_RST_PANIC:     return "panic";
    case ESP_RST_INT_WDT:   return "interrupt watchdog";
    case ESP_RST_TASK_WDT:  return "task watchdog";
    case ESP_RST_WDT:       return "other watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_SDIO:      return "SDIO";
#ifdef ESP_RST_USB
    case ESP_RST_USB:       return "USB";
#endif
#ifdef ESP_RST_JTAG
    case ESP_RST_JTAG:      return "JTAG";
#endif
#ifdef ESP_RST_EFUSE
    case ESP_RST_EFUSE:     return "eFuse";
#endif
#ifdef ESP_RST_PWR_GLITCH
    case ESP_RST_PWR_GLITCH: return "power glitch";
#endif
#ifdef ESP_RST_CPU_LOCKUP
    case ESP_RST_CPU_LOCKUP: return "CPU lockup";
#endif
    default:                return "other";
    }
}

void dorky_debug_log_reset_reason(void)
{
    esp_reset_reason_t r = esp_reset_reason();
    ESP_LOGI("dorky", "reset reason: %d (%s)", (int)r, reset_reason_str(r));
}

void dorky_debug_apply_log_levels(void)
{
    /* When debug is on, keep the "ble" tag at DEBUG even while a client is
     * connected (ble_transport otherwise clips it to WARN to avoid flooding
     * the notify path). When off, restore the connected-session clip if a
     * client is present, otherwise leave the default INFO. */
    if (s_enabled) {
        esp_log_level_set("ble", ESP_LOG_DEBUG);
        esp_log_level_set("debug", ESP_LOG_INFO);
    } else if (dorky_ble_connected()) {
        esp_log_level_set("ble", ESP_LOG_WARN);
    }
}

bool dorky_debug_enabled(void)
{
    return s_enabled;
}

esp_err_t dorky_debug_set(bool on)
{
    esp_err_t err = state_set(DEBUG_NS, DEBUG_KEY, on ? "1" : "0");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS write failed: %s", esp_err_to_name(err));
        return err;
    }
    s_enabled = on;
    dorky_debug_apply_log_levels();
    ESP_LOGI(TAG, "debug %s (persisted)", on ? "ON" : "OFF");
    return ESP_OK;
}

void dorky_debug_init(void)
{
    char buf[4] = {0};
    if (state_get(DEBUG_NS, DEBUG_KEY, buf, sizeof(buf)) == ESP_OK) {
        s_enabled = (buf[0] == '1');
    } else {
        s_enabled = (DORKY_DEBUG_DEFAULT != 0);
    }
    dorky_debug_apply_log_levels();
    ESP_LOGI(TAG, "debug flag %s (default compile-time %d)",
             s_enabled ? "ON" : "OFF", DORKY_DEBUG_DEFAULT);
}

void dorky_debug_console_init(void)
{
    int fd = fileno(stdin);
    if (fd < 0) return;
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0)
        s_stdin_nb = true;
    setvbuf(stdin, NULL, _IONBF, 0);
}

static const char *twai_state_str(twai_state_t s)
{
    switch (s) {
    case TWAI_STATE_STOPPED:     return "stopped";
    case TWAI_STATE_RUNNING:     return "running";
    case TWAI_STATE_BUS_OFF:     return "bus_off";
    case TWAI_STATE_RECOVERING:  return "recovering";
    default:                     return "?";
    }
}

void dorky_debug_heartbeat(void)
{
    if (!s_enabled) return;

    uint32_t up_s = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    uint32_t heap = (uint32_t)esp_get_free_heap_size();
    uint32_t min_heap = (uint32_t)esp_get_minimum_free_heap_size();
    uint32_t psram = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    char ble[96];
    dorky_ble_debug_snapshot(ble, sizeof(ble));

    /* Use can_bus_status only — can_bus_health() drains the RX-edge flag
     * and would fight the protocol tick's health computation. */
    char buses[160] = {0};
    size_t off = 0;
    for (int i = 0; i < CAN_BUS_COUNT; i++) {
        twai_status_info_t st;
        const char *st_s = "?";
        unsigned tec = 0, rec = 0;
        if (can_bus_status(i, &st) == ESP_OK) {
            st_s = twai_state_str(st.state);
            tec = st.tx_error_counter;
            rec = st.rx_error_counter;
        }
        int n = snprintf(buses + off, sizeof(buses) - off,
                         "%sbus%d %s TEC=%u REC=%u",
                         off ? " " : "", i, st_s, tec, rec);
        if (n < 0 || (size_t)n >= sizeof(buses) - off) break;
        off += (size_t)n;
    }

    ESP_LOGI(TAG, "hb up=%lus heap=%lu min=%lu psram=%lu %s %s",
             (unsigned long)up_s, (unsigned long)heap, (unsigned long)min_heap,
             (unsigned long)psram, ble, buses);

    /* Stack high-water marks: remaining free stack in bytes, lowest first. */
    TaskStatus_t tasks[24];
    UBaseType_t n = uxTaskGetSystemState(tasks, 24, NULL);
    /* Print a compact "name:free" list. */
    char stk[192];
    off = 0;
    for (UBaseType_t i = 0; i < n && off + 8 < sizeof(stk); i++) {
        int nwr = snprintf(stk + off, sizeof(stk) - off, "%s%s:%u",
                           off ? " " : "",
                           tasks[i].pcTaskName ? tasks[i].pcTaskName : "?",
                           (unsigned)tasks[i].usStackHighWaterMark);
        if (nwr < 0 || (size_t)nwr >= sizeof(stk) - off) break;
        off += (size_t)nwr;
    }
    if (n > 0) {
        ESP_LOGI(TAG, "hb stack(free) %s%s", stk, n >= 24 ? " …" : "");
    }
}

static void handle_console_line(char *line)
{
    /* trim */
    while (*line && isspace((unsigned char)*line)) line++;
    size_t n = strlen(line);
    while (n > 0 && isspace((unsigned char)line[n - 1])) line[--n] = '\0';
    if (n == 0) return;

    /* lowercase the command word in place for matching */
    char cmd[48];
    size_t cpy = n < sizeof(cmd) - 1 ? n : sizeof(cmd) - 1;
    memcpy(cmd, line, cpy);
    cmd[cpy] = '\0';
    for (char *p = cmd; *p; p++) *p = (char)tolower((unsigned char)*p);

    if (strcmp(cmd, "debug on") == 0 || strcmp(cmd, "debug 1") == 0) {
        if (dorky_debug_set(true) == ESP_OK)
            printf("debug ON (persisted in NVS)\n");
        else
            printf("debug: NVS write failed\n");
    } else if (strcmp(cmd, "debug off") == 0 || strcmp(cmd, "debug 0") == 0) {
        if (dorky_debug_set(false) == ESP_OK)
            printf("debug OFF (persisted in NVS)\n");
        else
            printf("debug: NVS write failed\n");
    } else if (strcmp(cmd, "debug") == 0 || strcmp(cmd, "debug status") == 0) {
        printf("debug %s\n", s_enabled ? "ON" : "OFF");
    } else if (strcmp(cmd, "help") == 0 || strcmp(cmd, "debug help") == 0) {
        printf("commands: debug on | debug off | debug status\n");
    } else {
        printf("unknown command: %s  (try: debug on | debug off | debug status)\n", line);
    }
}

void dorky_debug_console_poll(void)
{
    static char buf[64];
    static size_t len;
    if (!s_stdin_nb) return;
    int fd = fileno(stdin);
    if (fd < 0) return;

    char c;
    while (1) {
        ssize_t r = read(fd, &c, 1);
        if (r <= 0) {
            if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
                return;
            break;
        }
        if (c == '\r') continue;
        if (c == '\n') {
            buf[len] = '\0';
            handle_console_line(buf);
            len = 0;
            continue;
        }
        if (len + 1 < sizeof(buf)) buf[len++] = c;
        else len = 0; /* overflow: drop the line */
    }
}
