/*
 * BLE GATT server for the Dorky Commander web UI transport.
 *
 * Custom service with two characteristics:
 *   - RX (write): client sends requests (CBOR-encoded commands).
 *   - TX (notify): server sends responses and event notifications.
 *
 * Uses NimBLE (the only BLE stack available on ESP32-C6).
 *
 * Pairing security:
 *   - Locked by default; only `s_conn_authorized` sessions can write GATT.
 *     A connect is authorized when the pairing window is open OR the peer
 *     is BLE-bonded (peer_is_bonded checks the NVS bond store).
 *   - Unauthorized connections are silently terminated after 100 ms.
 *   - dorky_ble_unlock_pairing() opens a 60 s window (BOOT button / UI),
 *     during which any device may connect and is offered BLE pairing via
 *     ble_gap_security_initiate (needed for Android to prompt the user).
 *   - sm_bonding follows the window so unauthorized devices can never bond.
 *   - On bond establishment (ENC_CHANGE success) the window closes early.
 */
#include "ble/ble_transport.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "nimble/nimble_npl.h"
#include "debug/dorky_debug.h"

static const char *TAG = "ble";

/* Custom service UUID: randomly generated, stable.
 * 6e400001-b5a3-f393-e0a9-e50e24dcca9e  (Nordic UART Service convention) */
static const ble_uuid128_t DORKY_SVC_UUID =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                     0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e);

/* RX characteristic: client writes commands here. */
static const ble_uuid128_t DORKY_RX_UUID =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                     0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e);

/* TX characteristic: server sends notifications here. */
static const ble_uuid128_t DORKY_TX_UUID =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                     0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x00, 0x40, 0x6e);

static uint16_t s_conn_handle   = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_reject_handle = BLE_HS_CONN_HANDLE_NONE;
/* Reason code used by the reject callout; default is auth fail. */
static int s_reject_reason = BLE_ERR_AUTH_FAIL;
static uint16_t s_tx_attr_handle;
/* Set once the central has enabled notifications on the TX characteristic
 * (CCCD write, or restored for a bonded peer). Before that, notifies would
 * only compete with service discovery for the few ACL/mbuf buffers. */
static volatile bool s_tx_subscribed;
static volatile uint32_t s_ntf_ok, s_ntf_enomem, s_ntf_err, s_ntf_tx_events;
static uint32_t s_ntf_warn_ms;

/* Fallback: some centrals (Windows with a cached CCCD on a bonded device)
 * never rewrite the CCCD on reconnect. After this grace period notifies are
 * allowed anyway; by then service discovery is normally done. */
#define TX_SUBSCRIBE_GRACE_MS 2500
static volatile uint32_t s_connect_ms;

static bool tx_allowed(void)
{
    if (s_tx_subscribed) return true;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    return (now - s_connect_ms) >= TX_SUBSCRIBE_GRACE_MS;
}
static ble_request_cb_t s_on_request;
static void *s_on_request_ctx;
static bool s_pairing_open    = false;
static bool s_conn_authorized = false;

/* Peer we most recently kicked in favour of another client. Clients that
 * auto-reconnect would otherwise kick each other out in a loop, so refuse the
 * kicked peer for KICK_COOLDOWN_MS (unless the pairing window is open). */
#define KICK_COOLDOWN_MS 30000
static ble_addr_t s_kicked_addr;
static int64_t   s_kicked_until_ms = 0;

/* Peers for which peripheral-initiated SMP recently failed (or the link
 * dropped mid-pairing). Windows/Chrome Web Bluetooth often does not
 * complete Just Works pairing; calling ble_gap_security_initiate again
 * on the next connect just repeats a ~30 s SMP timeout and drops the
 * authorized session in a loop. Skip initiate for this long. */
#define SMP_FAIL_COOLDOWN_MS 600000
static ble_addr_t s_smp_fail_addr;
static int64_t    s_smp_fail_until_ms;
static bool       s_smp_inflight;


static struct ble_npl_callout s_adv_callout;
static struct ble_npl_callout s_reject_callout;
static struct ble_npl_callout s_lock_callout;
static struct ble_npl_callout s_adv_watchdog;

/* ---- Bond helpers ---- */

static bool peer_is_bonded(uint16_t conn_handle)
{
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(conn_handle, &desc) != 0) return false;
    struct ble_store_key_sec key;
    memset(&key, 0, sizeof key);
    key.peer_addr = desc.peer_id_addr;
    struct ble_store_value_sec val;
    return ble_store_read_peer_sec(&key, &val) == 0;
}

