#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include "stream_client.h"
#include "audio_player.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "STREAM";

#define READ_CHUNK_SIZE     2048
#define MAX_META_BLOCK      4080   // 255 * 16

static stream_metadata_cb_t s_meta_cb = NULL;
static stream_headers_cb_t s_hdr_cb = NULL;
static stream_error_cb_t s_err_cb = NULL;

static char s_url[256];
static volatile bool s_running = false;
static volatile bool s_stop_req = false;
static TaskHandle_t s_task = NULL;
static SemaphoreHandle_t s_done_sem = NULL;

// Per-connection ICY state
static int s_metaint = 0;
static stream_icy_headers_t s_icy_hdrs;
static char s_last_title[STREAM_META_MAX_LEN];

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_HEADER && evt->header_key && evt->header_value) {
        if (strcasecmp(evt->header_key, "icy-metaint") == 0) {
            s_metaint = atoi(evt->header_value);
        } else if (strcasecmp(evt->header_key, "icy-name") == 0) {
            strlcpy(s_icy_hdrs.station_name, evt->header_value, sizeof(s_icy_hdrs.station_name));
        } else if (strcasecmp(evt->header_key, "icy-genre") == 0) {
            strlcpy(s_icy_hdrs.genre, evt->header_value, sizeof(s_icy_hdrs.genre));
        } else if (strcasecmp(evt->header_key, "icy-br") == 0) {
            s_icy_hdrs.bitrate_kbps = atoi(evt->header_value);
        }
    }
    return ESP_OK;
}

static void parse_icy_metadata(const char *meta, size_t len)
{
    // Format: StreamTitle='Artist - Track';StreamUrl='...';
    const char *key = "StreamTitle='";
    const char *p = strstr(meta, key);
    if (!p) {
        return;
    }
    p += strlen(key);
    const char *end = strstr(p, "';");
    if (!end) {
        end = meta + len;
    }
    size_t title_len = (size_t)(end - p);
    if (title_len >= STREAM_META_MAX_LEN) {
        title_len = STREAM_META_MAX_LEN - 1;
    }
    char title[STREAM_META_MAX_LEN];
    memcpy(title, p, title_len);
    title[title_len] = '\0';

    // Trim trailing spaces
    while (title_len > 0 && (title[title_len - 1] == ' ' || title[title_len - 1] == '\r' || title[title_len - 1] == '\n')) {
        title[--title_len] = '\0';
    }

    if (title[0] != '\0' && strcmp(title, s_last_title) != 0) {
        strlcpy(s_last_title, title, sizeof(s_last_title));
        ESP_LOGI(TAG, "ICY StreamTitle: %s", s_last_title);
        if (s_meta_cb) {
            s_meta_cb(s_last_title);
        }
    }
}

