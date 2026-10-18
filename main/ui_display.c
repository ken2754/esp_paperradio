#include <stdio.h>
#include <string.h>
#include "ui_display.h"
#include "epd_ssd1677.h"
#include "gfx.h"
#include "artwork_manager.h"
#include "settings_manager.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

static const char *TAG = "UI";

static uint8_t *s_frame_buf = NULL;

static void draw_wifi_bars(int x, int y, int rssi, bool connected)
{
    int bars = 0;
    if (connected) {
        if (rssi >= -60)      bars = 4;
        else if (rssi >= -70) bars = 3;
        else if (rssi >= -80) bars = 2;
        else                  bars = 1;
    }
    for (int i = 0; i < 4; i++) {
        int bh = 4 + i * 4;
        int bx = x + i * 6;
        int by = y + 18 - bh;
        if (i < bars) {
            gfx_fill_rect(bx, by, 4, bh, GFX_BLACK);
        } else {
            gfx_rect(bx, by, 4, bh, 1, GFX_BLACK);
        }
    }
}

static void draw_battery_icon(int x, int y, int pct, bool charging)
{
    gfx_rect(x, y + 2, 32, 16, 2, GFX_BLACK);
    gfx_fill_rect(x + 32, y + 6, 3, 8, GFX_BLACK);
    if (pct >= 0) {
        int fill_w = (pct * 26) / 100;
        if (fill_w > 0) {
            gfx_fill_rect(x + 3, y + 5, fill_w, 10, GFX_BLACK);
        }
    }
    if (charging) {
        gfx_text(&font_terminus_16, 1, x + 38, y + 2, "+", GFX_BLACK);
    }
}

static void draw_header(const ui_state_t *st)
{
    // Top bar inverted (black background, white text)
    gfx_fill_rect(0, 0, GFX_WIDTH, 42, GFX_BLACK);
    gfx_text(&font_terminus_24, 1, 16, 9, "PAPERRADIO", GFX_WHITE);

    char net_info[96];
    if (st->wifi_connected) {
        snprintf(net_info, sizeof(net_info), "http://%s.local  (%s)",
                 CONFIG_PAPERRADIO_HOSTNAME, st->ip_str);
    } else if (st->ap_active) {
        snprintf(net_info, sizeof(net_info), "AP: %s  (http://192.168.4.1)",
                 CONFIG_PAPERRADIO_AP_SSID);
    } else {
        snprintf(net_info, sizeof(net_info), "Wi-Fi connecting...");
    }
    gfx_text(&font_terminus_16, 1, 168, 13, net_info, GFX_WHITE);

    // Battery text on right side of header
    char bat_str[32];
    if (st->battery_percent >= 0) {
        snprintf(bat_str, sizeof(bat_str), "%s%d%% (%d.%02dV)",
                 st->charging ? "CHG " : "",
                 st->battery_percent,
                 st->battery_mv / 1000, (st->battery_mv % 1000) / 10);
    } else {
        snprintf(bat_str, sizeof(bat_str), "USB PWR");
    }
    gfx_text_box(&font_terminus_16, 1, 560, 13, 224, GFX_ALIGN_RIGHT, bat_str, GFX_WHITE);
}

