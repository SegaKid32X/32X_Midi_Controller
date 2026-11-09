/* gfx_md.c - the Genesis-only build's canvas (the API of src/32x/gfx.h).
 *
 * The Mega Drive has no frame buffer: the screen is 40 x 28 tiles of 8 x 8
 * pixels, 16 colours. Plane A shows tiles 0-1119 in order, so a 4-bit canvas
 * kept in work RAM in tile order (35 KB) becomes the picture by copying it
 * into VRAM. gfx_present() copies only the tiles that changed (a hash per
 * tile), with the CPU rather than DMA so the GenMDM's interrupt and the
 * port 2 receiver keep running meanwhile (poll_hook).
 */
#include "gfx.h"
#include "hw.h"
#include "font.h"

#define VDP_DATA32  (*(volatile uint32_t *)0xC00000)
#define VDP_DATA    (*(volatile uint16_t *)0xC00000)
#define VDP_CTRL    (*(volatile uint16_t *)0xC00004)
#define VDP_CTRL32  (*(volatile uint32_t *)0xC00004)

#define TW      40
#define TH      28
#define NTILES  (TW * TH)
#define BLANK   NTILES                  /* an empty tile after the canvas */
#define DRAWN   0xFF                    /* pixels, changed since the last gfx_present() */
#define CLEAN   0xFE                    /* pixels, as VRAM has them */

/* Most of the screen is plain colour (backgrounds, panels): such a tile is
 * kept as just its colour (solid[t]) and only becomes pixels when something
 * is drawn on it; gfx_present() hashes only the drawn ones. */
static uint32_t canvas[NTILES * 8];     /* tile t, row r: canvas[t * 8 + r], pixel 0 in bits 28-31 */
static uint8_t solid[NTILES];           /* a colour, or DRAWN / CLEAN (the pixels are in canvas) */
static uint32_t hash[NTILES];           /* what VRAM holds */
static int first = 1;
uint16_t gfx_draw_frames, gfx_present_frames;   /* (test telemetry: the last redraw) */
static uint16_t clear_at;
extern uint16_t md_vblanks(void);

/* the font's 5 bits (bit 4: the left pixel) as nibbles, shifted to pixel s
 * of a tile row (hi) and what spills into the next tile (lo) */
#define E(b) ((((b) & 16) ? 0xF0000000u : 0) | (((b) & 8) ? 0x0F000000u : 0) | \
              (((b) & 4) ? 0x00F00000u : 0) | (((b) & 2) ? 0x000F0000u : 0) | (((b) & 1) ? 0x0000F000u : 0))
#define HI(b, s) (E(b) >> (4 * (s)))
#define LO(b, s) ((s) > 3 ? E(b) << (32 - 4 * (s)) : 0u)
#define X32(M, s) { M(0,s), M(1,s), M(2,s), M(3,s), M(4,s), M(5,s), M(6,s), M(7,s), M(8,s), M(9,s), \
    M(10,s), M(11,s), M(12,s), M(13,s), M(14,s), M(15,s), M(16,s), M(17,s), M(18,s), M(19,s), M(20,s), \
    M(21,s), M(22,s), M(23,s), M(24,s), M(25,s), M(26,s), M(27,s), M(28,s), M(29,s), M(30,s), M(31,s) }
static const uint32_t glyph_hi[8][32] = { X32(HI,0), X32(HI,1), X32(HI,2), X32(HI,3),
                                          X32(HI,4), X32(HI,5), X32(HI,6), X32(HI,7) };
static const uint32_t glyph_lo[8][32] = { X32(LO,0), X32(LO,1), X32(LO,2), X32(LO,3),
                                          X32(LO,4), X32(LO,5), X32(LO,6), X32(LO,7) };

static inline uint32_t fill_of(unsigned c) { return (c & 15) * 0x11111111u; }

/* tile t as pixels, before drawing on it */
static inline uint32_t *tile(int t)
{
    uint32_t *p = canvas + t * 8;
    if (solid[t] == CLEAN) {
        solid[t] = DRAWN;
    } else if (solid[t] != DRAWN) {
        uint32_t v = fill_of(solid[t]);
        p[0] = v; p[1] = v; p[2] = v; p[3] = v; p[4] = v; p[5] = v; p[6] = v; p[7] = v;
        solid[t] = DRAWN;
    }
    return p;
}

/* (rotate, add a shifted copy: no multiplies on a 68000, and no blind spot
 * for the repeating nibbles of plain colours) */
static uint32_t hash_tile(const uint32_t *p)
{
    uint32_t h = 0x2545F491u;
    for (int k = 0; k < 8; k++) {
        h ^= p[k];
        h = (h << 5 | h >> 27) + (h >> 13);
    }
    return h;
}

