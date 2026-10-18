#include <string.h>
#include "audio_player.h"
#include "board_pins.h"
#include "bsp_i2c.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "es8311_codec.h"
#include "mp3dec.h"

static const char *TAG = "AUDIO";

#define RING_BUFFER_SIZE    (CONFIG_PAPERRADIO_RING_BUFFER_KB * 1024)
#define PREBUFFER_BYTES     (CONFIG_PAPERRADIO_PREBUFFER_KB * 1024)
#define DEC_INBUF_SIZE      (MAINBUF_SIZE * 4)   // 7760 bytes
#define DEC_PCM_MAX_SAMPLES (MAX_NGRAN * MAX_NCHAN * MAX_NSAMP) // 2304 int16 samples

// --- Ring buffer in PSRAM ---
static uint8_t *s_ring_buf = NULL;
static size_t s_ring_head = 0;
static size_t s_ring_tail = 0;
static size_t s_ring_count = 0;
static SemaphoreHandle_t s_ring_mutex = NULL;
static SemaphoreHandle_t s_ring_data_sem = NULL;
static SemaphoreHandle_t s_ring_space_sem = NULL;

// --- Hardware handles ---
static i2s_chan_handle_t s_tx_chan = NULL;
static i2s_chan_handle_t s_rx_chan = NULL;
static esp_codec_dev_handle_t s_codec_dev = NULL;
static bool s_codec_opened = false;
static int s_current_rate = 0;

// --- Player state ---
static volatile audio_state_t s_state = AUDIO_STATE_STOPPED;
static volatile bool s_run_requested = false;
static volatile bool s_eos = false;
static volatile int s_volume = 40;
static audio_format_t s_format = { .sample_rate = 44100, .channels = 2, .bitrate_kbps = 128 };
static audio_state_cb_t s_state_cb = NULL;
static TaskHandle_t s_dec_task = NULL;
static SemaphoreHandle_t s_stop_done_sem = NULL;

static void set_state(audio_state_t st)
{
    if (s_state != st) {
        s_state = st;
        if (s_state_cb) {
            s_state_cb(st);
        }
    }
}

static void pa_enable(bool en)
{
    gpio_set_level(PIN_AUDIO_CTR, en ? 1 : 0);
}

static void ring_reset(void)
{
    xSemaphoreTake(s_ring_mutex, portMAX_DELAY);
    s_ring_head = 0;
    s_ring_tail = 0;
    s_ring_count = 0;
    xSemaphoreGive(s_ring_mutex);
}

static size_t ring_available(void)
{
    xSemaphoreTake(s_ring_mutex, portMAX_DELAY);
    size_t n = s_ring_count;
    xSemaphoreGive(s_ring_mutex);
    return n;
}

static size_t ring_read(uint8_t *dst, size_t len)
{
    xSemaphoreTake(s_ring_mutex, portMAX_DELAY);
    size_t to_read = (len < s_ring_count) ? len : s_ring_count;
    size_t first = RING_BUFFER_SIZE - s_ring_tail;
    if (first > to_read) {
        first = to_read;
    }
    memcpy(dst, s_ring_buf + s_ring_tail, first);
    if (to_read > first) {
        memcpy(dst + first, s_ring_buf, to_read - first);
    }
    s_ring_tail = (s_ring_tail + to_read) % RING_BUFFER_SIZE;
    s_ring_count -= to_read;
    xSemaphoreGive(s_ring_mutex);
    if (to_read > 0) {
        xSemaphoreGive(s_ring_space_sem);
    }
    return to_read;
}

size_t audio_player_write(const uint8_t *data, size_t len, uint32_t timeout_ms)
{
    if (!s_ring_buf || !data || len == 0) {
        return 0;
    }
    size_t written = 0;
    TickType_t start = xTaskGetTickCount();

    while (written < len && s_run_requested) {
        xSemaphoreTake(s_ring_mutex, portMAX_DELAY);
        size_t free_space = RING_BUFFER_SIZE - s_ring_count;
        if (free_space > 0) {
            size_t chunk = len - written;
            if (chunk > free_space) {
                chunk = free_space;
            }
            size_t first = RING_BUFFER_SIZE - s_ring_head;
            if (first > chunk) {
                first = chunk;
            }
            memcpy(s_ring_buf + s_ring_head, data + written, first);
            if (chunk > first) {
                memcpy(s_ring_buf, data + written + first, chunk - first);
            }
            s_ring_head = (s_ring_head + chunk) % RING_BUFFER_SIZE;
            s_ring_count += chunk;
            written += chunk;
            xSemaphoreGive(s_ring_mutex);
            xSemaphoreGive(s_ring_data_sem);
        } else {
            xSemaphoreGive(s_ring_mutex);
            uint32_t elapsed = pdTICKS_TO_MS(xTaskGetTickCount() - start);
            if (elapsed >= timeout_ms) {
                break;
            }
            uint32_t wait = timeout_ms - elapsed;
            if (wait > 50) {
                wait = 50;
            }
            xSemaphoreTake(s_ring_space_sem, pdMS_TO_TICKS(wait));
        }
    }
    return written;
}

