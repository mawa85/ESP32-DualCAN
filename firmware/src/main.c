#include "board/ext_ram.h"
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/twai.h"
#include "esp_log.h"
#include "esp_system.h"
#include "sdkconfig.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "berry.h"
#include "can/can.h"
#include "can/can_driver.h"
#include "led/led_rgb.h"
#include "scripting/berry_bindings.h"
#include "ble/ble_transport.h"
#include "storage/fs.h"
#include "storage/state.h"
#include "scripting/script_loader.h"
#include "protocol/protocol.h"
#include "tesla_ble/tesla_ble.h"
#include "tesla_ble/tesla_vehicle.h"
#include "wifi/wifi.h"
#include "http_file_server/http_file_server.h"
#include "ota/ota.h"
#include "board/board.h"
#include "debug/dorky_debug.h"

static const char *TAG = "dorky";

#define BITRATE_KBPS    500

static void status_led(int on)
{
    if (BOARD_STATUS_LED_GPIO == GPIO_NUM_NC) return;
    gpio_set_level(BOARD_STATUS_LED_GPIO, on);
}

DORKY_EXT_BSS static can_t bus0, bus1;   /* ~10 KB, task-only */

static uint32_t load_bus_bitrate_nvs(int bus_id)
{
    char key[4];
    snprintf(key, sizeof(key), "b%d", bus_id);
    char buf[8];
    if (state_get("busconfig", key, buf, sizeof(buf)) == ESP_OK) {
        uint32_t kbps = (uint32_t)atoi(buf);
        if (kbps == 125 || kbps == 250 || kbps == 500 || kbps == 1000) return kbps;
    }
    return BITRATE_KBPS;
}

static void load_bus_filter_nvs(int bus_id)
{
    char key[4];
    snprintf(key, sizeof(key), "f%d", bus_id);
    char csv[CAN_FILTER_MAX_IDS * 9 + 1];
    if (state_get("busconfig", key, csv, sizeof(csv)) == ESP_OK) {
        uint32_t ids[CAN_FILTER_MAX_IDS];
        int n = can_filter_parse_csv(csv, ids, CAN_FILTER_MAX_IDS);
        can_filter_set(bus_id, ids, n);
    }
}

static void hw_init(void)
{
    if (BOARD_STATUS_LED_GPIO != GPIO_NUM_NC) {
        gpio_reset_pin(BOARD_STATUS_LED_GPIO);
        gpio_set_direction(BOARD_STATUS_LED_GPIO, GPIO_MODE_OUTPUT);
    }

    led_rgb_init();

    dorky_mem_mark("boot");
    state_nvs_init();  /* must come before reading bitrates from NVS */
    dorky_debug_init();
    dorky_debug_console_init();

    /* RX filters first: can_bus_install programs the hardware filters. */
    load_bus_filter_nvs(0);
    load_bus_filter_nvs(1);

    can_bus_config_t c0, c1;
    board_can_config(0, load_bus_bitrate_nvs(0), &c0);
    board_can_config(1, load_bus_bitrate_nvs(1), &c1);
    ESP_ERROR_CHECK(can_bus_install(&c0));
    ESP_ERROR_CHECK(can_bus_install(&c1));

    can_init(&bus0, 0);
    can_init(&bus1, 1);

    dorky_mem_mark("can");
    fs_init();
    dorky_mem_mark("fs");
    wifi_init();
    dorky_mem_mark("wifi");
    tesla_ble_init();
    tesla_vehicle_init();
    dorky_mem_mark("tesla");
}

/* ---- safe mode + console commands ---------------------------------- */

DORKY_EXT_BSS static script_loader_t s_loader;
static bool s_safe_mode;
static bool s_scripts_restored;

/* Safe mode: hold BOOT (GPIO0) for >= SAFE_MODE_HOLD_MS during the first
 * SAFE_MODE_WINDOW_MS after reset. GPIO0 is a strapping pin, so it must be
 * pressed *after* reset/power-up (held during reset = ROM download mode).
 * Safe mode skips the autostart of persisted scripts for this boot only. */
#define SAFE_MODE_WINDOW_MS 3000U
#define SAFE_MODE_HOLD_MS    500U

