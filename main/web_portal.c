#include <string.h>
#include <stdlib.h>
#include "web_portal.h"
#include "radio_app.h"
#include "settings_manager.h"
#include "wifi_manager.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "cJSON.h"

static const char *TAG = "WEB";

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

static httpd_handle_t s_server = NULL;

static char *read_req_body(httpd_req_t *req)
{
    size_t len = req->content_len;
    if (len == 0 || len > 8192) {
        return NULL;
    }
    char *buf = malloc(len + 1);
    if (!buf) {
        return NULL;
    }
    size_t off = 0;
    while (off < len) {
        int r = httpd_req_recv(req, buf + off, len - off);
        if (r <= 0) {
            free(buf);
            return NULL;
        }
        off += (size_t)r;
    }
    buf[len] = '\0';
    return buf;
}

static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!s) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, s);
    free(s);
    return err;
}

static esp_err_t handle_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    size_t len = (size_t)(index_html_end - index_html_start);
    if (len > 0 && index_html_start[len - 1] == '\0') {
        len--;
    }
    return httpd_resp_send(req, (const char *)index_html_start, (ssize_t)len);
}

static esp_err_t handle_get_status(httpd_req_t *req)
{
    ui_state_t *st = calloc(1, sizeof(ui_state_t));
    if (!st) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    radio_app_get_status(st);

    const char *state_str = "STOPPED";
    if (st->audio_state == AUDIO_STATE_PLAYING) {
        state_str = "PLAYING";
    } else if (st->audio_state == AUDIO_STATE_BUFFERING) {
        state_str = "BUFFERING";
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "state", state_str);
    cJSON_AddBoolToObject(root, "playing_intent", radio_app_is_playing_intent());
    cJSON_AddNumberToObject(root, "station_index", st->station_index);
    cJSON_AddNumberToObject(root, "station_total", st->station_total);
    cJSON_AddStringToObject(root, "station_name", st->station_name);
    cJSON_AddStringToObject(root, "station_url", st->station_url);
    cJSON_AddStringToObject(root, "stream_title", st->stream_title);
    cJSON_AddStringToObject(root, "album_name", st->album_name);
    cJSON_AddStringToObject(root, "artwork_url", st->artwork_url);
    cJSON_AddStringToObject(root, "status_msg", st->status_msg);
    cJSON_AddNumberToObject(root, "bitrate_kbps", st->bitrate_kbps);
    cJSON_AddNumberToObject(root, "sample_rate", st->sample_rate);
    cJSON_AddNumberToObject(root, "volume", st->volume);
    cJSON_AddNumberToObject(root, "buffer_percent", st->buffer_percent);
    cJSON_AddBoolToObject(root, "wifi_connected", st->wifi_connected);
    cJSON_AddBoolToObject(root, "ap_active", st->ap_active);
    cJSON_AddStringToObject(root, "ssid", st->ssid);
    cJSON_AddStringToObject(root, "ip", st->ip_str);
    cJSON_AddNumberToObject(root, "rssi", st->rssi);
    cJSON_AddNumberToObject(root, "battery_percent", st->battery_percent);
    cJSON_AddNumberToObject(root, "battery_mv", st->battery_mv);
    cJSON_AddBoolToObject(root, "charging", st->charging);
    free(st);
    return send_json(req, root);
}

static esp_err_t handle_get_stations(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "stations");
    int count = settings_station_count();
    for (int i = 0; i < count; i++) {
        station_t st;
        if (settings_get_station(i, &st)) {
            cJSON *item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "name", st.name);
            cJSON_AddStringToObject(item, "url", st.url);
            cJSON_AddItemToArray(arr, item);
        }
    }
    cJSON_AddNumberToObject(root, "current_index", settings_get_current_station());
    char ssid[SETTINGS_SSID_LEN] = { 0 };
    char pass[SETTINGS_PASS_LEN] = { 0 };
    settings_get_wifi(ssid, pass);
    cJSON_AddStringToObject(root, "wifi_ssid", ssid);
    return send_json(req, root);
}