/* ---- Callout callbacks ---- */

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static const char *ble_hs_reason_str(int reason)
{
    /* NimBLE reports HCI errors as 0x200 + HCI status. Accept both forms. */
    int hci = (reason >= 0x200 && reason < 0x300) ? (reason - 0x200) : reason;
    switch (hci) {
    case 0x05: return "auth failure";
    case 0x08: return "supervision timeout";
    case 0x13: return "remote user terminated";
    case 0x15: return "remote device terminated due to power off";
    case 0x16: return "local host terminated";
    case 0x1f: return "unspecified";
    case 0x22: return "LMP/LL response timeout";
    case 0x3d: return "MIC failure";
    case 0x3e: return "failed to establish";
    default:   return "other";
    }
}

static void log_conn_params(const char *what, uint16_t conn_handle)
{
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(conn_handle, &desc) != 0) return;
    /* interval unit 1.25 ms, supervision timeout unit 10 ms, latency in events */
    unsigned itvl_x100 = (unsigned)desc.conn_itvl * 125u; /* hundredths of a ms */
    unsigned tmo_ms    = (unsigned)desc.supervision_timeout * 10u;
    ESP_LOGI(TAG, "%s handle=%d itvl=%u.%02ums latency=%u timeout=%ums peer=%02x:%02x:%02x:%02x:%02x:%02x",
             what, conn_handle, itvl_x100 / 100, itvl_x100 % 100,
             (unsigned)desc.conn_latency, tmo_ms,
             desc.peer_id_addr.val[5], desc.peer_id_addr.val[4],
             desc.peer_id_addr.val[3], desc.peer_id_addr.val[2],
             desc.peer_id_addr.val[1], desc.peer_id_addr.val[0]);
}

static void remember_smp_fail_addr(const ble_addr_t *addr, const char *why)
{
    if (!addr) return;
    s_smp_fail_addr = *addr;
    s_smp_fail_until_ms = now_ms() + SMP_FAIL_COOLDOWN_MS;
    s_smp_inflight = false;
    ESP_LOGW(TAG, "SMP failed (%s) — will skip security_initiate for this peer for %d s",
             why, SMP_FAIL_COOLDOWN_MS / 1000);
}

static void remember_smp_fail_handle(uint16_t conn_handle, const char *why)
{
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(conn_handle, &desc) == 0)
        remember_smp_fail_addr(&desc.peer_id_addr, why);
    else {
        s_smp_inflight = false;
        ESP_LOGW(TAG, "SMP failed (%s) handle=%d (no conn desc)", why, conn_handle);
    }
}

static bool peer_smp_recently_failed(uint16_t conn_handle)
{
    if (now_ms() >= s_smp_fail_until_ms) return false;
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(conn_handle, &desc) != 0) return false;
    return ble_addr_cmp(&desc.peer_id_addr, &s_smp_fail_addr) == 0;
}

static void close_pairing_window(const char *why)
{
    s_pairing_open = false;
    ble_hs_cfg.sm_bonding = 0;
    ble_npl_callout_stop(&s_lock_callout);
    ESP_LOGI(TAG, "pairing window closed (%s)", why ? why : "");
}


static bool peer_in_kick_cooldown(uint16_t conn_handle)
{
    if (now_ms() >= s_kicked_until_ms) return false;
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(conn_handle, &desc) != 0) return false;
    return ble_addr_cmp(&desc.peer_id_addr, &s_kicked_addr) == 0;
}

static void reject_callout_fn(struct ble_npl_event *ev)
{
    uint16_t h = s_reject_handle;
    s_reject_handle = BLE_HS_CONN_HANDLE_NONE;
    if (h == BLE_HS_CONN_HANDLE_NONE) return;
    ESP_LOGI(TAG, "rejecting unauthorized handle=%d reason=%d", h, s_reject_reason);
    ble_gap_terminate(h, s_reject_reason);
    /* Reset to default after use */
    s_reject_reason = BLE_ERR_AUTH_FAIL;
}

static void lock_callout_fn(struct ble_npl_event *ev)
{
    (void)ev;
    close_pairing_window("60 s timer");
}

static int gap_event_cb(struct ble_gap_event *event, void *arg);
static void start_advertising(void);
static void open_pairing_window(bool with_timeout);

