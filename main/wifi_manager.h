#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <stdbool.h>
#include "esp_err.h"

typedef void (*wifi_state_cb_t)(bool sta_connected, bool ap_active, const char *ip_str);

esp_err_t wifi_manager_init(wifi_state_cb_t cb);

/** Attempts STA connection with credentials from settings; falls back to SoftAP if none. */
esp_err_t wifi_manager_start(void);

/** Reconnects STA using updated credentials stored in settings_manager. */
esp_err_t wifi_manager_reconnect(void);

/** Enables the setup SoftAP (192.168.4.1) alongside STA. */
esp_err_t wifi_manager_enable_ap(void);

bool wifi_manager_is_connected(void);
bool wifi_manager_is_ap_active(void);
void wifi_manager_get_ip(char out_ip[20]);
void wifi_manager_get_ssid(char out_ssid[33]);
int wifi_manager_get_rssi(void);

#endif // WIFI_MANAGER_H
