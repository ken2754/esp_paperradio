#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "artwork_manager.h"
#include "gfx.h"
#include "wifi_manager.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "esp32s3/rom/tjpgd.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "ARTWORK";

#define MAX_JSON_BYTES      8192
#define MAX_JPEG_BYTES      32768
#define TJPGD_WORK_SIZE     4096

static SemaphoreHandle_t s_mutex = NULL;
static TaskHandle_t s_task = NULL;
static artwork_ready_cb_t s_ready_cb = NULL;

static uint8_t *s_bitmap_2bpp = NULL;
static bool s_has_bitmap = false;
static char s_last_title[160] = "";
static char s_pending_title[160] = "";
static char s_album_name[96] = "";
static char s_artwork_url[256] = "";
static volatile uint32_t s_req_seq = 0;

static void lock(void)   { xSemaphoreTake(s_mutex, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_mutex); }

// --- URL encoding helper ---

static void build_search_query(const char *stream_title, char *encoded_out, size_t out_sz)
{
    // Clean up "Artist - Title" into search words, stripping bracketed suffixes
    char clean[160];
    size_t ci = 0;
    for (size_t i = 0; stream_title[i] != '\0' && ci + 1 < sizeof(clean); i++) {
        char c = stream_title[i];
        if (c == '[' || c == '{') {
            break;
        }
        if (c == '-' || c == '/' || c == '_') {
            clean[ci++] = ' ';
        } else if ((unsigned char)c >= 0x20) {
            clean[ci++] = c;
        }
    }
    clean[ci] = '\0';

    static const char hex[] = "0123456789ABCDEF";
    size_t oi = 0;
    bool prev_space = true;
    for (size_t i = 0; clean[i] != '\0' && oi + 4 < out_sz; i++) {
        unsigned char c = (unsigned char)clean[i];
        if (isalnum(c) || c == '.' || c == '~') {
            encoded_out[oi++] = (char)c;
            prev_space = false;
        } else if (c == ' ') {
            if (!prev_space) {
                encoded_out[oi++] = '+';
                prev_space = true;
            }
        } else {
            encoded_out[oi++] = '%';
            encoded_out[oi++] = hex[(c >> 4) & 0x0F];
            encoded_out[oi++] = hex[c & 0x0F];
            prev_space = false;
        }
    }
    if (oi > 0 && encoded_out[oi - 1] == '+') {
        oi--;
    }
    encoded_out[oi] = '\0';
}

// --- Simple HTTPS buffer download ---

typedef struct {
    uint8_t *buf;
    size_t len;
    size_t cap;
} http_buf_t;

static esp_err_t http_event_cb(esp_http_client_event_t *evt)
{
    http_buf_t *hb = (http_buf_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && hb && evt->data && evt->data_len > 0) {
        size_t avail = (hb->len < hb->cap) ? (hb->cap - hb->len) : 0;
        size_t copy = ((size_t)evt->data_len < avail) ? (size_t)evt->data_len : avail;
        if (copy > 0) {
            memcpy(hb->buf + hb->len, evt->data, copy);
            hb->len += copy;
        }
    }
    return ESP_OK;
}

static size_t fetch_https_url(const char *url, uint8_t *buf, size_t max_len)
{
    http_buf_t hb = {
        .buf = buf,
        .len = 0,
        .cap = max_len,
    };
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 5000,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
        .event_handler = http_event_cb,
        .user_data = &hb,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .user_agent = "PaperRadio/1.0",
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return 0;
    }
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK || status != 200) {
        return 0;
    }
    return hb.len;
}

// --- TJpgDec callbacks ---

typedef struct {
    const uint8_t *jpeg;
    size_t jpeg_len;
    size_t jpeg_pos;
    uint8_t *gray_buf;
    int width;
    int height;
} jdec_ctx_t;

static UINT tjpgd_input_cb(JDEC *jd, BYTE *buff, UINT nbyte)
{
    jdec_ctx_t *ctx = (jdec_ctx_t *)jd->device;
    size_t rem = (ctx->jpeg_pos < ctx->jpeg_len) ? (ctx->jpeg_len - ctx->jpeg_pos) : 0;
    size_t n = (nbyte < rem) ? nbyte : rem;
    if (n > 0 && buff) {
        memcpy(buff, ctx->jpeg + ctx->jpeg_pos, n);
    }
    ctx->jpeg_pos += n;
    return (UINT)n;
}