/* Periodic safety net: if no client is connected and advertising has stopped
 * for any reason (failed start, controller stall, race condition), restart it.
 * Critical for an in-car device where the BOOT button isn't accessible.
 * If we can't recover for 6 consecutive ticks (60 s), reboot the device. */
static void adv_watchdog_fn(struct ble_npl_event *ev)
{
    static uint8_t stuck_ticks = 0;
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE && !ble_gap_adv_active()) {
        ESP_LOGW(TAG, "watchdog: advertising stopped while idle, restarting (tick %u)",
                 (unsigned)stuck_ticks);
        start_advertising();
        if (++stuck_ticks >= 6) {
            ESP_LOGE(TAG, "watchdog: BLE unrecoverable for 60s, rebooting");
            esp_restart();
        }
    } else {
        stuck_ticks = 0;
    }
    ble_npl_callout_reset(&s_adv_watchdog, ble_npl_time_ms_to_ticks32(10000));
}

static void adv_callout_fn(struct ble_npl_event *ev) { start_advertising(); }

static void schedule_advertising(void)
{
    ble_npl_callout_reset(&s_adv_callout, ble_npl_time_ms_to_ticks32(200));
}

/* ---- Service definition ---- */

static int gatt_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        /* Drop writes from any connection that's not the active authorized session. */
        if (conn_handle != s_conn_handle || !s_conn_authorized) return 0;

        struct os_mbuf *om = ctxt->om;
        uint16_t len = OS_MBUF_PKTLEN(om);
        uint8_t buf[512];
        if (len > sizeof(buf)) len = sizeof(buf);
        os_mbuf_copydata(om, 0, len, buf);

        if (s_on_request) {
            s_on_request(buf, len, s_on_request_ctx);
        }
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &DORKY_SVC_UUID.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = &DORKY_RX_UUID.u,
                .access_cb = gatt_access_cb,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid = &DORKY_TX_UUID.u,
                .access_cb = gatt_access_cb,
                .val_handle = &s_tx_attr_handle,
                .flags = BLE_GATT_CHR_F_NOTIFY,
            },
            { 0 },
        },
    },
    { 0 },
};

/* ---- GAP event handler ---- */

static void start_advertising(void)
{
    struct ble_gap_adv_params adv_params = {0};
    struct ble_hs_adv_fields fields = {0};

    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    fields.name = (uint8_t *)ble_svc_gap_device_name();
    fields.name_len = strlen(ble_svc_gap_device_name());
    fields.name_is_complete = 1;
    ble_gap_adv_set_fields(&fields);

    /* Also advertise the service UUID in scan response. */
    struct ble_hs_adv_fields rsp_fields = {0};
    rsp_fields.uuids128 = (ble_uuid128_t[]){ DORKY_SVC_UUID };
    rsp_fields.num_uuids128 = 1;
    rsp_fields.uuids128_is_complete = 1;
    ble_gap_adv_rsp_set_fields(&rsp_fields);

    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    int rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                               &adv_params, gap_event_cb, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGI(TAG, "adv start failed: %d — retrying", rc);
        if (rc == BLE_HS_ENOMEM || rc == BLE_HS_EBUSY) {
            schedule_advertising();
        }
    }
}

