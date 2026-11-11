/* viz.c - the visualizer (from the menu).
 *
 * The sound chips cannot be listened to from the console, so the wave is
 * worked out from what they are told: the 68000 keeps a copy of every
 * register (CMD_SHADOW) and says which FM channels are keyed on (CMD_VIZ).
 * Each FM channel's patch becomes one cycle of its wave (its algorithm,
 * feedback, operator ratios and levels), which is played at the channel's
 * real pitch, under an envelope that follows the patch's AR, D1R, SL and RR.
 * The PSG adds square waves at their volumes, the noise a scribble. Chords
 * move against each other as they would on a scope.
 *
 * The 32X: a sphere turning in the dark, the wave wrapped round it like a
 * texture. The newest wave runs round the equator; older ones ripple out
 * toward the poles, fading, over a faint grid of latitude and longitude. The
 * sphere is lit from the front, has a glowing rim, swells with the volume and
 * turns faster when it is loud; the rainbow round it flows (the palette).
 * It turns about its upright axis, so each row of the screen is one latitude
 * and a pixel's longitude is asin(x / the row's half-width) plus the turn:
 * a table and a multiply a pixel.
 *
 * The Genesis-only build (16 colours, a slow canvas): a vaporwave sky, a
 * striped sun on the horizon, a neon floor, and the wave in rainbow colours
 * with its reflection on the floor. Each column of the wave keeps one colour;
 * the palette moves the colours along, rolls the floor's lines toward you and
 * makes the sun glow, so a held note costs nothing to draw: only the pixels
 * the wave leaves or reaches are drawn.
 */
#include <string.h>
#include "hw.h"
#include "gfx.h"
#include "lib.h"
#include "viz.h"
#include "viz_tab.h"

#define W           SCREEN_W

static int8_t tab[6][128];              /* one cycle of each FM channel's wave, +-127 */
static uint16_t sig[6];                 /* the patch it is of (a checksum; 0: none yet) */
static uint8_t half[6];                 /* 1: the cycle is two of the note (a MUL of 0.5) */
static uint8_t e_ar[6], e_d1r[6], e_sl[6], e_rr[6];     /* the loudest carrier's envelope */
static int16_t loud[6];                 /* its level, 0-256 */
static int16_t env[6];                  /* 0-256 */
static uint8_t keyed, attacking, need;
static uint16_t run_ph[10];             /* FM 1-6, PSG 1-3, noise: where each is in its cycle */
static uint16_t scale = 480;            /* the points per cycle, as Hz * 16 * scale >> 12 */
static int drawn;
static uint16_t t;                      /* frames */
static int16_t level;                   /* 0-256: how loud, for the glow */
static uint16_t lfsr = 0xACE1;
static int since_read;
uint16_t viz_frames_run;                /* (test telemetry) */

/* ------------------------------------------------------------ a patch's wave */

static const uint8_t frac8[8] = { 255, 235, 215, 197, 181, 166, 152, 140 };
static int tl_amp(int tl) { return tl >= 64 ? 0 : (frac8[tl & 7] + 1) >> (tl >> 3); }   /* 0.75 dB a step */

/* what modulates each operator (bit n: OP n+1) in each algorithm, and the carriers */
static const uint8_t mods[8][4] = {
    { 0, 1, 2, 4 }, { 0, 0, 3, 4 }, { 0, 0, 2, 5 }, { 0, 1, 0, 6 },
    { 0, 1, 0, 4 }, { 0, 1, 1, 1 }, { 0, 1, 0, 0 }, { 0, 0, 0, 0 },
};
static const uint8_t cars[8] = { 8, 8, 8, 8, 10, 14, 14, 15 };

static uint16_t checksum(const fm_patch *p)
{
    const uint8_t *b = (const uint8_t *)p;
    uint16_t h = 0x5A5A;
    for (unsigned i = 0; i < sizeof *p; i++) h = (uint16_t)((h << 3 | h >> 13) ^ b[i]);
    return h | 1;
}

