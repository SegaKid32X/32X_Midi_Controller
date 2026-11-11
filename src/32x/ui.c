/* ui.c - the screens: the patch library (home) and the FM editor.
 *
 * LIBRARY  UP/DOWN choose a patch (it loads into the target channel a moment
 *          after you stop: you hear it at once), LEFT/RIGHT a page,
 *          C the target channel (1-6 or ALL), A the editor, Y delete,
 *          START the guide, MODE (or B on a 3-button pad) the controls.
 * EDITOR   the D-pad moves, A changes the setting (then UP/DOWN by 1,
 *          LEFT/RIGHT by 10, A or B done), X/Z by 10 at once, C the channel,
 *          START save, B (or Y) back, MODE the controls.
 *
 * What the GenMDM (or a DAW behind it) changes shows here as it happens:
 * the 68000 counts the writes it receives and we read the voice again.
 */
#include <string.h>
#include "hw.h"
#include "gfx.h"
#include "lib.h"
#include "ui.h"
#include "guide.h"
#include "synth.h"
#include "viz.h"

enum { SCR_LIBRARY, SCR_EDITOR, SCR_PSGED };
enum { OV_NONE, OV_HELP, OV_DELETE, OV_SAVE, OV_GUIDE, OV_MENU, OV_SETTINGS, OV_MONITOR, OV_PSGMENU, OV_VIZ };

#define ALL_CH      6
#define PWM_T       7           /* 7-10: the 32X's FM channels (MIDI 11-14) */
#define DRUMS_T     11          /* the 32X's drum kit (MIDI channel 16) */
#define PSG_T       12          /* 12-15: the PSG's tone channels and its noise (MIDI 7-10) */
#define IS_PSG(t)   ((t) >= PSG_T && (t) < PSG_T + 4)
/* the targets in the order C goes through them */
#ifdef MD_ONLY
static const uint8_t target_order[] = { 0, 1, 2, 3, 4, 5, 6, 12, 13, 14, 15 };  /* (no 32X) */
#define APP_TITLE   "MD MIDI CONTROLLER"
#else
static const uint8_t target_order[] = { 0, 1, 2, 3, 4, 5, 6, 12, 13, 14, 15, 7, 8, 9, 10, 11 };
#define APP_TITLE   "32X MIDI CONTROLLER"
#endif
#define NTARGET     ((int)sizeof target_order)
#ifdef MD_ONLY
#define NSLOT       6           /* voices with a patch: YM 1-6 */
#else
#define NSLOT       10          /* voices with a patch: YM 1-6, 32X 11-14 */
#endif
#define LIST_ROWS   19
#define LIST_Y      22

static int screen = SCR_LIBRARY, overlay = OV_NONE;
static int cursor, top, target;         /* target 0-5, or ALL_CH */
static int load_timer = -1;
static char chan_path[NSLOT][PATH_MAX_ + NAME_MAX + 2];   /* the file each slot came from ("": none) */
static int chan_edited[NSLOT];
static fm_patch voice;                  /* the voice shown (the editor's channel) */
static int ed_row, ed_col, save_choice, menu_choice, set_row;
static int ed_changing;                 /* the editor: A pressed, the D-pad changes the value */
static int pe_row, pe_step;             /* the PSG editor: the sequence, the step */
static int frame_el = 1;                /* frames since the last ui_frame() (the Genesis-only
                                         * build can take several to draw) */
/* (the screens' own settings, after the 68000's) */
#define SET_LOUD    SET_COUNT           /* 1: a patch loads with its loudest carrier at full */
#define SET_PWM_VOL (SET_COUNT + 1)     /* the 32X's voices */
#ifdef MD_ONLY
#define NSET        (SET_COUNT + 1)
#else
#define NSET        (SET_COUNT + 2)
#endif
static uint8_t settings[SET_COUNT + 2];
static int preview_note = -1;           /* the note X is playing, or -1 */
static void menu_input(uint16_t p);
static void settings_input(uint16_t p);
static char message[60];
static int message_timer;
static int dirty = 1;
int ui_continuous;
static uint16_t last_act;
static int act_timer;

static void say(const char *s)
{
    strncpy(message, s, sizeof message - 1);
    message[sizeof message - 1] = 0;
    message_timer = 180;
    dirty = 1;
}

static void say_err(const char *what, int e)
{
    char t[60], *o = t;
    while (*what) *o++ = *what++;
    *o++ = ' ';
    o = fmt_num(o, e);
    *o = 0;
    say(t);
}

/* the slot shown: YM channels 0-5, the 32X's FM channels 6-9 */
static int shown_ch(void)
{
    if (target >= PWM_T && target < PWM_T + 4) return target - 1;
    return target >= ALL_CH ? 0 : target;
}

static int is_pwm_slot(int slot) { return slot >= 6; }

static void slot_read(int slot, fm_patch *p)
{
    if (is_pwm_slot(slot)) synth_get_patch(slot - 6, p);
    else voice_read(slot, p);
}

static void slot_apply(int slot, const fm_patch *p)
{
    if (is_pwm_slot(slot)) synth_set_patch(slot - 6, p);
    else voice_apply(slot, p);
}

/* C: the next target (the editor skips the drums) */
static int next_target(int t, int editor)
{
    int k = 0;
    while (k < NTARGET && target_order[k] != t) k++;
    do {
        k = (k + 1) % NTARGET;
        t = target_order[k];
    } while (editor && (t == DRUMS_T || IS_PSG(t)));    /* (the editor: FM voices only) */
    return t;
}

/* ---- the PSG instruments (targets PSG 1-3, NOISE) ---- */
static char psg_name[4][40];            /* the file each came from ("": none) */
static char psg_rel[4][128];            /* its path inside MIDI_PATCHES ("": none or too long) */
static uint8_t psg_edited[4];
static uint32_t psg_from[4];            /* (a hash of its path: the green mark) */
static psg_patch psg_shown;             /* the shown target's, as the 68000 has it */

static uint32_t path_hash(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s) { h ^= (uint8_t)*s++; h = (h << 5 | h >> 27) + (h >> 11); }
    return h | 1;
}

/* ------------------------------------------------------------ input */

static uint16_t held_prev;
static int repeat_timer;

/* Buttons pressed this frame, plus auto-repeat for the D-pad, A and B */
static uint16_t held_now;

static uint16_t read_buttons(void)
{
    uint16_t now = hw_pad();
    uint16_t pressed = now & (uint16_t)~held_prev;
    const uint16_t rep = PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT | PAD_A | PAD_B;
    if (now & rep & held_prev) {
        if ((repeat_timer -= frame_el) <= 0) {
            pressed |= now & rep;
            repeat_timer = 4;
        }
    } else {
        repeat_timer = 18;
    }
    held_prev = now;
    held_now = now;
    return pressed;
}

/* ------------------------------------------------------------ drawing helpers */

static const char *const ch_names[16] = { "CH 1", "CH 2", "CH 3", "CH 4", "CH 5", "CH 6", "ALL",
                                                "32X 11", "32X 12", "32X 13", "32X 14", "DRUMS",
                                                "PSG 1", "PSG 2", "PSG 3", "NOISE" };
static const char *const slot_names[NSLOT] = { "CH 1", "CH 2", "CH 3", "CH 4", "CH 5", "CH 6",
#ifndef MD_ONLY
                                              "32X 11", "32X 12", "32X 13", "32X 14"
#endif
};
static int drum_pick;                   /* X on DRUMS: the sample it plays next */

/* ---- the MIDI lights: one per MIDI channel, at the top of every screen.
 * With our Teensy firmware: a message on that channel. With GenMDM's own
 * firmware the 68000 sees only sound-chip writes, so: a voice that started a
 * note, on the channel GenMDM plays it from (FM 1-6 = 1-6, PSG 7-9, noise 10). */
static uint8_t light[16];               /* frames each light stays on */
static uint16_t lights_shown;
static uint16_t mon_total;              /* MIDI messages so far (wraps) */

static void monitor_poll(void)
{
    md_cmd(CMD_MONITOR, 0, 0, 0);
    uint16_t chans = md_p(C_P1), voices = md_p(C_P2);
    mon_total = md_p(C_P3);
    uint16_t link = hw_flags() & FL_LINK;
    uint16_t m = link == LINK_MIDI ? chans : link == LINK_STOCK ? (uint16_t)(voices & 0x3FF) : 0;
    for (int c = 0; c < 16; c++) {
        if (m >> c & 1) light[c] = 10;
        else if (light[c]) light[c]--;
    }
}

static uint16_t lights_now(void)
{
    uint16_t m = 0;
    for (int c = 0; c < 16; c++) if (light[c]) m |= (uint16_t)(1u << c);
    return m;
}

static int light_x(int c) { return 122 + c * 6 + (c >= 6 ? 2 : 0) + (c >= 10 ? 2 : 0); }

static void light_box(int x, int y, int c, int on)
{
    gfx_rect(x, y, 5, 7, on ? (c >= 10 ? COL_ORANGE : COL_GREEN) : COL_PANEL2);
}

static void draw_lights(void)
{
    uint16_t m = lights_now();
    for (int c = 0; c < 16; c++) light_box(light_x(c), 3, c, m >> c & 1);
    lights_shown = m;
}

static void header(const char *title)
{
    gfx_rect(0, 0, SCREEN_W, 12, COL_PANEL);
    gfx_text(4, 3, title, COL_HI);
    draw_lights();
    uint16_t f = hw_flags();
    const char *link = (f & FL_LINK) == LINK_STOCK ? "GENMDM: STOCK"
                     : (f & FL_LINK) == LINK_MIDI ? "GENMDM: MIDI" : "GENMDM: WAITING";
    gfx_text(SCREEN_W - 4 - (int)strlen(link) * CHAR_W, 3, link,
             (f & FL_LINK) ? COL_GREEN : COL_DIM);
}

static void footer(const char *hint)
{
    gfx_rect(0, SCREEN_H - 12, SCREEN_W, 12, COL_PANEL);
    if (message_timer > 0)
        gfx_text(4, SCREEN_H - 9, message, COL_HI);
    else
        gfx_text(4, SCREEN_H - 9, hint, COL_DIM);
}

