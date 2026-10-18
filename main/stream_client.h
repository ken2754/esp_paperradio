#ifndef STREAM_CLIENT_H
#define STREAM_CLIENT_H

#include <stdbool.h>
#include "esp_err.h"

#define STREAM_META_MAX_LEN   160

typedef struct {
    char station_name[64];
    char genre[48];
    int bitrate_kbps;
} stream_icy_headers_t;

typedef void (*stream_metadata_cb_t)(const char *title);
typedef void (*stream_headers_cb_t)(const stream_icy_headers_t *hdrs);
typedef void (*stream_error_cb_t)(const char *message);

esp_err_t stream_client_init(stream_metadata_cb_t meta_cb,
                             stream_headers_cb_t hdr_cb,
                             stream_error_cb_t err_cb);

/** Starts streaming from `url` (stops any active stream first). */
esp_err_t stream_client_start(const char *url);

/** Stops the active stream and waits until the HTTP task is idle. */
void stream_client_stop(void);

bool stream_client_is_running(void);

#endif // STREAM_CLIENT_H