static void fm_wave(int c, const fm_patch *p)
{
    /* (16-bit sums and products throughout: the 68000 multiplies 16 x 16 in
     * one instruction, and 32 x 32 in a library call) */
    int16_t m[4], a[4], o[4] = { 0, 0, 0, 0 }, f1 = 0, f2 = 0;
    uint8_t ph[4] = { 0, 0, 0, 0 };
    int h = 0;
    for (int k = 0; k < 4; k++) if (!p->op[k].mul) h = 1;
    for (int k = 0; k < 4; k++) {
        int mul = p->op[k].mul;
        m[k] = (int16_t)(2 * (h ? (mul ? mul * 2 : 1) : mul));     /* 1/256 cycles a step */
        a[k] = (int16_t)tl_amp(p->op[k].tl);
    }
    const uint8_t *md = mods[p->alg & 7];
    int car = cars[p->alg & 7], fb = p->fb;
    int16_t raw[128];
    int16_t peak = 1;
    /* two cycles, 128 steps each: the first lets the feedback settle */
    for (int step = 0; step < 256; step++) {
        for (int k = 0; k < 4; k++) {
            int16_t mod = 0;
            if (k == 0) { if (fb) mod = (int16_t)(((int16_t)(f1 + f2) << fb) >> 6); }
            else for (int j = 0; j < k; j++) if (md[k] >> j & 1) mod = (int16_t)(mod + (o[j] << 3));  /* +-4 cycles */
            o[k] = a[k] ? (int16_t)((int16_t)viz_sin[(uint8_t)(ph[k] + mod)] * a[k] >> 8) : 0;
            ph[k] = (uint8_t)(ph[k] + m[k]);
        }
        f2 = f1; f1 = o[0];
        if (step >= 128) {
            int16_t v = 0;
            for (int k = 0; k < 4; k++) if (car >> k & 1) v = (int16_t)(v + o[k]);
            raw[step - 128] = v;
            if (v > peak) peak = v;
            if (-v > peak) peak = (int16_t)-v;
        }
        if ((step & 31) == 31 && poll_hook) poll_hook();
    }
    for (int i = 0; i < 128; i++) tab[c][i] = (int8_t)((int32_t)raw[i] * 127 / peak);
    int best = -1;
    for (int k = 0; k < 4; k++)
        if (car >> k & 1 && (best < 0 || p->op[k].tl < p->op[best].tl)) best = k;
    const fm_op *b = &p->op[best];
    loud[c] = (int16_t)tl_amp(b->tl);
    if (peak < 4) loud[c] = 0;
    e_ar[c] = b->ar; e_d1r[c] = b->d1r; e_sl[c] = b->sl; e_rr[c] = b->rr;
    half[c] = (uint8_t)h;
}

static void read_patch(int c)
{
    fm_patch p;
    voice_read(c, &p);
    uint16_t s = checksum(&p);
    if (s == sig[c]) return;
    for (int o = 0; o < 6; o++)                 /* (POLY: the same patch on all six) */
        if (o != c && sig[o] == s) {
            memcpy(tab[c], tab[o], sizeof tab[c]);
            loud[c] = loud[o]; half[c] = half[o];
            e_ar[c] = e_ar[o]; e_d1r[c] = e_d1r[o]; e_sl[c] = e_sl[o]; e_rr[c] = e_rr[o];
            sig[c] = s;
            return;
        }
    fm_wave(c, &p);
    sig[c] = s;
}

/* ------------------------------------------------------------ envelopes */

static void envelope(int c)
{
    int e = env[c], bit = 1 << c;
    if (keyed & bit) {
        if (attacking & bit) {
            e += ((256 - e) * (e_ar[c] + 1)) >> 5;
            if (e >= 248 || e_ar[c] >= 26) { e = 256; attacking &= (uint8_t)~bit; }
        } else {
            int s = e_sl[c] >= 15 ? 0 : (e_sl[c] & 1 ? 181 : 256) >> (e_sl[c] >> 1);
            if (e > s) e -= ((e - s) * (e_d1r[c] + 1)) >> 7;
        }
    } else {
        e -= (e * (e_rr[c] * 2 + 2)) >> 6;
        if (e < 3) e = 0;
    }
    env[c] = (int16_t)e;
}

/* ------------------------------------------------------------ the voices */

static void sh6(int i, uint8_t *b)
{
    md_cmd(CMD_SHADOW, (uint16_t)i, 0, 0);
    uint16_t w[3] = { md_p(C_P1), md_p(C_P2), md_p(C_P3) };
    for (int k = 0; k < 6; k++) b[k] = (uint8_t)(k & 1 ? w[k >> 1] : w[k >> 1] >> 8);
}

static const uint8_t psg_amp[16] = { 255, 203, 161, 128, 102, 81, 64, 51, 40, 32, 26, 20, 16, 13, 10, 0 };

