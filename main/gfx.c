#include <string.h>
#include "gfx.h"
#include "epd_ssd1677.h"

#define GLYPH_SPACE     0
#define GLYPH_QUESTION  ('?' - 0x20)
#define GLYPH_DOT       ('.' - 0x20)
#define MAX_GLYPHS      512

static uint8_t *s_fb = NULL;
static bool s_rot180 = false;

void gfx_bind(uint8_t *frame_buffer, bool rotate_180)
{
    s_fb = frame_buffer;
    s_rot180 = rotate_180;
}

static inline void map_xy(int x, int y, int *px, int *py)
{
    // Panel native buffer is 800 (X) x 480 (Y). Upright landscape orientation
    // derived from the verified portrait mapping of the ESPresso firmware.
    if (s_rot180) {
        *px = (EPD_WIDTH - 1) - x;
        *py = y;
    } else {
        *px = x;
        *py = (EPD_HEIGHT - 1) - y;
    }
}

void gfx_pixel(int x, int y, gfx_color_t color)
{
    if (!s_fb || x < 0 || y < 0 || x >= GFX_WIDTH || y >= GFX_HEIGHT) {
        return;
    }
    int px, py;
    map_xy(x, y, &px, &py);
    uint8_t *p = &s_fb[py * EPD_2BPP_ROW_BYTES + (px >> 2)];
    int shift = 6 - ((px & 3) << 1);
    *p = (uint8_t)((*p & ~(0x03 << shift)) | ((color & 0x03) << shift));
}

void gfx_clear(gfx_color_t color)
{
    if (s_fb) {
        static const uint8_t fill_byte[4] = { 0x00, 0x55, 0xAA, 0xFF };
        memset(s_fb, fill_byte[color & 3], EPD_FRAME_BUFFER_SIZE);
    }
}

void gfx_fill_rect(int x, int y, int w, int h, gfx_color_t color)
{
    for (int j = y; j < y + h; j++) {
        for (int i = x; i < x + w; i++) {
            gfx_pixel(i, j, color);
        }
    }
}

void gfx_hline(int x, int y, int w, gfx_color_t color)
{
    gfx_fill_rect(x, y, w, 1, color);
}

void gfx_vline(int x, int y, int h, gfx_color_t color)
{
    gfx_fill_rect(x, y, 1, h, color);
}

void gfx_rect(int x, int y, int w, int h, int t, gfx_color_t color)
{
    gfx_fill_rect(x, y, w, t, color);
    gfx_fill_rect(x, y + h - t, w, t, color);
    gfx_fill_rect(x, y, t, h, color);
    gfx_fill_rect(x + w - t, y, t, h, color);
}

void gfx_draw_bitmap_2bpp(int x, int y, int w, int h, const uint8_t *bmp2bpp)
{
    if (!s_fb || !bmp2bpp || w <= 0 || h <= 0) {
        return;
    }
    int row_bytes = (w + 3) / 4;
    for (int r = 0; r < h; r++) {
        const uint8_t *row_ptr = bmp2bpp + r * row_bytes;
        for (int c = 0; c < w; c++) {
            uint8_t b = row_ptr[c >> 2];
            int shift = 6 - ((c & 3) << 1);
            gfx_color_t col = (gfx_color_t)((b >> shift) & 0x03);
            gfx_pixel(x + c, y + r, col);
        }
    }
}

// --- UTF-8 -> glyph index ---