static UINT tjpgd_output_cb(JDEC *jd, void *bitmap, JRECT *rect)
{
    jdec_ctx_t *ctx = (jdec_ctx_t *)jd->device;
    const uint8_t *src = (const uint8_t *)bitmap;
    int rw = rect->right - rect->left + 1;
    int rh = rect->bottom - rect->top + 1;

    for (int y = 0; y < rh; y++) {
        int dy = rect->top + y;
        if (dy < 0 || dy >= ctx->height) {
            continue;
        }
        uint8_t *dst_row = ctx->gray_buf + dy * ctx->width;
        const uint8_t *src_row = src + y * rw * 3;
        for (int x = 0; x < rw; x++) {
            int dx = rect->left + x;
            if (dx >= 0 && dx < ctx->width) {
                uint8_t r = src_row[3 * x + 0];
                uint8_t g = src_row[3 * x + 1];
                uint8_t b = src_row[3 * x + 2];
                dst_row[dx] = (uint8_t)((77 * r + 150 * g + 29 * b) >> 8);
            }
        }
    }
    return 1;
}

// --- Auto-contrast & 4-Gray Floyd-Steinberg Dithering ---

static inline int clamp_i32(int v, int lo, int hi)
{
    return (v < lo) ? lo : ((v > hi) ? hi : v);
}

static void dither_to_2bpp(const uint8_t *src_gray, int src_w, int src_h, uint8_t *out_2bpp)
{
    int16_t *err_buf = heap_caps_calloc(ARTWORK_SIZE * ARTWORK_SIZE, sizeof(int16_t),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!err_buf) {
        return;
    }

    // Step 1: Resample to 144x144 and build histogram for auto-contrast
    uint32_t hist[256] = { 0 };
    for (int y = 0; y < ARTWORK_SIZE; y++) {
        int sy = (y * src_h) / ARTWORK_SIZE;
        for (int x = 0; x < ARTWORK_SIZE; x++) {
            int sx = (x * src_w) / ARTWORK_SIZE;
            uint8_t v = src_gray[sy * src_w + sx];
            err_buf[y * ARTWORK_SIZE + x] = v;
            hist[v]++;
        }
    }

    // Find 2nd and 98th percentiles
    const uint32_t total = ARTWORK_SIZE * ARTWORK_SIZE;
    uint32_t p_low_target = total / 50;
    uint32_t p_high_target = total - p_low_target;
    uint32_t acc = 0;
    int p_low = 0, p_high = 255;
    for (int i = 0; i < 256; i++) {
        acc += hist[i];
        if (acc <= p_low_target) {
            p_low = i;
        }
        if (acc >= p_high_target) {
            p_high = i;
            break;
        }
    }
    int span = p_high - p_low;
    if (span < 80) {
        span = 80;
    }

    for (int i = 0; i < ARTWORK_SIZE * ARTWORK_SIZE; i++) {
        int v = ((err_buf[i] - p_low) * 255) / span;
        err_buf[i] = (int16_t)clamp_i32(v, 0, 255);
    }

    // Step 2: Serpentine Floyd-Steinberg error diffusion into 4 gray levels (0, 85, 170, 255)
    memset(out_2bpp, 0, ARTWORK_BUF_BYTES);
    for (int y = 0; y < ARTWORK_SIZE; y++) {
        uint8_t *row_out = out_2bpp + y * ARTWORK_ROW_BYTES;
        for (int x = 0; x < ARTWORK_SIZE; x++) {
            int idx = y * ARTWORK_SIZE + x;
            int old_p = clamp_i32(err_buf[idx], 0, 255);

            int q_val;
            gfx_color_t col;
            if (old_p < 43) {
                q_val = 0;
                col = GFX_BLACK;       // 3
            } else if (old_p < 128) {
                q_val = 85;
                col = GFX_DARK_GRAY;   // 2
            } else if (old_p < 213) {
                q_val = 170;
                col = GFX_LIGHT_GRAY;  // 1
            } else {
                q_val = 255;
                col = GFX_WHITE;       // 0
            }

            int shift = 6 - ((x & 3) << 1);
            row_out[x >> 2] |= (uint8_t)((col & 0x03) << shift);

            int err = old_p - q_val;
            if (x + 1 < ARTWORK_SIZE) {
                err_buf[idx + 1] += (int16_t)((err * 7) / 16);
            }
            if (y + 1 < ARTWORK_SIZE) {
                if (x > 0) {
                    err_buf[idx + ARTWORK_SIZE - 1] += (int16_t)((err * 3) / 16);
                }
                err_buf[idx + ARTWORK_SIZE] += (int16_t)((err * 5) / 16);
                if (x + 1 < ARTWORK_SIZE) {
                    err_buf[idx + ARTWORK_SIZE + 1] += (int16_t)((err * 1) / 16);
                }
            }
        }
    }

    free(err_buf);
}

