#ifndef AUDIO_PLAYER_H
#define AUDIO_PLAYER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

typedef enum {
    AUDIO_STATE_STOPPED = 0,
    AUDIO_STATE_BUFFERING,   // waiting for pre-buffer / recovering from underrun
    AUDIO_STATE_PLAYING,
} audio_state_t;

typedef struct {
    int sample_rate;
    int channels;
    int bitrate_kbps;
} audio_format_t;

typedef void (*audio_state_cb_t)(audio_state_t state);

/**
 * Initializes I2S TX, ES8311 control, the PSRAM ring buffer and the MP3
 * decoder task (pinned to core 1).
 */
esp_err_t audio_player_init(audio_state_cb_t cb);

/** Clears the ring buffer and starts the decoder (enters BUFFERING). */
esp_err_t audio_player_start(void);

/** Stops decoding, mutes and powers down the amplifier. Blocks until idle. */
void audio_player_stop(void);

/**
 * Feeds compressed MP3 bytes into the ring buffer.
 * @return number of bytes accepted (may be < len on timeout).
 */
size_t audio_player_write(const uint8_t *data, size_t len, uint32_t timeout_ms);

/** Signals that no more data will arrive (lets the decoder drain). */
void audio_player_set_eos(void);

void audio_player_set_volume(int volume);   // 0..100

audio_state_t audio_player_get_state(void);
int audio_player_get_buffer_percent(void);
void audio_player_get_format(audio_format_t *out);

#endif // AUDIO_PLAYER_H