static void draw_station_section(const ui_state_t *st)
{
    // Status badge (inverted pill)
    const char *state_label = "STOPPED";
    if (st->audio_state == AUDIO_STATE_PLAYING) {
        state_label = "PLAYING";
    } else if (st->audio_state == AUDIO_STATE_BUFFERING) {
        state_label = "BUFFERING";
    }

    int badge_w = ((int)strlen(state_label) + 2) * 12;
    gfx_fill_rect(20, 56, badge_w, 30, GFX_BLACK);
    gfx_text_box(&font_terminus_24, 1, 20, 59, badge_w, GFX_ALIGN_CENTER, state_label, GFX_WHITE);

    // Station index / total + format info
    char meta_line[128];
    if (st->station_total > 0) {
        if (st->bitrate_kbps > 0) {
            snprintf(meta_line, sizeof(meta_line), "CH %02d/%02d  |  %d kbps MP3  |  %d.%d kHz",
                     st->station_index + 1, st->station_total,
                     st->bitrate_kbps,
                     st->sample_rate / 1000, (st->sample_rate % 1000) / 100);
        } else {
            snprintf(meta_line, sizeof(meta_line), "CH %02d/%02d  |  %s",
                     st->station_index + 1, st->station_total,
                     st->genre[0] ? st->genre : "Internet Radio");
        }
    } else {
        snprintf(meta_line, sizeof(meta_line), "No stations configured");
    }
    gfx_text(&font_terminus_24, 1, 20 + badge_w + 16, 59, meta_line, GFX_BLACK);

    // Wi-Fi bars & battery icon in top-right of station area
    draw_wifi_bars(700, 60, st->rssi, st->wifi_connected);
    draw_battery_icon(736, 60, st->battery_percent, st->charging);

    // Large station name
    const char *sname = st->station_name[0] ? st->station_name : "(No Station)";
    gfx_text_box(&font_terminus_32, 1, 20, 100, 760, GFX_ALIGN_LEFT, sname, GFX_BLACK);

    // Stream URL in small font
    gfx_text_box(&font_terminus_16, 1, 20, 140, 760, GFX_ALIGN_LEFT, st->station_url, GFX_BLACK);

    gfx_hline(20, 166, 760, GFX_BLACK);
}

static void draw_vinyl_placeholder(int ax, int ay, int sz)
{
    gfx_fill_rect(ax, ay, sz, sz, GFX_LIGHT_GRAY);
    int cx = ax + sz / 2;
    int cy = ay + sz / 2;
    int r_outer = (sz / 2) - 8;
    int r_mid1  = (sz / 2) - 20;
    int r_mid2  = (sz / 2) - 32;
    int r_label = 22;
    int r_hole  = 5;

    for (int y = -r_outer; y <= r_outer; y++) {
        for (int x = -r_outer; x <= r_outer; x++) {
            int d2 = x * x + y * y;
            if (d2 > r_outer * r_outer) {
                continue;
            }
            gfx_color_t col = GFX_BLACK;
            if (d2 <= r_hole * r_hole) {
                col = GFX_WHITE;
            } else if (d2 <= r_label * r_label) {
                col = GFX_LIGHT_GRAY;
            } else if ((d2 >= (r_mid1 - 1) * (r_mid1 - 1) && d2 <= (r_mid1 + 1) * (r_mid1 + 1)) ||
                       (d2 >= (r_mid2 - 1) * (r_mid2 - 1) && d2 <= (r_mid2 + 1) * (r_mid2 + 1))) {
                col = GFX_DARK_GRAY;
            }
            gfx_pixel(cx + x, cy + y, col);
        }
    }
}