/* The algorithm: boxes 1-4 and their connections, carriers to the output */
static const struct { int8_t pos[4][2]; uint8_t links; uint8_t carriers; } algs[8] = {
    /* links: bit (from*4 + to) for from 0-3 -> to 0-3 (16 bits as two bytes
     * below would be cleaner; the few we need fit in a table of pairs) */
    { { {0,0},{1,0},{2,0},{3,0} }, 0, 0x8 },
    { { {0,0},{0,1},{1,0},{2,0} }, 0, 0x8 },
    { { {1,0},{0,1},{1,1},{2,0} }, 0, 0x8 },
    { { {0,0},{1,0},{1,1},{2,0} }, 0, 0x8 },
    { { {0,0},{1,0},{0,1},{1,1} }, 0, 0xA },
    { { {0,1},{1,0},{1,1},{1,2} }, 0, 0xE },
    { { {0,0},{1,0},{1,1},{1,2} }, 0, 0xE },
    { { {0,0},{1,0},{2,0},{3,0} }, 0, 0xF },
};
static const int8_t alg_links[8][4][2] = {
    { {0,1},{1,2},{2,3},{-1,-1} },
    { {0,2},{1,2},{2,3},{-1,-1} },
    { {1,2},{2,3},{0,3},{-1,-1} },
    { {0,1},{1,3},{2,3},{-1,-1} },
    { {0,1},{2,3},{-1,-1},{-1,-1} },
    { {0,1},{0,2},{0,3},{-1,-1} },
    { {0,1},{-1,-1},{-1,-1},{-1,-1} },
    { {-1,-1},{-1,-1},{-1,-1},{-1,-1} },
};

static void draw_algorithm(int x, int y, int alg, int fb)
{
    const int bw = 13, bh = 11, sx = 22, sy = 15;
    int out_x = x + 4 * sx + 4;
    gfx_rect(out_x, y, 2, 3 * sy, COL_DIM);
    for (int k = 0; k < 4 && alg_links[alg][k][0] >= 0; k++) {
        int a = alg_links[alg][k][0], b = alg_links[alg][k][1];
        int ax = x + algs[alg].pos[a][0] * sx + bw, ay = y + algs[alg].pos[a][1] * sy + bh / 2;
        int bx = x + algs[alg].pos[b][0] * sx, by = y + algs[alg].pos[b][1] * sy + bh / 2;
        gfx_line(ax, ay, bx, by, COL_LINE);
    }
    for (int i = 0; i < 4; i++) {
        int bx = x + algs[alg].pos[i][0] * sx, by = y + algs[alg].pos[i][1] * sy;
        int carrier = algs[alg].carriers >> i & 1;
        if (carrier)
            gfx_line(bx + bw, by + bh / 2, out_x, by + bh / 2, COL_LINE);
        gfx_rect(bx, by, bw, bh, carrier ? COL_BLUE : COL_PANEL2);
        gfx_num(bx + 4, by + 2, i + 1, COL_TEXT);
    }
    if (fb) {                                   /* OP1's feedback loop */
        int bx = x + algs[alg].pos[0][0] * sx, by = y + algs[alg].pos[0][1] * sy;
        gfx_frame(bx - 2, by - 2, bw + 4, bh + 4, COL_ORANGE);
    }
}

/* An envelope: attack, decay to the sustain level, the second decay, release */
static void draw_envelope(int x, int y, int w, int h, const fm_op *o, uint8_t col)
{
    gfx_rect(x, y, w, h, COL_PANEL);
    int peak = h - 2 - (h - 4) * (127 - o->tl) / 127;
    int base = y + h - 2;
    int top = y + peak;
    int sus = top + (base - top) * o->sl / 15;
    int xa = x + 1 + (31 - o->ar) * (w / 4) / 31;
    int xd = xa + (31 - o->d1r) * (w / 4) / 31;
    int xs = x + w * 3 / 4;
    int s_end = o->d2r ? sus + (base - sus) * o->d2r / 31 : sus;
    int xr = xs + (15 - o->rr) * (w / 4 - 2) / 15 + 1;
    if (xr > x + w - 2) xr = x + w - 2;
    gfx_line(x + 1, base, xa, top, col);
    gfx_line(xa, top, xd, sus, col);
    gfx_line(xd, sus, xs, s_end, col);
    gfx_line(xs, s_end, xr, base, col);
}

/* ------------------------------------------------------------ the library */

static const char *target_path(void)
{
    if (target == DRUMS_T) return "";
    return chan_path[shown_ch()];
}

enum { PART_ROWS = 1, PART_LIST = 2, PART_PANEL = 4 };
static int part_dirty, drawn_cursor = -1;
static int extra_row = -1;              /* PART_ROWS: one more row to draw again */

/* list entry i is the file the target has */
static int is_loaded(int i)
{
    if (i < 0 || i >= lib_count || lib_is_dir(i)) return 0;
    char rel[PATH_MAX_ + NAME_MAX + 2];
    if (IS_PSG(target)) {
        if (!psg_from[target - PSG_T]) return 0;
        lib_rel(i, rel);
        return path_hash(rel) == psg_from[target - PSG_T];
    }
    const char *t = target_path();
    if (!t[0]) return 0;
    lib_rel(i, rel);
    return !strcmp(rel, t);
}

static void set_origin(int c, const char *rel)
{
    strncpy(chan_path[c], rel, sizeof chan_path[c] - 1);
    chan_path[c][sizeof chan_path[c] - 1] = 0;
    chan_edited[c] = 0;
}

/* POLY mode plays every note on any of the six FM channels: they all have
 * to have the sound, whatever the target says */
static int all_six(void)
{
    return target == ALL_CH || (settings[SET_MODE] == 1 && target < ALL_CH);
}

static void apply_to_target(const fm_patch *p, const char *rel)
{
    if (all_six()) {
        for (int c = 0; c < 6; c++) {
            voice_apply(c, p);
            set_origin(c, rel);
        }
    } else {
        slot_apply(shown_ch(), p);
        set_origin(shown_ch(), rel);
    }
}

static void load_cursor(void)
{
    if (lib_is_dir(cursor) || target == DRUMS_T) return;
    for (int r = 0; r < LIST_ROWS; r++)          /* (the old mark goes) */
        if (is_loaded(top + r)) extra_row = top + r;
    part_dirty |= PART_ROWS | PART_PANEL;
    fm_patch p;
    psg_patch *ps = &psg_shown;     /* (shown only for a PSG target, which then has it) */
    int e = lib_load_any(cursor, &p, ps);
    if (e != LOAD_FM && e != LOAD_PSG) {
        say_err("CANNOT READ THAT FILE:", e);
        return;
    }
    char rel[PATH_MAX_ + NAME_MAX + 2];
    lib_rel(cursor, rel);
    if (IS_PSG(target) != (e == LOAD_PSG)) {    /* the wrong kind for the target */
        say(e == LOAD_PSG ? "A PSG INSTRUMENT: PRESS C FOR PSG 1-3/NOISE"
                          : "AN FM PATCH: PRESS C FOR AN FM CHANNEL");
        return;
    }
    if (e == LOAD_PSG) {
        int c = target - PSG_T;
        lib_psg_set(c, ps);
        psg_from[c] = path_hash(rel);
        psg_edited[c] = 0;
        psg_rel[c][0] = 0;
        if (strlen(rel) < sizeof psg_rel[c]) strcpy(psg_rel[c], rel);
        strncpy(psg_name[c], base_name(rel), sizeof psg_name[c] - 1);
        psg_name[c][sizeof psg_name[c] - 1] = 0;
        return;
    }
    if (settings[SET_LOUD]) {                   /* its loudest carrier at full volume */
        int lo = 127;
        for (int i = 0; i < 4; i++)
            if (algs[p.alg].carriers >> i & 1 && p.op[i].tl < lo) lo = p.op[i].tl;
        for (int i = 0; i < 4; i++)
            if (algs[p.alg].carriers >> i & 1) p.op[i].tl = (uint8_t)(p.op[i].tl - lo);
    }
    apply_to_target(&p, rel);
    voice = p;
}

/* The library is drawn in parts, so moving the cursor, scrolling or loading
 * a patch redraws only what changed (the Genesis-only build draws slowly). */
static void lib_row(int i)                      /* row i of the list, cleared first */
{
    int r = i - top;
    if (r < 0 || r >= LIST_ROWS) return;
    int y = LIST_Y + r * CHAR_H;
    gfx_rect(0, y - 1, 189, CHAR_H, COL_BG);
    if (i >= lib_count) return;
    if (i == cursor)
        gfx_rect(2, y - 1, 186, CHAR_H, COL_CURSOR);
    if (is_loaded(i))                           /* the target's file: a green mark */
        gfx_rect(1, y, 3, CHAR_H - 2, COL_GREEN);
    if (lib_is_dir(i)) {                        /* a folder: a tab and a name in blue */
        gfx_rect(6, y + 1, 4, 2, COL_HEAD);
        gfx_rect(6, y + 2, 9, 5, COL_HEAD);
        int n = gfx_text_n(18, y, lib_name(i), 27, i == cursor ? COL_HI : COL_HEAD);
        gfx_text(18 + n * CHAR_W, y, "/", COL_HEAD);
    } else {
        gfx_text_n(6, y, lib_name(i), 30, i == cursor ? COL_HI : COL_TEXT);
    }
}

static void lib_list(void)                      /* every row and the scroll bar */
{
    gfx_rect(0, LIST_Y - 1, 194, LIST_ROWS * CHAR_H + 1, COL_BG);
    if (!lib_count && lib_path[0]) {
        gfx_text(16, 60, "THIS FOLDER HAS NO PATCH FILES", COL_HI);
        gfx_text(16, 76, "B: BACK UP A FOLDER", COL_TEXT);
    } else if (!lib_count) {
        gfx_text(16, 60, "NO PATCH FILES FOUND", COL_HI);
        gfx_text(16, 76, "PUT .TFI .DMP OR .Y12 FILES IN THE FOLDER", COL_TEXT);
        gfx_text(16, 88, "MIDI_PATCHES ON THE SD CARD.", COL_TEXT);
        gfx_text(16, 104, "A: LOOK AGAIN    START: THE GUIDE", COL_TEXT);
    }
    for (int r = 0; r < LIST_ROWS && top + r < lib_count; r++)
        lib_row(top + r);
    if (lib_count > LIST_ROWS) {                /* the scroll bar */
        int h = LIST_ROWS * CHAR_H;
        int bar = h * LIST_ROWS / lib_count;
        if (bar < 6) bar = 6;
        int pos = (h - bar) * top / (lib_count - LIST_ROWS);
        gfx_rect(190, LIST_Y - 1, 3, h, COL_PANEL);
        gfx_rect(190, LIST_Y - 1 + pos, 3, bar, COL_DIM);
    }
}

