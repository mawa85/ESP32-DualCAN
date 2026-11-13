/* Host run of model_y_sim.be against the real Berry VM, berry_bindings.c,
 * berry_registry.c, script_loader.c, can.c and rate_limit.c. Only the CAN
 * driver is stubbed: bus0 TX lands in bus1 RX (and vice versa), like the
 * CAN-A <-> CAN-B wire. argv: seconds [unplug_at_s [replug_at_s]]. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "berry.h"
#include "scripting/berry_bindings.h"
#include "scripting/script_loader.h"
#include "can/can_driver.h"
#include "can/tesla.h"
#include "debug/dorky_debug.h"

static int64_t g_us;
int64_t esp_timer_get_time(void) { return g_us; }
void vTaskDelay(unsigned t) { g_us += 1000 * (int64_t)t; }

#define QN 4096
static twai_message_t q[2][QN]; static int qh[2], qt[2];
static long txn[2], drop;
static bool unplugged;
static int chk_bad;
static FILE *dumpf;  /* SIM_DUMP=datei: alle TX-Frames als 'ms id hex' */

esp_err_t can_bus_send(int bus_id, uint32_t id, const uint8_t *data, uint8_t len, uint32_t timeout_ms)
{
    (void)timeout_ms;
    txn[bus_id]++;
    if (dumpf) { fprintf(dumpf, "%lld %03X ", (long long)(g_us / 1000), (unsigned)id); for (int i = 0; i < len; i++) fprintf(dumpf, "%02X", data[i]); fputc('\n', dumpf); }
    /* Verify checksum on frames that carry one (byte 0). */
    if (id == 0x118 || id == 0x129 || id == 0x257 || id == 0x39D) {
        if (tesla_checksum((uint16_t)id, 0, data, len) != data[0]) chk_bad++;
    }
    if (unplugged) return ESP_ERR_TIMEOUT;
    int o = bus_id ^ 1, n = (qh[o] + 1) % QN;
    if (n == qt[o]) { drop++; return ESP_FAIL; }
    twai_message_t *m = &q[o][qh[o]];
    memset(m, 0, sizeof(*m)); m->identifier = id; m->data_length_code = len; memcpy(m->data, data, len);
    qh[o] = n; return ESP_OK;
}
esp_err_t can_bus_receive(int bus_id, twai_message_t *out, uint32_t timeout_ms)
{
    (void)timeout_ms;
    if (qt[bus_id] == qh[bus_id]) return ESP_ERR_TIMEOUT;
    *out = q[bus_id][qt[bus_id]]; qt[bus_id] = (qt[bus_id] + 1) % QN; return ESP_OK;
}
void can_bus_off_recover(int bus_id) { (void)bus_id; }
esp_err_t led_rgb_set(uint8_t r, uint8_t g, uint8_t b) { (void)r; (void)g; (void)b; return 0; }
esp_err_t led_rgb_off(void) { return 0; }
esp_err_t state_set(const char *ns, const char *k, const char *v) { (void)ns; (void)k; (void)v; return 0; }
esp_err_t state_get(const char *ns, const char *k, char *b, size_t n) { (void)ns; (void)k; (void)b; (void)n; return -1; }
esp_err_t state_remove(const char *ns, const char *k) { (void)ns; (void)k; return 0; }
static bool dbg; bool dorky_debug_enabled(void) { return dbg; }
esp_err_t dorky_debug_set(bool on) { dbg = on; return 0; }

static void log_handler(const char *msg) { printf("[%7.2fs] %s\n", g_us / 1e6, msg); }

int main(int argc, char **argv)
{
    int secs = argc > 1 ? atoi(argv[1]) : 60;
    int unplug_at = argc > 2 ? atoi(argv[2]) : -1;
    int replug_at = argc > 3 ? atoi(argv[3]) : -1;
    if (getenv("SIM_DUMP")) dumpf = fopen(getenv("SIM_DUMP"), "w");
    static can_t b0, b1;
    can_init(&b0, 0); can_init(&b1, 1);
    bvm *vm = be_vm_new();
    berry_register_bindings(vm);
    berry_set_buses(&b0, &b1);
    berry_set_log_handler(log_handler);
    static script_loader_t loader;
    script_loader_scan(&loader, vm);
    int rc = -1;
    for (int i = 0; i < loader.count; i++)
        if (strcmp(loader.scripts[i].filename, "model_y_sim.be") == 0) {
            rc = script_loader_enable(&loader, i);
            if (rc) printf("ENABLE FAILED: %s\n", loader.scripts[i].error);
        }
    if (rc) return 1;
    long rx1 = 0;
    for (uint32_t ms = 1; ms <= (uint32_t)secs * 1000; ms++) {
        g_us = (int64_t)ms * 1000 + (ms * 7919) % 1000;   /* jitter inside the ms */
        if (unplug_at >= 0 && ms == (uint32_t)unplug_at * 1000) { unplugged = true; printf("--- Kabel ab ---\n"); }
        if (replug_at >= 0 && ms == (uint32_t)replug_at * 1000) { unplugged = false; printf("--- Kabel dran ---\n"); }
        rx1 += can_poll(&b0, ms) + can_poll(&b1, ms);
        berry_timer_tick(ms);
        for (int i = 0; i < loader.count; i++)
            if (loader.scripts[i].errored) { printf("SCRIPT ERROR: %s\n", loader.scripts[i].error); return 2; }
    }
    printf("driver tx bus0=%ld bus1=%ld, rx polled=%ld, drops=%ld, checksum_bad=%d\n", txn[0], txn[1], rx1, drop, chk_bad);
    script_loader_disable(&loader, 0);
    return chk_bad ? 3 : 0;
}
