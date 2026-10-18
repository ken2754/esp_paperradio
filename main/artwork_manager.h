#ifndef ARTWORK_MANAGER_H
#define ARTWORK_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#define ARTWORK_SIZE        144
#define ARTWORK_ROW_BYTES   (ARTWORK_SIZE / 4)                  // 36 bytes
#define ARTWORK_BUF_BYTES   (ARTWORK_ROW_BYTES * ARTWORK_SIZE)  // 5,184 bytes

typedef void (*artwork_ready_cb_t)(void);

esp_err_t artwork_manager_init(artwork_ready_cb_t cb);

/**
 * Requests asynchronous lookup, download, JPEG decode, and 4-grayscale dithering
 * for the given ICY StreamTitle ("Artist - Title").
 */
void artwork_manager_request(const char *stream_title);

/** Clears current artwork (e.g. on stop or station switch). */
void artwork_manager_clear(void);

/**
 * Copies the current 2bpp artwork (if available), album name, and artwork URL.
 * Returns true if valid artwork is present for the current track.
 */
bool artwork_manager_get_info(char *album_out, size_t album_len,
                              char *url_out, size_t url_len);

/**
 * Locks and returns a pointer to the internal 144x144 2bpp bitmap if available,
 * or NULL if no artwork is loaded. Must be paired with artwork_manager_unlock()
 * if non-NULL is returned.
 */
const uint8_t *artwork_manager_lock_bitmap(void);
void artwork_manager_unlock_bitmap(void);

#endif // ARTWORK_MANAGER_H