/* a PSG sequence as bars (lo..hi), the loop's step marked under it */
static void psg_graph(int x, int y, int w, int h, const psg_seq *q, int lo, int hi, uint8_t col)
{
    gfx_rect(x, y, w, h, COL_PANEL2);
    if (!q->len) { gfx_text(x + 3, y + h / 2 - 3, "-", COL_DIM); return; }
    int bw = w / q->len;
    if (bw < 1) bw = 1;
    if (bw > 6) bw = 6;
    int zero = y + h - 1 - (0 - lo) * (h - 1) / (hi - lo);
    if (zero > y + h - 1) zero = y + h - 1;
    for (int i = 0; i < q->len && (i + 1) * bw <= w; i++) {
        int v = q->v[i];
        if (v < lo) v = lo;
        if (v > hi) v = hi;
        int py = y + h - 1 - (v - lo) * (h - 1) / (hi - lo);
        int t = py < zero ? py : zero, b = py < zero ? zero : py;
        gfx_rect(x + i * bw, t, bw > 1 ? bw - 1 : 1, b - t + 1, col);
    }
    if (q->loop < q->len && (q->loop + 1) * bw <= w)
        gfx_rect(x + q->loop * bw, y + h + 1, bw > 1 ? bw - 1 : 1, 2, COL_HI);
}

static void psg_panel(int px)
{
    int c = target - PSG_T;
    char t[24] = "MIDI CH ";
    *fmt_num(t + 8, 7 + c) = 0;
    gfx_text(px, 32, t, COL_DIM);
    if (!psg_name[c][0]) {
        gfx_text(px, 48, "NO INSTRUMENT:", COL_HI);
        gfx_text(px, 60, "A PLAIN TONE AT THE", COL_TEXT);
        gfx_text(px, 70, "KEY'S VOLUME.", COL_TEXT);
        gfx_text(px, 86, "CHOOSE A .DMP PSG", COL_TEXT);
        gfx_text(px, 96, "INSTRUMENT FILE", COL_TEXT);
        gfx_text(px, 106, "(DEFLEMASK, GENESIS", COL_TEXT);
        gfx_text(px, 116, "OR MASTER SYSTEM).", COL_TEXT);
        gfx_text(px, 132, "NEEDS THE NEW", COL_DIM);
        gfx_text(px, 142, "TEENSY FIRMWARE", COL_DIM);
        return;
    }
    gfx_text_n(px, 44, psg_name[c], 19, COL_HEAD);
    gfx_text(px, 58, "VOLUME", COL_DIM);
    psg_graph(px, 68, 112, 22, &psg_shown.vol, 0, 15, COL_GREEN);
    gfx_text(px, 98, psg_shown.arp_fixed ? "NOTES (FIXED)" : "ARPEGGIO", COL_DIM);
    psg_graph(px, 108, 112, 22, &psg_shown.arp, psg_shown.arp_fixed ? 0 : -12,
              psg_shown.arp_fixed ? 96 : 12, COL_ORANGE);
    if (c == 3) {
        gfx_text(px, 138, "NOISE: HIGH = WHITE", COL_DIM);
        psg_graph(px, 148, 112, 10, &psg_shown.noise, 0, 1, COL_BLUE);
    }
    gfx_text(px, 168, "YELLOW: THE LOOP", COL_DIM);
}

static void lib_panel(void)                     /* the target and its voice */
{
    int px = 200;
    gfx_rect(px - 4, 16, SCREEN_W - px + 4, SCREEN_H - 32, COL_PANEL);
    gfx_text(px, 20, "TARGET", COL_DIM);
    gfx_text(px + 42, 20, ch_names[target], COL_HI);
    gfx_text(SCREEN_W - 10, 20, "C", COL_DIM);
    if (target == DRUMS_T) {                    /* the kit, note by note */
        if (!synth_count) {
            gfx_text(px, 40, "NO DRUM SAMPLES", COL_HI);
            gfx_text(px, 54, "PUT .WAV FILES IN", COL_TEXT);
            gfx_text(px, 64, "MIDI_DRUMS ON THE", COL_TEXT);
            gfx_text(px, 74, "SD CARD.  THEY", COL_TEXT);
            gfx_text(px, 84, "PLAY ON MIDI", COL_TEXT);
            gfx_text(px, 94, "CHANNEL 16.", COL_TEXT);
            gfx_text(px, 112, "NEEDS THE NEW", COL_DIM);
            gfx_text(px, 122, "TEENSY FIRMWARE", COL_DIM);
        }
        gfx_text(px, 32, "MIDI CH 16   X HEAR", COL_DIM);
        for (int i = 0; i < synth_count && i < 17; i++) {
            int y = 44 + i * 9;
            gfx_num(px, y, synth_note_of(i), i == drum_pick ? COL_HI : COL_HEAD);
            gfx_text_n(px + 22, y, synth_names[i], 15, i == drum_pick ? COL_HI : COL_TEXT);
        }
        if (synth_count > 17) gfx_text(px, 44 + 17 * 9, "...", COL_DIM);
        return;
    }
    if (IS_PSG(target)) {
        psg_panel(px);
        return;
    }
    char t[40], *o = t;
    o = (char *)memcpy(o, "ALG ", 4) + 4; o = fmt_num(o, voice.alg);
    o = (char *)memcpy(o, "  FB ", 5) + 5; o = fmt_num(o, voice.fb); *o = 0;
    gfx_text(px, 34, t, COL_TEXT);
    draw_algorithm(px + 2, 48, voice.alg, voice.fb);
    for (int i = 0; i < 4; i++) {
        gfx_text(px, 104 + i * 20, "OP", COL_DIM);
        gfx_num(px + 12, 104 + i * 20, i + 1, COL_DIM);
        draw_envelope(px + 22, 100 + i * 20, 92, 17, &voice.op[i], COL_ORANGE);
    }
}

static const char *footer_shown;         /* (the library's last hint: not drawn again) */

static void lib_footer(void)
{
    const char *h;
    if (lib_count && lib_is_dir(cursor))
        h = lib_path[0] ? "A OPEN THE FOLDER  B UP  START MENU" : "A OPEN THE FOLDER  START MENU";
    else if (lib_count)
        h = lib_path[0] ? "UP/DN PATCH  A PICK/EDIT  C CHAN  B UP  X HEAR"
                        : "UP/DN PATCH  A PICK/EDIT  C CHAN  X HEAR  START MENU";
    else
        h = lib_path[0] ? "B: BACK UP A FOLDER" : "A: LOOK AGAIN   START: MENU";
    if (h == footer_shown && message_timer <= 0) return;
    footer(h);
    footer_shown = message_timer > 0 ? 0 : h;
}

static void draw_library(void)
{
    footer_shown = 0;
    gfx_clear(COL_BG);
    header(APP_TITLE);
    {                                           /* where we are */
        char where[PATH_MAX_ + 16] = "MIDI_PATCHES";
        if (lib_path[0]) { strcat(where, "/"); strcat(where, lib_path); }
        int n = (int)strlen(where), max = 31;
        gfx_text(6, 13, n > max ? where + n - max : where, COL_HEAD);
    }
    lib_list();
    if (lib_skipped) {                          /* files we could not list */
        char t[48], *o = fmt_num(t, lib_skipped);
        strcpy(o, " FILES LEFT OUT: NAMES TOO LONG/TOO MANY");
        gfx_rect(0, LIST_Y + LIST_ROWS * CHAR_H, 194, CHAR_H, COL_BG);
        gfx_text_n(4, LIST_Y + LIST_ROWS * CHAR_H, t, 31, COL_ORANGE);
    }
    lib_panel();
    lib_footer();
    drawn_cursor = cursor;
    part_dirty = 0;
}

/* only the parts that changed (the library with nothing over it) */
static void draw_library_parts(void)
{
    if (part_dirty & PART_LIST) {
        lib_list();
    } else if (part_dirty & PART_ROWS) {
        if (drawn_cursor != cursor) lib_row(drawn_cursor);
        if (extra_row >= 0 && extra_row != cursor && extra_row != drawn_cursor) lib_row(extra_row);
        lib_row(cursor);
    }
    extra_row = -1;
    if (part_dirty & PART_PANEL) lib_panel();
    lib_footer();
    drawn_cursor = cursor;
    part_dirty = 0;
    gfx_present();
}

static void library_input(uint16_t p)
{
    int old = cursor;
    if (p & PAD_UP) cursor--;
    if (p & PAD_DOWN) cursor++;
    if (p & PAD_LEFT) cursor -= LIST_ROWS;
    if (p & PAD_RIGHT) cursor += LIST_ROWS;
    if (cursor >= lib_count) cursor = lib_count - 1;
    if (cursor < 0) cursor = 0;
    int old_top = top;
#ifdef MD_ONLY
    /* (the Genesis draws a whole list slowly: past an edge, half a page at a time) */
    if (cursor < top) top = cursor - LIST_ROWS / 2;
    if (cursor >= top + LIST_ROWS) top = cursor - LIST_ROWS / 2;
    if (top > lib_count - LIST_ROWS) top = lib_count - LIST_ROWS;
    if (top < 0) top = 0;
#else
    if (cursor < top) top = cursor;
    if (cursor >= top + LIST_ROWS) top = cursor - LIST_ROWS + 1;
#endif
    if (cursor != old) {
        load_timer = lib_is_dir(cursor) ? -1 : 10;  /* load once the cursor rests */
        part_dirty |= top != old_top ? PART_LIST : PART_ROWS;
    }
    if (p & PAD_C) {
        target = next_target(target, 0);
        slot_read(shown_ch(), &voice);
        if (IS_PSG(target)) lib_psg_get(target - PSG_T, &psg_shown);
        part_dirty |= PART_PANEL | PART_LIST;      /* (the marks too) */
    }
    if (p & PAD_START) {
        overlay = OV_MENU;
        menu_choice = 0;
        dirty = 1;
        return;
    }
    if ((p & PAD_B) && lib_path[0]) {           /* up a folder, the cursor on it */
        char from[NAME_MAX + 1];
        strncpy(from, base_name(lib_path), NAME_MAX);
        from[NAME_MAX] = 0;
        lib_up();
        cursor = top = 0;
        for (int i = 0; i < lib_count; i++)
            if (lib_is_dir(i) && !strcmp(lib_name(i), from)) cursor = i;
        if (cursor >= LIST_ROWS) top = cursor - LIST_ROWS / 2;
        if (top > lib_count - LIST_ROWS) top = lib_count - LIST_ROWS;
        if (top < 0) top = 0;
        load_timer = -1;
        dirty = 1;
        return;
    }
    if ((p & PAD_A) && !lib_count) {
        if (!lib_path[0]) ui_rescan();
        return;
    }
    if ((p & PAD_A) && target == DRUMS_T && !lib_is_dir(cursor)) {
        say("DRUMS: CHOOSE A CHANNEL WITH C TO EDIT");
        return;
    }
    if ((p & PAD_A) && lib_is_dir(cursor)) {
        if (lib_enter(cursor) < 0) say("COULD NOT OPEN THAT FOLDER (SD CARD)");
        cursor = top = 0;
        load_timer = lib_count && !lib_is_dir(0) ? 10 : -1;     /* (the first file: chosen) */
        dirty = 1;
        return;
    }
    if ((p & PAD_A) && !is_loaded(cursor)) {     /* A on a file: choose it now */
        load_timer = -1;
        load_cursor();
        return;
    }
    if ((p & PAD_A) && IS_PSG(target)) {        /* the PSG instrument editor */
        screen = SCR_PSGED;
        pe_row = pe_step = 0;
        lib_psg_get(target - PSG_T, &psg_shown);
        dirty = 1;
        return;
    }
    if (p & PAD_A) {                            /* A on the chosen one: edit it */
        screen = SCR_EDITOR;
        ed_row = ed_col = 0;
        ed_changing = 0;
        slot_read(shown_ch(), &voice);
        dirty = 1;
    }
    if ((p & PAD_Y) && lib_count) {
        if (lib_is_dir(cursor)) say("FOLDERS ARE NOT DELETED HERE");
        else overlay = OV_DELETE;
        dirty = 1;
    }
}

