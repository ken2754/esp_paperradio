#include <string.h>
#include "settings_manager.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "SETTINGS";

#define NVS_NS              "paperradio"
#define KEY_SSID            "wifi_ssid"
#define KEY_PASS            "wifi_pass"
#define KEY_WIFI_PROFS      "wifi_profs"
#define KEY_STATIONS        "stations"
#define KEY_CUR_STATION     "cur_station"
#define KEY_VOLUME          "volume"
#define KEY_AUTOPLAY        "autoplay"

#define DEFAULT_VOLUME      70
#define VOLUME_SAVE_DELAY_US (3 * 1000 * 1000)

static const station_t k_default_stations[] = {
    { "SomaFM Groove Salad",   "http://ice1.somafm.com/groovesalad-128-mp3" },
    { "SomaFM Drone Zone",     "http://ice1.somafm.com/dronezone-128-mp3" },
    { "SomaFM Secret Agent",   "http://ice1.somafm.com/secretagent-128-mp3" },
    { "SomaFM Lush",           "http://ice1.somafm.com/lush-128-mp3" },
    { "SomaFM Deep Space One", "http://ice1.somafm.com/deepspaceone-128-mp3" },
    { "SomaFM Indie Pop Rocks!", "http://ice1.somafm.com/indiepop-128-mp3" },
    { "SomaFM DEF CON Radio",  "http://ice1.somafm.com/defcon-128-mp3" },
    { "SomaFM Beat Blender",   "http://ice1.somafm.com/beatblender-128-mp3" },
};
#define DEFAULT_STATION_COUNT ((int)(sizeof(k_default_stations) / sizeof(k_default_stations[0])))

static SemaphoreHandle_t s_mutex = NULL;
static wifi_profile_t s_wifi_profiles[SETTINGS_MAX_WIFI_PROFILES];
static int s_wifi_profile_count = 0;
static station_t s_stations[SETTINGS_MAX_STATIONS];
static int s_station_count = 0;
static int s_current = 0;
static int s_volume = DEFAULT_VOLUME;
static volatile bool s_volume_dirty = false;
static bool s_autoplay = true;
static esp_timer_handle_t s_volume_timer = NULL;

static void lock(void)   { xSemaphoreTake(s_mutex, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_mutex); }

