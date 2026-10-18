#ifndef EPD_SSD1677_H
#define EPD_SSD1677_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/*
 * Waveshare 3.97" e-Paper (SSD1677), 800x480, 2bpp (4-level grayscale).
 * Logical 2bpp frame buffer layout:
 *   200 bytes per row, 480 rows (96,000 bytes total);
 *   4 pixels per byte (bits 7..6 = pixel 0, ..., bits 1..0 = pixel 3).
 *   0 = White, 1 = Light Gray, 2 = Dark Gray, 3 = Black.
 */
#define EPD_WIDTH                   800
#define EPD_HEIGHT                  480
#define EPD_PLANE_ROW_BYTES         (EPD_WIDTH / 8)                         // 100 bytes
#define EPD_PLANE_BUFFER_SIZE       (EPD_PLANE_ROW_BYTES * EPD_HEIGHT)      // 48,000 bytes
#define EPD_2BPP_ROW_BYTES          (EPD_WIDTH / 4)                         // 200 bytes
#define EPD_FRAME_BUFFER_SIZE       (EPD_2BPP_ROW_BYTES * EPD_HEIGHT)       // 96,000 bytes

#define EPD_FULL_REFRESH_INTERVAL   15

esp_err_t epd_init(void);

/**
 * Pushes a 2bpp (4-grayscale, 96,000-byte) frame to the panel using the
 * SSD1677 4-Gray waveform LUT (0x1A=0x5A, 0x22=0xD7).
 */
esp_err_t epd_display(const uint8_t *frame, bool partial);

/** Forces the next epd_display() to perform a full reset before refresh. */
void epd_request_full_refresh(void);

/** Puts the controller in deep sleep (panel keeps the image). */
void epd_sleep(void);

#endif // EPD_SSD1677_H