/* ------------------------------------------------------------ the editor */

enum { P_ALG, P_FB, P_PAN, P_AMS, P_FMS, P_LFO, NGLOBAL };
static const char *const gname[NGLOBAL] = { "ALG", "FB", "PAN", "AMS", "FMS", "LFO" };
enum { O_MUL, O_DT, O_TL, O_RS, O_AR, O_D1R, O_SL, O_D2R, O_RR, O_AM, O_SSG, NOP_ROWS };
static const char *const oname[NOP_ROWS] = { "MUL", "DT", "TL", "RS", "AR", "D1R", "SL", "D2R", "RR", "AM", "SSG" };
static const uint8_t omax[NOP_ROWS] = { 15, 6, 127, 3, 31, 31, 15, 31, 15, 1, 8 };

static int lfo_value(void)
{
    uint8_t r = shadow_byte(0x22);
    return (r & 8) ? (r & 7) + 1 : 0;
}

static int dt_to_ui(int dt) { return dt < 4 ? dt + 3 : 3 - (dt - 4); }   /* 0..6, 3 = none */
static int dt_from_ui(int v) { return v >= 3 ? v - 3 : 4 + (3 - v); }

static int op_get(const fm_op *o, int row)
{
    switch (row) {
    case O_MUL: return o->mul;
    case O_DT: return dt_to_ui(o->dt);
    case O_TL: return o->tl;
    case O_RS: return o->rs;
    case O_AR: return o->ar;
    case O_D1R: return o->d1r;
    case O_SL: return o->sl;
    case O_D2R: return o->d2r;
    case O_RR: return o->rr;
    case O_AM: return o->am;
    default: return (o->ssg & 8) ? (o->ssg & 7) + 1 : 0;
    }
}

static void op_set(fm_op *o, int row, int v)
{
    switch (row) {
    case O_MUL: o->mul = (uint8_t)v; break;
    case O_DT: o->dt = (uint8_t)dt_from_ui(v); break;
    case O_TL: o->tl = (uint8_t)v; break;
    case O_RS: o->rs = (uint8_t)v; break;
    case O_AR: o->ar = (uint8_t)v; break;
    case O_D1R: o->d1r = (uint8_t)v; break;
    case O_SL: o->sl = (uint8_t)v; break;
    case O_D2R: o->d2r = (uint8_t)v; break;
    case O_RR: o->rr = (uint8_t)v; break;
    case O_AM: o->am = (uint8_t)v; break;
    default: o->ssg = (uint8_t)(v ? 8 | (v - 1) : 0); break;
    }
}

static void cell_text(char *t, int row, int v)
{
    char *o = t;
    if (row == O_DT) {
        v -= 3;
        *o++ = v > 0 ? '+' : v < 0 ? '-' : ' ';
        o = fmt_num(o, v < 0 ? -v : v);
    } else if (row == O_SSG && !v) {
        *o++ = '-';
    } else if (row == O_AM) {
        o = (char *)memcpy(o, v ? "ON" : "--", 2) + 2;
    } else {
        o = fmt_num(o, v);
    }
    *o = 0;
}

static void draw_editor(void)
{
    gfx_clear(COL_BG);
    char t[60], *o;
    o = (char *)memcpy(t, "EDIT ", 5) + 5;
    strcpy(o, ch_names[target]);
    header(t);
    int ch = shown_ch();
    const char *name = chan_path[ch][0] ? chan_path[ch] : "(NOT FROM A FILE)";
    gfx_text_n(4, 16, name, 34, COL_TEXT);
    if (chan_edited[ch]) gfx_text(4 + 35 * CHAR_W, 16, "*", COL_ORANGE);

    /* the global row */
    static const char *const pans[4] = { "OFF", "R", "L", "LR" };
    int gv[NGLOBAL] = { voice.alg, voice.fb, voice.lr, voice.ams, voice.fms, lfo_value() };
    for (int i = 0; i < NGLOBAL; i++) {
        int x = 4 + i * 34, y = 30;
        int sel = ed_row == 0 && ed_col == i;
        if (sel) gfx_rect(x - 2, y - 2, 32, 21, ed_changing ? COL_ORANGE : COL_CURSOR);
        gfx_text(x, y, gname[i], COL_DIM);
        o = t;
        if (i == P_PAN) o = stpcpy(o, pans[gv[i] & 3]);
        else if (i == P_LFO && !gv[i]) o = stpcpy(o, "OFF");
        else o = fmt_num(o, i == P_LFO ? gv[i] - 1 : gv[i]);
        *o = 0;
        gfx_text(x, y + 10, t, sel ? (ed_changing ? COL_BG : COL_HI) : COL_TEXT);
    }
    draw_algorithm(214, 26, voice.alg, voice.fb);

    /* the operator table */
    int tx = 40, ty = 80, cw = 36;
    for (int i = 0; i < 4; i++) {
        o = (char *)memcpy(t, "OP", 2) + 2;
        o = fmt_num(o, i + 1);
        *o = 0;
        gfx_text(tx + i * cw, ty - 11, t, COL_HEAD);
    }
    for (int r = 0; r < NOP_ROWS; r++) {
        int y = ty + r * CHAR_H;
        gfx_text(4, y, oname[r], COL_DIM);
        for (int i = 0; i < 4; i++) {
            int sel = ed_row == r + 1 && ed_col == i;
            if (sel) gfx_rect(tx + i * cw - 2, y - 1, cw - 4, CHAR_H, ed_changing ? COL_ORANGE : COL_CURSOR);
            cell_text(t, r, op_get(&voice.op[i], r));
            gfx_text(tx + i * cw, y, t, sel ? (ed_changing ? COL_BG : COL_HI) : COL_TEXT);
        }
    }
    for (int i = 0; i < 4; i++) {
        int ex = 186 + (i & 1) * 66, ey = 84 + (i >> 1) * 52;
        draw_envelope(ex, ey, 62, 44, &voice.op[i],
                      (algs[voice.alg].carriers >> i & 1) ? COL_ORANGE : COL_GREEN);
        gfx_num(ex + 54, ey + 2, i + 1, COL_DIM);
    }
    gfx_text(186, 190, "ORANGE: CARRIERS", COL_DIM);
    gfx_text(186, 199, "GREEN: MODULATORS", COL_DIM);
    /* what the setting under the cursor does (two lines of 29) */
    static const char *const ghint[NGLOBAL][2] = {
        { "ALG: WHO FEEDS WHOM. MORE", "CARRIERS = FULLER, 0 = HARSH" },
        { "FB: OP1 FEEDS ITSELF. 1-3", "SAW-LIKE, 6-7 GRIT AND NOISE" },
        { "PAN: LEFT, RIGHT OR BOTH", "(NOT SAVED IN TFI OR DMP)" },
        { "AMS: TREMOLO DEPTH ON THE", "OPERATORS WITH AM ON" },
        { "FMS: VIBRATO DEPTH (THE LFO)", "2-3 SOUNDS NATURAL" },
        { "LFO: SPEED, SHARED BY ALL", "SIX CHANNELS. OFF = NONE" },
    };
    static const char *const ohint[NOP_ROWS][2] = {
        { "MUL: PITCH x THE NOTE. 0=1/2", "MOD 1 SAW, 2 SQUARE, 7 BELL" },
        { "DT: A SLIGHT DETUNE. +2/-2 ON", "TWO CARRIERS = CHORUS" },
        { "TL: 0 LOUDEST. CARRIER = VOL,", "MODULATOR = BRIGHTNESS" },
        { "RS: HIGH NOTES DECAY FASTER", "1-2 FOR PIANO AND GUITAR" },
        { "AR: ATTACK. 31 INSTANT, 12", "SLOW SWELL, 0 = SILENT" },
        { "D1R: FALL FROM THE PEAK TO", "SL. 0 = NO FALL" },
        { "SL: WHERE D1R STOPS. 0 = PEAK", "15 = ALL THE WAY DOWN" },
        { "D2R: FADE WHILE HELD. 0 HOLDS", "(ORGAN), 1-5 PIANO-LIKE" },
        { "RR: AFTER KEY UP. 15 CUTS,", "7-9 SHORT TAIL, 2-5 RINGS" },
        { "AM: THIS OPERATOR FOLLOWS THE", "LFO TREMOLO (AMS)" },
        { "SSG: LOOPING ENVELOPE SHAPES", "1-8, FOR BUZZ AND SWEEPS" },
    };
    const char *const *hint = ed_row == 0 ? ghint[ed_col] : ohint[ed_row - 1];
    gfx_text(4, 190, hint[0], COL_HEAD);
    gfx_text(4, 199, hint[1], COL_DIM);
    footer(ed_changing ? "UP/DOWN 1  LEFT/RIGHT 10  A OR B: DONE"
                       : "A CHANGE  B BACK  C CHAN  START SAVE");
}

static void voice_changed(void)
{
    if (all_six()) {
        for (int c = 0; c < 6; c++) { voice_apply(c, &voice); chan_edited[c] = 1; }
    } else {
        slot_apply(shown_ch(), &voice);
        chan_edited[shown_ch()] = 1;
    }
    dirty = 1;
}

static void editor_change(int d)
{
    if (ed_row == 0) {
        int v;
        switch (ed_col) {
        case P_ALG: v = (voice.alg + d) & 7; voice.alg = (uint8_t)v; break;
        case P_FB: v = (voice.fb + d) & 7; voice.fb = (uint8_t)v; break;
        case P_PAN: v = (voice.lr + d) & 3; voice.lr = (uint8_t)v; break;
        case P_AMS: v = (voice.ams + d) & 3; voice.ams = (uint8_t)v; break;
        case P_FMS: v = (voice.fms + d) & 7; voice.fms = (uint8_t)v; break;
        default: {
            v = lfo_value() + d;
            if (v < 0) v = 0;
            if (v > 8) v = 8;
            ym_global(0x22, (uint8_t)(v ? 8 | (v - 1) : 0));
            dirty = 1;
            return;
        }
        }
        voice_changed();
        return;
    }
    int r = ed_row - 1, v = op_get(&voice.op[ed_col], r) + d;
    if (v < 0) v = 0;
    if (v > omax[r]) v = omax[r];
    op_set(&voice.op[ed_col], r, v);
    voice_changed();
}