static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            uint16_t new_h = event->connect.conn_handle;
            ble_att_set_preferred_mtu(512);
            bool authorized = s_pairing_open || peer_is_bonded(new_h);
            if (authorized && !s_pairing_open &&
                s_conn_handle != BLE_HS_CONN_HANDLE_NONE &&
                peer_in_kick_cooldown(new_h)) {
                ESP_LOGI(TAG, "handle=%d reconnected right after being kicked — rejecting",
                         new_h);
                authorized = false;
            }

            if (!authorized && s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
                /* Unauthorized new device — reject it, keep the existing session. */
                ESP_LOGI(TAG, "connected (handle=%d) — unauthorized, rejecting without kick",
                         new_h);
                s_reject_handle = new_h;
                ble_npl_callout_reset(&s_reject_callout, ble_npl_time_ms_to_ticks32(100));
            } else {
                if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
                    ESP_LOGI(TAG, "new authorized connection, kicking handle=%d", s_conn_handle);
                    struct ble_gap_conn_desc old_desc;
                    if (ble_gap_conn_find(s_conn_handle, &old_desc) == 0) {
                        s_kicked_addr     = old_desc.peer_id_addr;
                        s_kicked_until_ms = now_ms() + KICK_COOLDOWN_MS;
                    }
                    uint8_t msg[] = {0xFD, 0x01};
                    dorky_ble_notify(msg, sizeof(msg));
                    /* Schedule a short delayed terminate so notify can flush. */
                    s_reject_handle = s_conn_handle;
                    s_reject_reason = BLE_ERR_REM_USER_CONN_TERM;
                    ble_npl_callout_reset(&s_reject_callout, ble_npl_time_ms_to_ticks32(100));
                }
                s_conn_handle = new_h;
                s_tx_subscribed = false;
                s_connect_ms = (uint32_t)(esp_timer_get_time() / 1000);
                if (!authorized) {
                    ESP_LOGI(TAG, "connected (handle=%d) — not bonded, rejecting in 2.5s", new_h);
                    s_conn_authorized = false;
                    s_reject_handle = new_h;
                    ble_npl_callout_reset(&s_reject_callout, ble_npl_time_ms_to_ticks32(100));
                } else {
                    ESP_LOGI(TAG, "connected (handle=%d) — authorized (bonded=%d pairing_open=%d)",
                             new_h, peer_is_bonded(new_h), s_pairing_open);
                    s_conn_authorized = true;
                    log_conn_params("conn params", new_h);
                    /* Clip the transport's own GAP traces while a client is
                     * connected so they don't flood the log hook / notify
                     * queue — unless the persistent debug flag is on. */
                    if (!dorky_debug_enabled())
                        esp_log_level_set("ble", ESP_LOG_WARN);
                    if (s_pairing_open) {
                        /* Peripheral-initiated SMP triggers the OS pairing
                         * dialog on Android. Windows/Chrome Web Bluetooth
                         * often does not complete Just Works pairing and the
                         * 30 s SMP timeout drops an otherwise authorized
                         * session. Do not close the pairing window until a
                         * bond is actually stored, and skip re-initiate for
                         * a peer that recently failed SMP. */
                        if (peer_smp_recently_failed(new_h)) {
                            ESP_LOGI(TAG, "skipping security_initiate handle=%d (recent SMP failure)",
                                     new_h);
                        } else {
                            int rc = ble_gap_security_initiate(new_h);
                            ESP_LOGI(TAG, "security_initiate handle=%d rc=%d", new_h, rc);
                            if (rc == 0) {
                                s_smp_inflight = true;
                            } else if (rc != BLE_HS_EALREADY) {
                                remember_smp_fail_handle(new_h, "security_initiate rc");
                            }
                        }
                    }
                }
            }

            /* Keep advertising while connected so a second device can find
             * us and (if authorized) kick the first — needed for the "swap
             * PC for phone" flow. Controller may refuse with ENOMEM; logged
             * once, no retry loop (see start_advertising). */
            schedule_advertising();
        } else {
            ESP_LOGW(TAG, "connect failed status=%d (0x%02x %s)",
                     event->connect.status, event->connect.status,
                     ble_hs_reason_str(event->connect.status));
            if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE)
                schedule_advertising();
        }
        break;

    case BLE_GAP_EVENT_ENC_CHANGE: {
        uint16_t h = event->enc_change.conn_handle;
        if (h != s_conn_handle) break;
        bool bonded = peer_is_bonded(h);
        ESP_LOGI(TAG, "enc_change handle=%d status=%d (0x%02x %s) bonded=%d",
                 h, event->enc_change.status, event->enc_change.status,
                 ble_hs_reason_str(event->enc_change.status), bonded);
        s_smp_inflight = false;
        if (event->enc_change.status == 0 && bonded) {
            if (s_pairing_open)
                close_pairing_window("bond established");
        } else if (event->enc_change.status != 0) {
            remember_smp_fail_handle(h, "enc_change");
        }
        break;
    }

    case BLE_GAP_EVENT_DISCONNECT: {
        uint16_t h = event->disconnect.conn.conn_handle;
        int reason = event->disconnect.reason;
        /* WARN so this stays visible even when the "ble" tag is clipped to
         * WARN during an authorized session (otherwise the reconnect loop
         * hides its own cause). */
        ESP_LOGW(TAG, "disconnected handle=%d reason=%d (0x%03x %s)",
                 h, reason, reason, ble_hs_reason_str(reason));
        if (h == s_reject_handle) {
            ble_npl_callout_stop(&s_reject_callout);
            s_reject_handle = BLE_HS_CONN_HANDLE_NONE;
        }
        if (h == s_conn_handle) {
            if (s_smp_inflight)
                remember_smp_fail_addr(&event->disconnect.conn.peer_id_addr,
                                       "disconnect mid-pairing");
            s_smp_inflight = false;
            s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            s_tx_subscribed = false;
            s_conn_authorized = false;
            /* If the connection ended without producing a bond and we still
             * have zero bonds, re-open the window so the user can retry
             * (e.g. after wiping the stale OS bond that caused a 517). */
            if (!s_pairing_open && dorky_ble_bond_count() == 0) {
                open_pairing_window(false);
            }
            /* Defer 200 ms so HCI finishes processing the disconnect — calling
             * adv_start synchronously here often returns ENOMEM. */
            schedule_advertising();
        }
        break;
    }

    case BLE_GAP_EVENT_ADV_COMPLETE:
        start_advertising();
        break;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU updated: handle=%d mtu=%d",
                 event->mtu.conn_handle, event->mtu.value);
        break;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.conn_handle == s_conn_handle &&
            event->subscribe.attr_handle == s_tx_attr_handle) {
            s_tx_subscribed = event->subscribe.cur_notify != 0;
        }
        ESP_LOGI(TAG, "subscribe: handle=%d attr=%d notify=%d indicate=%d",
                 event->subscribe.conn_handle,
                 event->subscribe.attr_handle,
                 event->subscribe.cur_notify,
                 event->subscribe.cur_indicate);
        break;

    case BLE_GAP_EVENT_CONN_UPDATE:
        ESP_LOGI(TAG, "conn_update handle=%d status=%d",
                 event->conn_update.conn_handle, event->conn_update.status);
        if (event->conn_update.status == 0)
            log_conn_params("conn params (updated)", event->conn_update.conn_handle);
        break;

    /* ---- SMP diagnostics ---- */
    case BLE_GAP_EVENT_PASSKEY_ACTION:
        ESP_LOGI(TAG, "passkey_action: conn=%d action=%d",
                 event->passkey.conn_handle, event->passkey.params.action);
        break;

    case BLE_GAP_EVENT_PARING_COMPLETE:   /* typo is upstream NimBLE */
        ESP_LOGI(TAG, "pairing_complete: conn=%d status=%d (0x%02x %s)",
                 event->pairing_complete.conn_handle,
                 event->pairing_complete.status,
                 event->pairing_complete.status,
                 ble_hs_reason_str(event->pairing_complete.status));
        if (event->pairing_complete.status != 0)
            remember_smp_fail_handle(event->pairing_complete.conn_handle, "pairing_complete");
        break;

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        ESP_LOGI(TAG, "repeat_pairing: conn=%d — accepting",
                 event->repeat_pairing.conn_handle);
        return BLE_GAP_REPEAT_PAIRING_RETRY;

    case BLE_GAP_EVENT_NOTIFY_TX:
        /* One per notify: count only (was a per-notify debug line). */
        s_ntf_tx_events++;
        break;

    default:
        ESP_LOGD(TAG, "gap event type=%d", event->type);
        break;
    }
    return 0;
}