enum { K_TAB, K_SQUARE, K_NOISE, K_SINE };
typedef struct { const int8_t *tab; uint16_t ph, inc; int16_t amp; uint8_t kind, who; uint32_t hz; } voice_t;

/* the frames since the last call (the 68000 counts them) and what sounds */
static uint16_t vols_now;
static int frame_start(void)
{
    static uint16_t last;
    md_cmd(CMD_VIZ, 0, 0, 0);
    uint16_t k = md_p(C_P1), now = md_p(C_P3);
    vols_now = md_p(C_P2);
    int el = drawn ? (uint16_t)(now - last) : 1;
    last = now;
    if (el < 1) el = 1;
    if (el > 30) el = 30;
    t = (uint16_t)(t + el);
    viz_frames_run++;
    uint8_t ev = (uint8_t)(k >> 8);
    keyed = (uint8_t)(k & 0x3F);
    need |= ev;
    attacking |= ev;
    for (int c = 0; c < 6; c++) if (ev >> c & 1 && e_ar[c] >= 26) env[c] = 256;
    return el;
}

/* The voices sounding, el frames on. The wave over n_pts points: the lowest
 * note makes `cycles` cycles of it (the highest at most `most`), and one loud
 * voice is `big` high (all together at most `top`). The count. */
static int voices(voice_t *v, int el, int n_pts, int cycles, int most, int big, int top)
{
    uint16_t vols = vols_now;
    if ((since_read += el) >= 40) { since_read = 0; need |= keyed; }
    for (int c = 0; c < 6; c++)
        if (need >> c & 1) { need &= (uint8_t)~(1 << c); read_patch(c); break; }     /* (one a frame) */
    for (int c = 0; c < 6; c++)
        for (int i = 0; i < el; i++) envelope(c);

    int n = 0, total = 0, any_fm = 0, low_who = 0;
    uint32_t low = 0, high = 0;
    for (int c = 0; c < 6; c++) if (env[c] && loud[c]) any_fm = 1;
    if (any_fm) {
        uint8_t a[6], b[6];
        for (int part = 0; part < 2; part++) {
            sh6(part * 256 + 0xA0, a);          /* A0-A2 the low bits, A4-A6 the block and high bits */
            sh6(part * 256 + 0xA6, b);
            for (int i = 0; i < 3; i++) {
                int c = part * 3 + i, amp = env[c] * loud[c] >> 8;
                if (amp < 3) continue;
                int hi = i < 2 ? a[4 + i] : b[0];
                uint32_t fn = (uint32_t)((hi & 7) << 8 | a[i]) << (hi >> 3 & 7);
                uint32_t hz = fn * 416 >> 10;   /* Hz * 16 */
                if (half[c]) hz >>= 1;
                if (!hz) continue;
                v[n].tab = tab[c]; v[n].kind = K_TAB; v[n].who = (uint8_t)c;
                v[n].hz = hz; v[n].amp = (int16_t)amp;
                n++; total += amp;
            }
        }
    }
    if ((vols & 0x0FFF) != 0x0FFF) {
        uint8_t tn[6];
        sh6(512, tn);
        for (int i = 0; i < 3; i++) {
            int vol = vols >> (i * 4) & 15, tone = (tn[2 * i] << 8 | tn[2 * i + 1]) & 0x3FF;
            if (vol == 15 || tone < 2) continue;
            v[n].kind = K_SQUARE; v[n].who = (uint8_t)(6 + i);
            v[n].hz = 1789776u / (unsigned)tone;
            v[n].amp = (int16_t)(psg_amp[vol] * 160 >> 8);
            total += v[n].amp;
            n++;
        }
    }
    if ((vols >> 12) != 15) {
        v[n].kind = K_NOISE; v[n].who = 9; v[n].hz = 0;
        v[n].amp = (int16_t)(psg_amp[vols >> 12] * 120 >> 8);
        total += v[n].amp;
        n++;
    }
    for (int j = 0; j < n; j++) {
        if (!v[j].hz) continue;
        if (!low || v[j].hz < low) { low = v[j].hz; low_who = v[j].who; }
        if (v[j].hz > high) high = v[j].hz;
    }
    if (low) {
        uint32_t per = (uint32_t)cycles * 65536u / (unsigned)n_pts, cap = (uint32_t)most * 65536u / (unsigned)n_pts;
        uint32_t want = (per << 12) / low;
        if ((want * high >> 12) > cap) want = (cap << 12) / high;
        if (want < 8) want = 8;
        if (want > 30000) want = 30000;
        scale = (uint16_t)((int32_t)scale + (((int32_t)want - (int32_t)scale) >> 1));
    }
    for (int j = 0; j < n; j++) {
        int w = v[j].who;
        run_ph[w] = (uint16_t)(run_ph[w] + (v[j].hz * 273 >> 2) * (uint32_t)el);
        v[j].inc = (uint16_t)(v[j].hz * scale >> 12);
    }
    for (int j = 0; j < n; j++) v[j].ph = (uint16_t)(run_ph[v[j].who] - run_ph[low_who]);
    int sum = total * big >> 8;
    for (int j = 0; j < n; j++)
        v[j].amp = (int16_t)(sum > top ? v[j].amp * top / total : v[j].amp * big >> 8);
    int lv = total > 256 ? 256 : total;
    level = (int16_t)(level + ((lv - level) >> 2));
    if (!n) {                                   /* quiet: a slow breath */
        v[0].kind = K_SINE; v[0].amp = (int16_t)(big / 9 + 1); v[0].ph = (uint16_t)(t * 300);
        v[0].inc = (uint16_t)((uint32_t)cycles * 65536u / (unsigned)n_pts);
        n = 1;
    }
    return n;
}