static void editor_input(uint16_t p)
{
    if (ed_changing) {                          /* the D-pad changes the value */
        if (p & PAD_UP) editor_change(+1);
        if (p & PAD_DOWN) editor_change(-1);
        if (p & PAD_RIGHT) editor_change(+10);
        if (p & PAD_LEFT) editor_change(-10);
        if (p & PAD_X) editor_change(+10);
        if (p & PAD_Z) editor_change(-10);
        if (p & (PAD_A | PAD_B | PAD_Y)) { ed_changing = 0; dirty = 1; }
        if (p & (PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT)) dirty = 1;
        return;
    }
    if (p & PAD_UP) ed_row = ed_row > 0 ? ed_row - 1 : NOP_ROWS;
    if (p & PAD_DOWN) ed_row = ed_row < NOP_ROWS ? ed_row + 1 : 0;
    int ncols = ed_row == 0 ? NGLOBAL : 4;
    if (ed_col >= ncols) ed_col = ncols - 1;
    if (p & PAD_LEFT) ed_col = ed_col > 0 ? ed_col - 1 : ncols - 1;
    if (p & PAD_RIGHT) ed_col = ed_col < ncols - 1 ? ed_col + 1 : 0;
    if (p & (PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT)) dirty = 1;
    if (p & PAD_A) { ed_changing = 1; dirty = 1; }
    if (p & PAD_X) editor_change(+10);
    if (p & PAD_Z) editor_change(-10);
    if (p & PAD_C) {
        target = next_target(target, 1);
        slot_read(shown_ch(), &voice);
        dirty = 1;
    }
    if (p & PAD_START) {
        overlay = OV_SAVE;
        save_choice = chan_path[shown_ch()][0] ? 0 : 1;
        dirty = 1;
    }
    if (p & (PAD_B | PAD_Y)) {                  /* back to the library */
        screen = SCR_LIBRARY;
        dirty = 1;
    }
}

/* ------------------------------------------------------------ the PSG editor */

static void panel(int x, int y, int w, int h);

/* The target's PSG instrument (psg_shown) as bar graphs, a step a frame:
 * the volume, the arpeggio and (NOISE) white / periodic. LEFT/RIGHT the
 * step, UP/DOWN its value, A the next sequence, X / Z a step more / less,
 * Y the loop here, START more (save ...), B back. Heard at once. */
#define PE_X    46                      /* the graphs' left edge; 5 pixels a step */
#define PE_W    5

static psg_seq *pe_seq(int r)
{
    return r == 0 ? &psg_shown.vol : r == 1 ? &psg_shown.arp : &psg_shown.noise;
}

static int pe_rows(void) { return target == PSG_T + 3 ? 3 : 2; }

static void pe_range(int r, int *lo, int *hi)
{
    if (r == 0) { *lo = 0; *hi = 15; }
    else if (r == 2) { *lo = 0; *hi = 1; }
    else if (psg_shown.arp_fixed) { *lo = 0; *hi = 95; }
    else { *lo = -48; *hi = 48; }
}

static void pe_changed(void)
{
    int c = target - PSG_T;
    lib_psg_set(c, &psg_shown);
    psg_edited[c] = 1;
    dirty = 1;
}

static void pe_add(void)                         /* a step more (the last one again) */
{
    psg_seq *q = pe_seq(pe_row);
    if (q->len >= PSG_STEPS) { say("48 STEPS AT MOST"); return; }
    q->v[q->len] = q->len ? q->v[q->len - 1] : (pe_row == 0 ? 15 : 0);
    q->len++;
    pe_step = q->len - 1;
    pe_changed();
}

static void pe_remove(void)
{
    psg_seq *q = pe_seq(pe_row);
    if (!q->len) return;
    q->len--;
    if (q->loop != 0xFF && q->loop >= q->len) q->loop = 0xFF;
    if (pe_step >= q->len && pe_step) pe_step = q->len ? q->len - 1 : 0;
    pe_changed();
}

static void pe_graph(int r, int y, int h)
{
    psg_seq *q = pe_seq(r);
    int lo, hi;
    pe_range(r, &lo, &hi);
    int glo = r == 1 && !psg_shown.arp_fixed ? -12 : lo, ghi = r == 1 && !psg_shown.arp_fixed ? 12 : hi;
    static const char *const names[3] = { "VOLUME", "ARP", "NOISE" };
    int sel = r == pe_row;
    gfx_text(4, y, r == 1 && psg_shown.arp_fixed ? "NOTES" : names[r], sel ? COL_HI : COL_DIM);
    char t[16], *o = t;
    o = fmt_num((char *)memcpy(t, "LEN ", 4) + 4, q->len);
    *o = 0;
    gfx_text(4, y + 11, t, COL_DIM);
    if (q->loop != 0xFF) { o = fmt_num((char *)memcpy(t, "LP ", 3) + 3, q->loop + 1); *o = 0; }
    else strcpy(t, "NO LP");
    gfx_text(4, y + 21, t, q->loop != 0xFF ? COL_HI : COL_DIM);
    gfx_rect(PE_X - 1, y - 1, PSG_STEPS * PE_W + 2, h + 2, sel ? COL_PANEL2 : COL_PANEL);
    if (sel) gfx_rect(PE_X + pe_step * PE_W, y - 1, PE_W, h + 2, COL_CURSOR);
    int zero = y + h - 1 - (0 - glo) * (h - 1) / (ghi - glo);
    if (zero > y + h - 1) zero = y + h - 1;
    if (glo < 0) gfx_rect(PE_X, zero, PSG_STEPS * PE_W, 1, COL_LINE);
    uint8_t col = r == 0 ? COL_GREEN : r == 1 ? COL_ORANGE : COL_BLUE;
    for (int i = 0; i < q->len; i++) {
        int v = q->v[i];
        if (v < glo) v = glo;
        if (v > ghi) v = ghi;
        int py = y + h - 1 - (v - glo) * (h - 1) / (ghi - glo);
        int a = py < zero ? py : zero, b = py < zero ? zero : py;
        gfx_rect(PE_X + i * PE_W, a, PE_W - 1, b - a + 1, sel && i == pe_step ? COL_HI : col);
    }
    if (q->loop != 0xFF)
        gfx_rect(PE_X + q->loop * PE_W, y + h + 2, PE_W - 1, 2, COL_HI);
}

static void draw_psg_editor(void)
{
    int c = target - PSG_T;
    gfx_clear(COL_BG);
    char t[60];
    strcpy(t, "EDIT ");
    strcat(t, ch_names[target]);
    header(t);
    gfx_text_n(4, 15, psg_name[c][0] ? psg_name[c] : "(A NEW INSTRUMENT)", 40, COL_TEXT);
    if (psg_edited[c]) gfx_text(4 + 41 * CHAR_W, 15, "*", COL_ORANGE);
    pe_graph(0, 30, 45);
    pe_graph(1, 92, 45);
    if (pe_rows() == 3) pe_graph(2, 156, 12);
    /* the step under the cursor */
    psg_seq *q = pe_seq(pe_row);
    char *o = (char *)memcpy(t, "STEP ", 5) + 5;
    o = fmt_num(o, pe_step + 1);
    if (pe_step < q->len) {
        o = (char *)memcpy(o, ": ", 2) + 2;
        int v = q->v[pe_step];
        if (pe_row == 2) o = (char *)memcpy(o, v & 1 ? "WHITE" : "PERIODIC", v & 1 ? 5 : 8) + (v & 1 ? 5 : 8);
        else { if (pe_row == 1 && !psg_shown.arp_fixed && v > 0) *o++ = '+'; o = fmt_num(o, v); }
    } else {
        o = (char *)memcpy(o, ": - (X ADDS ONE)", 16) + 16;
    }
    *o = 0;
    gfx_text(PE_X, pe_rows() == 3 ? 180 : 156, t, COL_HI);
    gfx_text(PE_X, pe_rows() == 3 ? 192 : 168, "X/Z STEP +/-  Y LOOP  HOLD X: HEAR", COL_DIM);
    footer("L/R STEP  U/D VALUE  A ROW  START MORE  B BACK");
}

static void psg_editor_input(uint16_t p)
{
    psg_seq *q = pe_seq(pe_row);
    int lo, hi;
    pe_range(pe_row, &lo, &hi);
    if (p & PAD_LEFT) { pe_step = pe_step > 0 ? pe_step - 1 : (q->len ? q->len - 1 : 0); dirty = 1; }
    if (p & PAD_RIGHT) { pe_step = pe_step + 1 < q->len ? pe_step + 1 : 0; dirty = 1; }
    if (p & (PAD_UP | PAD_DOWN)) {
        if (pe_step >= q->len) { pe_add(); q = pe_seq(pe_row); }
        int v = q->v[pe_step] + ((p & PAD_UP) ? 1 : -1);
        if (v < lo) v = lo;
        if (v > hi) v = hi;
        q->v[pe_step] = (int8_t)v;
        pe_changed();
    }
    if (p & PAD_A) {                            /* the next sequence */
        pe_row = (pe_row + 1) % pe_rows();
        q = pe_seq(pe_row);
        if (pe_step >= q->len) pe_step = q->len ? q->len - 1 : 0;
        dirty = 1;
    }
    if (p & PAD_X) pe_add();
    if (p & PAD_Z) pe_remove();
    if ((p & PAD_Y) && pe_step < q->len) {      /* loop from here (again: none) */
        q->loop = q->loop == pe_step ? 0xFF : (uint8_t)pe_step;
        pe_changed();
    }
    if (p & PAD_START) { overlay = OV_PSGMENU; menu_choice = 0; dirty = 1; }
    if (p & PAD_B) { screen = SCR_LIBRARY; dirty = 1; }
}

