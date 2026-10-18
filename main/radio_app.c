#include <string.h>
#include <stdio.h>
#include "radio_app.h"
#include "settings_manager.h"
#include "audio_player.h"
#include "stream_client.h"
#include "artwork_manager.h"
#include "wifi_manager.h"
#include "pmic_axp2101.h"
#include "input_driver.h"
#include "epd_ssd1677.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

static const char *TAG = "RADIO";

#define VOLUME_STEP         5
#define UI_COALESCE_MS      220

static SemaphoreHandle_t s_lock = NULL;
static QueueHandle_t s_input_queue = NULL;

static bool s_playing_intent = false;
static bool s_sleeping = false;
static char s_stream_title[160] = "";
static char s_status_msg[80] = "";
static char s_icy_genre[48] = "";
static int s_icy_br = 0;

static volatile bool s_ui_dirty = true;
static volatile bool s_ui_full = true;
static TaskHandle_t s_ui_task = NULL;

static void lock(void)   { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_lock); }

void radio_app_request_ui_refresh(bool full)
{
    if (full) {
        s_ui_full = true;
    }
    s_ui_dirty = true;
    if (s_ui_task) {
        xTaskNotifyGive(s_ui_task);
    }
}

// --- Stream & Audio Callbacks ---

static void on_artwork_ready(void)
{
    // Trigger a 4-gray full refresh when new album artwork arrives
    radio_app_request_ui_refresh(true);
}

static void on_stream_metadata(const char *title)
{
    lock();
    strlcpy(s_stream_title, title ? title : "", sizeof(s_stream_title));
    s_status_msg[0] = '\0';
    unlock();
    artwork_manager_request(title);
    radio_app_request_ui_refresh(false);
}

static void on_stream_headers(const stream_icy_headers_t *hdrs)
{
    if (!hdrs) {
        return;
    }
    lock();
    strlcpy(s_icy_genre, hdrs->genre, sizeof(s_icy_genre));
    s_icy_br = hdrs->bitrate_kbps;
    s_status_msg[0] = '\0';
    unlock();
    radio_app_request_ui_refresh(false);
}

static void on_stream_error(const char *msg)
{
    lock();
    strlcpy(s_status_msg, msg ? msg : "Stream error", sizeof(s_status_msg));
    unlock();
    radio_app_request_ui_refresh(false);
}

static void on_audio_state(audio_state_t st)
{
    (void)st;
    radio_app_request_ui_refresh(false);
}

static void on_wifi_state(bool sta_connected, bool ap_active, const char *ip_str)
{
    (void)ip_str;
    if (sta_connected) {
        if (s_playing_intent && !stream_client_is_running()) {
            int idx = settings_get_current_station();
            station_t st;
            if (settings_get_station(idx, &st)) {
                stream_client_start(st.url);
            }
        }
    } else {
        // Guarantee audio decoding and NS4150B PA amplifier remain completely muted when offline / SoftAP
        if (stream_client_is_running()) {
            stream_client_stop();
        }
        audio_player_stop();

        lock();
        if (ap_active) {
            strlcpy(s_status_msg, "Setup AP: 192.168.4.1", sizeof(s_status_msg));
        }
        unlock();
    }
    radio_app_request_ui_refresh(false);
}

// --- Public Control Functions ---

void radio_app_play(void)
{
    int idx = settings_get_current_station();
    station_t st;
    if (!settings_get_station(idx, &st)) {
        ESP_LOGW(TAG, "no station at index %d", idx);
        return;
    }

    lock();
    s_playing_intent = true;
    s_stream_title[0] = '\0';
    s_icy_genre[0] = '\0';
    s_icy_br = 0;
    strlcpy(s_status_msg, "Connecting...", sizeof(s_status_msg));
    unlock();

    artwork_manager_clear();
    settings_set_autoplay(true);
    radio_app_request_ui_refresh(false);

    if (wifi_manager_is_connected()) {
        stream_client_start(st.url);
    } else {
        lock();
        strlcpy(s_status_msg, "Waiting for Wi-Fi...", sizeof(s_status_msg));
        unlock();
        radio_app_request_ui_refresh(false);
    }
}

void radio_app_stop(void)
{
    lock();
    s_playing_intent = false;
    s_stream_title[0] = '\0';
    s_status_msg[0] = '\0';
    unlock();

    artwork_manager_clear();
    settings_set_autoplay(false);
    stream_client_stop();
    radio_app_request_ui_refresh(false);
}

void radio_app_select_station(int index, bool start_playing)
{
    int count = settings_station_count();
    if (count <= 0) {
        return;
    }
    if (index < 0) {
        index = count - 1;
    } else if (index >= count) {
        index = 0;
    }
    settings_set_current_station(index);

    lock();
    s_stream_title[0] = '\0';
    s_icy_genre[0] = '\0';
    s_icy_br = 0;
    s_status_msg[0] = '\0';
    unlock();
    artwork_manager_clear();

    if (start_playing) {
        radio_app_play();
    } else {
        radio_app_request_ui_refresh(false);
    }
}

void radio_app_set_volume(int volume)
{
    if (volume < 0) {
        volume = 0;
    }
    if (volume > 100) {
        volume = 100;
    }
    settings_set_volume(volume);
    audio_player_set_volume(volume);
    radio_app_request_ui_refresh(false);
}

void radio_app_adjust_volume(int delta)
{
    radio_app_set_volume(settings_get_volume() + delta);
}

bool radio_app_is_playing_intent(void)
{
    return s_playing_intent;
}