/* the wave's points, each `step` apart: out[i] = the height at point i */
static void wave(voice_t *v, int n, int16_t *out, int n_pts, int step)
{
    for (int j = 0; j < n; j++) v[j].inc = (uint16_t)(v[j].inc * step);
    for (int x = 0; x < n_pts; x++) {
        int16_t s = 0;
        for (int j = 0; j < n; j++) {
            voice_t *q = &v[j];
            switch (q->kind) {
            case K_TAB: s = (int16_t)(s + ((int16_t)q->tab[q->ph >> 9] * q->amp >> 7)); break;
            case K_SQUARE: s = (int16_t)(s + (q->ph & 0x8000 ? q->amp : -q->amp)); break;
            case K_NOISE:
                lfsr = (uint16_t)((lfsr >> 1) ^ (-(lfsr & 1) & 0xB400));
                s = (int16_t)(s + ((int16_t)((lfsr & 255) - 128) * q->amp >> 7));
                break;
            default: s = (int16_t)(s + ((int16_t)viz_sin[q->ph >> 8] * q->amp >> 7)); break;
            }
            q->ph = (uint16_t)(q->ph + q->inc);
        }
        out[x] = s;
        if ((x & 31) == 31 && poll_hook) poll_hook();
    }
}

/* ------------------------------------------------------------ colours */

static uint16_t bgr(int r, int g, int b)
{
    if (r > 31) r = 31;
    if (g > 31) g = 31;
    if (b > 31) b = 31;
    return (uint16_t)((b & 31) << 10 | (g & 31) << 5 | (r & 31));
}

static void hue_rgb(int h, int *r, int *g, int *b)  /* 0-1535: red, yellow, green, cyan, blue, magenta: 0-255 */
{
    int f = h & 255;
    switch ((h >> 8) % 6) {
    case 0: *r = 255; *g = f; *b = 0; break;
    case 1: *r = 255 - f; *g = 255; *b = 0; break;
    case 2: *r = 0; *g = 255; *b = f; break;
    case 3: *r = 0; *g = 255 - f; *b = 255; break;
    case 4: *r = f; *g = 0; *b = 255; break;
    default: *r = 255; *g = 0; *b = 255 - f; break;
    }
}

static void reset(void)
{
    need = 0x3F;
    drawn = 0;
    keyed = attacking = 0;
    for (int c = 0; c < 6; c++) env[c] = 0;
}

void viz_close(void)
{
    gfx_ui_palette();
}

#ifndef MD_ONLY
/* ====================================================================== the 32X: the sphere */

#define CX      160
#define CY      112
#define NHUE    32
#define PAL_RB  128                     /* 128 + level * 32 + hue: the wave (level 0 brightest) */
#define NLIGHT  8                       /* shades of light on the sphere */
enum { C_BLACK, C_WHITE, C_GRID, C_BODY = C_GRID + NLIGHT, C_RIM = C_BODY + NLIGHT, C_N = C_RIM + 3 };
#define NTRACE  7
#define A_LAT   26                      /* a full wave: this many 128ths of a quarter turn */
#define THICK   2