static esp_err_t handle_post_stations(httpd_req_t *req)
{
    char *body = read_req_body(req);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
        return ESP_FAIL;
    }
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *arr = cJSON_GetObjectItem(root, "stations");
    if (!cJSON_IsArray(arr)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing stations array");
        return ESP_FAIL;
    }

    station_t *list = calloc(SETTINGS_MAX_STATIONS, sizeof(station_t));
    if (!list) {
        cJSON_Delete(root);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    int count = 0;
    cJSON *el = NULL;
    cJSON_ArrayForEach(el, arr) {
        if (count >= SETTINGS_MAX_STATIONS) {
            break;
        }
        cJSON *name = cJSON_GetObjectItem(el, "name");
        cJSON *url = cJSON_GetObjectItem(el, "url");
        if (cJSON_IsString(url) && url->valuestring[0] != '\0') {
            strlcpy(list[count].name,
                    (cJSON_IsString(name) && name->valuestring[0]) ? name->valuestring : "Station",
                    SETTINGS_NAME_LEN);
            strlcpy(list[count].url, url->valuestring, SETTINGS_URL_LEN);
            count++;
        }
    }
    cJSON_Delete(root);

    settings_set_stations(list, count);
    free(list);

    if (radio_app_is_playing_intent()) {
        radio_app_play();
    } else {
        radio_app_request_ui_refresh(false);
    }

    cJSON *res = cJSON_CreateObject();
    cJSON_AddBoolToObject(res, "ok", true);
    cJSON_AddNumberToObject(res, "count", count);
    return send_json(req, res);
}

static esp_err_t handle_reset_stations(httpd_req_t *req)
{
    settings_reset_stations();
    radio_app_request_ui_refresh(false);
    cJSON *res = cJSON_CreateObject();
    cJSON_AddBoolToObject(res, "ok", true);
    return send_json(req, res);
}

static esp_err_t handle_post_control(httpd_req_t *req)
{
    char *body = read_req_body(req);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
        return ESP_FAIL;
    }
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *act = cJSON_GetObjectItem(root, "action");
    if (cJSON_IsString(act)) {
        const char *a = act->valuestring;
        if (strcmp(a, "play") == 0) {
            radio_app_play();
        } else if (strcmp(a, "stop") == 0) {
            radio_app_stop();
        } else if (strcmp(a, "select") == 0) {
            cJSON *idx = cJSON_GetObjectItem(root, "index");
            cJSON *play = cJSON_GetObjectItem(root, "play");
            if (cJSON_IsNumber(idx)) {
                radio_app_select_station(idx->valueint, cJSON_IsTrue(play));
            }
        } else if (strcmp(a, "volume") == 0) {
            cJSON *vol = cJSON_GetObjectItem(root, "volume");
            if (cJSON_IsNumber(vol)) {
                radio_app_set_volume(vol->valueint);
            }
        } else if (strcmp(a, "refresh") == 0) {
            radio_app_request_ui_refresh(true);
        }
    }
    cJSON_Delete(root);

    cJSON *res = cJSON_CreateObject();
    cJSON_AddBoolToObject(res, "ok", true);
    return send_json(req, res);
}

static esp_err_t handle_post_wifi(httpd_req_t *req)
{
    char *body = read_req_body(req);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
        return ESP_FAIL;
    }
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *ssid = cJSON_GetObjectItem(root, "ssid");
    cJSON *pass = cJSON_GetObjectItem(root, "password");
    if (cJSON_IsString(ssid) && ssid->valuestring[0] != '\0') {
        char cur_ssid[SETTINGS_SSID_LEN] = { 0 };
        char cur_pass[SETTINGS_PASS_LEN] = { 0 };
        settings_get_wifi(cur_ssid, cur_pass);
        const char *new_pass = (cJSON_IsString(pass) && pass->valuestring[0] != '\0')
                               ? pass->valuestring : cur_pass;
        settings_set_wifi(ssid->valuestring, new_pass);
        wifi_manager_reconnect();
    }
    cJSON_Delete(root);

    cJSON *res = cJSON_CreateObject();
    cJSON_AddBoolToObject(res, "ok", true);
    return send_json(req, res);
}

esp_err_t web_portal_start(void)
{
    if (s_server) {
        return ESP_OK;
    }
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 10;
    cfg.stack_size = 10240;
    cfg.lru_purge_enable = true;

    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

    const httpd_uri_t uris[] = {
        { .uri = "/",                   .method = HTTP_GET,  .handler = handle_index },
        { .uri = "/api/status",         .method = HTTP_GET,  .handler = handle_get_status },
        { .uri = "/api/stations",       .method = HTTP_GET,  .handler = handle_get_stations },
        { .uri = "/api/stations",       .method = HTTP_POST, .handler = handle_post_stations },
        { .uri = "/api/stations/reset", .method = HTTP_POST, .handler = handle_reset_stations },
        { .uri = "/api/control",        .method = HTTP_POST, .handler = handle_post_control },
        { .uri = "/api/wifi",           .method = HTTP_POST, .handler = handle_post_wifi },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(s_server, &uris[i]);
    }

    ESP_LOGI(TAG, "web portal started on port %d", cfg.server_port);
    return ESP_OK;
}
