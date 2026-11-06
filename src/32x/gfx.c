/* gfx.c - the canvas and its drawing (see gfx.h). Drawing happens in SDRAM,
 * which the SH-2 reaches far faster than the frame buffer; the finished
 * picture goes over 32 bits at a time. */
#include "gfx.h"
#include "hw.h"
#include "font.h"

static uint8_t canvas[SCREEN_W * SCREEN_H] __attribute__((aligned(16)));

static const uint16_t palette[COL_COUNT] = {
    /* BGR555 */
    [COL_BG]     = 0x0000 | (4 << 10) | (2 << 5) | 2,
    [COL_TEXT]   = 0x7FFF & ((28 << 10) | (29 << 5) | 29),
    [COL_DIM]    = (15 << 10) | (14 << 5) | 13,
    [COL_HI]     = (6 << 10) | (28 << 5) | 31,          /* yellow */
    [COL_HEAD]   = (31 << 10) | (26 << 5) | 10,         /* light blue */
    [COL_PANEL]  = (9 << 10) | (5 << 5) | 4,
    [COL_RED]    = (6 << 10) | (6 << 5) | 30,
    [COL_GREEN]  = (10 << 10) | (28 << 5) | 10,
    [COL_ORANGE] = (4 << 10) | (18 << 5) | 31,
    [COL_CURSOR] = (20 << 10) | (9 << 5) | 6,
    [COL_LINE]   = (12 << 10) | (9 << 5) | 8,
    [COL_BLUE]   = (31 << 10) | (14 << 5) | 6,
    [COL_PANEL2] = (14 << 10) | (8 << 5) | 6,
};

void gfx_init(void)
{
    hw_video_init();
    hw_load_palette(palette, COL_COUNT);
}

void gfx_clear(uint8_t c)
{
    uint32_t v = c * 0x01010101u;
    uint32_t *p = (uint32_t *)(void *)canvas;
    for (int i = 0; i < SCREEN_W * SCREEN_H / 4; i++)
        p[i] = v;
}

void gfx_rect(int x, int y, int w, int h, uint8_t c)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > SCREEN_W) w = SCREEN_W - x;
    if (y + h > SCREEN_H) h = SCREEN_H - y;
    for (int r = 0; r < h; r++) {
        uint8_t *p = canvas + (y + r) * SCREEN_W + x;
        for (int i = 0; i < w; i++)
            p[i] = c;
    }
}

void gfx_frame(int x, int y, int w, int h, uint8_t c)
{
    gfx_rect(x, y, w, 1, c);
    gfx_rect(x, y + h - 1, w, 1, c);
    gfx_rect(x, y, 1, h, c);
    gfx_rect(x + w - 1, y, 1, h, c);
}

static inline void plot(int x, int y, uint8_t c)
{
    if ((unsigned)x < SCREEN_W && (unsigned)y < SCREEN_H)
        canvas[y * SCREEN_W + x] = c;
}

void gfx_line(int x0, int y0, int x1, int y1, uint8_t c)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
    int e = dx + dy;
    for (;;) {
        plot(x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * e;
        if (e2 >= dy) { e += dy; x0 += sx; }
        if (e2 <= dx) { e += dx; y0 += sy; }
    }
}

static void glyph(int x, int y, int ch, uint8_t c)
{
    if (ch >= 'a' && ch <= 'z') ch -= 32;
    if (ch == '{') ch = '[';
    if (ch == '}') ch = ']';
    if (ch < 32 || ch > 95) ch = '?';
    const unsigned char *g = font5x7[ch - 32];
    for (int r = 0; r < 7; r++) {
        unsigned bits = g[r];
        if (!bits) continue;
        uint8_t *p = canvas + (y + r) * SCREEN_W + x;
        if ((unsigned)(y + r) >= SCREEN_H) continue;
        for (int i = 0; i < 5; i++)
            if (bits & (0x10 >> i) && (unsigned)(x + i) < SCREEN_W)
                p[i] = c;
    }
}

int gfx_text_n(int x, int y, const char *s, int max, uint8_t c)
{
    int n = 0;
    for (; *s && n < max; s++, n++, x += CHAR_W)
        if (*s != ' ')
            glyph(x, y, (uint8_t)*s, c);
    return n;
}

void gfx_text(int x, int y, const char *s, uint8_t c)
{
    gfx_text_n(x, y, s, 1000, c);
}

char *fmt_num(char *o, int v)
{
    char t[12];
    int n = 0;
    if (v < 0) { *o++ = '-'; v = -v; }
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) *o++ = t[--n];
    return o;
}

void gfx_num(int x, int y, int v, uint8_t c)
{
    char t[12];
    *fmt_num(t, v) = 0;
    gfx_text(x, y, t, c);
}

void gfx_present(void)
{
    volatile uint32_t *fb = (volatile uint32_t *)(void *)hw_row(0);
    const uint32_t *s = (const uint32_t *)(void *)canvas;
    for (int i = 0; i < SCREEN_W * SCREEN_H / 4; i++) {
        fb[i] = s[i];
        if (!(i & 1023) && poll_hook) poll_hook();
    }
    hw_flip();
}