static int8_t hist[NTRACE][256];        /* the waves round the sphere, newest first, in latitude */
static const int8_t trace_at[NTRACE] = { 0, -30, 30, -60, 60, -90, 90 };
static const uint8_t trace_age[NTRACE] = { 0, 1, 1, 2, 2, 3, 3 };
static uint16_t turn;                   /* 256ths of a turn << 8 */
static int hist_timer;

void viz_draw(void)
{
    gfx_clear(C_BLACK);
    drawn = 0;
}

static void colours(void)
{
    uint16_t pal[PAL_RB + 4 * NHUE];        /* (on the stack: SDRAM is full) */
    int glow = 170 + level * 86 / 256;
    pal[C_BLACK] = 0;
    pal[C_WHITE] = bgr(31, 31, 31);
    for (int i = 0; i < NLIGHT; i++) {           /* light falling off: 1 to 1/8 */
        int f = 256 - i * 224 / (NLIGHT - 1);
        pal[C_GRID + i] = bgr((14 * f >> 8) * glow >> 8, (9 * f >> 8) * glow >> 8, (31 * f >> 8) * glow >> 8);
        pal[C_BODY + i] = bgr(7 * f >> 8, 4 * f >> 8, 17 * f >> 8);
    }
    pal[C_RIM] = bgr(16 * glow >> 8, 5, 28 * glow >> 8);
    pal[C_RIM + 1] = bgr(9 * glow >> 8, 3, 18 * glow >> 8);
    pal[C_RIM + 2] = bgr(4, 1, 9);
    static const uint8_t lv[4] = { 255, 215, 150, 85 };
    for (int h = 0; h < NHUE; h++) {
        int r, g, b;
        hue_rgb((h * (1536 / NHUE) + 1536 * 8 - t * 8) % 1536, &r, &g, &b);
        for (int l = 0; l < 4; l++) {
            int rr = r * lv[l] >> 8, gg = g * lv[l] >> 8, bb = b * lv[l] >> 8;
            if (!l) { rr = (rr * 3 + 255) >> 2; gg = (gg * 3 + 255) >> 2; bb = (bb * 3 + 255) >> 2; }  /* (a white-hot core) */
            pal[PAL_RB + l * NHUE + h] = bgr(rr >> 3, gg >> 3, bb >> 3);
        }
    }
    gfx_palette(0, pal, C_N);
    gfx_palette(PAL_RB, pal + PAL_RB, 4 * NHUE);
}

/* a circle of radius r's half-width at height y (0 past it) */
__attribute__((section(".data.sphere"))) static int half_w(int r, int y)
{
    if (y < 0) y = -y;
    if (y >= r) return 0;
    return r * viz_circle[(y << 8) / r] >> 8;
}

/* (in SDRAM, not the cartridge with the rest: it is the busy part)
 *
 * Drawn forward, not pixel by pixel: the body row by row in runs (one for
 * each ring of light), the meridians a dot a row, and each wave a short span
 * of pixels for each step round the sphere it covers. */
#define HOT __attribute__((section(".data.sphere")))
HOT static void fill(uint8_t *row, int a, int b, uint8_t c)
{
    uint8_t *p = row + a, *e = row + b + 1;
    while (p < e && ((uintptr_t)p & 3)) *p++ = c;
    uint32_t v = c * 0x01010101u;
    for (; p + 4 <= e; p += 4) *(uint32_t *)(void *)p = v;
    while (p < e) *p++ = c;
}

