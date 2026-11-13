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
#include "protocol/protocol.h"
#include "sdkconfig.h"
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#endif

static const char *TAG = "debug";

#define DEBUG_NS  "dorky"
#define DEBUG_KEY "debug"

static bool s_enabled;
static bool s_stdin_nb;
static bool s_usj_driver;
static dorky_console_hook_t s_console_hook;

void dorky_debug_set_console_hook(dorky_console_hook_t hook) { s_console_hook = hook; }

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
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    /* Without the driver, the IDF USB-Serial-JTAG VFS is TX-only in practice:
     * the non-driver read path never drains the RX FIFO, so a host writing to
     * COMx just times out. Install the driver (ISR lands on the calling core,
     * i.e. the app core) and route the VFS through it. TX stays safe without a
     * host attached: the VFS blocks at most once for 50 ms, then drops. */
    if (!usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_driver_config_t cfg = {
            .tx_buffer_size = 1024,
            .rx_buffer_size = 256,
        };
        if (usb_serial_jtag_driver_install(&cfg) == ESP_OK) {
            usb_serial_jtag_vfs_use_driver();
            s_usj_driver = true;
        }
    } else {
        s_usj_driver = true;
    }
    if (s_usj_driver) {
        ESP_LOGI(TAG, "console: USB-Serial-JTAG input ready (type 'help')");
        return;
    }
#endif
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

    ESP_LOGI(TAG, "hb up=%lus heap=%lu min=%lu psram=%lu iram=%lu iram_min=%lu iram_big=%lu %s %s",
             (unsigned long)up_s, (unsigned long)heap, (unsigned long)min_heap,
             (unsigned long)psram,
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned long)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             ble, buses);
    {
        char st[256];
        protocol_stats_line(st, sizeof st);
        ESP_LOGI(TAG, "hb %s", st);
    }

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
    char cmd[96];
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
        if (s_console_hook) s_console_hook(line, "help");
    } else if (s_console_hook && s_console_hook(line, cmd)) {
        /* handled by the app hook */
    } else {
        printf("unknown command: %s  (try: help)\n", line);
    }
}

static int console_read_byte(char *c)
{
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    if (s_usj_driver) return usb_serial_jtag_read_bytes(c, 1, 0) == 1 ? 1 : 0;
#endif
    if (!s_stdin_nb) return -1;
    int fd = fileno(stdin);
    if (fd < 0) return -1;
    ssize_t r = read(fd, c, 1);
    if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK) return -1;
    return r > 0 ? 1 : 0;
}

void dorky_debug_console_poll(void)
{
    static char buf[96];
    static size_t len;
    char c;
    for (int guard = 0; guard < 256; guard++) {
        if (console_read_byte(&c) <= 0) break;
        if (c == '\r' || c == '\n') {
            if (len == 0) continue;
            buf[len] = '\0';
            handle_console_line(buf);
            len = 0;
            continue;
        }
        if (c == 0x08 || c == 0x7f) { if (len) len--; continue; }
        if (len + 1 < sizeof(buf)) buf[len++] = c;
        else len = 0; /* overflow: drop the line */
    }
}
