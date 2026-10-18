#ifndef UI_DISPLAY_H
#define UI_DISPLAY_H

#include <stdbool.h>
#include "esp_err.h"
#include "audio_player.h"

typedef struct {
    // Station info
    int station_index;
    int station_total;
    char station_name[64];
    char station_url[256];
    char genre[48];
    int bitrate_kbps;
    int sample_rate;

    // Now playing metadata
    char stream_title[160];
    char album_name[96];
    char artwork_url[256];
    char status_msg[80];

    // Playback & controls
    audio_state_t audio_state;
    int volume;               // 0..100
    int buffer_percent;       // 0..100

    // Network & power
    bool wifi_connected;
    bool ap_active;
    char ssid[33];
    char ip_str[20];
    int rssi;
    int battery_percent;      // -1 if no battery
    int battery_mv;
    bool charging;
} ui_state_t;

esp_err_t ui_display_init(void);

/** Renders `state` into the PSRAM frame buffer and refreshes the e-paper. */
esp_err_t ui_display_render(const ui_state_t *state, bool full_refresh);

/** Renders a full-screen power-off / sleep banner and puts the EPD to sleep. */
void ui_display_show_poweroff(const char *label);

#endif // UI_DISPLAY_H