static esp_err_t nvs_write_i32(const char *key, int32_t v)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_i32(h, key, v);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static esp_err_t save_stations_locked(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    // Store a zero-length marker when the list is empty so defaults are not restored
    size_t len = s_station_count * sizeof(station_t);
    err = nvs_set_blob(h, KEY_STATIONS, s_stations, len ? len : 1);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static esp_err_t save_wifi_profiles_locked(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    size_t len = s_wifi_profile_count * sizeof(wifi_profile_t);
    err = nvs_set_blob(h, KEY_WIFI_PROFS, s_wifi_profiles, len ? len : 1);
    if (err == ESP_OK) {
        if (s_wifi_profile_count > 0) {
            nvs_set_str(h, KEY_SSID, s_wifi_profiles[0].ssid);
            nvs_set_str(h, KEY_PASS, s_wifi_profiles[0].pass);
        } else {
            nvs_set_str(h, KEY_SSID, "");
            nvs_set_str(h, KEY_PASS, "");
        }
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static void volume_save_cb(void *arg)
{
    (void)arg;
    s_volume_dirty = true;
}

esp_err_t settings_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "erasing NVS (%s)", esp_err_to_name(err));
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        return err;
    }

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        return ESP_ERR_NO_MEM;
    }

    const esp_timer_create_args_t targs = { .callback = volume_save_cb, .name = "vol_save" };
    esp_timer_create(&targs, &s_volume_timer);

    nvs_handle_t h;
    bool loaded_stations = false;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(s_wifi_profiles);
        if (nvs_get_blob(h, KEY_WIFI_PROFS, s_wifi_profiles, &len) == ESP_OK) {
            s_wifi_profile_count = (len % sizeof(wifi_profile_t) == 0) ? (int)(len / sizeof(wifi_profile_t)) : 0;
            if (s_wifi_profile_count > SETTINGS_MAX_WIFI_PROFILES) {
                s_wifi_profile_count = SETTINGS_MAX_WIFI_PROFILES;
            }
        } else {
            // Legacy fallback: read single SSID/pass and migrate
            char legacy_ssid[SETTINGS_SSID_LEN] = { 0 };
            char legacy_pass[SETTINGS_PASS_LEN] = { 0 };
            len = sizeof(legacy_ssid);
            if (nvs_get_str(h, KEY_SSID, legacy_ssid, &len) == ESP_OK && legacy_ssid[0] != '\0') {
                len = sizeof(legacy_pass);
                if (nvs_get_str(h, KEY_PASS, legacy_pass, &len) != ESP_OK) {
                    legacy_pass[0] = '\0';
                }
                strlcpy(s_wifi_profiles[0].ssid, legacy_ssid, sizeof(s_wifi_profiles[0].ssid));
                strlcpy(s_wifi_profiles[0].pass, legacy_pass, sizeof(s_wifi_profiles[0].pass));
                s_wifi_profile_count = 1;
            }
        }

        len = sizeof(s_stations);
        if (nvs_get_blob(h, KEY_STATIONS, s_stations, &len) == ESP_OK) {
            s_station_count = (len % sizeof(station_t) == 0) ? (int)(len / sizeof(station_t)) : 0;
            loaded_stations = true;
        }

        int32_t v;
        if (nvs_get_i32(h, KEY_CUR_STATION, &v) == ESP_OK) {
            s_current = v;
        }
        if (nvs_get_i32(h, KEY_VOLUME, &v) == ESP_OK && v >= 0 && v <= 100) {
            s_volume = v;
        }
        if (nvs_get_i32(h, KEY_AUTOPLAY, &v) == ESP_OK) {
            s_autoplay = (v != 0);
        }
        nvs_close(h);
    }

    if (!loaded_stations) {
        ESP_LOGI(TAG, "no stations stored, loading SomaFM defaults");
        settings_reset_stations();
    }
    for (int i = 0; i < s_station_count; i++) {
        s_stations[i].name[SETTINGS_NAME_LEN - 1] = '\0';
        s_stations[i].url[SETTINGS_URL_LEN - 1] = '\0';
    }
    for (int i = 0; i < s_wifi_profile_count; i++) {
        s_wifi_profiles[i].ssid[SETTINGS_SSID_LEN - 1] = '\0';
        s_wifi_profiles[i].pass[SETTINGS_PASS_LEN - 1] = '\0';
    }
    if (s_current < 0 || s_current >= s_station_count) {
        s_current = 0;
    }

    ESP_LOGI(TAG, "loaded: %d Wi-Fi profiles (primary='%s'), %d stations, current=%d, volume=%d",
             s_wifi_profile_count,
             s_wifi_profile_count > 0 ? s_wifi_profiles[0].ssid : "(none)",
             s_station_count, s_current, s_volume);
    return ESP_OK;
}

int settings_wifi_count(void)
{
    lock();
    int n = s_wifi_profile_count;
    unlock();
    return n;
}

bool settings_get_wifi_profile(int index, wifi_profile_t *out)
{
    if (!out) {
        return false;
    }
    lock();
    bool ok = false;
    if (index >= 0 && index < s_wifi_profile_count) {
        *out = s_wifi_profiles[index];
        ok = true;
    }
    unlock();
    return ok;
}

esp_err_t settings_set_wifi_profiles(const wifi_profile_t *profiles, int count)
{
    if (count < 0) {
        count = 0;
    }
    if (count > SETTINGS_MAX_WIFI_PROFILES) {
        count = SETTINGS_MAX_WIFI_PROFILES;
    }

    lock();
    s_wifi_profile_count = 0;
    for (int i = 0; i < count; i++) {
        if (profiles[i].ssid[0] != '\0') {
            s_wifi_profiles[s_wifi_profile_count] = profiles[i];
            s_wifi_profiles[s_wifi_profile_count].ssid[SETTINGS_SSID_LEN - 1] = '\0';
            s_wifi_profiles[s_wifi_profile_count].pass[SETTINGS_PASS_LEN - 1] = '\0';
            s_wifi_profile_count++;
        }
    }
    esp_err_t err = save_wifi_profiles_locked();
    unlock();
    return err;
}