static uint32_t solid_hash(unsigned c)
{
    uint32_t v = fill_of(c), p[8] = { v, v, v, v, v, v, v, v };
    return hash_tile(p);
}

static const uint16_t palette[COL_COUNT] = {
    /* (the 32X build's BGR555 colours; turned into the Mega Drive's 3 bits) */
    [COL_BG]     = 0x0000 | (4 << 10) | (2 << 5) | 2,
    [COL_TEXT]   = 0x7FFF & ((28 << 10) | (29 << 5) | 29),
    [COL_DIM]    = (15 << 10) | (14 << 5) | 13,
    [COL_HI]     = (6 << 10) | (28 << 5) | 31,
    [COL_HEAD]   = (31 << 10) | (26 << 5) | 10,
    [COL_PANEL]  = (9 << 10) | (5 << 5) | 4,
    [COL_RED]    = (6 << 10) | (6 << 5) | 30,
    [COL_GREEN]  = (10 << 10) | (28 << 5) | 10,
    [COL_ORANGE] = (4 << 10) | (18 << 5) | 31,
    [COL_CURSOR] = (20 << 10) | (9 << 5) | 6,
    [COL_LINE]   = (12 << 10) | (9 << 5) | 8,
    [COL_BLUE]   = (31 << 10) | (14 << 5) | 6,
    [COL_PANEL2] = (14 << 10) | (8 << 5) | 6,
};

static uint16_t md_colour(uint16_t c)
{
    unsigned r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    return (uint16_t)((r >> 2) << 1 | (g >> 2) << 5 | (b >> 2) << 9);
}

static void vram_write_at(unsigned addr)
{
    VDP_CTRL32 = 0x40000000u | (uint32_t)(addr & 0x3FFF) << 16 | (addr >> 14);
}

void gfx_init(void)
{
    static const uint16_t regs[] = {
        0x8004, 0x8164,         /* no HINT; display on, the vertical interrupt (frames), mode 5 */
        0x8230, 0x8407,         /* plane A 0xC000, plane B 0xE000 */
        0x8578, 0x8700,         /* sprites 0xF000; backdrop: colour 0 */
        0x8B08,                 /* the external interrupt (port 2's TH) on */
        0x8C81, 0x8D3E,         /* H40; H scroll table 0xF800 */
        0x8F02, 0x9001,         /* auto-increment 2; planes 64 x 32 */
        0x9100, 0x9200,         /* no window */
    };
    for (unsigned i = 0; i < sizeof regs / sizeof regs[0]; i++) VDP_CTRL = regs[i];
    vram_write_at(0);
    for (int i = 0; i < 0x8000; i++) VDP_DATA = 0;
    vram_write_at(0xC000);                      /* plane A: the canvas's tiles in order */
    for (int r = 0; r < 32; r++)
        for (int c = 0; c < 64; c++)
            VDP_DATA = (uint16_t)(r < TH && c < TW ? r * TW + c : BLANK);
    vram_write_at(0xE000);                      /* plane B: nothing */
    for (int i = 0; i < 64 * 32; i++) VDP_DATA = BLANK;
    VDP_CTRL32 = 0xC0000000u;                   /* palette 0 */
    for (int i = 0; i < 16; i++) VDP_DATA = i < COL_COUNT ? md_colour(palette[i]) : 0;
    VDP_CTRL32 = 0x40000010u;                   /* V scroll 0 */
    VDP_DATA32 = 0;
    first = 1;
    for (int t = 0; t < NTILES; t++) solid[t] = 0;
}

void gfx_clear(uint8_t c)
{
    clear_at = md_vblanks();
    for (int t = 0; t < NTILES; t++) solid[t] = (uint8_t)(c & 15);
}

void gfx_rect(int x, int y, int w, int h, uint8_t c)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > SCREEN_W) w = SCREEN_W - x;
    if (y + h > SCREEN_H) h = SCREEN_H - y;
    if (w <= 0 || h <= 0) return;
    uint32_t v = fill_of(c);
    int x1 = x + w, y1 = y + h;
    for (int ty = y >> 3; ty <= (y1 - 1) >> 3; ty++) {
        int r0 = ty * 8 < y ? y - ty * 8 : 0, r1 = ty * 8 + 8 > y1 ? y1 - ty * 8 : 8;
        for (int tx = x >> 3; tx <= (x1 - 1) >> 3; tx++) {
            int a = tx * 8 < x ? x - tx * 8 : 0, b = tx * 8 + 8 > x1 ? x1 - tx * 8 : 8;
            int t = ty * TW + tx;
            if (a == 0 && b == 8 && r0 == 0 && r1 == 8) {   /* the whole tile: one colour */
                solid[t] = (uint8_t)(c & 15);
                continue;
            }
            if (solid[t] == (c & 15)) continue;             /* (that colour already) */
            uint32_t m = 0xFFFFFFFFu >> (a * 4);
            if (b < 8) m &= ~(0xFFFFFFFFu >> (b * 4));
            uint32_t *p = tile(t);
            for (int r = r0; r < r1; r++) p[r] = (p[r] & ~m) | (v & m);
        }
        if (poll_hook) poll_hook();
    }
}

