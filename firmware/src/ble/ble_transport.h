#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* Callback for incoming requests from the BLE client (web UI). */
typedef void (*ble_request_cb_t)(const uint8_t *data, size_t len, void *ctx);

/* Initialize the BLE GATT server. Starts advertising as "Dorky".
 * `on_request` is called when the client writes to the RX characteristic. */
int dorky_ble_init(ble_request_cb_t on_request, void *ctx);

/* Send a notification to the connected client via the TX characteristic.
 * Silently drops if no client is connected. */
int dorky_ble_notify(const uint8_t *data, size_t len);

/* Returns true if a client is currently connected. */
bool dorky_ble_connected(void);
/* Connected AND the central enabled notifications on TX (or the 2.5 s
 * post-connect grace period has passed). */
bool dorky_ble_tx_ready(void);
/* Free NimBLE msys buffers (rough back-pressure signal for droppable pushes). */
int dorky_ble_free_bufs(void);

/* Optional traffic (trace, status, log mirror) may send `chunks` notifies
 * now: connected+subscribed, enough msys buffers left above a reserve, and
 * enough internal RAM left for the BT controller. */
/* Two send gates:
 *  - dorky_ble_tx_credit(chunks): control/status pushes (bus_status,
 *    script status, logs, berry print). Only needs host buffers for the
 *    frame plus a tiny reserve; never looks at heap so low-rate status
 *    keeps flowing even when internal RAM is tight.
 *  - dorky_ble_trace_credit(): bulk trace. Needs a larger msys reserve AND
 *    an internal-RAM floor (free + largest block) with hysteresis, because
 *    the BT controller allocates from internal RAM at runtime. */
#define DORKY_BLE_MSYS_STATUS_RESERVE   2
#define DORKY_BLE_MSYS_RESERVE          8
#define DORKY_BLE_TRACE_IRAM_FLOOR      (10 * 1024)
#define DORKY_BLE_TRACE_IRAM_BIG_FLOOR  (4 * 1024)
#define DORKY_BLE_TRACE_IRAM_HYST       (4 * 1024)
bool dorky_ble_tx_credit(int chunks);
bool dorky_ble_trace_credit(void);
/* Runtime override of the trace floors (bytes); 0 keeps the current value. */
void dorky_ble_trace_gate_set(uint32_t iram_floor, uint32_t iram_big_floor);

typedef struct {
    uint32_t ok, enomem, err, tx_events;
    int msys_free;
    bool     gate_open;          /* trace gate state */
    uint32_t gate_closed;        /* times the trace gate closed */
    uint32_t iram_floor, iram_big_floor;
} dorky_ble_tx_stats_t;
void dorky_ble_tx_stats(dorky_ble_tx_stats_t *out);

/* Returns the negotiated ATT MTU for the active connection, or 0 if disconnected. */
uint16_t dorky_ble_mtu(void);

/* Returns true while the pairing window is open (new, unbonded devices accepted). */
bool dorky_ble_pairing_open(void);

/* Open the pairing window for 60 s. Safe to call from any task. */
void dorky_ble_unlock_pairing(void);

/* Number of bonded peers currently stored in NVS. */
int dorky_ble_bond_count(void);

/* Wipe all stored bonds. Disconnects the active client (if any) so the next
 * connection has to re-pair from scratch. Returns 0 on success. */
int dorky_ble_reset_pairings(void);

/* Compact BLE status for the debug heartbeat. Always safe to call. */
void dorky_ble_debug_snapshot(char *buf, size_t len);

/* True while the active connection is authorized to write GATT. */
bool dorky_ble_authorized(void);