static void draw_now_playing_box(const ui_state_t *st)
{
    int bx = 20, by = 178, bw = 490, bh = 196;
    gfx_rect(bx, by, bw, bh, 2, GFX_BLACK);

    // Header label tab
    gfx_fill_rect(bx, by, 164, 26, GFX_BLACK);
    gfx_text(&font_terminus_16, 1, bx + 12, by + 5, "NOW PLAYING", GFX_WHITE);

    if (st->status_msg[0]) {
        gfx_text_box(&font_terminus_16, 1, bx + 174, by + 5, bw - 184,
                     GFX_ALIGN_RIGHT, st->status_msg, GFX_BLACK);
    }

    // 144x144 Album Artwork area on the left side of the Now Playing box
    int ax = bx + 12;
    int ay = by + 38;
    gfx_rect(ax - 2, ay - 2, ARTWORK_SIZE + 4, ARTWORK_SIZE + 4, 2, GFX_BLACK);

    const uint8_t *art_bmp = artwork_manager_lock_bitmap();
    if (art_bmp) {
        gfx_draw_bitmap_2bpp(ax, ay, ARTWORK_SIZE, ARTWORK_SIZE, art_bmp);
        artwork_manager_unlock_bitmap();
    } else {
        draw_vinyl_placeholder(ax, ay, ARTWORK_SIZE);
    }

    // Track info to the right of the artwork
    int tx = ax + ARTWORK_SIZE + 16;
    int tw = (bx + bw - 12) - tx;

    const char *track = st->stream_title;
    if (!track || track[0] == '\0') {
        if (st->audio_state == AUDIO_STATE_PLAYING) {
            track = "(Waiting for track metadata...)";
        } else if (st->audio_state == AUDIO_STATE_BUFFERING) {
            track = "Buffering audio stream...";
        } else {
            track = "Press PUSH to play.\nUP/DOWN selects station.";
        }
        gfx_text_wrap(&font_terminus_24, 1, tx, by + 44, tw, 4, 6,
                      GFX_ALIGN_LEFT, track, GFX_BLACK);
        return;
    }

    // Split "Artist - Title" if separator is present
    const char *sep = strstr(track, " - ");
    if (sep && sep > track) {
        char artist[96];
        size_t alen = (size_t)(sep - track);
        if (alen >= sizeof(artist)) {
            alen = sizeof(artist) - 1;
        }
        memcpy(artist, track, alen);
        artist[alen] = '\0';
        const char *song = sep + 3;

        int artist_lines = gfx_text_wrap(&font_terminus_24, 1, tx, by + 38, tw, 2, 4,
                                         GFX_ALIGN_LEFT, artist, GFX_BLACK);
        int song_y = by + 38 + artist_lines * 28 + 6;
        gfx_hline(tx, song_y - 4, tw, GFX_BLACK);
        int max_song_lines = (st->album_name[0] != '\0') ? (3 - artist_lines) : (4 - artist_lines);
        if (max_song_lines < 1) {
            max_song_lines = 1;
        }
        int song_lines = gfx_text_wrap(&font_terminus_24, 1, tx, song_y, tw, max_song_lines, 4,
                                       GFX_ALIGN_LEFT, song, GFX_BLACK);
        if (st->album_name[0] != '\0') {
            int album_y = song_y + song_lines * 28 + 6;
            if (album_y + 16 <= by + bh - 8) {
                gfx_text_box(&font_terminus_16, 1, tx, album_y, tw,
                             GFX_ALIGN_LEFT, st->album_name, GFX_BLACK);
            }
        }
    } else {
        int lines = gfx_text_wrap(&font_terminus_24, 1, tx, by + 40, tw, 3, 6,
                                  GFX_ALIGN_LEFT, track, GFX_BLACK);
        if (st->album_name[0] != '\0') {
            int album_y = by + 40 + lines * 30 + 6;
            if (album_y + 16 <= by + bh - 8) {
                gfx_text_box(&font_terminus_16, 1, tx, album_y, tw,
                             GFX_ALIGN_LEFT, st->album_name, GFX_BLACK);
            }
        }
    }
}

static void draw_preset_list_box(const ui_state_t *st)
{
    int bx = 524, by = 178, bw = 256, bh = 196;
    gfx_rect(bx, by, bw, bh, 2, GFX_BLACK);
    gfx_fill_rect(bx, by, bw, 26, GFX_BLACK);
    gfx_text(&font_terminus_16, 1, bx + 10, by + 5,
             st->audio_state == AUDIO_STATE_STOPPED ? "STATIONS (UP/DN)" : "STATION PRESETS",
             GFX_WHITE);

    int total = settings_station_count();
    if (total <= 0) {
        gfx_text(&font_terminus_16, 1, bx + 12, by + 40, "Empty list", GFX_BLACK);
        return;
    }

    // Show a window of up to 6 stations around the selected station
    int visible = 6;
    int start = st->station_index - visible / 2;
    if (start + visible > total) {
        start = total - visible;
    }
    if (start < 0) {
        start = 0;
    }

    for (int i = 0; i < visible && (start + i) < total; i++) {
        int idx = start + i;
        station_t s;
        if (!settings_get_station(idx, &s)) {
            continue;
        }
        int ry = by + 30 + i * 26;
        bool sel = (idx == st->station_index);
        if (sel) {
            gfx_fill_rect(bx + 4, ry, bw - 8, 24, GFX_BLACK);
        }
        char line[64];
        snprintf(line, sizeof(line), "%02d %s", idx + 1, s.name);
        gfx_text_box(&font_terminus_16, 1, bx + 10, ry + 4, bw - 20,
                     GFX_ALIGN_LEFT, line, sel ? GFX_WHITE : GFX_BLACK);
    }
}