static bool decode_and_dither_jpeg(const uint8_t *jpeg, size_t jpeg_len, uint8_t *out_2bpp)
{
    void *work = heap_caps_malloc(TJPGD_WORK_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!work) {
        return false;
    }

    JDEC jdec;
    jdec_ctx_t ctx = {
        .jpeg = jpeg,
        .jpeg_len = jpeg_len,
        .jpeg_pos = 0,
        .gray_buf = NULL,
        .width = 0,
        .height = 0,
    };

    JRESULT rc = jd_prepare(&jdec, tjpgd_input_cb, work, TJPGD_WORK_SIZE, &ctx);
    if (rc != JDR_OK || jdec.width == 0 || jdec.height == 0 || jdec.width > 512 || jdec.height > 512) {
        ESP_LOGW(TAG, "jd_prepare failed: rc=%d (%ux%u)", rc, jdec.width, jdec.height);
        free(work);
        return false;
    }

    ctx.width = (int)jdec.width;
    ctx.height = (int)jdec.height;
    ctx.gray_buf = heap_caps_malloc((size_t)ctx.width * (size_t)ctx.height,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ctx.gray_buf) {
        free(work);
        return false;
    }

    rc = jd_decomp(&jdec, tjpgd_output_cb, 0);
    bool ok = (rc == JDR_OK);
    if (ok) {
        dither_to_2bpp(ctx.gray_buf, ctx.width, ctx.height, out_2bpp);
    } else {
        ESP_LOGW(TAG, "jd_decomp failed: rc=%d", rc);
    }

    free(ctx.gray_buf);
    free(work);
    return ok;
}

static void rewrite_mzstatic_size(const char *src_url, char *dst_url, size_t dst_sz, const char *target_suffix)
{
    strlcpy(dst_url, src_url, dst_sz);
    char *slash = strrchr(dst_url, '/');
    if (slash && strstr(slash, "bb.jpg")) {
        *(slash + 1) = '\0';
        strlcat(dst_url, target_suffix, dst_sz);
    }
}