__attribute__((noinline, section(".data.sphere"))) static void sphere(void)
{
    uint8_t *cv = gfx_canvas();
    int R = 84 + (level * 14 >> 8);
    /* the light, from the upper left: rings round the brightest point */
    int lx = -R * 3 / 10, ly = -R * 3 / 10;
    int rl[NLIGHT - 1], r2[NLIGHT - 1];
    for (int i = 0; i < NLIGHT - 1; i++) {
        rl[i] = R * (77 + i * 231 / (NLIGHT - 2)) >> 8;
        r2[i] = rl[i] * rl[i];
    }
    int rot = turn >> 8;
    int16_t hwr[2 * 100 + 1];               /* each row's half-width (R <= 98) */
    for (int y = -R; y <= R; y++) hwr[y + R] = (int16_t)half_w(R, y);

    /* the rim's glow, then the body: the dark outside, each brighter ring over it */
    int vprev = -999;
    for (int y = -R - 4; y <= R + 4; y++) {
        uint8_t *row = cv + (CY + y) * W + CX;
        int hw = y >= -R && y <= R ? hwr[y + R] : 0, hh = half_w(R + 4, y);
        for (int x = hw + 1; x <= hh; x++) {
            uint8_t c = (uint8_t)(C_RIM + (x - hw > 2 ? 2 : x - hw > 1 ? 1 : 0));
            row[x] = c;
            row[-x] = c;
        }
        if (!hw) continue;
        int ay = y < 0 ? -y : y;
        int v = viz_asin[(ay << 7) / R];
        if (y < 0) v = -v;
        int latline = vprev != -999 && (v >> 5) != (vprev >> 5);
        vprev = v;
        uint8_t base = latline ? C_GRID : C_BODY;
        int ca = 0, cb = -1;                    /* what is filled: each ring once, from the inside */
        for (int i = 0; i < NLIGHT - 1; i++) {
            int xl = half_w(rl[i], y - ly);
            if (!xl) continue;
            int a = lx - xl + 1, b = lx + xl - 1;
            if (a < -hw) a = -hw;
            if (b > hw) b = hw;
            if (a > b) continue;
            if (ca > cb) fill(row, a, b, (uint8_t)(base + i));
            else {
                if (a < ca) fill(row, a, ca - 1, (uint8_t)(base + i));
                if (b > cb) fill(row, cb + 1, b, (uint8_t)(base + i));
            }
            ca = a; cb = b;
        }
        if (ca > cb) fill(row, -hw, hw, (uint8_t)(base + NLIGHT - 1));
        else {
            if (-hw < ca) fill(row, -hw, ca - 1, (uint8_t)(base + NLIGHT - 1));
            if (cb < hw) fill(row, cb + 1, hw, (uint8_t)(base + NLIGHT - 1));
        }
        /* the meridians, every eighth of a turn */
        for (int m = 0; m < 8; m++) {
            int lon = ((m * 32 - rot + 128) & 255) - 128;
            if (lon <= -64 || lon >= 64) continue;
            int x = hw * viz_sin[lon & 255] * 129 >> 14;
            int dx = x - lx, dy = y - ly, d = dx * dx + dy * dy, l = 0;
            while (l < NLIGHT - 1 && d >= r2[l]) l++;
            row[x] = (uint8_t)(C_GRID + l);
        }
    }
    if (poll_hook) poll_hook();

    /* the waves, the oldest first (the newest on top, with its glow); each in
     * the light of the body under it */
    uint8_t light[256];
    for (int c = 0; c < 256; c++)
        light[c] = (uint8_t)(c >= C_GRID && c < C_GRID + NLIGHT ? c - C_GRID :
                             c >= C_BODY && c < C_BODY + NLIGHT ? c - C_BODY :
                             c >= PAL_RB ? ((c - PAL_RB) / NHUE) * 2 : NLIGHT - 1);
    int16_t sx[129];                        /* sin of each visible step's longitude, *128 */
    int u0 = (rot - 63) & 255;              /* the first visible step */
    for (int i = 0; i <= 128; i++) {
        int lon = ((u0 + i - rot + 128) & 255) - 128;
        if (lon < -64) lon = -64;
        if (lon > 64) lon = 64;
        sx[i] = (int16_t)(viz_sin[lon & 255] * 129 >> 7);
        if (lon == 64) sx[i] = 128;
        if (lon == -64) sx[i] = -128;
    }
    for (int k = NTRACE - 1; k >= 0; k--) {
        for (int pass = k ? 1 : 0; pass < 2; pass++) {          /* (the newest: its glow first) */
            int grow = !pass ? THICK + 1 : k ? 0 : 1;              /* (thin lines; the newest a little wider) */
            for (int i = 0; i < 127; i++) {
                int u = (u0 + i) & 255;
                int a = trace_at[k] + hist[k][u], b = trace_at[k] + hist[k][(u + 1) & 255];
                int lo = (a < b ? a : b) - grow, hi = (a ^ b ^ (a < b ? a : b)) + grow;
                if (lo < -127) lo = -127;
                if (hi > 127) hi = 127;
                if (lo > hi) continue;
                /* latitude (128ths of a quarter turn) to rows: R sin */
                int ya = R * viz_sin[(lo >> 1) & 255] >> 7, yb = R * viz_sin[(hi >> 1) & 255] >> 7;
                int hue = (u >> 2) & 31;
                for (int y = ya; y <= yb; y++) {
                    if (y < -R || y > R) continue;
                    int hw = hwr[y + R];
                    int xa = hw * sx[i] >> 7, xb = hw * sx[i + 1] >> 7;
                    if (xb < xa) { int s = xa; xa = xb; xb = s; }
                    if (xb > xa) xb--;                          /* (meet the next step's span) */
                    uint8_t *row = cv + (CY + y) * W + CX;
                    int l = light[row[(xa + xb) >> 1]];         /* (the body's shade there) */
                    int lv = pass ? (l >> 1) + trace_age[k] : 3;
                    fill(row, xa, xb, (uint8_t)(PAL_RB + (lv > 3 ? 3 : lv) * NHUE + hue));
                }
            }
        }
        if (poll_hook) poll_hook();
    }
}