// Smooth C1-continuous rational soft-knee saturator:
// Linear up to T=24576 (75% FS, derivative=1.0), smoothly approaches 32767 above T without hard flat-topping.
static inline int16_t soft_limit_s16(int32_t x)
{
    const int32_t T = 24576;
    const int32_t H = 32767 - T; // 8191
    if (x >= -T && x <= T) {
        return (int16_t)x;
    }
    int32_t sign = (x < 0) ? -1 : 1;
    int32_t over = (x < 0) ? (-x - T) : (x - T);
    int32_t y = T + (H * over) / (over + H);
    if (y > 32767) {
        y = 32767;
    }
    return (int16_t)(sign * y);
}

static esp_codec_dev_vol_map_t s_vol_curve[] = {
    { .vol = 0,   .db_value = -50.0f },
    { .vol = 80,  .db_value =   0.0f },
    { .vol = 100, .db_value =  +3.0f },
};

static esp_err_t codec_configure_rate(int sample_rate)
{
    if (!s_codec_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_codec_opened && s_current_rate == sample_rate) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "opening ES8311 at %d Hz (16-bit mono)", sample_rate);
    if (bsp_i2c_lock(500)) {
        if (s_codec_opened) {
            esp_codec_dev_close(s_codec_dev);
            s_codec_opened = false;
            i2s_channel_enable(s_tx_chan);
            i2s_channel_enable(s_rx_chan);
        }
        esp_codec_dev_sample_info_t fs = {
            .sample_rate = (uint32_t)sample_rate,
            .channel = 1,
            .bits_per_sample = 16,
        };
        int ret = esp_codec_dev_open(s_codec_dev, &fs);
        if (ret == ESP_CODEC_DEV_OK) {
            s_codec_opened = true;
            s_current_rate = sample_rate;
            esp_codec_dev_vol_curve_t curve = {
                .count = sizeof(s_vol_curve) / sizeof(s_vol_curve[0]),
                .vol_map = s_vol_curve,
            };
            esp_codec_dev_set_vol_curve(s_codec_dev, &curve);
            esp_codec_dev_set_out_vol(s_codec_dev, s_volume);
        } else {
            ESP_LOGE(TAG, "esp_codec_dev_open(%d) failed: %d", sample_rate, ret);
        }
        bsp_i2c_unlock();
    }
    return s_codec_opened ? ESP_OK : ESP_FAIL;
}

