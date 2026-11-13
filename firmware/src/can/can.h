#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/twai.h"
#include "can/rate_limit.h"

#define CAN_MSG_CACHE_MAX   64  /* number of CAN IDs that can be cached */
#define CAN_FRAME_CACHE    3  /* ring buffer depth per cache slot */

/* CAN-layer error codes */
#define CAN_ERR_FULL    (-202)
#define CAN_ERR_NO_FRAME (-203)

/* Ring buffer of twai_message_t at fixed depth (CAN_FRAME_CACHE).
 * frames[0].identifier == 0 means a cache slot is free.
 * last_us = esp_timer_get_time() at most recent push (microseconds). */
typedef struct {
    twai_message_t frames[CAN_FRAME_CACHE]; // mini ring buffer
    uint8_t        head;     /* next write index */
    uint8_t        count;    /* frames stored (0..CAN_FRAME_CACHE) */
    uint32_t       last_us;  /* μs timestamp of most recent push */
} msg_ring_t;

typedef struct {
    int                bus_id;
    rate_limiter_t     rate_limiter;
    bool               loaded;
    bool               sim_mode;       /* true = TX goes to log, not bus */

    /* Frame cache: per-ID ring buffers. Only caches IDs scripts have read. */
    msg_ring_t         msg_cache[CAN_MSG_CACHE_MAX];
} can_t;

int  can_init(can_t *c, int bus_id);
void can_free(can_t *c);
int  can_poll(can_t *c, uint32_t now_ms);

/* Raw frame observer — called for every frame received on any bus.
 * Used by the trace feature to forward frames to the UI. Single global slot. */
typedef void (*can_raw_observer_t)(int bus_id, const twai_message_t *frame, uint32_t now_ms);
void     can_set_raw_observer(can_raw_observer_t cb);
uint32_t can_last_rx_ms(int bus_id);  /* ms timestamp of last received frame, 0 if none */
uint16_t can_bus_rx_rate(int bus_id); /* RX frames/sec over last ~1 s, 0 when idle */

/* Non-blocking (timeout_ms == 0) or blocking read (default 1000 ms):
 * ensure msg_id slot exists, returns last frame data when available or -1. */
int  can_read(can_t *c, uint32_t msg_id, uint8_t *out_data, uint8_t *out_dlc, uint32_t timeout_ms);

/* Send a raw CAN frame. Rate-limited per ID. */
int  can_send(can_t *c, uint32_t id, const uint8_t *data, uint8_t dlc);

void can_set_sim_mode(can_t *c, bool enabled);

/* ---- RX acceptance filter (per bus) ----
 * Empty list = accept everything (default). With a list, can_poll() still
 * counts every frame for bus health / rx rate, but only listed IDs reach the
 * trace observer (BLE streaming) and the script frame cache. can_driver uses
 * the same list for the hardware filters: exact MCP2518FD filters (<= 32 IDs)
 * and a coarse TWAI acceptance mask. TX is never filtered. */
#define CAN_FILTER_MAX_IDS 64
int  can_filter_set(int bus_id, const uint32_t *ids, int n);   /* n = 0: accept all */
int  can_filter_get(int bus_id, uint32_t *ids, int max);        /* returns count */
bool can_filter_allows(int bus_id, uint32_t id);
/* Parse / format "118,257,3F5" (hex, no 0x needed). */
int  can_filter_parse_csv(const char *csv, uint32_t *ids, int max);
int  can_filter_format_csv(const uint32_t *ids, int n, char *out, int out_size);