/* START in the PSG editor */
#define NPSGMENU 7
static void psg_menu_item(int i, char *t)
{
    int c = target - PSG_T;
    switch (i) {
    case 0: strcpy(t, "ADD A STEP (X)"); break;
    case 1: strcpy(t, "REMOVE THE LAST STEP (Z)"); break;
    case 2: strcpy(t, "LOOP FROM THIS STEP (Y)"); break;
    case 3: strcpy(t, "NO LOOP"); break;
    case 4: strcpy(t, psg_shown.arp_fixed ? "ARPEGGIO: FIXED NOTES" : "ARPEGGIO: ADDED TO THE KEY"); break;
    case 5:
        strcpy(t, "OVERWRITE ");
        if (psg_rel[c][0]) strncat(t, base_name(psg_rel[c]), 26);
        else strcat(t, "(NOT FROM A FILE)");
        break;
    default: strcpy(t, "SAVE AS A NEW .DMP FILE"); break;
    }
}

static void draw_psg_menu(void)
{
    char t[48];
    panel(30, 60, 260, 20 + NPSGMENU * 11 + 12);
    gfx_text(40, 67, "PSG INSTRUMENT", COL_HI);
    for (int i = 0; i < NPSGMENU; i++) {
        int y = 81 + i * 11;
        if (i == menu_choice) gfx_rect(36, y - 2, 248, 11, COL_CURSOR);
        psg_menu_item(i, t);
        gfx_text_n(40, y, t, 40, i == menu_choice ? COL_HI : COL_TEXT);
    }
    gfx_text(40, 81 + NPSGMENU * 11 + 2, "A: DO IT   B: BACK", COL_DIM);
}

static void psg_save(int overwrite)
{
    int c = target - PSG_T;
    char name[PATH_MAX_ + NAME_MAX + 2];
    if (overwrite) {
        if (!psg_rel[c][0]) { say("NOT FROM A FILE: SAVE A NEW ONE"); return; }
        strcpy(name, psg_rel[c]);
    } else {
        char base[NAME_MAX + 1];
        lib_new_name(base, "dmp");
        name[0] = 0;
        if (lib_path[0]) { strcpy(name, lib_path); strcat(name, "/"); }
        strcat(name, base);
    }
    say("SAVING...");
    ui_draw();
    int e = lib_save_psg(name, &psg_shown);
    if (e) { say_err("COULD NOT SAVE:", e); return; }
    lib_scan();
    psg_from[c] = path_hash(name);
    psg_rel[c][0] = 0;
    if (strlen(name) < sizeof psg_rel[c]) strcpy(psg_rel[c], name);
    strncpy(psg_name[c], base_name(name), sizeof psg_name[c] - 1);
    psg_name[c][sizeof psg_name[c] - 1] = 0;
    psg_edited[c] = 0;
    char t[60] = "SAVED ";
    strncat(t, base_name(name), 50);
    say(t);
}

static void psg_menu_input(uint16_t p)
{
    if (p & PAD_UP) menu_choice = (menu_choice + NPSGMENU - 1) % NPSGMENU;
    if (p & PAD_DOWN) menu_choice = (menu_choice + 1) % NPSGMENU;
    if (p & (PAD_UP | PAD_DOWN)) dirty = 1;
    if (p & PAD_A) {
        psg_seq *q = pe_seq(pe_row);
        overlay = OV_NONE;
        dirty = 1;
        switch (menu_choice) {
        case 0: pe_add(); break;
        case 1: pe_remove(); break;
        case 2: if (pe_step < q->len) { q->loop = (uint8_t)pe_step; pe_changed(); } break;
        case 3: q->loop = 0xFF; pe_changed(); break;
        case 4: psg_shown.arp_fixed ^= 1; pe_changed(); break;
        case 5: psg_save(1); break;
        default: psg_save(0); break;
        }
    } else if (p & (PAD_B | PAD_START)) {
        overlay = OV_NONE;
        dirty = 1;
    }
}

/* ------------------------------------------------------------ overlays */

static void panel(int x, int y, int w, int h)
{
    gfx_rect(x, y, w, h, COL_PANEL2);
    gfx_frame(x, y, w, h, COL_HEAD);
}

static void draw_help(void)
{
    static const char *const lines[] = {
        "CONTROLS",
        "",
        "LIBRARY",
        "UP/DOWN     CHOOSE A PATCH (IT LOADS)",
        "LEFT/RIGHT  A PAGE UP / DOWN",
        "C           TARGET: CH 1-6, ALL, PSG, NOISE",
        "A           CHOOSE IT (AGAIN: EDIT IT)",
        "Y           DELETE THE PATCH FILE",
        "X           HEAR IT (MIDDLE C, HOLD)",
        "START       MENU: GUIDE, SETTINGS, MONITOR",
        "",
        "EDITOR",
        "D-PAD       CHOOSE A SETTING",
        "A           CHANGE IT: UP/DOWN 1, L/R 10",
        "A OR B      DONE CHANGING",
        "C           NEXT CHANNEL",
        "START       SAVE TO THE SD CARD",
        "B (OR Y)    BACK TO THE LIBRARY",
        "",
        "MODE (3-BUTTON PAD: B)  THIS SCREEN",
        "PRESS ANY BUTTON TO CLOSE",
    };
    int n = (int)(sizeof lines / sizeof lines[0]);
    panel(20, 14, 280, n * CHAR_H + 10);
    for (int i = 0; i < n; i++) {
        int head = i == 0 || i == 2 || i == 11 || i == n - 1;
        gfx_text(30, 20 + i * CHAR_H, lines[i], head ? COL_HI : COL_TEXT);
    }
}

static void draw_delete(void)
{
    panel(30, 80, 260, 50);
    gfx_text(40, 88, "DELETE THIS FILE FROM THE SD CARD?", COL_HI);
    gfx_text_n(40, 100, lib_name(cursor), 36, COL_TEXT);
    gfx_text(40, 116, "A: DELETE      B: KEEP IT", COL_TEXT);
}

static void draw_save(void)
{
    int ch = shown_ch();
    char t[80];
    panel(20, 70, 280, 74);
    gfx_text(30, 78, "SAVE THE VOICE OF", COL_HI);
    gfx_text(30 + 18 * CHAR_W, 78, slot_names[ch], COL_HI);
    const char *opt[4] = { "OVERWRITE ", "NEW .TFI FILE", "NEW .DMP FILE", "NEW .Y12 FILE" };
    for (int i = 0; i < 4; i++) {
        int y = 92 + i * 11;
        if (i == 0 && !chan_path[ch][0]) {
            gfx_text(40, y, "OVERWRITE (NOT FROM A FILE)", COL_DIM);
            continue;
        }
        if (i == save_choice) gfx_rect(36, y - 2, 248, 11, COL_CURSOR);
        if (i == 0) {
            strcpy(t, opt[0]);
            strncat(t, base_name(chan_path[ch]), 30);
            gfx_text(40, y, t, COL_TEXT);
        } else {
            gfx_text(40, y, opt[i], COL_TEXT);
        }
    }
    gfx_text(30, 136, "A: SAVE   B: CANCEL", COL_DIM);
}

static void do_save(void)
{
    int ch = shown_ch();
    char name[PATH_MAX_ + NAME_MAX + 2];
    static const char *const ext[4] = { "", "tfi", "dmp", "y12" };
    if (save_choice == 0) {
        strcpy(name, chan_path[ch]);
    } else {                                    /* a new file in the folder shown */
        char base[NAME_MAX + 1];
        lib_new_name(base, ext[save_choice]);
        name[0] = 0;
        if (lib_path[0]) { strcpy(name, lib_path); strcat(name, "/"); }
        strcat(name, base);
    }
    say("SAVING...");
    ui_draw();
    int e = lib_save(name, &voice);
    if (e) {
        say_err("COULD NOT SAVE:", e);
        return;
    }
    lib_scan();
    if (target == ALL_CH) {
        for (int c = 0; c < 6; c++) set_origin(c, name);
    } else {
        set_origin(ch, name);
    }
    char t[60] = "SAVED ";
    strncat(t, base_name(name), 50);
    say(t);
}

static void overlay_input(uint16_t p)
{
    switch (overlay) {
    case OV_MENU: menu_input(p); break;
    case OV_SETTINGS: settings_input(p); break;
    case OV_GUIDE: {
        int redraw = 0;
        if (!guide_input(p, &redraw)) { overlay = OV_NONE; dirty = 1; }
        else if (redraw) dirty = 1;
        break;
    }
    case OV_HELP:
        if (p) { overlay = OV_NONE; dirty = 1; }
        break;
    case OV_PSGMENU: psg_menu_input(p); break;
    case OV_MONITOR:
        if (p & (PAD_B | PAD_START | PAD_Y)) { overlay = OV_NONE; dirty = 1; }
        break;
    case OV_VIZ:
        if (p & (PAD_B | PAD_START | PAD_Y)) { viz_close(); overlay = OV_NONE; dirty = 1; }
        break;
    case OV_DELETE:
        if (p & PAD_A) {
            char name[PATH_MAX_ + NAME_MAX + 2];
            lib_rel(cursor, name);
            int e = lib_delete(name);
            overlay = OV_NONE;
            if (e) { say_err("COULD NOT DELETE:", e); break; }
            for (int c = 0; c < NSLOT; c++)
                if (!strcmp(chan_path[c], name)) chan_path[c][0] = 0;
            lib_scan();
            if (cursor >= lib_count) cursor = lib_count - 1;
            if (cursor < 0) cursor = 0;
            say("DELETED");
        } else if (p & (PAD_B | PAD_C | PAD_START)) {
            overlay = OV_NONE;
            dirty = 1;
        }
        break;
    case OV_SAVE: {
        int first = chan_path[shown_ch()][0] ? 0 : 1;
        if (p & PAD_UP) save_choice = save_choice > first ? save_choice - 1 : 3;
        if (p & PAD_DOWN) save_choice = save_choice < 3 ? save_choice + 1 : first;
        if (p & (PAD_UP | PAD_DOWN)) dirty = 1;
        if (p & PAD_A) { overlay = OV_NONE; do_save(); }
        else if (p & (PAD_B | PAD_Y)) { overlay = OV_NONE; dirty = 1; }
        break;
    }
    }
}

/* ------------------------------------------------------------ menu, settings */

#define NMENU 5
static const char *const menu_items[NMENU] = { "THE GUIDE: HOW IT ALL WORKS", "SETTINGS",
                                               "MIDI MONITOR: WHAT COMES IN", "RAINBOW VISUALIZER",
                                               "LOOK FOR PATCHES AGAIN" };

static void draw_menu(void)
{
    panel(60, 70, 200, 84);
    gfx_text(70, 77, "MENU", COL_HI);
    for (int i = 0; i < NMENU; i++) {
        int y = 91 + i * 11;
        if (i == menu_choice) gfx_rect(66, y - 2, 188, 11, COL_CURSOR);
        gfx_text(70, y, menu_items[i], i == menu_choice ? COL_HI : COL_TEXT);
    }
    gfx_text(70, 146, "A: CHOOSE   B: BACK", COL_DIM);
}