static void draw_footer(const ui_state_t *st)
{
    int fy = 390;
    gfx_hline(20, fy, 760, GFX_BLACK);

    // Volume bar
    gfx_text(&font_terminus_24, 1, 20, fy + 14, "VOL", GFX_BLACK);
    int bar_x = 76, bar_y = fy + 16, bar_w = 280, bar_h = 20;
    gfx_rect(bar_x, bar_y, bar_w, bar_h, 2, GFX_BLACK);
    int fill = ((bar_w - 8) * st->volume) / 100;
    if (fill > 0) {
        gfx_fill_rect(bar_x + 4, bar_y + 4, fill, bar_h - 8, GFX_BLACK);
    }
    char vol_txt[16];
    snprintf(vol_txt, sizeof(vol_txt), "%3d%%", st->volume);
    gfx_text(&font_terminus_24, 1, bar_x + bar_w + 12, fy + 14, vol_txt, GFX_BLACK);

    // Buffer gauge on the right
    char buf_txt[32];
    snprintf(buf_txt, sizeof(buf_txt), "BUF %d%%", st->buffer_percent);
    gfx_text_box(&font_terminus_16, 1, 660, fy + 18, 120, GFX_ALIGN_RIGHT, buf_txt, GFX_BLACK);

    // Bottom control hints bar
    gfx_fill_rect(0, 444, GFX_WIDTH, 36, GFX_BLACK);
    const char *hint = (st->audio_state == AUDIO_STATE_STOPPED)
        ? "[STOPPED]  UP/DOWN: Select Station   PUSH: Play    BOOT: Refresh / Hold 3s Setup AP"
        : "[PLAYING]  UP/DOWN: Volume +/-5%     PUSH: Stop    BOOT: Refresh / Hold 3s Setup AP";
    gfx_text_box(&font_terminus_16, 1, 12, 454, GFX_WIDTH - 24, GFX_ALIGN_CENTER, hint, GFX_WHITE);
}

esp_err_t ui_display_init(void)
{
    s_frame_buf = heap_caps_malloc(EPD_FRAME_BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_frame_buf) {
        ESP_LOGE(TAG, "failed to allocate EPD frame buffer in PSRAM");
        return ESP_ERR_NO_MEM;
    }
#ifdef CONFIG_PAPERRADIO_DISPLAY_ROTATE_180
    bool rot = true;
#else
    bool rot = false;
#endif
    gfx_bind(s_frame_buf, rot);
    gfx_clear(GFX_WHITE);
    return epd_init();
}

esp_err_t ui_display_render(const ui_state_t *state, bool full_refresh)
{
    if (!s_frame_buf || !state) {
        return ESP_ERR_INVALID_STATE;
    }
    gfx_clear(GFX_WHITE);
    draw_header(state);
    draw_station_section(state);
    draw_now_playing_box(state);
    draw_preset_list_box(state);
    draw_footer(state);

    if (full_refresh) {
        epd_request_full_refresh();
    }
    return epd_display(s_frame_buf, !full_refresh);
}

void ui_display_show_poweroff(const char *label)
{
    if (!s_frame_buf) {
        return;
    }
    gfx_clear(GFX_WHITE);
    gfx_rect(40, 60, 720, 360, 3, GFX_BLACK);
    gfx_text_box(&font_terminus_32, 1, 60, 180, 680, GFX_ALIGN_CENTER, "PAPERRADIO", GFX_BLACK);
    gfx_text_box(&font_terminus_24, 1, 60, 240, 680, GFX_ALIGN_CENTER,
                 label ? label : "[ OFF ]", GFX_BLACK);
    epd_request_full_refresh();
    epd_display(s_frame_buf, false);
    epd_sleep();
}
