#include <string.h>
#include <stdio.h>
#include "wifi_manager.h"
#include "settings_manager.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "mdns.h"

static const char *TAG = "WIFI";

#define MAX_STA_RETRY   5

static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif = NULL;
static wifi_state_cb_t s_cb = NULL;
static bool s_sta_connected = false;
static bool s_ap_active = false;
static int s_retry_cnt = 0;
static char s_ip_str[20] = "0.0.0.0";
static char s_cur_ssid[33] = "";

static void notify(void)
{
    if (s_cb) {
        s_cb(s_sta_connected, s_ap_active, s_ip_str);
    }
}

static void start_mdns_once(void)
{
    static bool started = false;
    if (started) {
        return;
    }
    if (mdns_init() == ESP_OK) {
        mdns_hostname_set(CONFIG_PAPERRADIO_HOSTNAME);
        mdns_instance_name_set("PaperRadio Internet Radio");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
        started = true;
        ESP_LOGI(TAG, "mDNS started: http://%s.local", CONFIG_PAPERRADIO_HOSTNAME);
    }
}

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_cur_ssid[0] != '\0') {
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_sta_connected = false;
        if (s_retry_cnt < MAX_STA_RETRY && s_cur_ssid[0] != '\0') {
            s_retry_cnt++;
            ESP_LOGW(TAG, "disconnected, retry %d/%d", s_retry_cnt, MAX_STA_RETRY);
            esp_wifi_connect();
        } else {
            ESP_LOGW(TAG, "STA connection failed, enabling Setup SoftAP '%s'", CONFIG_PAPERRADIO_AP_SSID);
            wifi_manager_enable_ap();
        }
        notify();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&ev->ip_info.ip));
        s_sta_connected = true;
        s_retry_cnt = 0;
        ESP_LOGI(TAG, "connected, IP=%s", s_ip_str);
        start_mdns_once();
        notify();
    }
}

esp_err_t wifi_manager_init(wifi_state_cb_t cb)
{
    s_cb = cb;
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    esp_netif_set_hostname(s_sta_netif, CONFIG_PAPERRADIO_HOSTNAME);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    // Disable Wi-Fi power-save during audio streaming to minimize latency / packet bursts
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));
    return ESP_OK;
}

esp_err_t wifi_manager_enable_ap(void)
{
    wifi_config_t ap_cfg = { 0 };
    strlcpy((char *)ap_cfg.ap.ssid, CONFIG_PAPERRADIO_AP_SSID, sizeof(ap_cfg.ap.ssid));
    ap_cfg.ap.ssid_len = (uint8_t)strlen(CONFIG_PAPERRADIO_AP_SSID);
    ap_cfg.ap.channel = 1;
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.authmode = WIFI_AUTH_OPEN;

    esp_wifi_stop();
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    if (s_cur_ssid[0] != '\0') {
        wifi_config_t sta_cfg = { 0 };
        char pass[SETTINGS_PASS_LEN] = { 0 };
        settings_get_wifi(s_cur_ssid, pass);
        strlcpy((char *)sta_cfg.sta.ssid, s_cur_ssid, sizeof(sta_cfg.sta.ssid));
        strlcpy((char *)sta_cfg.sta.password, pass, sizeof(sta_cfg.sta.password));
        esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    }
    ESP_ERROR_CHECK(esp_wifi_start());
    s_ap_active = true;
    if (!s_sta_connected) {
        strlcpy(s_ip_str, "192.168.4.1", sizeof(s_ip_str));
    }
    ESP_LOGI(TAG, "SoftAP '%s' active at http://192.168.4.1", CONFIG_PAPERRADIO_AP_SSID);
    notify();
    return ESP_OK;
}

esp_err_t wifi_manager_start(void)
{
    char pass[SETTINGS_PASS_LEN] = { 0 };
    if (!settings_get_wifi(s_cur_ssid, pass)) {
        ESP_LOGW(TAG, "no Wi-Fi credentials saved, starting Setup SoftAP");
        return wifi_manager_enable_ap();
    }

    wifi_config_t sta_cfg = { 0 };
    strlcpy((char *)sta_cfg.sta.ssid, s_cur_ssid, sizeof(sta_cfg.sta.ssid));
    strlcpy((char *)sta_cfg.sta.password, pass, sizeof(sta_cfg.sta.password));
    sta_cfg.sta.threshold.authmode = (pass[0] == '\0') ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;

    s_retry_cnt = 0;
    s_ap_active = false;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "connecting to SSID '%s'...", s_cur_ssid);
    return ESP_OK;
}

esp_err_t wifi_manager_reconnect(void)
{
    char pass[SETTINGS_PASS_LEN] = { 0 };
    settings_get_wifi(s_cur_ssid, pass);
    if (s_cur_ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_config_t sta_cfg = { 0 };
    strlcpy((char *)sta_cfg.sta.ssid, s_cur_ssid, sizeof(sta_cfg.sta.ssid));
    strlcpy((char *)sta_cfg.sta.password, pass, sizeof(sta_cfg.sta.password));

    s_retry_cnt = 0;
    s_sta_connected = false;
    esp_wifi_disconnect();
    esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    return esp_wifi_connect();
}

bool wifi_manager_is_connected(void)
{
    return s_sta_connected;
}

bool wifi_manager_is_ap_active(void)
{
    return s_ap_active;
}

void wifi_manager_get_ip(char out_ip[20])
{
    strlcpy(out_ip, s_ip_str, 20);
}

void wifi_manager_get_ssid(char out_ssid[33])
{
    strlcpy(out_ssid, s_cur_ssid, 33);
}

int wifi_manager_get_rssi(void)
{
    if (!s_sta_connected) {
        return -100;
    }
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        return ap.rssi;
    }
    return -100;
}