/* ---- the MIDI monitor (from the menu): every channel, what it plays, the
 * last key pressed on it, and the last message */
static uint8_t mon_ch[16][3];           /* last note (0xFF none), velocity (0 let go), keys down */
static uint8_t mon_last[4];             /* status, byte 1, byte 2, length */
static int mon_timer;
static uint16_t mon_bytes, mon_errors;
static uint8_t mon_pins[6];

static int monitor_fetch(void)          /* 1: something changed */
{
    uint8_t ch[16][3], last[4];
    md_cmd(CMD_MONITOR, 1, 0, 0);
    uint16_t a = md_p(C_P1), b = md_p(C_P2);
    last[0] = (uint8_t)(a >> 8); last[1] = (uint8_t)a; last[2] = (uint8_t)(b >> 8); last[3] = (uint8_t)b;
    for (int k = 0; k < 8; k++) {
        md_cmd(CMD_MONITOR, (uint16_t)(2 + k), 0, 0);
        uint16_t w[3] = { md_p(C_P1), md_p(C_P2), md_p(C_P3) };
        for (int j = 0; j < 6; j++)
            ch[k * 2 + j / 3][j % 3] = (uint8_t)(j & 1 ? w[j >> 1] : w[j >> 1] >> 8);
    }
    md_cmd(CMD_MONITOR, 10, 0, 0);
    uint16_t nb = md_p(C_P1), ne = md_p(C_P2);
    int changed = memcmp(ch, mon_ch, sizeof ch) || memcmp(last, mon_last, sizeof last)
                  || nb != mon_bytes || ne != mon_errors;
    mon_bytes = nb;
    mon_errors = ne;
    md_cmd(CMD_MONITOR, 11, 0, 0);
    uint16_t w[3] = { md_p(C_P1), md_p(C_P2), md_p(C_P3) };
    uint8_t pins[6];
    for (int j = 0; j < 6; j++) pins[j] = (uint8_t)(j & 1 ? w[j >> 1] : w[j >> 1] >> 8);
    if (memcmp(pins, mon_pins, sizeof pins)) changed = 1;
    memcpy(mon_pins, pins, sizeof pins);
    memcpy(mon_ch, ch, sizeof ch);
    memcpy(mon_last, last, sizeof last);
    return changed;
}

static char *put_str(char *o, const char *s) { while (*s) *o++ = *s++; return o; }