/* ---- NimBLE host sync callback ---- */

static void on_sync(void)
{
    uint8_t own_addr_type;
    ble_hs_id_infer_auto(0, &own_addr_type);

    /* Suffix with the 2 device-unique MAC bytes (NimBLE returns addr
     * little-endian, so addr[0..1] are the low end — not Espressif's OUI). */
    uint8_t addr[6];
    if (ble_hs_id_copy_addr(own_addr_type, addr, NULL) == 0) {
        char name[16];
        snprintf(name, sizeof(name), "Dorky-%02X%02X", addr[1], addr[0]);
        ble_svc_gap_device_name_set(name);
    }

    int bond_count = 0;
    ble_store_util_count(BLE_STORE_OBJ_TYPE_PEER_SEC, &bond_count);
    if (bond_count == 0) {
        /* No bonds: hold the window open with no timer. Otherwise an in-car
         * device with no BOOT-button access becomes unreachable after 60 s. */
        open_pairing_window(false);
    }

    start_advertising();
    ble_npl_callout_reset(&s_adv_watchdog, ble_npl_time_ms_to_ticks32(10000));
    ESP_LOGI(TAG, "BLE ready as \"%s\" — pairing %s (%d bond(s))",
             ble_svc_gap_device_name(), s_pairing_open ? "OPEN" : "locked", bond_count);
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "BLE host reset (reason=%d)", reason);
}