void radio_app_get_status(ui_state_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));

    int idx = settings_get_current_station();
    int total = settings_station_count();
    station_t st = { 0 };
    settings_get_station(idx, &st);

    out->station_index = idx;
    out->station_total = total;
    strlcpy(out->station_name, st.name, sizeof(out->station_name));
    strlcpy(out->station_url, st.url, sizeof(out->station_url));

    audio_format_t fmt;
    audio_player_get_format(&fmt);
    out->sample_rate = fmt.sample_rate;
    out->bitrate_kbps = fmt.bitrate_kbps;

    lock();
    strlcpy(out->stream_title, s_stream_title, sizeof(out->stream_title));
    strlcpy(out->status_msg, s_status_msg, sizeof(out->status_msg));
    strlcpy(out->genre, s_icy_genre, sizeof(out->genre));
    if (out->bitrate_kbps <= 0) {
        out->bitrate_kbps = s_icy_br;
    }
    unlock();

    artwork_manager_get_info(out->album_name, sizeof(out->album_name),
                             out->artwork_url, sizeof(out->artwork_url));

    out->audio_state = audio_player_get_state();
    out->volume = settings_get_volume();
    out->buffer_percent = audio_player_get_buffer_percent();

    out->wifi_connected = wifi_manager_is_connected();
    out->ap_active = wifi_manager_is_ap_active();
    wifi_manager_get_ssid(out->ssid);
    wifi_manager_get_ip(out->ip_str);
    out->rssi = wifi_manager_get_rssi();

    out->battery_percent = pmic_get_battery_percent();
    out->battery_mv = pmic_get_battery_mv();
    out->charging = pmic_is_charging();
}

// --- Background UI & Input Tasks ---

static void ui_render_task(void *arg)
{
    (void)arg;
    static ui_state_t st;
    TickType_t last_bat_poll = xTaskGetTickCount();

    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));
        settings_flush_pending();

        if (s_ui_dirty) {
            // Coalesce rapid updates (e.g. rotary volume turns) into a single EPD refresh
            vTaskDelay(pdMS_TO_TICKS(UI_COALESCE_MS));
            while (ulTaskNotifyTake(pdTRUE, 0) > 0) {
                vTaskDelay(pdMS_TO_TICKS(80));
            }
        }

        if (s_sleeping) {
            continue;
        }

        // Periodic battery/RSSI refresh every 60s when idle
        TickType_t now = xTaskGetTickCount();
        if (!s_ui_dirty && pdTICKS_TO_MS(now - last_bat_poll) < 60000) {
            continue;
        }
        last_bat_poll = now;

        bool full = s_ui_full;
        s_ui_full = false;
        s_ui_dirty = false;

        radio_app_get_status(&st);
        ui_display_render(&st, full);
    }
}

static void handle_power_off(void)
{
    ESP_LOGW(TAG, "shutting down...");
    radio_app_stop();
    ui_display_show_poweroff("[ OFF ]");
    pmic_power_off();
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_deep_sleep_start();
}

static void toggle_sleep_mode(void)
{
    if (!s_sleeping) {
        ESP_LOGI(TAG, "entering sleep mode");
        s_sleeping = true;
        radio_app_stop();
        ui_display_show_poweroff("[ SLEEP - Press PWR or BOOT to wake ]");
    } else {
        ESP_LOGI(TAG, "waking from sleep mode");
        s_sleeping = false;
        radio_app_request_ui_refresh(true);
    }
}

static void input_dispatch_task(void *arg)
{
    (void)arg;
    input_event_t ev;
    for (;;) {
        if (xQueueReceive(s_input_queue, &ev, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (s_sleeping) {
            if (ev == INPUT_EVT_PWR_LONG) {
                handle_power_off();
            } else {
                toggle_sleep_mode();
            }
            continue;
        }

        bool is_playing = s_playing_intent || (audio_player_get_state() != AUDIO_STATE_STOPPED);

        switch (ev) {
        case INPUT_EVT_ROTARY_UP:
            if (is_playing) {
                radio_app_adjust_volume(+VOLUME_STEP);
            } else {
                radio_app_select_station(settings_get_current_station() + 1, false);
            }
            break;

        case INPUT_EVT_ROTARY_DOWN:
            if (is_playing) {
                radio_app_adjust_volume(-VOLUME_STEP);
            } else {
                radio_app_select_station(settings_get_current_station() - 1, false);
            }
            break;

        case INPUT_EVT_ROTARY_PUSH:
            if (is_playing) {
                radio_app_stop();
            } else {
                radio_app_play();
            }
            break;

        case INPUT_EVT_BOOT_SHORT:
            radio_app_request_ui_refresh(true);
            break;

        case INPUT_EVT_BOOT_LONG:
            wifi_manager_enable_ap();
            radio_app_request_ui_refresh(true);
            break;

        case INPUT_EVT_PWR_SHORT:
            toggle_sleep_mode();
            break;

        case INPUT_EVT_PWR_LONG:
            handle_power_off();
            break;
        }
    }
}

esp_err_t radio_app_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_input_queue = xQueueCreate(16, sizeof(input_event_t));

    ESP_ERROR_CHECK(audio_player_init(on_audio_state));
    audio_player_set_volume(settings_get_volume());

    ESP_ERROR_CHECK(artwork_manager_init(on_artwork_ready));
    ESP_ERROR_CHECK(stream_client_init(on_stream_metadata, on_stream_headers, on_stream_error));
    ESP_ERROR_CHECK(wifi_manager_init(on_wifi_state));
    ESP_ERROR_CHECK(input_driver_init(s_input_queue));

    xTaskCreatePinnedToCore(ui_render_task, "ui_render", 8192, NULL, 3, &s_ui_task, 0);
    xTaskCreatePinnedToCore(input_dispatch_task, "input_disp", 4096, NULL, 5, NULL, 0);

    s_playing_intent = settings_get_autoplay();
    return ESP_OK;
}