static char *put_note(char *o, int n)   /* 60 = C4 */
{
    static const char *const nm[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    o = put_str(o, nm[n % 12]);
    return fmt_num(o, n / 12 - 1);
}

static const char *chan_plays(int c)
{
    static const char *const fm[6] = { "FM 1", "FM 2", "FM 3", "FM 4", "FM 5", "FM 6" };
    static const char *const psg[3] = { "PSG 1", "PSG 2", "PSG 3" };
    static const char *const pwm[4] = { "32X FM 11", "32X FM 12", "32X FM 13", "32X FM 14" };
    if (settings[SET_MODE] && c == settings[SET_POLY_CH]) return "FM 1-6 POLY";
#ifdef MD_ONLY
    if (c >= 10) return "-";
#endif
    if (c < 6) return fm[c];
    if (c < 9) return psg[c - 6];
    if (c == 9) return "PSG NOISE";
    if (c < 14) return pwm[c - 10];
    if (c == 15) return "32X DRUMS";
    return "-";
}

static void draw_monitor(void)
{
    gfx_clear(COL_BG);
    header("MIDI MONITOR");
    uint16_t link = hw_flags() & FL_LINK;
    int midi = link == LINK_MIDI;
    char t[64], *o;
    gfx_text(6, 16, "CH  PLAYS", COL_DIM);
    gfx_text(100, 16, "KEY", COL_DIM);
    gfx_text(142, 16, "VEL", COL_DIM);
    gfx_text(172, 16, "DOWN", COL_DIM);
    uint16_t lit = lights_now();
    for (int c = 0; c < 16; c++) {
        int y = 27 + c * 10;
        light_box(6, y, c, lit >> c & 1);
        gfx_num(c < 9 ? 20 : 14, y, c + 1, COL_HEAD);
        gfx_text(30, y, chan_plays(c), link == LINK_STOCK && c >= 10 ? COL_DIM : COL_TEXT);
        if (!midi || mon_ch[c][0] == 0xFF) continue;
        o = put_note(t, mon_ch[c][0]);
        *o = 0;
        uint8_t col = mon_ch[c][1] ? COL_HI : COL_DIM;
        gfx_text(100, y, t, col);
        if (mon_ch[c][1]) gfx_num(142, y, mon_ch[c][1], COL_HI);
        else gfx_text(142, y, "OFF", COL_DIM);
        if (mon_ch[c][2]) gfx_num(178, y, mon_ch[c][2], COL_GREEN);
    }

    /* the right-hand notes */
    int px = 212;
    gfx_rect(px - 4, 16, SCREEN_W - px + 4, 172, COL_PANEL);
    static const char *const n_midi[] = { "A LIGHT: A MESSAGE", "ON THAT CHANNEL.", "",
                                          "KEY, VEL: THE LAST", "NOTE PLAYED ON IT", "(OFF: LET GO).", "",
                                          "DOWN: HOW MANY", "KEYS ARE HELD.", 0 };
    static const char *const n_stock[] = { "GENMDM'S OWN", "FIRMWARE SENDS", "SOUND-CHIP WRITES,",
                                           "NOT MIDI: THE", "LIGHTS SHOW WHICH", "VOICE PLAYS, ON",
                                           "THE CHANNEL THAT", "PLAYS IT.", "",
                                           "FOR NOTES AND", "VELOCITY, AND FOR", "CHANNELS 11-16:",
                                           "THE NEW TEENSY", "FIRMWARE.", 0 };
    static const char *const n_none[] = { "NOTHING HEARD ON", "PORT 2 YET.", "",
                                          "IS THE GENMDM IN", "CONTROLLER PORT 2", "AND ITS USB OR", "MIDI CABLE IN?",
                                          "", "PLAY A KEY.", 0 };
    const char *const *n = midi ? n_midi : link == LINK_STOCK ? n_stock : n_none;
    for (int i = 0; n[i]; i++) gfx_text(px, 20 + i * 10, n[i], i == 0 && !midi ? COL_HI : COL_TEXT);
    if (midi) {
        /* the Teensy's pin watch: the pins that moved (the MIDI jack: D2) */
        gfx_text(px, 126, "TEENSY PINS MOVING:", COL_DIM);
        o = t;
        for (int k = 0; k < 5; k++)
            for (int b = 0; b < 8; b++)
                if (mon_pins[k] >> b & 1 && o < t + 16) {
                    *o++ = "BCDEF"[k];
                    *o++ = (char)('0' + b);
                    *o++ = ' ';
                }
        if (o == t) o = put_str(t, "NONE");
        *o = 0;
        gfx_text(px, 136, t, o == t + 4 && t[0] == 'N' ? COL_DIM : COL_ORANGE);
        o = put_str(t, "BYTES ");
        o = fmt_num(o, mon_bytes);
        *o = 0;
        gfx_text(px, 156, t, COL_DIM);
        o = put_str(t, "BAD BYTES ");
        o = fmt_num(o, mon_errors);
        *o = 0;
        gfx_text(px, 166, t, mon_errors ? COL_ORANGE : COL_DIM);
        o = put_str(t, "MESSAGES ");
        o = fmt_num(o, mon_total);
        *o = 0;
        gfx_text(px, 176, t, COL_DIM);
    }

    /* the last message */
    gfx_rect(0, 190, SCREEN_W, 20, COL_PANEL2);
    o = put_str(t, "LAST: ");
    if (!midi || !mon_last[0]) {
        o = put_str(o, midi ? "NOTHING YET" : "-");
    } else {
        int st = mon_last[0] & 0xF0, a = mon_last[1], b = mon_last[2];
        switch (st) {
        case 0x90:
            if (b) {
                o = put_str(o, "NOTE ON ");  o = put_note(o, a);
                o = put_str(o, " VEL ");     o = fmt_num(o, b);
                break;
            }
            /* (velocity 0: a note off) */
            /* fall through */
        case 0x80: o = put_str(o, "NOTE OFF "); o = put_note(o, a); break;
        case 0xA0: o = put_str(o, "KEY PRESSURE "); o = put_note(o, a); o = put_str(o, " "); o = fmt_num(o, b); break;
        case 0xB0: o = put_str(o, "CC ");    o = fmt_num(o, a); o = put_str(o, " = "); o = fmt_num(o, b); break;
        case 0xC0: o = put_str(o, "PROGRAM "); o = fmt_num(o, a + 1); break;
        case 0xD0: o = put_str(o, "PRESSURE "); o = fmt_num(o, a); break;
        case 0xE0: {
            int v = (a | b << 7) - 8192;
            o = put_str(o, "PITCH BEND ");
            if (v > 0) *o++ = '+';
            o = fmt_num(o, v);
            break;
        }
        }
        o = put_str(o, "  ON CH ");
        o = fmt_num(o, (mon_last[0] & 15) + 1);
    }
    *o = 0;
    gfx_text(6, 196, t, COL_HI);
    footer("B: BACK");
}

static const char *const set_names[SET_COUNT + 2] = { "MODE", "POLY MIDI CHANNEL", "TUNING", "VELOCITY",
                                             "STEREO SPREAD", "LOUDNESS", "32X VOLUME (CH 11-16)" };
static const uint8_t set_max[SET_COUNT + 2] = { 1, 15, 1, 1, 1, 1, 10 };

static void settings_fetch(void)
{
    for (int i = 0; i < SET_COUNT; i++)
        settings[i] = (uint8_t)md_cmd(CMD_SETTING, (uint16_t)i, 0xFFFF, 0);
    static int first = 1;
    if (first) { settings[SET_PWM_VOL] = 8; settings[SET_LOUD] = 1; first = 0; }
}

static void draw_settings(void)
{
    char t[24];
    panel(16, 30, 288, 160);
    gfx_text(26, 37, "SETTINGS", COL_HI);
    for (int i = 0; i < NSET; i++) {
        int y = 50 + i * 12;
        int v = settings[i];
        if (i == set_row) gfx_rect(22, y - 3, 276, 13, COL_CURSOR);
        gfx_text(28, y, set_names[i], COL_TEXT);
        switch (i) {
        case SET_MODE: strcpy(t, v ? "POLY (6 VOICES)" : "GENMDM (CH 1-6)"); break;
        case SET_POLY_CH: *fmt_num(t, v + 1) = 0; break;
        case SET_REGION: strcpy(t, v ? "PAL" : "NTSC"); break;
        case SET_PWM_VOL: *fmt_num(t, v) = 0; break;
        case SET_LOUD: strcpy(t, v ? "FULL" : "AS SAVED"); break;
        default: strcpy(t, v ? "ON" : "OFF"); break;
        }
        gfx_text(190, y, t, i == set_row ? COL_HI : COL_TEXT);
    }
    static const char *const help[SET_COUNT + 2] = {
        "GENMDM: MIDI CH 1-6 PLAY FM 1-6.  POLY: ONE",
        "THE CHANNEL YOUR KEYBOARD SENDS ON (POLY)",
        "MATCH YOUR CONSOLE SO NOTES ARE IN TUNE",
        "HARDER KEYS PLAY LOUDER",
        "EACH POLY NOTE LEFT, RIGHT OR CENTRE",
        "FULL: A PATCH LOADS AT FULL VOLUME (GAME",
        "THE 32X'S VOICES AGAINST THE MEGA DRIVE'S",
    };
    gfx_text(28, 128 + 6, help[set_row], COL_DIM);
    if (set_row == SET_MODE)
        gfx_text(28, 139 + 6, "CHANNEL PLAYS ALL SIX FM VOICES", COL_DIM);
    if (set_row == SET_LOUD)
        gfx_text(28, 139 + 6, "PATCHES ARE OFTEN SAVED QUIETER)", COL_DIM);
    if ((hw_flags() & FL_LINK) != LINK_MIDI) {
        gfx_text(28, 157, "THESE NEED THE NEW TEENSY FIRMWARE:", COL_ORANGE);
        gfx_text(28, 167, "THE STOCK ONE DECIDES ALL THIS ITSELF", COL_ORANGE);
    }
    gfx_text(28, 178, "UP/DOWN CHOOSE  LEFT/RIGHT CHANGE  B BACK", COL_DIM);
}

static void menu_input(uint16_t p)
{
    if (p & PAD_UP) menu_choice = (menu_choice + NMENU - 1) % NMENU;
    if (p & PAD_DOWN) menu_choice = (menu_choice + 1) % NMENU;
    if (p & (PAD_UP | PAD_DOWN)) dirty = 1;
    if (p & (PAD_A | PAD_START)) {
        overlay = OV_NONE;
        if (menu_choice == 0) { guide_open(); overlay = OV_GUIDE; }
        else if (menu_choice == 1) { settings_fetch(); set_row = 0; overlay = OV_SETTINGS; }
        else if (menu_choice == 2) { settings_fetch(); monitor_fetch(); mon_timer = 0; overlay = OV_MONITOR; }
        else if (menu_choice == 3) { settings_fetch(); viz_open(); overlay = OV_VIZ; }
        else ui_rescan();
        dirty = 1;
    } else if (p & PAD_B) {
        overlay = OV_NONE;
        dirty = 1;
    }
}

static void settings_input(uint16_t p)
{
    if (p & PAD_UP) set_row = (set_row + NSET - 1) % NSET;
    if (p & PAD_DOWN) set_row = (set_row + 1) % NSET;
    int d = (p & (PAD_RIGHT | PAD_A)) ? 1 : (p & PAD_LEFT) ? -1 : 0;
    if (d) {
        int v = settings[set_row] + d;
        if (v < 0) v = set_max[set_row];
        if (v > set_max[set_row]) v = 0;
        if (set_row == SET_PWM_VOL) {
            settings[set_row] = (uint8_t)v;
            synth_set_volume(v);
        } else if (set_row == SET_LOUD) {
            settings[set_row] = (uint8_t)v;
        } else {
            settings[set_row] = (uint8_t)md_cmd(CMD_SETTING, (uint16_t)set_row, (uint16_t)v, 0);
        }
        if (set_row == SET_MODE && v == 1 && target <= ALL_CH) {   /* POLY: the sound on all six */
            int from = shown_ch();
            fm_patch p;
            slot_read(from, &p);
            char rel[sizeof chan_path[0]];
            strcpy(rel, chan_path[from]);
            int ed = chan_edited[from];
            for (int c = 0; c < 6; c++) {
                voice_apply(c, &p);
                set_origin(c, rel);
                chan_edited[c] = ed;
            }
            target = ALL_CH;
        }
    }
    if (p & (PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT | PAD_A)) dirty = 1;
    if (p & (PAD_B | PAD_START)) {
        overlay = OV_NONE;
        dirty = 1;
    }
}

/* X on the home screen: middle C while it is held, on the target's channel */
static void preview(void)
{
    if (overlay != OV_VIZ && ((screen != SCR_LIBRARY && screen != SCR_PSGED) || overlay != OV_NONE)) return;
    int down = (held_now & PAD_X) != 0;
    if (down && preview_note < 0) {
        int mch = settings[SET_MODE] ? settings[SET_POLY_CH] : (target == ALL_CH ? 0 : target);
        if (target >= PWM_T && target < PWM_T + 4) mch = 10 + target - PWM_T;   /* MIDI 11-14 */
        if (IS_PSG(target)) mch = 6 + target - PSG_T;                           /* MIDI 7-10 */
        int note = 60;
        if (target == DRUMS_T) {                /* the kit: each sample in turn */
            if (!synth_count) return;
            if (drum_pick >= synth_count) drum_pick = 0;
            mch = 15;
            note = synth_note_of(drum_pick);
            dirty = 1;
        }
        preview_note = note | mch << 8;
        md_cmd(CMD_MIDI, (uint16_t)(3 << 8 | (0x90 | mch)), (uint16_t)(note << 8 | 110), 0);
    } else if (!down && preview_note >= 0) {
        md_cmd(CMD_MIDI, (uint16_t)(3 << 8 | (0x80 | (preview_note >> 8))), (uint16_t)((preview_note & 0xFF) << 8), 0);
        preview_note = -1;
        if (target == DRUMS_T && synth_count) { drum_pick = (drum_pick + 1) % synth_count; dirty = 1; }
    }
}

/* ------------------------------------------------------------ the loop */

void ui_draw(void)
{
    part_dirty = 0;
    if (overlay == OV_VIZ) {
        viz_draw();
        gfx_present();
        dirty = 0;
        return;
    }
    if (overlay == OV_GUIDE || overlay == OV_MONITOR) {
        if (overlay == OV_GUIDE) guide_draw();
        else draw_monitor();
        gfx_present();
        dirty = 0;
        return;
    }
    if (screen == SCR_LIBRARY) draw_library();
    else if (screen == SCR_PSGED) draw_psg_editor();
    else draw_editor();
    if (overlay == OV_HELP) draw_help();
    else if (overlay == OV_DELETE) draw_delete();
    else if (overlay == OV_SAVE) draw_save();
    else if (overlay == OV_MENU) draw_menu();
    else if (overlay == OV_SETTINGS) draw_settings();
    else if (overlay == OV_PSGMENU) draw_psg_menu();
    gfx_present();
    dirty = 0;
}

void ui_rescan(void)
{
    gfx_clear(COL_BG);
    header(APP_TITLE);
    gfx_text(16, 60, "LOOKING FOR PATCHES IN MIDI_PATCHES...", COL_TEXT);
    gfx_present();
    int n = lib_scan();
    synth_load_drums(0);
    if (n < 0) {
        if (-n == ERR_NO_ED) say("NO EVERDRIVE: CANNOT READ THE SD CARD");
        else say_err("SD CARD ERROR", -n);
    }
    cursor = top = 0;
    dirty = 1;
}

void ui_start(void)
{
    gfx_init();
    ui_rescan();
    shadow_refresh();
    voice_read(0, &voice);
    settings_fetch();
    last_act = hw_activity();
}

void ui_frame(void)
{
#ifdef MD_ONLY
    extern uint16_t md_vblanks(void);
    static uint16_t last_t;
    uint16_t now_t = md_vblanks();
    frame_el = (uint16_t)(now_t - last_t);
    last_t = now_t;
    if (frame_el < 1) frame_el = 1;
    if (frame_el > 30) frame_el = 30;
#endif
    uint16_t p = read_buttons();
    /* (a 3-button pad: B is the help on the home screen's top folder; in a
     * folder it goes up) */
    int help_key = (hw_flags() & FL_PAD3) ? (screen == SCR_LIBRARY && !lib_path[0] ? PAD_B : 0) : PAD_MODE;
    if (overlay != OV_NONE) {
        overlay_input(p);
    } else if (p & help_key) {
        overlay = OV_HELP;
        dirty = 1;
    } else if (screen == SCR_LIBRARY) {
        library_input(p);
    } else if (screen == SCR_PSGED) {
        psg_editor_input(p);
    } else {
        editor_input(p);
    }
    preview();
    if (load_timer > 0 && (load_timer -= frame_el) <= 0) {
        load_timer = -1;
        if (lib_count) load_cursor();
        part_dirty |= PART_PANEL;
    }
    ui_continuous = overlay == OV_VIZ;
    if (overlay == OV_VIZ) {                    /* (it draws itself, every frame) */
        if (dirty) ui_draw();
        part_dirty = 0;
        viz_frame(frame_el);
        return;
    }
    /* the GenMDM changed something: show it (at most every few frames) */
    uint16_t act = hw_activity();
    if (act != last_act && (act_timer += frame_el) >= 6) {
        act_timer = 0;
        last_act = act;
        fm_patch now;
        uint8_t lfo = shadow_byte(0x22);
        if (is_pwm_slot(shown_ch())) { act_timer = 0; last_act = act; goto no_refresh; }
        voice_read(shown_ch(), &now);
        if (memcmp(&now, &voice, sizeof now) || lfo != shadow_byte(0x22)) {
            voice = now;
            if (screen == SCR_LIBRARY) part_dirty |= PART_PANEL;
            else dirty = 1;
        }
    }
no_refresh:
    /* the MIDI lights (and the monitor) */
    monitor_poll();
    if (overlay == OV_MONITOR && (mon_timer += frame_el) >= 3) {
        mon_timer = 0;
        if (monitor_fetch()) dirty = 1;
    }
    static int light_timer;
    if (++light_timer >= 3 && lights_now() != lights_shown && overlay != OV_GUIDE) {
        light_timer = 0;
        if (overlay == OV_MONITOR) dirty = 1;
        else if (!dirty) { draw_lights(); if (!part_dirty) gfx_present(); }
    }
    if (message_timer > 0 && (message_timer -= frame_el) <= 0)
        dirty = 1;
    static uint16_t last_flags;
    if ((hw_flags() & 0xFF) != last_flags) {    /* (the high byte counts the 32X's events) */
        last_flags = hw_flags() & 0xFF;
        dirty = 1;
    }
    if (dirty)
        ui_draw();
    else if (part_dirty && screen == SCR_LIBRARY && overlay == OV_NONE)
        draw_library_parts();
    else if (part_dirty)
        ui_draw();
}