static void stream_task(void *arg)
{
    (void)arg;
    uint8_t *rx_buf = malloc(READ_CHUNK_SIZE);
    char *meta_buf = malloc(MAX_META_BLOCK + 1);
    if (!rx_buf || !meta_buf) {
        ESP_LOGE(TAG, "alloc failed");
        vTaskDelete(NULL);
        return;
    }

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (s_stop_req) {
            s_running = false;
            xSemaphoreGive(s_done_sem);
            continue;
        }

        s_running = true;
        s_last_title[0] = '\0';
        audio_player_start();

        while (!s_stop_req) {
            s_metaint = 0;
            memset(&s_icy_hdrs, 0, sizeof(s_icy_hdrs));

            esp_http_client_config_t cfg = {
                .url = s_url,
                .event_handler = http_event_handler,
                .timeout_ms = 8000,
                .buffer_size = 2048,
                .buffer_size_tx = 1024,
                .user_agent = "PaperRadio/1.0 (ESP32-S3)",
                .crt_bundle_attach = esp_crt_bundle_attach,
                .max_redirection_count = 5,
            };

            esp_http_client_handle_t client = esp_http_client_init(&cfg);
            if (!client) {
                if (s_err_cb) {
                    s_err_cb("Invalid stream URL");
                }
                break;
            }

            esp_http_client_set_header(client, "Icy-MetaData", "1");
            esp_http_client_set_header(client, "Accept", "*/*");

            ESP_LOGI(TAG, "connecting to %s", s_url);
            esp_err_t err = esp_http_client_open(client, 0);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "open failed: %s", esp_err_to_name(err));
                esp_http_client_cleanup(client);
                if (s_err_cb) {
                    s_err_cb("Connect failed, retrying...");
                }
                for (int i = 0; i < 30 && !s_stop_req; i++) {
                    vTaskDelay(pdMS_TO_TICKS(100));
                }
                continue;
            }

            int64_t content_len = esp_http_client_fetch_headers(client);
            (void)content_len;
            int status = esp_http_client_get_status_code(client);
            ESP_LOGI(TAG, "HTTP %d, icy-metaint=%d, icy-name='%s', icy-br=%d",
                     status, s_metaint, s_icy_hdrs.station_name, s_icy_hdrs.bitrate_kbps);

            if (status != 200) {
                esp_http_client_close(client);
                esp_http_client_cleanup(client);
                if (s_err_cb) {
                    s_err_cb("Stream returned non-200 status");
                }
                for (int i = 0; i < 30 && !s_stop_req; i++) {
                    vTaskDelay(pdMS_TO_TICKS(100));
                }
                continue;
            }

            if (s_hdr_cb) {
                s_hdr_cb(&s_icy_hdrs);
            }

            // Stream demux loop: separate MP3 bytes from ICY metadata blocks
            int bytes_until_meta = s_metaint;
            int meta_remaining = 0;
            int meta_len = 0;
            int meta_pos = 0;
            bool reading_meta_len = false;

            while (!s_stop_req) {
                int r = esp_http_client_read(client, (char *)rx_buf, READ_CHUNK_SIZE);
                if (r <= 0) {
                    ESP_LOGW(TAG, "stream read ended (r=%d)", r);
                    break;
                }

                if (s_metaint <= 0) {
                    audio_player_write(rx_buf, (size_t)r, 500);
                    continue;
                }

                int idx = 0;
                while (idx < r && !s_stop_req) {
                    if (reading_meta_len) {
                        meta_len = rx_buf[idx++] * 16;
                        reading_meta_len = false;
                        if (meta_len == 0) {
                            bytes_until_meta = s_metaint;
                        } else {
                            meta_remaining = meta_len;
                            meta_pos = 0;
                        }
                    } else if (meta_remaining > 0) {
                        int copy = r - idx;
                        if (copy > meta_remaining) {
                            copy = meta_remaining;
                        }
                        if (meta_pos + copy <= MAX_META_BLOCK) {
                            memcpy(meta_buf + meta_pos, rx_buf + idx, copy);
                        }
                        meta_pos += copy;
                        idx += copy;
                        meta_remaining -= copy;
                        if (meta_remaining == 0) {
                            int safe_len = (meta_len <= MAX_META_BLOCK) ? meta_len : MAX_META_BLOCK;
                            meta_buf[safe_len] = '\0';
                            parse_icy_metadata(meta_buf, (size_t)safe_len);
                            bytes_until_meta = s_metaint;
                        }
                    } else {
                        int audio_chunk = r - idx;
                        if (audio_chunk > bytes_until_meta) {
                            audio_chunk = bytes_until_meta;
                        }
                        audio_player_write(rx_buf + idx, (size_t)audio_chunk, 500);
                        idx += audio_chunk;
                        bytes_until_meta -= audio_chunk;
                        if (bytes_until_meta == 0) {
                            reading_meta_len = true;
                        }
                    }
                }
            }

            esp_http_client_close(client);
            esp_http_client_cleanup(client);

            if (!s_stop_req) {
                ESP_LOGW(TAG, "stream disconnected, reconnecting in 2s...");
                for (int i = 0; i < 20 && !s_stop_req; i++) {
                    vTaskDelay(pdMS_TO_TICKS(100));
                }
            }
        }

        audio_player_stop();
        s_running = false;
        xSemaphoreGive(s_done_sem);
    }
}

esp_err_t stream_client_init(stream_metadata_cb_t meta_cb,
                             stream_headers_cb_t hdr_cb,
                             stream_error_cb_t err_cb)
{
    s_meta_cb = meta_cb;
    s_hdr_cb = hdr_cb;
    s_err_cb = err_cb;
    s_done_sem = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCore(stream_task, "stream_http", 6144, NULL, 5, &s_task, 0);
    return ESP_OK;
}

esp_err_t stream_client_start(const char *url)
{
    if (!url || url[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    stream_client_stop();
    strlcpy(s_url, url, sizeof(s_url));
    s_stop_req = false;
    xSemaphoreTake(s_done_sem, 0);
    xTaskNotifyGive(s_task);
    return ESP_OK;
}

void stream_client_stop(void)
{
    if (!s_running) {
        return;
    }
    s_stop_req = true;
    audio_player_stop();
    xSemaphoreTake(s_done_sem, pdMS_TO_TICKS(3000));
}

bool stream_client_is_running(void)
{
    return s_running;
}
