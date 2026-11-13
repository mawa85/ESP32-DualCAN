#include "wifi/wifi.h"
#include "storage/state.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

static const char *TAG = "wifi";

#define WIFI_NS                "wifi"
#define WIFI_KEY_SSID          "ssid"
#define WIFI_KEY_PSK           "psk"
#define WIFI_RETRY_BACKOFF_MS  5000

static esp_netif_t   *s_sta_netif;
static EventGroupHandle_t s_event_group;
#define BIT_CONNECTED (1 << 0)

static char s_current_ip[16];

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        bool was_connected = s_current_ip[0] != '\0';
        s_current_ip[0] = '\0';
        xEventGroupClearBits(s_event_group, BIT_CONNECTED);
        if (was_connected) {
            ESP_LOGI(TAG, "disconnected");
        }
        ESP_LOGD(TAG, "reconnecting attempt in %d ms", WIFI_RETRY_BACKOFF_MS);
        vTaskDelay(pdMS_TO_TICKS(WIFI_RETRY_BACKOFF_MS));
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        snprintf(s_current_ip, sizeof(s_current_ip), IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "got ip %s", s_current_ip);
        xEventGroupSetBits(s_event_group, BIT_CONNECTED);
    }
}

static int load_creds(char *ssid, size_t ssid_len, char *psk, size_t psk_len)
{
    if (state_get(WIFI_NS, WIFI_KEY_SSID, ssid, ssid_len) != ESP_OK) return -1;
    if (state_get(WIFI_NS, WIFI_KEY_PSK,  psk,  psk_len)  != ESP_OK) {
        psk[0] = '\0';   /* allow open networks */
    }
    if (ssid[0] == '\0') return -1;
    return 0;
}

static int apply_creds(const char *ssid, const char *psk)
{
    wifi_config_t cfg = {0};
    strncpy((char *)cfg.sta.ssid,     ssid, sizeof(cfg.sta.ssid)     - 1);
    strncpy((char *)cfg.sta.password, psk,  sizeof(cfg.sta.password) - 1);
    cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;   /* let the AP dictate */
    if (esp_wifi_set_config(WIFI_IF_STA, &cfg) != ESP_OK) return -1;
    return 0;
}

/* The WiFi driver (+ netif/LWIP, wifi task, static RX buffers) costs
 * roughly 40-60 KB of internal RAM that the BT controller needs at runtime.
 * It is only brought up when credentials exist (at boot) or when they are
 * set over BLE (wifi.set_creds). Without credentials WiFi stays off. */
static bool s_driver_up;

static esp_err_t wifi_driver_start(const char *ssid, const char *psk)
{
    if (s_driver_up) return ESP_OK;

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    if (!s_sta_netif) s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init);
    if (err != ESP_OK) { ESP_LOGE(TAG, "esp_wifi_init: %s", esp_err_to_name(err)); return err; }
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    if (apply_creds(ssid, psk) != 0) {
        ESP_LOGE(TAG, "apply_creds failed for %s", ssid);
    }
    err = esp_wifi_start();
    if (err != ESP_OK) { ESP_LOGE(TAG, "esp_wifi_start: %s", esp_err_to_name(err)); return err; }
    s_driver_up = true;
    ESP_LOGI(TAG, "connecting to %s", ssid);
    return ESP_OK;
}

void wifi_init(void)
{
    s_event_group = xEventGroupCreate();

    char ssid[33] = {0}, psk[65] = {0};
    if (load_creds(ssid, sizeof(ssid), psk, sizeof(psk)) == 0) {
        wifi_driver_start(ssid, psk);
    } else {
        ESP_LOGI(TAG, "no NVS credentials; WiFi off (starts on wifi.set_creds)");
    }
}

bool wifi_driver_running(void)
{
    return s_driver_up;
}

bool wifi_connected(void)
{
    return s_event_group &&
           (xEventGroupGetBits(s_event_group) & BIT_CONNECTED) != 0;
}

void wifi_get_ip(char *buf, size_t buf_len)
{
    if (!buf || buf_len == 0) return;
    strncpy(buf, s_current_ip, buf_len - 1);
    buf[buf_len - 1] = '\0';
}

void wifi_get_ssid(char *buf, size_t buf_len)
{
    if (!buf || buf_len == 0) return;
    if (state_get(WIFI_NS, WIFI_KEY_SSID, buf, buf_len) != ESP_OK) {
        buf[0] = '\0';
    }
}

int wifi_set_creds(const char *ssid, const char *psk)
{
    if (!psk) psk = "";

    /* Empty SSID → forget network */
    if (!ssid || ssid[0] == '\0') {
        state_remove(WIFI_NS, WIFI_KEY_SSID);
        state_remove(WIFI_NS, WIFI_KEY_PSK);
        if (!s_driver_up) {
            ESP_LOGW(TAG, "credentials cleared (WiFi was off)");
            return 0;
        }
        /* Driver stays initialised until reboot; then WiFi stays off. */
        esp_wifi_disconnect();
        /* Apply an empty config so the driver doesn't auto-connect */
        wifi_config_t empty = {0};
        esp_wifi_set_config(WIFI_IF_STA, &empty);
        s_current_ip[0] = '\0';
        xEventGroupClearBits(s_event_group, BIT_CONNECTED);
        ESP_LOGW(TAG, "credentials cleared — network forgotten");
        return 0;
    }

    if (state_set(WIFI_NS, WIFI_KEY_SSID, ssid) != ESP_OK) return -1;
    if (state_set(WIFI_NS, WIFI_KEY_PSK,  psk)  != ESP_OK) return -1;

    if (!s_driver_up) {
        return wifi_driver_start(ssid, psk) == ESP_OK ? 0 : -1;
    }

    /* Bring the link down, push new config, bring it back up. */
    esp_wifi_disconnect();
    if (apply_creds(ssid, psk) != 0) return -1;
    esp_wifi_connect();
    ESP_LOGI(TAG, "credentials updated; reconnecting to %s", ssid);
    return 0;
}