static void mp3_decode_task(void *arg)
{
    (void)arg;
    uint8_t *inbuf = heap_caps_malloc(DEC_INBUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int16_t *pcm = heap_caps_malloc(DEC_PCM_MAX_SAMPLES * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!inbuf || !pcm) {
        ESP_LOGE(TAG, "decoder buffer alloc failed");
        vTaskDelete(NULL);
        return;
    }

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!s_run_requested) {
            xSemaphoreGive(s_stop_done_sem);
            continue;
        }

        HMP3Decoder hmp3 = MP3InitDecoder();
        if (!hmp3) {
            ESP_LOGE(TAG, "MP3InitDecoder failed");
            set_state(AUDIO_STATE_STOPPED);
            xSemaphoreGive(s_stop_done_sem);
            continue;
        }

        int bytes_left = 0;
        uint8_t *read_ptr = inbuf;
        bool prebuffered = false;
        set_state(AUDIO_STATE_BUFFERING);
        pa_enable(false);

        while (s_run_requested) {
            // Pre-buffer check before starting or after an underrun
            if (!prebuffered) {
                size_t avail = ring_available();
                if (avail >= PREBUFFER_BYTES || s_eos) {
                    prebuffered = true;
                    set_state(AUDIO_STATE_PLAYING);
                    pa_enable(s_volume > 0);
                    ESP_LOGI(TAG, "pre-buffer ready (%u bytes), starting playback", (unsigned)avail);
                } else {
                    xSemaphoreTake(s_ring_data_sem, pdMS_TO_TICKS(40));
                    continue;
                }
            }

            // Refill decoder input buffer
            if (bytes_left < MAINBUF_SIZE * 2) {
                if (bytes_left > 0 && read_ptr != inbuf) {
                    memmove(inbuf, read_ptr, bytes_left);
                }
                read_ptr = inbuf;
                size_t space = DEC_INBUF_SIZE - bytes_left;
                size_t got = ring_read(inbuf + bytes_left, space);
                bytes_left += (int)got;

                if (bytes_left < MAINBUF_SIZE && !s_eos) {
                    // Underrun: mute PA and re-enter buffering
                    pa_enable(false);
                    prebuffered = false;
                    set_state(AUDIO_STATE_BUFFERING);
                    ESP_LOGW(TAG, "stream underrun, re-buffering...");
                    xSemaphoreTake(s_ring_data_sem, pdMS_TO_TICKS(50));
                    continue;
                }
                if (bytes_left == 0 && s_eos) {
                    break;
                }
            }

            // Skip ID3v2 header if present at stream start
            if (bytes_left >= 10 && memcmp(read_ptr, "ID3", 3) == 0) {
                uint32_t tag_sz = ((read_ptr[6] & 0x7F) << 21) |
                                  ((read_ptr[7] & 0x7F) << 14) |
                                  ((read_ptr[8] & 0x7F) << 7)  |
                                  (read_ptr[9] & 0x7F);
                uint32_t skip = tag_sz + 10;
                while (skip > 0 && s_run_requested) {
                    if ((uint32_t)bytes_left <= skip) {
                        skip -= bytes_left;
                        bytes_left = (int)ring_read(inbuf, DEC_INBUF_SIZE);
                        read_ptr = inbuf;
                        if (bytes_left == 0) {
                            vTaskDelay(pdMS_TO_TICKS(10));
                        }
                    } else {
                        read_ptr += skip;
                        bytes_left -= (int)skip;
                        skip = 0;
                    }
                }
                continue;
            }

            int offset = MP3FindSyncWord(read_ptr, bytes_left);
            if (offset < 0) {
                bytes_left = 0;
                read_ptr = inbuf;
                continue;
            }
            if (offset > 0) {
                read_ptr += offset;
                bytes_left -= offset;
            }

            int err = MP3Decode(hmp3, &read_ptr, &bytes_left, pcm, 0);
            if (err == ERR_MP3_NONE) {
                MP3FrameInfo info;
                MP3GetLastFrameInfo(hmp3, &info);
                if (info.samprate <= 0 || info.outputSamps <= 0) {
                    continue;
                }
                s_format.sample_rate = info.samprate;
                s_format.channels = info.nChans;
                if (info.bitrate > 0) {
                    s_format.bitrate_kbps = info.bitrate / 1000;
                }

                if (codec_configure_rate(info.samprate) != ESP_OK) {
                    vTaskDelay(pdMS_TO_TICKS(20));
                    continue;
                }

                // Downmix stereo -> mono with +2.2 dB makeup gain (~1.29x = 330/256)
                // and smooth C1 soft-knee limiting above 75% FS to prevent harsh clipping.
                int mono_samples = (info.nChans == 2) ? (info.outputSamps / 2) : info.outputSamps;
                if (info.nChans == 2) {
                    for (int i = 0; i < mono_samples; i++) {
                        int32_t sum = (int32_t)pcm[2 * i] + (int32_t)pcm[2 * i + 1];
                        pcm[i] = soft_limit_s16((sum * 330) >> 9);
                    }
                } else {
                    for (int i = 0; i < mono_samples; i++) {
                        pcm[i] = soft_limit_s16(((int32_t)pcm[i] * 330) >> 8);
                    }
                }

                pa_enable(s_volume > 0);
                esp_codec_dev_write(s_codec_dev, pcm, mono_samples * sizeof(int16_t));
            } else if (err == ERR_MP3_MAINDATA_UNDERFLOW || err == ERR_MP3_INDATA_UNDERFLOW) {
                if (s_eos && ring_available() == 0) {
                    break;
                }
                // Wait for more bytes on next iteration
                if (bytes_left < MAINBUF_SIZE) {
                    xSemaphoreTake(s_ring_data_sem, pdMS_TO_TICKS(20));
                }
            } else {
                // Corrupt frame: advance 1 byte and search next sync word
                if (bytes_left > 0) {
                    read_ptr++;
                    bytes_left--;
                }
            }
        }

        pa_enable(false);
        MP3FreeDecoder(hmp3);
        set_state(AUDIO_STATE_STOPPED);
        xSemaphoreGive(s_stop_done_sem);
    }
}