void viz_frame(int el)
{
    el = frame_start();
    voice_t v[10];
    int n = voices(v, el, 256, 4, 24, 46, 58);
    int16_t w[256];
    wave(v, n, w, 256, 1);
    if ((hist_timer += el) >= 3 || !drawn) {    /* the older waves move out toward the poles */
        hist_timer = 0;
        memmove(hist[1], hist[0], sizeof hist - sizeof hist[0]);
    }
    for (int u = 0; u < 256; u++) hist[0][u] = (int8_t)(-w[u] * A_LAT / 58);
    turn = (uint16_t)(turn + el * (180 + level * 3));

    gfx_clear(C_BLACK);
    sphere();
    drawn = 1;
    gfx_present();
    colours();
}

void viz_open(void)
{
    reset();
    memset(hist, 0, sizeof hist);
}

#else
/* ====================================================================== the Genesis: the vaporwave scene */

#define NRB     8               /* rainbow colours (16 in all on the Mega Drive) */
#define NSUN    2
enum { VC_SKY, VC_WHITE, VC_RB, VC_SUN = VC_RB + NRB, VC_MAG = VC_SUN + NSUN, VC_GRID, VC_N = VC_GRID + 3 };

#define WAVE_Y      76
#define WAVE_TOP    4
#define WAVE_BOT    (VIZ_HOR - 4)
#define REFL_Y      186

static uint8_t oy[W + 1];               /* the wave as drawn */

static int sun_col(int dy) { return VC_SUN + (VIZ_SUN_R - dy) * NSUN / (VIZ_SUN_R + 1); }

static uint8_t bg_at(int x, int y)
{
    if (y < VIZ_HOR) {
        int dy = VIZ_HOR - y, dx = x - VIZ_SUN_X;
        if (dy <= VIZ_SUN_R) {
            int hw = viz_sun_half[dy];
            if (hw && dx > -hw && dx < hw) return (uint8_t)sun_col(dy);
        }
        return viz_star[x] == y ? VC_WHITE : VC_SKY;
    }
    if (y < VIZ_HOR + 2) return VC_MAG;
    int r = y - VIZ_HOR;
    if (viz_floor_row[r]) return (uint8_t)(VC_GRID + viz_floor_row[r] - 1);
    if (viz_floor_v[r * 10 + (x >> 5)] >> (31 - (x & 31)) & 1) return VC_MAG;
    return VC_SKY;
}

/* the picture under column x, rows ya to yb (all above the horizon) */
static void bg_span(int x, int ya, int yb)
{
    int sun = viz_sun_top[x];
    int sky_end = yb < sun ? yb : sun - 1;
    if (ya <= sky_end) {
        gfx_vspan(x, ya, sky_end, VC_SKY);
        int s = viz_star[x];
        if (s >= ya && s <= sky_end) gfx_pixel(x, s, VC_WHITE);
    }
    for (int y = ya > sun ? ya : sun; y <= yb; y++) gfx_pixel(x, y, bg_at(x, y));
}

void viz_draw(void)
{
    gfx_clear(VC_SKY);
    for (int dy = 1; dy <= VIZ_SUN_R; dy++) {
        int hw = viz_sun_half[dy];
        if (hw) gfx_rect(VIZ_SUN_X - hw + 1, VIZ_HOR - dy, 2 * hw - 1, 1, (uint8_t)sun_col(dy));
    }
    for (int x = 0; x < W; x++)
        if (viz_star[x]) gfx_pixel(x, viz_star[x], VC_WHITE);
    gfx_rect(0, VIZ_HOR, W, 2, VC_MAG);
    for (int r = 2; r < SCREEN_H - VIZ_HOR; r++) {
        if (viz_floor_row[r]) {
            gfx_rect(0, VIZ_HOR + r, W, 1, (uint8_t)(VC_GRID + viz_floor_row[r] - 1));
            continue;
        }
        for (int k = 0; k < 10; k++) {
            uint32_t b = viz_floor_v[r * 10 + k];
            for (int i = 0; b; i++, b <<= 1)
                if (b & 0x80000000u) gfx_pixel(k * 32 + i, VIZ_HOR + r, VC_MAG);
        }
    }
    gfx_rect(4, 212, 6 * 7 + 4, 11, VC_SKY);
    gfx_text(6, 214, "B: BACK", VC_WHITE);
    drawn = 0;
}