static void nimble_host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* ---- Public API ---- */

/* Implemented by nimble/host/store/config — registers the config store as
 * NimBLE's ble_hs_cfg.store_* callbacks. With CONFIG_BT_NIMBLE_NVS_PERSIST=y
 * the config store transparently persists bonds to NVS. Without this call,
 * NimBLE has no store at all and SMP keys live only in RAM for the session. */
extern void ble_store_config_init(void);

int dorky_ble_init(ble_request_cb_t on_request, void *ctx)
{
    s_on_request = on_request;
    s_on_request_ctx = ctx;

    int rc = nimble_port_init();
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %d", rc);
        return -1;
    }

    ble_store_config_init();

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb  = on_sync;

    /* Security manager: Just Works, bonding toggled by pairing window. */
    ble_hs_cfg.sm_io_cap        = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_sc            = 1;
    ble_hs_cfg.sm_bonding       = 0;   /* flipped to 1 by dorky_ble_unlock_pairing() */
    ble_hs_cfg.sm_our_key_dist   = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_init();
    ble_svc_gatt_init();

    rc = ble_gatts_count_cfg(gatt_svcs);
    if (rc != 0) { ESP_LOGE(TAG, "gatts_count_cfg: %d", rc); return -1; }
    rc = ble_gatts_add_svcs(gatt_svcs);
    if (rc != 0) { ESP_LOGE(TAG, "gatts_add_svcs: %d", rc); return -1; }

    /* Name is finalized in on_sync() once the BLE MAC is readable so it
     * can be suffixed with the device id (e.g. "Dorky-A3F1"). */
    ble_svc_gap_device_name_set("Dorky");

    ble_npl_callout_init(&s_adv_callout,    nimble_port_get_dflt_eventq(), adv_callout_fn,    NULL);
    ble_npl_callout_init(&s_reject_callout, nimble_port_get_dflt_eventq(), reject_callout_fn, NULL);
    ble_npl_callout_init(&s_lock_callout,   nimble_port_get_dflt_eventq(), lock_callout_fn,   NULL);
    ble_npl_callout_init(&s_adv_watchdog,   nimble_port_get_dflt_eventq(), adv_watchdog_fn,   NULL);

    nimble_port_freertos_init(nimble_host_task);

    ESP_LOGI(TAG, "BLE transport initialized");
    return 0;
}

/* ---- notify statistics + back-pressure ----
 * Never log per notify: at a few hundred notifies/s the log line itself
 * (USB console + BLE log mirror) becomes the load. Count instead and let
 * dorky_ble_tx_stats() / the heartbeat print the totals. */

int dorky_ble_notify(const uint8_t *data, size_t len)
{
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) return -1;
    if (!tx_allowed()) return -1;

    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
    if (!om) {
        s_ntf_enomem++;
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        if (dorky_debug_enabled() && (now - s_ntf_warn_ms) >= 5000) {
            s_ntf_warn_ms = now;
            ESP_LOGW(TAG, "notify ENOMEM (mbuf) x%" PRIu32 " total, msys free=%d",
                     s_ntf_enomem, os_msys_num_free());
        }
        return BLE_HS_ENOMEM;
    }

    /* ble_gatts_notify_custom consumes om on success and on failure. */
    int rc = ble_gatts_notify_custom(s_conn_handle, s_tx_attr_handle, om);
    if (rc == 0) s_ntf_ok++;
    else         s_ntf_err++;
    return rc;
}

bool dorky_ble_tx_credit(int chunks)
{
    if (!dorky_ble_tx_ready()) return false;
    /* Status/control pushes: just enough host buffers for this frame plus a
     * tiny reserve for ATT responses. No heap check (see header). */
    return os_msys_num_free() >= chunks + DORKY_BLE_MSYS_STATUS_RESERVE;
}

static uint32_t s_trace_iram_floor = DORKY_BLE_TRACE_IRAM_FLOOR;
static uint32_t s_trace_iram_big_floor = DORKY_BLE_TRACE_IRAM_BIG_FLOOR;
static bool     s_trace_gate_open = true;
static uint32_t s_trace_gate_closed;