static esp_err_t init_i2s_and_codec(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 8;
    chan_cfg.dma_frame_num = 480;
    chan_cfg.auto_clear = true;
    esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx_chan, &s_rx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(err));
        return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100),
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_16BIT,
            .slot_mode = I2S_SLOT_MODE_MONO,
            .slot_mask = I2S_STD_SLOT_LEFT,
            .ws_width = I2S_DATA_BIT_WIDTH_16BIT,
            .ws_pol = false,
            .bit_shift = true,
            .left_align = true,
            .big_endian = false,
            .bit_order_lsb = false,
        },
        .gpio_cfg = {
            .mclk = I2S_MCLK_GPIO,
            .bclk = I2S_SCLK_BCLK_GPIO,
            .ws = I2S_LRCK_WS_GPIO,
            .dout = I2S_DSOUT_DOUT_GPIO,
            .din = I2S_DSDIN_DIN_GPIO,
        },
    };
    err = i2s_channel_init_std_mode(s_tx_chan, &std_cfg);
    if (err != ESP_OK) {
        return err;
    }
    err = i2s_channel_init_std_mode(s_rx_chan, &std_cfg);
    if (err != ESP_OK) {
        return err;
    }
    // esp_codec_dev_open's _i2s_data_set_fmt unconditionally calls i2s_channel_disable()
    // before reconfiguring clocks; pre-enabling avoids "the channel has not been enabled yet"
    i2s_channel_enable(s_tx_chan);
    i2s_channel_enable(s_rx_chan);

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = I2C_MASTER_PORT,
        .addr = (I2C_ADDR_ES8311 << 1),   // esp_codec_dev expects 8-bit shifted addr (0x30)
        .bus_handle = bsp_i2c_get_bus(),
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_0,
        .rx_handle = s_rx_chan,
        .tx_handle = s_tx_chan,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);

    es8311_codec_cfg_t es_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .master_mode = false,
        .use_mclk = true,
        .pa_pin = -1,    // NS4150B controlled explicitly via PIN_AUDIO_CTR
        .hw_gain = { .pa_voltage = 3.3f, .codec_dac_voltage = 3.3f },
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&es_cfg);
    if (!codec_if) {
        ESP_LOGE(TAG, "es8311_codec_new failed");
        return ESP_FAIL;
    }

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
        .codec_if = codec_if,
        .data_if = data_if,
    };
    s_codec_dev = esp_codec_dev_new(&dev_cfg);
    return s_codec_dev ? ESP_OK : ESP_FAIL;
}

esp_err_t audio_player_init(audio_state_cb_t cb)
{
    s_state_cb = cb;

    // Configure NS4150B PA_EN pin, keep muted initially
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_AUDIO_CTR),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io);
    pa_enable(false);

    s_ring_buf = heap_caps_malloc(RING_BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_ring_buf) {
        ESP_LOGE(TAG, "failed to allocate %d KB PSRAM ring buffer", CONFIG_PAPERRADIO_RING_BUFFER_KB);
        return ESP_ERR_NO_MEM;
    }
    s_ring_mutex = xSemaphoreCreateMutex();
    s_ring_data_sem = xSemaphoreCreateBinary();
    s_ring_space_sem = xSemaphoreCreateBinary();
    s_stop_done_sem = xSemaphoreCreateBinary();

    esp_err_t err = init_i2s_and_codec();
    if (err != ESP_OK) {
        return err;
    }

    xTaskCreatePinnedToCore(mp3_decode_task, "mp3_dec", 8192, NULL, 8, &s_dec_task, 1);
    ESP_LOGI(TAG, "initialized (ring=%d KB, prebuf=%d KB)",
             CONFIG_PAPERRADIO_RING_BUFFER_KB, CONFIG_PAPERRADIO_PREBUFFER_KB);
    return ESP_OK;
}

esp_err_t audio_player_start(void)
{
    audio_player_stop();
    ring_reset();
    s_eos = false;
    s_run_requested = true;
    xSemaphoreTake(s_stop_done_sem, 0);
    xTaskNotifyGive(s_dec_task);
    return ESP_OK;
}

void audio_player_stop(void)
{
    if (!s_run_requested && s_state == AUDIO_STATE_STOPPED) {
        return;
    }
    s_run_requested = false;
    s_eos = true;
    pa_enable(false);
    xSemaphoreGive(s_ring_data_sem);
    xSemaphoreGive(s_ring_space_sem);
    xSemaphoreTake(s_stop_done_sem, pdMS_TO_TICKS(1500));
    ring_reset();
}

void audio_player_set_eos(void)
{
    s_eos = true;
    xSemaphoreGive(s_ring_data_sem);
}

void audio_player_set_volume(int volume)
{
    if (volume < 0) {
        volume = 0;
    }
    if (volume > 100) {
        volume = 100;
    }
    s_volume = volume;
    if (s_codec_dev) {
        if (bsp_i2c_lock(200)) {
            esp_codec_dev_set_out_vol(s_codec_dev, volume);
            bsp_i2c_unlock();
        }
    }
    if (s_state == AUDIO_STATE_PLAYING) {
        pa_enable(volume > 0);
    }
}

audio_state_t audio_player_get_state(void)
{
    return s_state;
}

int audio_player_get_buffer_percent(void)
{
    size_t n = ring_available();
    int pct = (int)((n * 100) / RING_BUFFER_SIZE);
    return (pct > 100) ? 100 : pct;
}

void audio_player_get_format(audio_format_t *out)
{
    if (out) {
        *out = s_format;
    }
}
