#include "can/can.h"
#include "can/can_driver.h"
#include "scripting/berry_bindings.h"

#include <stdlib.h>
#include <string.h>

#include <inttypes.h>

#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "can";

static can_raw_observer_t s_raw_observer;

/* ---- acceptance filter ---- */
static uint32_t s_filter_ids[CAN_BUS_COUNT][CAN_FILTER_MAX_IDS];
static int      s_filter_n[CAN_BUS_COUNT];
static uint8_t  s_filter_std[CAN_BUS_COUNT][2048 / 8];   /* bitmap for 11-bit IDs */

int can_filter_set(int bus_id, const uint32_t *ids, int n)
{
    if (bus_id < 0 || bus_id >= CAN_BUS_COUNT) return -1;
    if (n < 0) n = 0;
    if (n > CAN_FILTER_MAX_IDS) n = CAN_FILTER_MAX_IDS;
    memset(s_filter_std[bus_id], 0, sizeof(s_filter_std[bus_id]));
    int k = 0;
    for (int i = 0; i < n; i++) {
        uint32_t id = ids[i] & 0x1FFFFFFFu;
        bool dup = false;
        for (int j = 0; j < k; j++) if (s_filter_ids[bus_id][j] == id) dup = true;
        if (dup) continue;
        s_filter_ids[bus_id][k++] = id;
        if (id < 2048) s_filter_std[bus_id][id >> 3] |= (uint8_t)(1u << (id & 7));
    }
    s_filter_n[bus_id] = k;
    ESP_LOGI(TAG, "bus%d: rx filter %s (%d IDs)", bus_id, k ? "on" : "off (accept all)", k);
    return k;
}

int can_filter_get(int bus_id, uint32_t *ids, int max)
{
    if (bus_id < 0 || bus_id >= CAN_BUS_COUNT) return 0;
    int n = s_filter_n[bus_id];
    if (ids) for (int i = 0; i < n && i < max; i++) ids[i] = s_filter_ids[bus_id][i];
    return n;
}

bool can_filter_allows(int bus_id, uint32_t id)
{
    if (bus_id < 0 || bus_id >= CAN_BUS_COUNT) return true;
    if (s_filter_n[bus_id] == 0) return true;
    if (id < 2048) return (s_filter_std[bus_id][id >> 3] >> (id & 7)) & 1u;
    for (int i = 0; i < s_filter_n[bus_id]; i++)
        if (s_filter_ids[bus_id][i] == id) return true;
    return false;
}

int can_filter_parse_csv(const char *csv, uint32_t *ids, int max)
{
    int n = 0;
    const char *p = csv;
    while (p && *p && n < max) {
        while (*p == ',' || *p == ' ') p++;
        if (!*p) break;
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
        char *end = NULL;
        unsigned long v = strtoul(p, &end, 16);
        if (end == p) break;
        ids[n++] = (uint32_t)v;
        p = end;
    }
    return n;
}

int can_filter_format_csv(const uint32_t *ids, int n, char *out, int out_size)
{
    int off = 0;
    if (out_size > 0) out[0] = '\0';
    for (int i = 0; i < n; i++) {
        int w = snprintf(out + off, out_size - off, "%s%" PRIX32, i ? "," : "", ids[i]);
        if (w < 0 || w >= out_size - off) return -1;
        off += w;
    }
    return off;
}
static uint32_t           s_last_rx_ms[CAN_BUS_COUNT];

/* Rolling RX rate, recomputed on each can_poll() once the window elapses. */
#define RATE_WINDOW_MS 1000U
static uint32_t s_rate_window_start[CAN_BUS_COUNT];
static uint32_t s_rate_count[CAN_BUS_COUNT];
static uint16_t s_rx_rate[CAN_BUS_COUNT];

void can_set_raw_observer(can_raw_observer_t cb) { s_raw_observer = cb; }

uint32_t can_last_rx_ms(int bus_id)
{
    if (bus_id < 0 || bus_id >= CAN_BUS_COUNT) return 0;
    return s_last_rx_ms[bus_id];
}

uint16_t can_bus_rx_rate(int bus_id)
{
    if (bus_id < 0 || bus_id >= CAN_BUS_COUNT) return 0;
    return s_rx_rate[bus_id];
}

/* ---- Ring buffer helpers ----
 *
 * Each cache slot is a depth-3 ring buffer.  `ring_latest` only returns
 * the most recently pushed frame — the older 2 frames are never read.
 * They are reserved for future sliding-window or burst analysis use. */

static void ring_push(msg_ring_t *ring, const twai_message_t *msg)
{
    ring->frames[ring->head] = *msg;
    ring->head = (ring->head + 1) % CAN_FRAME_CACHE;
    if (ring->count < CAN_FRAME_CACHE) ring->count++;
    ring->last_us = (uint32_t)esp_timer_get_time();
}