static bool boot_button_safe_mode_window(void)
{
    gpio_reset_pin(BOARD_BOOT_BTN_GPIO);
    gpio_set_direction(BOARD_BOOT_BTN_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BOARD_BOOT_BTN_GPIO, GPIO_PULLUP_ONLY);
    vTaskDelay(pdMS_TO_TICKS(5));

    ESP_LOGI(TAG, "hold BOOT now (%u s window) for SAFE MODE (no script autostart)",
             SAFE_MODE_WINDOW_MS / 1000U);
    uint32_t t0 = (uint32_t)(esp_timer_get_time() / 1000);
    uint32_t held_since = 0;
    for (;;) {
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        bool pressed = gpio_get_level(BOARD_BOOT_BTN_GPIO) == 0;
        if (pressed) {
            if (!held_since) held_since = now ? now : 1;
            if ((now - held_since) >= SAFE_MODE_HOLD_MS) return true;
        } else {
            held_since = 0;
            if ((now - t0) >= SAFE_MODE_WINDOW_MS) return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static int stop_all_scripts(void)
{
    int stopped = 0;
    for (int i = 0; i < s_loader.count; i++) {
        if (s_loader.scripts[i].loaded || s_loader.scripts[i].enabled) {
            script_loader_disable(&s_loader, i);
            stopped++;
        }
    }
    return stopped;
}

static int find_script(const char *name)
{
    for (int i = 0; i < s_loader.count; i++)
        if (strcmp(s_loader.scripts[i].filename, name) == 0) return i;
    return -1;
}

static bool console_hook(const char *line, const char *lower)
{
    if (strcmp(lower, "help") == 0) {
        printf("          scripts | script stop all | script stop <file> | script start <file>\n"
               "          safe (stop all + autostart off) | filter | reboot\n"
               "          trace status | trace gate <free> <big> (bytes) | mem\n");
        return true;
    }
    if (strcmp(lower, "scripts") == 0 || strcmp(lower, "script list") == 0) {
        printf("%d script(s)%s\n", s_loader.count, s_safe_mode ? "  [SAFE MODE]" : "");
        for (int i = 0; i < s_loader.count; i++) {
            const script_entry_t *e = &s_loader.scripts[i];
            printf("  %-32s %s%s%s\n", e->filename,
                   e->loaded ? "running" : (e->enabled ? "enabled" : "stopped"),
                   e->errored ? "  ERROR: " : "", e->errored ? e->error : "");
        }
        return true;
    }
    if (strcmp(lower, "script stop all") == 0) {
        /* Runtime only: the autostart list (.enabled) is kept, so the
         * scripts come back after a reboot. Use `safe` to clear it. */
        int n = stop_all_scripts();
        s_scripts_restored = true;
        printf("stopped %d script(s) until reboot (autostart unchanged; 'safe' clears it)\n", n);
        return true;
    }
    if (strcmp(lower, "safe") == 0) {
        int n = stop_all_scripts();
        script_loader_save_enabled(&s_loader);   /* writes an empty .enabled */
        s_scripts_restored = true;
        s_safe_mode = true;
        printf("SAFE: stopped %d script(s), autostart cleared. BLE/web UI stay up.\n", n);
        return true;
    }
    if (strncmp(lower, "script stop ", 12) == 0 || strncmp(lower, "script start ", 13) == 0) {
        bool start = lower[7] == 's' && lower[8] == 't' && lower[9] == 'a';
        const char *name = line + (start ? 13 : 12);
        while (*name == ' ') name++;
        int idx = find_script(name);
        if (idx < 0) { printf("no such script: %s\n", name); return true; }
        int rc = start ? script_loader_enable(&s_loader, idx) : script_loader_disable(&s_loader, idx);
        script_loader_save_enabled(&s_loader);
        printf("%s %s: %s\n", start ? "start" : "stop", name, rc == 0 ? "ok" : "failed");
        return true;
    }
    if (strcmp(lower, "filter") == 0) {
        for (int b = 0; b < CAN_BUS_COUNT; b++) {
            uint32_t ids[CAN_FILTER_MAX_IDS];
            int n = can_filter_get(b, ids, CAN_FILTER_MAX_IDS);
            char csv[CAN_FILTER_MAX_IDS * 9 + 1];
            can_filter_format_csv(ids, n, csv, sizeof(csv));
            printf("bus%d: %s\n", b, n ? csv : "accept all");
        }
        return true;
    }
    if (strcmp(lower, "trace status") == 0 || strcmp(lower, "trace") == 0) {
        char st[384];
        protocol_stats_line(st, sizeof st);
        printf("%s\n", st);
        return true;
    }
    if (strncmp(lower, "trace gate ", 11) == 0) {
        unsigned fl = 0, big = 0;
        if (sscanf(lower + 11, "%u %u", &fl, &big) < 1) {
            printf("usage: trace gate <iram_free_floor> [<iram_big_floor>]\n");
            return true;
        }
        dorky_ble_trace_gate_set(fl, big);
        char st[384];
        protocol_stats_line(st, sizeof st);
        printf("%s\n", st);
        return true;
    }
    if (strcmp(lower, "mem") == 0) {
        dorky_mem_report();
        return true;
    }
    if (strcmp(lower, "reboot") == 0) {
        printf("rebooting...\n");
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
        return true;
    }
    return false;
}

static void app_task(void *arg);

void app_main(void)
{
    /* Everything that is not BLE runs on core 1. The NimBLE host and the BT
     * controller are pinned to core 0; the CAN/SPI/Berry load (ACAN2517FD
     * masks interrupts around every SPI transfer) used to share that core and
     * starved the BLE link under sustained CAN traffic. ISRs installed from
     * app_task (GPIO, TWAI, USB-Serial-JTAG) also land on core 1. */
#if CONFIG_FREERTOS_NUMBER_OF_CORES > 1
    const BaseType_t app_core = 1;
#else
    const BaseType_t app_core = 0;   /* single-core targets (ESP32-C6) */
#endif
    xTaskCreatePinnedToCore(app_task, "dorky_app", CONFIG_ESP_MAIN_TASK_STACK_SIZE,
                            NULL, 1, NULL, app_core);
}

static void app_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Dorky Commander starting on %s (version %s), app core %d",
             BOARD_NAME, DORKY_FIRMWARE_VERSION, xPortGetCoreID());
    dorky_debug_log_reset_reason();
    ota_log_boot_info();
    /* Replaces the old fixed 2 s delay (USB console attach time). */
    s_safe_mode = boot_button_safe_mode_window();
    if (s_safe_mode) ESP_LOGW(TAG, "SAFE MODE: BOOT held at startup - scripts will NOT autostart");

    hw_init();
    dorky_debug_set_console_hook(console_hook);

    bvm *vm = be_vm_new();
    berry_set_buses(&bus0, &bus1);
    berry_register_bindings(vm);

    script_loader_scan(&s_loader, vm);
    /* Defer restore_enabled until we've seen a CAN frame OR the startup window
     * elapses — keeps a faulty script from sending into a dead bus on boot. */
    protocol_init(&s_loader);
    dorky_mem_mark("scripts");
    dorky_ble_init(protocol_on_ble_write, NULL);
    dorky_mem_mark("ble");
    /* HTTP file server only once WiFi has an IP (saves its 8 KB internal
     * stack + sockets when WiFi is unused). See loop below. */
    bool http_started = false;

    /* Startup-delay guardrail: don't enable user scripts until we've seen the
     * buses come alive, or until this timeout fires. */
    const uint32_t STARTUP_DELAY_MS = 5000;
    s_scripts_restored = s_safe_mode;
    uint32_t boot_ms = (uint32_t)(esp_timer_get_time() / 1000);

    uint32_t tick = 0;
    while (1) {
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        int rx_count = 0;
        protocol_tick();
        rx_count += can_poll(&bus0, now);
        rx_count += can_poll(&bus1, now);
        berry_timer_tick(now);

        if (!s_scripts_restored) {
            bool elapsed = (now - boot_ms) >= STARTUP_DELAY_MS;
            if (rx_count > 0 || elapsed) {
                ESP_LOGI(TAG, "enabling persisted scripts (%s)",
                         rx_count > 0 ? "bus alive" : "timeout");
                script_loader_restore_enabled(&s_loader);
                s_scripts_restored = true;
            }
        }

        /* BOOT button (active-low).
         * Short press (<15 s): unlock pairing window.
         * Hold 15 s: factory reset (wipe NVS + LittleFS, reboot). */
#define FACTORY_RESET_HOLD_MS 15000U
        /* Starts "pressed" if BOOT is still held from the safe-mode window,
         * so releasing it does not count as a short press. */
        static bool btn_prev      = false;
        static bool btn_held      = false;
        static uint32_t btn_press_ms = 0;

        bool btn_now = gpio_get_level(BOARD_BOOT_BTN_GPIO);
        if (btn_prev && !btn_now) {
            /* falling edge */
            btn_held     = true;
            btn_press_ms = now;
        } else if (!btn_prev && btn_now && btn_held) {
            /* rising edge — short press */
            btn_held = false;
            if ((now - btn_press_ms) < FACTORY_RESET_HOLD_MS)
                dorky_ble_unlock_pairing();
        } else if (btn_held && !btn_now && (now - btn_press_ms) >= FACTORY_RESET_HOLD_MS) {
            /* held 15 s — factory reset */
            btn_held = false;
            ESP_LOGW(TAG, "BOOT held 15 s — factory reset");
            status_led(1);
            vTaskDelay(pdMS_TO_TICKS(200));
            fs_format();
            state_nvs_erase_all();
            esp_restart();
        }
        btn_prev = btn_now;

        /* Blue LED: fast-blink as countdown warning while BOOT held > 3 s;
         * solid when pairing window open; brief blink on CAN activity. */
        bool led_override = btn_held && (now - btn_press_ms) >= 3000;
        if (led_override) {
            uint32_t held = now - btn_press_ms;
            uint32_t period = held >= 10000 ? 100 : 300;
            status_led((now / period) % 2);
        } else {
            static uint32_t led_on_until = 0;
            static uint32_t led_last_blink = 0;
            if (rx_count > 0 && (now - led_last_blink) >= 200) {
                led_on_until = now + 50;
                led_last_blink = now;
            }
            status_led(dorky_ble_pairing_open() || now < led_on_until ? 1 : 0);
        }

        dorky_debug_console_poll();

        if (!http_started && wifi_connected()) {
            http_started = http_file_server_start() == ESP_OK;
        }

        static uint32_t last_log_ms = 0;
        if ((now - last_log_ms) >= 1000) {
            last_log_ms = now;
            ESP_LOGV(TAG, "tick %" PRIu32 " | free heap: %" PRIu32 " bytes",
                     tick, (uint32_t)esp_get_free_heap_size());
        }
        static uint32_t last_hb_ms = 0;
        if (dorky_debug_enabled() && (now - last_hb_ms) >= 10000) {
            last_hb_ms = now;
            dorky_debug_heartbeat();
        }
        tick++;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
