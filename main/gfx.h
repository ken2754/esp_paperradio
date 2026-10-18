#ifndef GFX_H
#define GFX_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * 2bpp (4-level grayscale) graphics on top of the EPD frame buffer.
 * Logical coordinates are landscape: x = 0..799 (right), y = 0..479 (down).
 */
#define GFX_WIDTH   800
#define GFX_HEIGHT  480

typedef enum {
    GFX_WHITE      = 0,
    GFX_LIGHT_GRAY = 1,
    GFX_DARK_GRAY  = 2,
    GFX_BLACK      = 3,
} gfx_color_t;

typedef struct {
    uint8_t width;
    uint8_t height;
    uint8_t row_bytes;
    const uint8_t *bitmap;   // glyphs for U+0020..U+007E then U+00A0..U+00FF
} gfx_font_t;

extern const gfx_font_t font_terminus_16;   //  8 x 16
extern const gfx_font_t font_terminus_24;   // 12 x 24
extern const gfx_font_t font_terminus_32;   // 16 x 32

typedef enum {
    GFX_ALIGN_LEFT = 0,
    GFX_ALIGN_CENTER,
    GFX_ALIGN_RIGHT,
} gfx_align_t;

/** Binds the 2bpp frame buffer (EPD_FRAME_BUFFER_SIZE = 96,000 bytes). */
void gfx_bind(uint8_t *frame_buffer, bool rotate_180);

void gfx_clear(gfx_color_t color);
void gfx_pixel(int x, int y, gfx_color_t color);
void gfx_fill_rect(int x, int y, int w, int h, gfx_color_t color);
void gfx_rect(int x, int y, int w, int h, int thickness, gfx_color_t color);
void gfx_hline(int x, int y, int w, gfx_color_t color);
void gfx_vline(int x, int y, int h, gfx_color_t color);

/**
 * Draws a packed 2bpp (4-grayscale) bitmap of dimensions `w` x `h` at `(x, y)`.
 * `w` must be a multiple of 4; each byte in `bmp2bpp` holds 4 horizontal pixels
 * (bits 7..6 = pixel 0, ..., bits 1..0 = pixel 3) using `gfx_color_t` values 0..3.
 */
void gfx_draw_bitmap_2bpp(int x, int y, int w, int h, const uint8_t *bmp2bpp);

/** Number of glyphs in a UTF-8 string (unsupported code points count as 1). */
size_t gfx_utf8_len(const char *s);

/** Pixel width of a UTF-8 string with the given font and integer scale. */
int gfx_text_width(const gfx_font_t *font, int scale, const char *s);

/**
 * Draws UTF-8 text with top-left at (x, y). Code points outside Latin-1 are
 * drawn as '?'. Returns the x coordinate after the last glyph.
 */
int gfx_text(const gfx_font_t *font, int scale, int x, int y, const char *s, gfx_color_t color);

/**
 * Draws a single line of text in a box of width `w`, aligned and truncated
 * with "..." when it does not fit.
 */
void gfx_text_box(const gfx_font_t *font, int scale, int x, int y, int w,
                  gfx_align_t align, const char *s, gfx_color_t color);

/**
 * Word-wraps text into a box; at most `max_lines` lines are drawn, the last
 * line is truncated with "..." if needed. Returns the number of lines drawn.
 */
int gfx_text_wrap(const gfx_font_t *font, int scale, int x, int y, int w, int max_lines,
                  int line_spacing, gfx_align_t align, const char *s, gfx_color_t color);

#endif // GFX_H