bool settings_get_wifi(char ssid[SETTINGS_SSID_LEN], char pass[SETTINGS_PASS_LEN])
{
    lock();
    bool ok = false;
    if (s_wifi_profile_count > 0 && s_wifi_profiles[0].ssid[0] != '\0') {
        strlcpy(ssid, s_wifi_profiles[0].ssid, SETTINGS_SSID_LEN);
        strlcpy(pass, s_wifi_profiles[0].pass, SETTINGS_PASS_LEN);
        ok = true;
    } else {
        if (ssid) ssid[0] = '\0';
        if (pass) pass[0] = '\0';
    }
    unlock();
    return ok;
}

esp_err_t settings_set_wifi(const char *ssid, const char *pass)
{
    wifi_profile_t prof = { 0 };
    strlcpy(prof.ssid, ssid ? ssid : "", sizeof(prof.ssid));
    strlcpy(prof.pass, pass ? pass : "", sizeof(prof.pass));
    return settings_set_wifi_profiles(&prof, (prof.ssid[0] != '\0') ? 1 : 0);
}

int settings_station_count(void)
{
    lock();
    int n = s_station_count;
    unlock();
    return n;
}

bool settings_get_station(int index, station_t *out)
{
    bool ok = false;
    lock();
    if (index >= 0 && index < s_station_count && out) {
        *out = s_stations[index];
        ok = true;
    }
    unlock();
    return ok;
}

esp_err_t settings_set_stations(const station_t *list, int count)
{
    if (count < 0 || count > SETTINGS_MAX_STATIONS || (count > 0 && !list)) {
        return ESP_ERR_INVALID_ARG;
    }
    lock();
    bool changed = (count != s_station_count);
    for (int i = 0; i < count; i++) {
        if (!changed &&
            (strncmp(s_stations[i].name, list[i].name, SETTINGS_NAME_LEN) != 0 ||
             strncmp(s_stations[i].url, list[i].url, SETTINGS_URL_LEN) != 0)) {
            changed = true;
        }
        memset(&s_stations[i], 0, sizeof(station_t));
        strlcpy(s_stations[i].name, list[i].name, SETTINGS_NAME_LEN);
        strlcpy(s_stations[i].url, list[i].url, SETTINGS_URL_LEN);
    }
    s_station_count = count;
    if (s_current >= s_station_count) {
        s_current = 0;
    }
    esp_err_t err = changed ? save_stations_locked() : ESP_OK;
    unlock();
    ESP_LOGI(TAG, "station list saved (%d, flash_write=%d)", count, changed ? 1 : 0);
    return err;
}

esp_err_t settings_reset_stations(void)
{
    return settings_set_stations(k_default_stations, DEFAULT_STATION_COUNT);
}

int settings_get_current_station(void)
{
    lock();
    int c = s_current;
    unlock();
    return c;
}

void settings_set_current_station(int index)
{
    lock();
    bool changed = (index != s_current) && index >= 0 && index < s_station_count;
    if (changed) {
        s_current = index;
    }
    unlock();
    if (changed) {
        nvs_write_i32(KEY_CUR_STATION, index);
    }
}

int settings_get_volume(void)
{
    lock();
    int v = s_volume;
    unlock();
    return v;
}

void settings_set_volume(int volume)
{
    if (volume < 0) {
        volume = 0;
    }
    if (volume > 100) {
        volume = 100;
    }
    lock();
    bool changed = (s_volume != volume);
    s_volume = volume;
    unlock();
    if (changed && s_volume_timer) {
        esp_timer_stop(s_volume_timer);
        esp_timer_start_once(s_volume_timer, VOLUME_SAVE_DELAY_US);
    }
}

void settings_flush_pending(void)
{
    if (s_volume_dirty) {
        s_volume_dirty = false;
        lock();
        int v = s_volume;
        unlock();
        nvs_write_i32(KEY_VOLUME, v);
        ESP_LOGI(TAG, "volume %d saved", v);
    }
}

bool settings_get_autoplay(void)
{
    lock();
    bool a = s_autoplay;
    unlock();
    return a;
}

void settings_set_autoplay(bool autoplay)
{
    lock();
    s_autoplay = autoplay;
    unlock();
    nvs_write_i32(KEY_AUTOPLAY, autoplay ? 1 : 0);
}