static uint32_t utf8_next(const char **ps)
{
    const uint8_t *s = (const uint8_t *)*ps;
    uint32_t cp;
    int n;
    if (s[0] < 0x80) {
        cp = s[0];
        n = 1;
    } else if ((s[0] & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
        cp = ((s[0] & 0x1F) << 6) | (s[1] & 0x3F);
        n = 2;
    } else if ((s[0] & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
        cp = ((s[0] & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        n = 3;
    } else if ((s[0] & 0xF8) == 0xF0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 &&
               (s[3] & 0xC0) == 0x80) {
        cp = ((s[0] & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
        n = 4;
    } else {
        // Invalid UTF-8: treat the byte as Latin-1 (common in legacy ICY metadata)
        cp = s[0];
        n = 1;
    }
    *ps += n;
    return cp;
}

static uint8_t cp_to_glyph(uint32_t cp)
{
    if (cp >= 0x20 && cp <= 0x7E) {
        return (uint8_t)(cp - 0x20);
    }
    if (cp >= 0xA0 && cp <= 0xFF) {
        return (uint8_t)(95 + (cp - 0xA0));
    }
    switch (cp) {
    case '\t':
    case 0x3000:            // ideographic space
        return GLYPH_SPACE;
    case 0x2018: case 0x2019: case 0x201A: case 0x2032:
        return '\'' - 0x20;
    case 0x201C: case 0x201D: case 0x201E: case 0x2033:
        return '"' - 0x20;
    case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2015: case 0x2212:
        return '-' - 0x20;
    case 0x2026:
        return GLYPH_DOT;
    default:
        return GLYPH_QUESTION;
    }
}

static size_t decode(const char *s, uint8_t *out, size_t max)
{
    size_t n = 0;
    if (!s) {
        return 0;
    }
    while (*s && n < max) {
        uint32_t cp = utf8_next(&s);
        if (cp < 0x20 && cp != '\t') {
            continue;   // drop control chars
        }
        out[n++] = cp_to_glyph(cp);
    }
    return n;
}

size_t gfx_utf8_len(const char *s)
{
    uint8_t tmp[MAX_GLYPHS];
    return decode(s, tmp, sizeof(tmp));
}

static void draw_glyph(const gfx_font_t *f, int scale, int x, int y, uint8_t g, gfx_color_t color)
{
    const uint8_t *bm = f->bitmap + (size_t)g * f->row_bytes * f->height;
    for (int row = 0; row < f->height; row++) {
        const uint8_t *r = bm + row * f->row_bytes;
        for (int col = 0; col < f->width; col++) {
            if (r[col >> 3] & (0x80 >> (col & 7))) {
                if (scale == 1) {
                    gfx_pixel(x + col, y + row, color);
                } else {
                    gfx_fill_rect(x + col * scale, y + row * scale, scale, scale, color);
                }
            }
        }
    }
}

static int draw_glyphs(const gfx_font_t *f, int scale, int x, int y,
                       const uint8_t *g, size_t n, gfx_color_t color)
{
    int adv = f->width * scale;
    for (size_t i = 0; i < n; i++) {
        draw_glyph(f, scale, x, y, g[i], color);
        x += adv;
    }
    return x;
}

int gfx_text_width(const gfx_font_t *font, int scale, const char *s)
{
    return (int)gfx_utf8_len(s) * font->width * scale;
}

int gfx_text(const gfx_font_t *font, int scale, int x, int y, const char *s, gfx_color_t color)
{
    uint8_t g[MAX_GLYPHS];
    size_t n = decode(s, g, MAX_GLYPHS);
    return draw_glyphs(font, scale, x, y, g, n, color);
}

static void draw_line_box(const gfx_font_t *f, int scale, int x, int y, int w, gfx_align_t align,
                          const uint8_t *g, size_t n, bool force_ellipsis, gfx_color_t color)
{
    int adv = f->width * scale;
    size_t cap = (size_t)(w / adv);
    if (cap == 0) {
        return;
    }
    uint8_t buf[MAX_GLYPHS];
    size_t len = n;
    memcpy(buf, g, n);

    if (len > cap || force_ellipsis) {
        size_t keep = (cap >= 3) ? cap - 3 : 0;
        if (keep > len) {
            keep = len;
        }
        while (keep > 0 && buf[keep - 1] == GLYPH_SPACE) {
            keep--;
        }
        len = keep;
        for (int i = 0; i < 3 && len < cap; i++) {
            buf[len++] = GLYPH_DOT;
        }
    }

    int tw = (int)len * adv;
    int dx = x;
    if (align == GFX_ALIGN_CENTER) {
        dx = x + (w - tw) / 2;
    } else if (align == GFX_ALIGN_RIGHT) {
        dx = x + w - tw;
    }
    draw_glyphs(f, scale, dx, y, buf, len, color);
}

void gfx_text_box(const gfx_font_t *font, int scale, int x, int y, int w,
                  gfx_align_t align, const char *s, gfx_color_t color)
{
    uint8_t g[MAX_GLYPHS];
    size_t n = decode(s, g, MAX_GLYPHS);
    draw_line_box(font, scale, x, y, w, align, g, n, false, color);
}

int gfx_text_wrap(const gfx_font_t *font, int scale, int x, int y, int w, int max_lines,
                  int line_spacing, gfx_align_t align, const char *s, gfx_color_t color)
{
    uint8_t g[MAX_GLYPHS];
    size_t n = decode(s, g, MAX_GLYPHS);
    size_t cap = (size_t)(w / (font->width * scale));
    if (cap == 0 || max_lines <= 0) {
        return 0;
    }

    int line_h = font->height * scale + line_spacing;
    size_t pos = 0;
    int lines = 0;

    while (pos < n && lines < max_lines) {
        while (pos < n && g[pos] == GLYPH_SPACE) {
            pos++;   // skip leading spaces
        }
        if (pos >= n) {
            break;
        }
        size_t remain = n - pos;
        bool last_line = (lines == max_lines - 1);

        if (remain <= cap) {
            draw_line_box(font, scale, x, y + lines * line_h, w, align, &g[pos], remain, false, color);
            lines++;
            break;
        }
        if (last_line) {
            draw_line_box(font, scale, x, y + lines * line_h, w, align, &g[pos], remain, false, color);
            lines++;
            break;
        }
        // Break at the last space within capacity, else hard-break
        size_t brk = cap;
        for (size_t i = cap; i > cap / 3; i--) {
            if (g[pos + i] == GLYPH_SPACE) {
                brk = i;
                break;
            }
        }
        size_t len = brk;
        while (len > 0 && g[pos + len - 1] == GLYPH_SPACE) {
            len--;
        }
        draw_line_box(font, scale, x, y + lines * line_h, w, align, &g[pos], len, false, color);
        lines++;
        pos += brk;
    }
    return lines;
}