static const twai_message_t *ring_latest(const msg_ring_t *ring)
{
    if (ring->count == 0) return NULL;
    uint8_t pos = (ring->head + CAN_FRAME_CACHE - 1) % CAN_FRAME_CACHE;
    return &ring->frames[pos];
}

/* ---- Cache helpers ---- */

static int cache_find(msg_ring_t *cache, uint32_t id)
{
    for (int i = 0; i < CAN_MSG_CACHE_MAX; i++) {
        if (cache[i].frames[0].identifier == id) return i;
    }
    return -1;
}

static int cache_check(msg_ring_t *cache, uint32_t msg_id)
{
    int i = cache_find(cache, msg_id);
    if (i >= 0) return i;  /* already registered */
    i = cache_find(cache, 0);  /* find free slot */
    if (i < 0) return -1;
    cache[i].frames[0].identifier = msg_id;
    cache[i].head = 0;
    cache[i].count = 0;
    cache[i].last_us = 0;
    return i;
}

static void cache_update(msg_ring_t *cache, const twai_message_t *rx)
{
    int i = cache_find(cache, rx->identifier);
    if (i < 0) return;
    ring_push(&cache[i], rx);
}

int can_init(can_t *c, int bus_id)
{
    memset(c, 0, sizeof(*c));
    c->bus_id = bus_id;
    rate_limiter_init(&c->rate_limiter, RATE_LIMIT_DEFAULT_HZ);
    c->loaded = true;
    ESP_LOGI(TAG, "bus%d: initialized (no DBC — scripts handle signal processing)", bus_id);
    return 0;
}

void can_free(can_t *c)
{
    memset(c, 0, sizeof(*c));
}

int can_poll(can_t *c, uint32_t now_ms)
{
    can_bus_off_recover(c->bus_id);
    int rx_count = 0;
    twai_message_t rx;
    while (can_bus_receive(c->bus_id, &rx, 0) == ESP_OK) {
        rx_count++;
        s_rate_count[c->bus_id]++;
        s_last_rx_ms[c->bus_id] = now_ms;
        /* Filtered IDs still count for bus health and rx rate above, but are
         * neither streamed over BLE nor cached for scripts. */
        if (!can_filter_allows(c->bus_id, rx.identifier)) continue;
        if (s_raw_observer) s_raw_observer(c->bus_id, &rx, now_ms);

        /* Update frame cache — only if a script has registered this ID */
        cache_update(c->msg_cache, &rx);
    }

    /* Roll the rate window */
    if (s_rate_window_start[c->bus_id] == 0) {
        s_rate_window_start[c->bus_id] = now_ms;
    } else {
        uint32_t elapsed = now_ms - s_rate_window_start[c->bus_id];
        if (elapsed >= RATE_WINDOW_MS) {
            uint32_t rate = (s_rate_count[c->bus_id] * 1000U) / elapsed;
            s_rx_rate[c->bus_id] = rate > UINT16_MAX ? UINT16_MAX : (uint16_t)rate;
            s_rate_count[c->bus_id] = 0;
            s_rate_window_start[c->bus_id] = now_ms;
        }
    }
    return rx_count;
}

int can_read(can_t *c, uint32_t msg_id, uint8_t *out_data, uint8_t *out_dlc, uint32_t timeout_ms)
{
    int i = cache_check(c->msg_cache, msg_id);
    if (i < 0) return -1;
    msg_ring_t *ring = &c->msg_cache[i];

    uint64_t deadline = esp_timer_get_time() + (uint64_t)timeout_ms * 1000;
    do {
        const twai_message_t *msg = ring_latest(ring);
        if (msg) {
            *out_dlc = msg->data_length_code;
            memcpy(out_data, msg->data, *out_dlc);
            return 0;
        }
        if (timeout_ms > 0) vTaskDelay(1);
    } while (esp_timer_get_time() < deadline);
    return -1;
}

int can_send(can_t *c, uint32_t id, const uint8_t *data, uint8_t dlc)
{
    uint32_t now_us = (uint32_t)(esp_timer_get_time());
    if (!rate_limiter_allow(&c->rate_limiter, id, now_us)) {
        ESP_LOGD(TAG, "bus%d: rate-limited id=0x%03" PRIx32, c->bus_id, id);
        return -1;
    }

    if (c->sim_mode) {
        ESP_LOGI(TAG, "SIM bus%d tx id=0x%03" PRIx32 " dlc=%d", c->bus_id, id, dlc);
        return 0;
    }
    return can_bus_send(c->bus_id, id, data, dlc, 50);
}

void can_set_sim_mode(can_t *c, bool enabled)
{
    c->sim_mode = enabled;
    ESP_LOGI(TAG, "bus%d: simulation mode %s", c->bus_id, enabled ? "ON" : "OFF");
}