static void artwork_task(void *arg)
{
    (void)arg;
    uint8_t *http_buf = heap_caps_malloc(MAX_JPEG_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *temp_2bpp = heap_caps_malloc(ARTWORK_BUF_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!http_buf || !temp_2bpp) {
        ESP_LOGE(TAG, "PSRAM buffer alloc failed");
        vTaskDelete(NULL);
        return;
    }

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        char title[160];
        uint32_t seq;
        lock();
        strlcpy(title, s_pending_title, sizeof(title));
        seq = s_req_seq;
        unlock();

        if (title[0] == '\0' || !wifi_manager_is_connected()) {
            continue;
        }

        char query[256];
        build_search_query(title, query, sizeof(query));
        if (query[0] == '\0') {
            continue;
        }

        char api_url[384];
        snprintf(api_url, sizeof(api_url),
                 "https://itunes.apple.com/search?term=%s&media=music&entity=song&limit=1",
                 query);

        ESP_LOGI(TAG, "searching artwork for '%s'", title);
        size_t json_len = fetch_https_url(api_url, http_buf, MAX_JSON_BYTES - 1);
        if (json_len == 0 || seq != s_req_seq) {
            continue;
        }
        http_buf[json_len] = '\0';

        char art_url_100[256] = "";
        char album[96] = "";
        cJSON *root = cJSON_Parse((const char *)http_buf);
        if (root) {
            cJSON *results = cJSON_GetObjectItem(root, "results");
            cJSON *first = (cJSON_IsArray(results) && cJSON_GetArraySize(results) > 0)
                           ? cJSON_GetArrayItem(results, 0) : NULL;
            if (first) {
                cJSON *u = cJSON_GetObjectItem(first, "artworkUrl100");
                cJSON *col = cJSON_GetObjectItem(first, "collectionName");
                if (cJSON_IsString(u) && u->valuestring[0]) {
                    strlcpy(art_url_100, u->valuestring, sizeof(art_url_100));
                }
                if (cJSON_IsString(col) && col->valuestring[0]) {
                    strlcpy(album, col->valuestring, sizeof(album));
                }
            }
            cJSON_Delete(root);
        }

        if (art_url_100[0] == '\0' || seq != s_req_seq) {
            ESP_LOGI(TAG, "no artwork match for '%s'", title);
            continue;
        }

        char jpg_url[256];
        rewrite_mzstatic_size(art_url_100, jpg_url, sizeof(jpg_url), "144x144bb.jpg");

        size_t jpg_len = fetch_https_url(jpg_url, http_buf, MAX_JPEG_BYTES);
        if (jpg_len == 0 || seq != s_req_seq) {
            continue;
        }

        if (decode_and_dither_jpeg(http_buf, jpg_len, temp_2bpp) && seq == s_req_seq) {
            char web_url[256];
            rewrite_mzstatic_size(art_url_100, web_url, sizeof(web_url), "300x300bb.jpg");

            lock();
            memcpy(s_bitmap_2bpp, temp_2bpp, ARTWORK_BUF_BYTES);
            s_has_bitmap = true;
            strlcpy(s_album_name, album, sizeof(s_album_name));
            strlcpy(s_artwork_url, web_url, sizeof(s_artwork_url));
            unlock();

            ESP_LOGI(TAG, "artwork ready (%u bytes JPEG, album='%s')", (unsigned)jpg_len, album);
            if (s_ready_cb) {
                s_ready_cb();
            }
        }
    }
}

esp_err_t artwork_manager_init(artwork_ready_cb_t cb)
{
    s_ready_cb = cb;
    s_mutex = xSemaphoreCreateMutex();
    s_bitmap_2bpp = heap_caps_calloc(1, ARTWORK_BUF_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_mutex || !s_bitmap_2bpp) {
        return ESP_ERR_NO_MEM;
    }
    xTaskCreatePinnedToCore(artwork_task, "artwork", 8192, NULL, 3, &s_task, 0);
    return ESP_OK;
}

void artwork_manager_request(const char *stream_title)
{
    if (!s_mutex) {
        return;
    }
    if (!stream_title || stream_title[0] == '\0') {
        artwork_manager_clear();
        return;
    }

    lock();
    if (strcmp(s_last_title, stream_title) == 0) {
        unlock();
        return;
    }
    strlcpy(s_last_title, stream_title, sizeof(s_last_title));
    strlcpy(s_pending_title, stream_title, sizeof(s_pending_title));
    s_has_bitmap = false;
    s_album_name[0] = '\0';
    s_artwork_url[0] = '\0';
    s_req_seq++;
    unlock();

    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

void artwork_manager_clear(void)
{
    if (!s_mutex) {
        return;
    }
    lock();
    s_last_title[0] = '\0';
    s_pending_title[0] = '\0';
    s_has_bitmap = false;
    s_album_name[0] = '\0';
    s_artwork_url[0] = '\0';
    s_req_seq++;
    unlock();
}

bool artwork_manager_get_info(char *album_out, size_t album_len,
                              char *url_out, size_t url_len)
{
    if (!s_mutex) {
        return false;
    }
    lock();
    bool has = s_has_bitmap;
    if (album_out && album_len > 0) {
        strlcpy(album_out, s_album_name, album_len);
    }
    if (url_out && url_len > 0) {
        strlcpy(url_out, s_artwork_url, url_len);
    }
    unlock();
    return has;
}

const uint8_t *artwork_manager_lock_bitmap(void)
{
    if (!s_mutex) {
        return NULL;
    }
    lock();
    if (!s_has_bitmap) {
        unlock();
        return NULL;
    }
    return s_bitmap_2bpp;
}

void artwork_manager_unlock_bitmap(void)
{
    if (s_mutex) {
        unlock();
    }
}