void gfx_frame(int x, int y, int w, int h, uint8_t c)
{
    gfx_rect(x, y, w, 1, c);
    gfx_rect(x, y + h - 1, w, 1, c);
    gfx_rect(x, y, 1, h, c);
    gfx_rect(x + w - 1, y, 1, h, c);
}

static void plot(int x, int y, uint8_t c)
{
    if ((unsigned)x >= SCREEN_W || (unsigned)y >= SCREEN_H) return;
    uint32_t *p = tile((y >> 3) * TW + (x >> 3)) + (y & 7);
    int s = 28 - ((x & 7) << 2);
    *p = (*p & ~(15u << s)) | (uint32_t)(c & 15) << s;
}

void gfx_line(int x0, int y0, int x1, int y1, uint8_t c)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
    int e = dx + dy, n = 0;
    for (;;) {
        plot(x0, y0, c);
        if ((++n & 63) == 0 && poll_hook) poll_hook();
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
    if (x < 0 || y < 0 || x + 5 > SCREEN_W || y + 7 > SCREEN_H) {     /* (at an edge: by pixel) */
        for (int r = 0; r < 7; r++)
            for (int i = 0; i < 5; i++)
                if (g[r] & (0x10 >> i)) plot(x + i, y + r, c);
        return;
    }
    /* a glyph row is one or two masked writes into tile rows */
    uint32_t v = fill_of(c);
    int s = x & 7, spill = s > 3, ry = y & 7;
    const uint32_t *hi = glyph_hi[s], *lo = glyph_lo[s];
    int t = (y >> 3) * TW + (x >> 3);
    uint32_t *p = tile(t) + ry, *q = spill ? tile(t + 1) + ry : 0;
    for (int r = 0; r < 7; r++) {
        unsigned bits = g[r];
        if (bits) {
            uint32_t m = hi[bits];
            *p = (*p & ~m) | (v & m);
            if (spill) { m = lo[bits]; *q = (*q & ~m) | (v & m); }
        }
        if (++ry == 8 && r < 6) {                   /* into the tiles below */
            ry = 0;
            t += TW;
            p = tile(t);
            q = spill ? tile(t + 1) : 0;
        } else {
            p++;
            if (spill) q++;
        }
    }
}

int gfx_text_n(int x, int y, const char *s, int max, uint8_t c)
{
    int n = 0;
    for (; *s && n < max; s++, n++, x += CHAR_W) {
        if (*s != ' ') glyph(x, y, (uint8_t)*s, c);
        if ((n & 3) == 3 && poll_hook) poll_hook();
    }
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

/* the changed tiles into VRAM (as they are found: the CPU is the limit,
 * blank or not) */
void gfx_present(void)
{
    uint16_t at = md_vblanks();
    uint32_t sh[16];
    for (int c = 0; c < 16; c++) sh[c] = solid_hash((unsigned)c);
    int next = -1;
    for (int t = 0; t < NTILES; t++) {
        if ((t & 15) == 15 && poll_hook) poll_hook();
        unsigned s = solid[t];
        if (s == CLEAN && !first) continue;     /* (untouched since it went up) */
        uint32_t h = s >= CLEAN ? hash_tile(canvas + t * 8) : sh[s];
        if (s == DRAWN) solid[t] = CLEAN;
        if (!first && h == hash[t]) continue;
        hash[t] = h;
        if (t != next) vram_write_at((unsigned)t * 32);
        if (s >= CLEAN) {
            const uint32_t *p = canvas + t * 8;
            VDP_DATA32 = p[0]; VDP_DATA32 = p[1]; VDP_DATA32 = p[2]; VDP_DATA32 = p[3];
            VDP_DATA32 = p[4]; VDP_DATA32 = p[5]; VDP_DATA32 = p[6]; VDP_DATA32 = p[7];
        } else {
            uint32_t v = fill_of(s);
            VDP_DATA32 = v; VDP_DATA32 = v; VDP_DATA32 = v; VDP_DATA32 = v;
            VDP_DATA32 = v; VDP_DATA32 = v; VDP_DATA32 = v; VDP_DATA32 = v;
        }
        next = t + 1;
    }
    first = 0;
    gfx_draw_frames = (uint16_t)(at - clear_at);
    gfx_present_frames = (uint16_t)(md_vblanks() - at);
}