static int refl(int y) { return REFL_Y + ((WAVE_Y - y) >> 2); }

static void draw_wave(const uint8_t *ny)
{
    int band = 0, acc = 0;
    for (int x = 0; x < W; x++) {
        uint8_t c = (uint8_t)(VC_RB + band);
        if ((acc += NRB) >= W / 2) {            /* (two rainbows across the screen) */
            acc -= W / 2;
            if (++band == NRB) band = 0;
        }
        int p = x ? x - 1 : 0;
        int na = ny[x] < ny[p] ? ny[x] : ny[p], nb = (ny[x] ^ ny[p] ^ na) + 1;
        if (drawn) {
            int oa = oy[x] < oy[p] ? oy[x] : oy[p], ob = (oy[x] ^ oy[p] ^ oa) + 1;
            if (oa != na || ob != nb) {
                if (nb < oa || na > ob) {               /* apart: all of each */
                    bg_span(x, oa, ob);
                    gfx_vspan(x, na, nb, c);
                } else {
                    if (oa < na) bg_span(x, oa, na - 1);
                    if (ob > nb) bg_span(x, nb + 1, ob);
                    if (na < oa) gfx_vspan(x, na, oa - 1, c);
                    if (nb > ob) gfx_vspan(x, ob + 1, nb, c);
                }
            }
        } else {
            gfx_vspan(x, na, nb, c);
        }
        if (!(x & 1)) {                         /* the reflection, dotted like water */
            int r = refl(ny[x]), o = refl(oy[x]);
            if (!drawn || o != r) {
                if (drawn) gfx_pixel(x, o, bg_at(x, o));
                gfx_pixel(x, r, c);
            }
        }
        if ((x & 15) == 15 && poll_hook) poll_hook();
    }
    memcpy(oy, ny, sizeof oy);
    drawn = 1;
}

static void colours(void)
{
    uint16_t pal[VC_N];
    pal[VC_SKY] = bgr(5, 1, 10);
    pal[VC_WHITE] = bgr(31, 31, 31);
    for (int j = 0; j < NRB; j++) {
        int r, g, b;
        hue_rgb((j * 1536 / NRB + 1536 * 4 - (t * 10) % 1536) % 1536, &r, &g, &b);
        pal[VC_RB + j] = bgr((r >> 3) | 2, (g >> 3) | 2, (b >> 3) | 2);
    }
    pal[VC_SUN] = bgr(31, 29, 8);               /* (3 bits a colour: dimmed, it turns muddy) */
    pal[VC_SUN + 1] = bgr(31, 7, 22);
    int glow = 150 + level * 106 / 256;
    pal[VC_MAG] = bgr(31 * glow >> 8, 4, 28 * glow >> 8);
    static const uint8_t grid[3][3] = { { 31, 12, 30 }, { 18, 4, 22 }, { 9, 2, 14 } };
    int step = (t / 6) % 3;
    for (int g = 0; g < 3; g++) {
        const uint8_t *q = grid[(g + 3 - step) % 3];
        pal[VC_GRID + g] = bgr(q[0], q[1], q[2]);
    }
    gfx_palette(0, pal, VC_N);
}

void viz_frame(int el)
{
    el = frame_start();
    voice_t v[10];
    int n = voices(v, el, W, 2, 12, 46, 58);
    int16_t w[W / 2 + 1];
    wave(v, n, w, W / 2 + 1, 2);                /* (every other column; the rest between) */
    uint8_t ny[W + 1];
    for (int x = 0; x <= W; x++) {
        int s = x & 1 ? (w[x >> 1] + w[(x >> 1) + 1]) >> 1 : w[x >> 1];
        int y = WAVE_Y - s;
        ny[x] = (uint8_t)(y < WAVE_TOP ? WAVE_TOP : y > WAVE_BOT ? WAVE_BOT : y);
    }
    draw_wave(ny);
    gfx_present();
    colours();
}

void viz_open(void)
{
    reset();
}
#endif
