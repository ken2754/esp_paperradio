#ifndef SETTINGS_MANAGER_H
#define SETTINGS_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#define SETTINGS_MAX_STATIONS     20
#define SETTINGS_NAME_LEN         48
#define SETTINGS_URL_LEN          256
#define SETTINGS_SSID_LEN         33
#define SETTINGS_PASS_LEN         65

typedef struct {
    char name[SETTINGS_NAME_LEN];
    char url[SETTINGS_URL_LEN];
} station_t;

esp_err_t settings_init(void);

// --- Wi-Fi credentials ---
bool settings_get_wifi(char ssid[SETTINGS_SSID_LEN], char pass[SETTINGS_PASS_LEN]);
esp_err_t settings_set_wifi(const char *ssid, const char *pass);

// --- Station presets ---
int settings_station_count(void);
bool settings_get_station(int index, station_t *out);
/** Replaces the whole station list (count may be 0..SETTINGS_MAX_STATIONS). */
esp_err_t settings_set_stations(const station_t *list, int count);
/** Restores the SomaFM default preset list. */
esp_err_t settings_reset_stations(void);

// --- Playback state ---
int settings_get_current_station(void);
void settings_set_current_station(int index);
int settings_get_volume(void);
/** Volume is persisted lazily (a few seconds after the last change). */
void settings_set_volume(int volume);
void settings_flush_pending(void);
bool settings_get_autoplay(void);
void settings_set_autoplay(bool autoplay);

#endif // SETTINGS_MANAGER_H