void dorky_ble_trace_gate_set(uint32_t iram_floor, uint32_t iram_big_floor)
{
    if (iram_floor) s_trace_iram_floor = iram_floor;
    if (iram_big_floor) s_trace_iram_big_floor = iram_big_floor;
    s_trace_gate_open = true;     /* re-evaluated on the next call */
}

bool dorky_ble_trace_credit(void)
{
    if (!dorky_ble_tx_ready()) return false;
    /* One trace notify is <= 509 B = at most 2 msys blocks; keep a reserve
     * for ATT responses / SMP / L2CAP on top. */
    if (os_msys_num_free() < 2 + DORKY_BLE_MSYS_RESERVE) return false;

    size_t free_b = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t big_b  = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (s_trace_gate_open) {
        if (free_b < s_trace_iram_floor || big_b < s_trace_iram_big_floor) {
            s_trace_gate_open = false;
            s_trace_gate_closed++;
        }
    } else {
        /* Hysteresis: reopen only with clear headroom. */
        if (free_b >= s_trace_iram_floor + DORKY_BLE_TRACE_IRAM_HYST &&
            big_b >= s_trace_iram_big_floor)
            s_trace_gate_open = true;
    }
    return s_trace_gate_open;
}

void dorky_ble_tx_stats(dorky_ble_tx_stats_t *out)
{
    if (!out) return;
    out->ok = s_ntf_ok;
    out->enomem = s_ntf_enomem;
    out->err = s_ntf_err;
    out->tx_events = s_ntf_tx_events;
    out->msys_free = os_msys_num_free();
    out->gate_open = s_trace_gate_open;
    out->gate_closed = s_trace_gate_closed;
    out->iram_floor = s_trace_iram_floor;
    out->iram_big_floor = s_trace_iram_big_floor;
}

bool dorky_ble_tx_ready(void)
{
    return s_conn_handle != BLE_HS_CONN_HANDLE_NONE && tx_allowed();
}

int dorky_ble_free_bufs(void)
{
    return os_msys_num_free();
}

bool dorky_ble_connected(void)
{
    return s_conn_handle != BLE_HS_CONN_HANDLE_NONE;
}

bool dorky_ble_authorized(void)
{
    return s_conn_authorized && s_conn_handle != BLE_HS_CONN_HANDLE_NONE;
}

void dorky_ble_debug_snapshot(char *buf, size_t len)
{
    if (!buf || len == 0) return;
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        snprintf(buf, len, "ble=idle pairing=%s bonds=%d",
                 s_pairing_open ? "open" : "locked", dorky_ble_bond_count());
        return;
    }
    snprintf(buf, len, "ble=conn h=%d auth=%d mtu=%u pairing=%s bonds=%d smp=%s",
             s_conn_handle, s_conn_authorized, (unsigned)ble_att_mtu(s_conn_handle),
             s_pairing_open ? "open" : "locked", dorky_ble_bond_count(),
             s_smp_inflight ? "inflight" : "idle");
}

uint16_t dorky_ble_mtu(void)
{
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) return 0;
    return ble_att_mtu(s_conn_handle);
}

bool dorky_ble_pairing_open(void)
{
    return s_pairing_open;
}

static void open_pairing_window(bool with_timeout)
{
    s_pairing_open = true;
    ble_hs_cfg.sm_bonding = 1;
    if (with_timeout) {
        ble_npl_callout_reset(&s_lock_callout, ble_npl_time_ms_to_ticks32(60000));
        ESP_LOGI(TAG, "pairing window open for 60 s");
    } else {
        ble_npl_callout_stop(&s_lock_callout);
        ESP_LOGI(TAG, "pairing window OPEN (held until first bond)");
    }
}

void dorky_ble_unlock_pairing(void)
{
    open_pairing_window(true);
}

int dorky_ble_bond_count(void)
{
    int n = 0;
    ble_store_util_count(BLE_STORE_OBJ_TYPE_PEER_SEC, &n);
    return n;
}

int dorky_ble_reset_pairings(void)
{
    int rc = ble_store_clear();
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_store_clear failed: %d", rc);
        return rc;
    }
    ESP_LOGI(TAG, "all bonds wiped");
    /* Terminate the active client so it has to reconnect and re-pair. */
    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
    /* Same reasoning as on_sync: with zero bonds the only way back in is the
     * pairing window, so leave it open until something pairs. */
    open_pairing_window(false);
    return 0;
}
