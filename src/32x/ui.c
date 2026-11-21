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
#include "song.h"

enum { SCR_LIBRARY, SCR_EDITOR, SCR_PSGED, SCR_ARRANGE, SCR_ROLL };
enum { OV_NONE, OV_HELP, OV_DELETE, OV_SAVE, OV_GUIDE, OV_MENU, OV_SETTINGS, OV_MONITOR, OV_PSGMENU, OV_VIZ, OV_TKMENU, OV_SONGS };

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
static int tk_from_lib;                 /* the library was opened from the tracker */
static void tk_enter(void);
static void sound_of_target(void);
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
    if (pressed & rep) {                /* (a new press: its own wait, even with A held) */
        repeat_timer = 18;
    } else if (now & rep & held_prev) {
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

static int use_patch(int e, fm_patch *pp, psg_patch *ps, const char *rel);
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
    if (!use_patch(e, &p, ps, rel) && tk_from_lib) sound_of_target();    /* (choosing for a track: it has it now) */
}

/* a patch read already (e: LOAD_FM / LOAD_PSG) into the target -> 0, or 1: the wrong kind */
static int use_patch(int e, fm_patch *pp, psg_patch *ps, const char *rel)
{
    fm_patch p = *pp;
    if (IS_PSG(target) != (e == LOAD_PSG)) {    /* the wrong kind for the target */
        say(e == LOAD_PSG ? "A PSG INSTRUMENT: PRESS C FOR PSG 1-3/NOISE"
                          : "AN FM PATCH: PRESS C FOR AN FM CHANNEL");
        return 1;
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
        return 0;
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
    return 0;
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
    if (tk_from_lib && lib_count && !lib_is_dir(cursor)) h = "UP/DN A SOUND  C CHAN  X HEAR  START: BACK TO SONG";
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
    if ((p & PAD_START) && tk_from_lib) {      /* (a sound for a track of the song) */
        tk_enter();
        return;
    }
    if (p & PAD_START) {
        overlay = OV_MENU;
        menu_choice = 0;
        dirty = 1;
        return;
    }
    if (p & PAD_Z) { tk_enter(); return; }
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

static void tkmenu_input(uint16_t p);
static void songs_input(uint16_t p);
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
    case OV_TKMENU: tkmenu_input(p); break;
    case OV_SONGS: songs_input(p); break;
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

#define NMENU 6
static const char *const menu_items[NMENU] = { "THE GUIDE: HOW IT ALL WORKS", "SETTINGS",
                                               "MIDI MONITOR: WHAT COMES IN", "RAINBOW VISUALIZER",
                                               "LOOK FOR PATCHES AGAIN", "SONGS: RECORD, ARRANGE, EDIT" };

static void draw_menu(void)
{
    panel(60, 70, 200, 95);
    gfx_text(70, 77, "MENU", COL_HI);
    for (int i = 0; i < NMENU; i++) {
        int y = 91 + i * 11;
        if (i == menu_choice) gfx_rect(66, y - 2, 188, 11, COL_CURSOR);
        gfx_text(70, y, menu_items[i], i == menu_choice ? COL_HI : COL_TEXT);
    }
    gfx_text(70, 157, "A: CHOOSE   B: BACK", COL_DIM);
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
                                             "STEREO SPREAD", "MIDI OUT TO COMPUTER",
                                             "LOUDNESS", "32X VOLUME (CH 11-16)" };
static const uint8_t set_max[SET_COUNT + 2] = { 1, 15, 1, 1, 1, 2, 1, 10 };

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
        int y = 49 + i * 11;
        int v = settings[i];
        if (i == set_row) gfx_rect(22, y - 2, 276, 11, COL_CURSOR);
        gfx_text(28, y, set_names[i], COL_TEXT);
        switch (i) {
        case SET_MODE: strcpy(t, v ? "POLY (6 VOICES)" : "GENMDM (CH 1-6)"); break;
        case SET_POLY_CH: *fmt_num(t, v + 1) = 0; break;
        case SET_REGION: strcpy(t, v ? "PAL" : "NTSC"); break;
        case SET_PWM_VOL: *fmt_num(t, v) = 0; break;
        case SET_LOUD: strcpy(t, v ? "FULL" : "AS SAVED"); break;
        case SET_MIDIOUT: strcpy(t, v == 2 ? "SONG + KEYS" : v ? "THE SONG" : "OFF"); break;
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
        "THE TRACKER'S SONG (+ YOUR KEYS) TO USB",
        "FULL: A PATCH LOADS AT FULL VOLUME (GAME",
        "THE 32X'S VOICES AGAINST THE MEGA DRIVE'S",
    };
    gfx_text(28, 136, help[set_row], COL_DIM);
    if (set_row == SET_MODE)
        gfx_text(28, 146, "CHANNEL PLAYS ALL SIX FM VOICES", COL_DIM);
    if (set_row == SET_LOUD)
        gfx_text(28, 146, "PATCHES ARE OFTEN SAVED QUIETER)", COL_DIM);
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
        else if (menu_choice == 5) tk_enter();
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

/* ------------------------------------------------------------ the song: arrange and piano roll
 *
 * The song lives on the 68000 (src/md/md_seq.c, src/common/song.h), which
 * plays and records it, so the timing never depends on the drawing. These
 * are its screens: the ARRANGE view (the 16 tracks across the bars, where
 * their notes are; the keyboard plays the selected track) and the PIANO
 * ROLL of a track (its notes as bars: pitch up, time across).
 */
#ifdef MD_ONLY
#define FAST_SCREEN 0                   /* (the Genesis draws slowly: the playhead only on the ruler) */
#else
#define FAST_SCREEN 1
#endif

#define AR_X        104                 /* the arrange view: the first column; 6 pixels a beat */
#define AR_COLW     6
#define AR_COLS     32
#define AR_Y        33                  /* the first track; 10 pixels each */
#define AR_H        10
#define PR_X        24                  /* the piano roll: 4 pixels a 16th (24 ticks) */
#define PR_Y        33
#define PR_H        4                   /* a semitone */
#define PR_N        42                  /* semitones shown */
#define PR_W        (SCREEN_W - PR_X)
#define PR_SPAN     ((uint32_t)PR_W * 6)        /* ticks across */

static int sq_flags, sq_sel, sq_bpm = 120, sq_beats = 4, sq_sflags, sq_cyc_a, sq_cyc_b = 4;
static uint32_t sq_pos;                 /* the play position (or where it starts) */
static uint16_t sq_edits;
static uint8_t tr_kind[SONG_TRACKS], tr_ch[SONG_TRACKS], tr_flags[SONG_TRACKS], tr_quant[SONG_TRACKS];
static uint32_t ar_t0;                  /* the arrange view's first tick (a bar) */
static uint32_t pr_t0, pr_cur = 0;      /* the piano roll's first tick, its cursor */
static int pr_low = 48, pr_note = 60, pr_len = 48, pr_vel = 100;
static int sq_shown_mark = -1;          /* (where the ruler shows the playhead) */
static int sq_a_down, sq_b_down, sq_c_down, sq_b_used, sq_c_used, sq_confirm = -1;
static int ov_dirty;                    /* only the menu over the screen to draw again */
static int sq_init_done;
static char sq_file[16];                /* the song's file in MIDI_SONGS ("": not saved yet) */
static int tk_bar_dirty;                /* the transport line (and the ruler) */

static unsigned sq(int op, int a, int p2, int p3)
{
    return (unsigned)md_cmd(CMD_SEQ, (uint16_t)(op << 8 | (a & 0xFF)), (uint16_t)p2, (uint16_t)p3);
}

static void song_poke(int off, int bytes, int v) { sq(SQ_POKE, bytes, off, v); }

static uint32_t bar_len(void) { return (uint32_t)(sq_beats ? sq_beats : 4) * SONG_PPQ; }

static char *put_n(char *o, int v, int digits)  /* zero-padded */
{
    char t[6];
    for (int i = digits - 1; i >= 0; i--) { t[i] = (char)('0' + v % 10); v /= 10; }
    for (int i = 0; i < digits; i++) *o++ = t[i];
    return o;
}

static void sq_fetch(void)              /* the header and the track heads */
{
    sq(SQ_PEEK, 0, SO_VER, 0);          /* version, flags, tempo (2), beats, selected */
    sq_sflags = md_p(C_P1) & 0xFF;
    sq_bpm = md_p(C_P2);
    sq_beats = md_p(C_P3) >> 8;
    sq_sel = md_p(C_P3) & 15;
    sq(SQ_PEEK, 0, SO_CYC_A, 0);
    sq_cyc_a = md_p(C_P1);
    sq_cyc_b = md_p(C_P2);
    for (int t = 0; t < SONG_TRACKS; t++) {
        sq(SQ_PEEK, 0, SO_TRACKS + t * TR_BYTES, 0);
        tr_kind[t] = (uint8_t)(md_p(C_P1) >> 8);
        tr_ch[t] = (uint8_t)(md_p(C_P1) & 15);
        tr_flags[t] = (uint8_t)(md_p(C_P2) >> 8);
        tr_quant[t] = (uint8_t)(md_p(C_P2) & 0xFF);
    }
}

static void track_name(int t, char *out)        /* (16 characters at most) */
{
    for (int i = 0; i < 16; i += 6) {
        sq(SQ_PEEK, 0, SO_TRACKS + t * TR_BYTES + TR_NAME + i, 0);
        uint16_t w[3] = { md_p(C_P1), md_p(C_P2), md_p(C_P3) };
        for (int k = 0; k < 6 && i + k < 16; k++) out[i + k] = (char)(k & 1 ? w[k >> 1] : w[k >> 1] >> 8);
    }
    out[16] = 0;
}

static const char *const kind_names[TK_COUNT] = { "FM", "PSG", "NOISE", "32X FM", "DRUMS", "MIDI" };
static const uint8_t kind_cols[TK_COUNT] = { COL_HEAD, COL_GREEN, COL_GREEN, COL_ORANGE, COL_ORANGE, COL_DIM };

/* a track -> the home screen's target that holds its sound while you choose it (-1: none) */
static int track_target(int t)
{
    switch (tr_kind[t]) {
    case TK_FM: return 0;
    case TK_PSG: return PSG_T;
    case TK_NOISE: return PSG_T + 3;
#ifndef MD_ONLY
    case TK_X32: return PWM_T + ((tr_ch[t] - 10) & 3);
#endif
    }
    return -1;
}

/* ---- sounds: the song keeps each track's (FM: 30 register values, PSG: psg_pack) */
static int fm_blob(const fm_patch *p, uint8_t *b)
{
    uint8_t regs[32][2];
    int n = patch_regs(p, 0, regs);
    for (int i = 0; i < n; i++) {
        int r = regs[i][0];
        if (r >= 0x30 && r < 0xA0) b[((r - 0x30) >> 4) * 4 + ((r & 0x0C) >> 2)] = regs[i][1];
        else if (r == 0xB0) b[28] = regs[i][1];
        else if (r == 0xB4) b[29] = regs[i][1];
    }
    return FM_SOUND;
}

static void blob_patch(const uint8_t *b, fm_patch *p)
{
    uint8_t sh[528];
    memset(sh, 0, sizeof sh);
    for (int i = 0; i < 28; i++) sh[0x30 + (i >> 2) * 0x10 + (i & 3) * 4] = b[i];
    sh[0xB0] = b[28];
    sh[0xB4] = b[29];
    patch_from_regs(p, 0, sh);
}

static int put_buf(int at, const uint8_t *b, int n)     /* into the 68000's buffer */
{
    for (int off = 0; off < n; off += 4) {
        uint8_t q[4] = { 0, 0, 0, 0 };
        for (int k = 0; k < 4 && off + k < n; k++) q[k] = b[off + k];
        int r = md_cmd(CMD_BUF_PUT, (uint16_t)(at + off), (uint16_t)(q[0] << 8 | q[1]), (uint16_t)(q[2] << 8 | q[3]));
        if (r) return r;
    }
    return 0;
}

static void track_set_name(int t, const char *name)
{
    char nm[16];
    int k = 0;
    while (k < 16 && name[k] && name[k] != '.') { nm[k] = name[k]; k++; }
    while (k < 16) nm[k++] = 0;
    for (int i = 0; i < 16; i += 2) song_poke(SO_TRACKS + t * TR_BYTES + TR_NAME + i, 2, (uint8_t)nm[i] << 8 | (uint8_t)nm[i + 1]);
}

/* the sound target t has now (as the library loaded it) -> track k's */
static void sound_from(int tgt, int k, const char *name)
{
    uint8_t b[SOUND_MAX];
    int n = 0;
    if (IS_PSG(tgt)) {
        psg_patch ps;
        if (lib_psg_get(tgt - PSG_T, &ps)) n = psg_pack(&ps, b);
    } else {
        fm_patch p;
        slot_read(tgt >= PWM_T ? tgt - 1 : tgt, &p);
        n = fm_blob(&p, b);
    }
    put_buf(BUF_DATA, b, n);
    sq(SQ_SOUND, k, n, 0);
    if (name) track_set_name(k, name);
}

static void sound_of_target(void)       /* (the library, choosing for the selected track) */
{
    int slot = target >= PWM_T && target < PWM_T + 4 ? target - 1 : target;
    const char *nm = IS_PSG(target) ? psg_name[target - PSG_T] : chan_path[slot][0] ? base_name(chan_path[slot]) : "";
    sound_from(target, sq_sel, nm);
}

/* a new song's tracks: the sounds the home screen has now */
static void sounds_from_home(void)
{
    sq_fetch();
    int fm = 0, psg = 0;
    for (int k = 0; k < SONG_TRACKS; k++) {
        int tgt = -1;
        if (tr_kind[k] == TK_FM) tgt = fm++ % 6;
        else if (tr_kind[k] == TK_PSG) tgt = PSG_T + psg++ % 3;
        else if (tr_kind[k] == TK_NOISE || tr_kind[k] == TK_X32) tgt = track_target(k);
        if (tgt < 0) continue;
        int slot = tgt >= PWM_T && tgt < PWM_T + 4 ? tgt - 1 : tgt;
        const char *nm = IS_PSG(tgt) ? psg_name[tgt - PSG_T] : chan_path[slot][0] ? base_name(chan_path[slot]) : "";
        sound_from(tgt, k, nm[0] ? nm : 0);
    }
}

static void x32_sounds(void)            /* (an opened song: the 32X's FM channels get theirs) */
{
#ifndef MD_ONLY
    uint8_t b[SOUND_MAX];
    for (int k = 0; k < SONG_TRACKS; k++) {
        if (tr_kind[k] != TK_X32) continue;
        int n = (int)sq(SQ_SOUND, k, 0xFFFF, 0);
        if (n < FM_SOUND) continue;
        lib_get(BUF_DATA, b, FM_SOUND);
        fm_patch p;
        blob_patch(b, &p);
        synth_set_patch((tr_ch[k] - 10) & 3, &p);
    }
#endif
}

/* ---- the state: where the 68000 plays */
static void sq_poll(void)
{
    int f = (int)sq(SQ_STATE, 0, 0, 0);
    uint32_t pos = (uint32_t)md_p(C_P1) << 16 | md_p(C_P2);
    uint16_t ed = md_p(C_P3);
    if ((f & SQS_FULL) && !(sq_flags & SQS_FULL)) say("THE SONG MEMORY IS FULL: NOTES LEFT OUT");
    if (f != sq_flags) { sq_flags = f; tk_bar_dirty = 1; }
    if (pos / 24 != sq_pos / 24) tk_bar_dirty = 1;
    sq_pos = pos;
    if (ed != sq_edits) { sq_edits = ed; dirty = 1; }
    /* the view follows the playhead */
    if (screen == SCR_ARRANGE) {
        uint32_t span = (uint32_t)AR_COLS * SONG_PPQ;
        if (pos < ar_t0 || pos >= ar_t0 + span) { ar_t0 = pos / bar_len() * bar_len(); dirty = 1; }
        if (FAST_SCREEN && (sq_flags & SQS_PLAY) && (int)(pos / SONG_PPQ) != sq_shown_mark) dirty = 1;
    } else if (screen == SCR_ROLL && (sq_flags & SQS_PLAY)) {
        if (pos < pr_t0 || pos >= pr_t0 + PR_SPAN - 24) { pr_t0 = pos / bar_len() * bar_len(); dirty = 1; }
        if (FAST_SCREEN && (int)(pos / 24) != sq_shown_mark) dirty = 1;
    }
}

static void pos_text(char *o, uint32_t t)       /* "BAR 003.2" */
{
    uint32_t bar = t / bar_len(), beat = t % bar_len() / SONG_PPQ;
    o = put_str(o, "BAR ");
    o = put_n(o, (int)(bar + 1), 3);
    *o++ = '.';
    o = fmt_num(o, (int)beat + 1);
    *o = 0;
}

static void sq_draw_bar(void)           /* the transport line */
{
    char t[64], *o = t;
    int f = sq_flags;
    gfx_rect(0, 13, SCREEN_W, 10, COL_BG);
    const char *st = (f & SQS_COUNTIN) ? "1 2 3 4" : (f & SQS_REC) ? "REC" : (f & SQS_PLAY) ? "PLAY" : "STOP";
    gfx_text(4, 14, st, (f & SQS_REC) ? COL_RED : (f & SQS_PLAY) ? COL_GREEN : COL_DIM);
    pos_text(o, sq_pos);
    o += strlen(o);
    o = put_str(o, "  ");
    o = fmt_num(o, sq_bpm);
    o = put_str(o, " BPM  ");
    o = fmt_num(o, sq_beats);
    o = put_str(o, "/4");
    if (sq_sflags & SF_CYCLE) {
        o = put_str(o, "  CYCLE ");
        o = fmt_num(o, sq_cyc_a + 1);
        *o++ = '-';
        o = fmt_num(o, sq_cyc_b);
    }
    *o = 0;
    gfx_text(52, 14, t, COL_TEXT);
}

/* the ruler: bar numbers, the cycle, the playhead's mark */
static void sq_draw_ruler(int x0, int w, uint32_t t0, int px_per_bar)
{
    char t[6];
    gfx_rect(0, 23, SCREEN_W, 9, COL_PANEL);
    uint32_t bl = bar_len();
    for (int b = 0; ; b++) {
        int x = x0 + b * px_per_bar;
        if (x >= x0 + w) break;
        gfx_vspan(x, 23, 31, COL_LINE);
        *fmt_num(t, (int)(t0 / bl) + b + 1) = 0;
        if (x + 2 + (int)strlen(t) * CHAR_W <= x0 + w) gfx_text(x + 2, 23, t, COL_DIM);
    }
    if (sq_sflags & SF_CYCLE) {
        int a = x0 + (int)(((int32_t)(sq_cyc_a * bl) - (int32_t)t0) * px_per_bar / (int32_t)bl);
        int b = x0 + (int)(((int32_t)(sq_cyc_b * bl) - (int32_t)t0) * px_per_bar / (int32_t)bl);
        if (a < x0) a = x0;
        if (b > x0 + w) b = x0 + w;
        if (b > a) gfx_rect(a, 30, b - a, 2, COL_ORANGE);
    }
    if (sq_pos >= t0) {
        int x = x0 + (int)((sq_pos - t0) * (uint32_t)px_per_bar / bl);
        if (x < x0 + w) gfx_rect(x - 1, 23, 3, 9, (sq_flags & SQS_REC) ? COL_RED : COL_GREEN);
    }
}

/* ---- the arrange view */
static void ar_row(int k, const uint8_t *bits)
{
    char t[20], nm[17];
    int y = AR_Y + k * AR_H, sel = k == sq_sel;
    gfx_rect(0, y, AR_X - 2, AR_H, sel ? COL_CURSOR : COL_BG);
    gfx_rect(AR_X - 2, y, SCREEN_W - AR_X + 2, AR_H, sel ? COL_PANEL : COL_BG);
    *put_n(t, k + 1, 2) = 0;
    gfx_text(2, y + 1, t, sel ? COL_HI : COL_DIM);
    track_name(k, nm);
    gfx_text_n(16, y + 1, nm[0] ? nm : kind_names[tr_kind[k] % TK_COUNT], 12, sel ? COL_HI : kind_cols[tr_kind[k] % TK_COUNT]);
    if (tr_flags[k] & TF_MUTE) gfx_text(92, y + 1, "M", COL_RED);
    else if (tr_flags[k] & TF_SOLO) gfx_text(92, y + 1, "S", COL_HI);
    for (int c = 0; c < AR_COLS; c++) {
        int x = AR_X + c * AR_COLW;
        if ((uint32_t)c % (uint32_t)(sq_beats ? sq_beats : 4) == 0) gfx_vspan(x, y, y + AR_H - 1, COL_LINE);
        if (bits[k * 4 + c / 8] >> (c & 7) & 1)
            gfx_rect(x + 1, y + 2, AR_COLW - 1, AR_H - 4, (tr_flags[k] & TF_MUTE) ? COL_DIM : kind_cols[tr_kind[k] % TK_COUNT]);
    }
}

static void ar_info(void)               /* the selected track, under the list */
{
    char t[64], *o = t;
    int k = sq_sel;
    static const char *const qn[Q_COUNT] = { "OFF", "1/4", "1/8", "1/8T", "1/16", "1/16T", "1/32" };
    o = put_str(o, "TRACK ");
    o = fmt_num(o, k + 1);
    o = put_str(o, ": ");
    o = put_str(o, kind_names[tr_kind[k] % TK_COUNT]);
    o = put_str(o, "  MIDI CH ");
    o = fmt_num(o, tr_ch[k] + 1);
    o = put_str(o, "  QUANTIZE ");
    o = put_str(o, qn[tr_quant[k] % Q_COUNT]);
    *o = 0;
    gfx_rect(0, 196, SCREEN_W, 12, COL_BG);
    gfx_text(4, 198, t, COL_DIM);
}

static void draw_arrange(void)
{
    uint8_t bits[SONG_TRACKS * 4];
    char t[40] = "SONG: ";
    gfx_clear(COL_BG);
    strcat(t, sq_file[0] ? sq_file : "NEW");
    header(t);
    sq_draw_bar();
    sq_shown_mark = (int)(sq_pos / SONG_PPQ);
    sq_draw_ruler(AR_X, AR_COLS * AR_COLW, ar_t0, AR_COLW * (sq_beats ? sq_beats : 4));
    sq(SQ_OVERVIEW, 0, (int)(ar_t0 / 24), 4);
    lib_get(BUF_DATA, bits, sizeof bits);
    for (int k = 0; k < SONG_TRACKS; k++) ar_row(k, bits);
    if (FAST_SCREEN && sq_pos >= ar_t0 && sq_pos < ar_t0 + (uint32_t)AR_COLS * SONG_PPQ)
        gfx_vspan(AR_X + (int)((sq_pos - ar_t0) * AR_COLW / SONG_PPQ), AR_Y, AR_Y + SONG_TRACKS * AR_H - 1,
                  (sq_flags & SQS_REC) ? COL_RED : COL_GREEN);
    ar_info();
    footer((sq_flags & SQS_PLAY) ? "START STOP  UP/DN TRACK  L/R A BAR  C MENU"
                                 : "START PLAY  X RECORD  A PIANO ROLL  Y SOUND  C MENU");
    tk_bar_dirty = 0;
}

static void ar_redraw_rows(int a, int b)        /* (the selection moved) */
{
    uint8_t bits[SONG_TRACKS * 4];
    sq(SQ_OVERVIEW, 0, (int)(ar_t0 / 24), 4);
    lib_get(BUF_DATA, bits, sizeof bits);
    ar_row(a, bits);
    if (b != a) ar_row(b, bits);
    ar_info();
}

/* ---- the piano roll */
static int is_black(int n) { return (0x54A >> (n % 12)) & 1; }

static void pr_row_bg(int r)            /* row r (0: the lowest note shown) */
{
    int n = pr_low + r, y = PR_Y + (PR_N - 1 - r) * PR_H;
    gfx_rect(PR_X, y, PR_W, PR_H, is_black(n) ? COL_PANEL : COL_BG);
    if (n % 12 == 0) gfx_rect(PR_X, y + PR_H - 1, PR_W, 1, COL_LINE);
    gfx_rect(0, y, PR_X - 1, PR_H, is_black(n) ? COL_PANEL2 : COL_TEXT);
}

static void pr_keys_text(void)
{
    char t[4];
    for (int r = 0; r < PR_N; r++) {
        int n = pr_low + r;
        if (n % 12) continue;
        t[0] = 'C';
        t[1] = n < 12 ? '-' : (char)('0' + n / 12 - 1);
        t[2] = 0;
        int y = PR_Y + (PR_N - 1 - r) * PR_H - 5;
        if (y >= PR_Y) gfx_text(2, y, t, COL_BG);
    }
}

/* the notes in view: for each, its box (and whether the cursor is on it) */
typedef void (*note_fn)(int idx, uint32_t tick, int note, int vel, int dur);

static int pr_notes(uint32_t t0, uint32_t span, note_fn fn)
{
    sq(SQ_NOTES, sq_sel, (int)(t0 / 24), (int)((span + 23) / 24));
    uint8_t h[2];
    lib_get(BUF_DATA, h, 2);
    int n = h[0] << 8 | h[1];
    for (int i = 0; i < n; i++) {
        uint8_t r[NOTE_REC + 1];
        lib_get(BUF_DATA + 2 + i * NOTE_REC, r, NOTE_REC);
        fn(r[0] << 8 | r[1], (uint32_t)r[2] << 16 | (uint32_t)r[3] << 8 | r[4], r[5], r[6], r[7] << 8 | r[8]);
    }
    return n;
}

static int pr_sel_idx = -1, pr_sel_vel, pr_sel_dur;
static uint32_t pr_sel_tick;

static void note_box(int idx, uint32_t tick, int note, int vel, int dur)
{
    if (note < pr_low || note >= pr_low + PR_N) return;
    int32_t x0 = PR_X + ((int32_t)tick - (int32_t)pr_t0) / 6, x1 = PR_X + ((int32_t)(tick + (uint32_t)dur) - (int32_t)pr_t0) / 6;
    if (x1 <= x0) x1 = x0 + 1;
    if (x0 < PR_X) x0 = PR_X;
    if (x1 > SCREEN_W) x1 = SCREEN_W;
    if (x1 <= x0) return;
    int y = PR_Y + (PR_N - 1 - (note - pr_low)) * PR_H;
    uint8_t c = idx == pr_sel_idx ? COL_HI : (idx & 0x8000) || (vel & 0x80) ? COL_GREEN : vel >= 96 ? COL_ORANGE : COL_HEAD;
    gfx_rect((int)x0, y, (int)(x1 - x0), PR_H - 1, c);
    if (x1 - x0 > 2) gfx_rect((int)x0, y, 1, PR_H - 1, COL_BG);    /* (where it starts) */
}

static void find_sel(int idx, uint32_t tick, int note, int vel, int dur)
{
    if (note != pr_note || idx & 0x8000) return;
    if (pr_sel_idx < 0 || tick > pr_sel_tick) {         /* (the latest under the cursor) */
        pr_sel_idx = idx; pr_sel_tick = tick; pr_sel_vel = vel & 0x7F; pr_sel_dur = dur;
    }
}

static void pr_select(void)             /* the note under the cursor (-1: none) */
{
    pr_sel_idx = -1;
    pr_notes(pr_cur, 24, find_sel);
}

static void pr_cursor_box(void)
{
    if (pr_note < pr_low || pr_note >= pr_low + PR_N || pr_cur < pr_t0 || pr_cur >= pr_t0 + PR_SPAN) return;
    int x = PR_X + (int)((pr_cur - pr_t0) / 6), y = PR_Y + (PR_N - 1 - (pr_note - pr_low)) * PR_H;
    if (pr_sel_idx < 0) gfx_frame(x, y - 1, 4, PR_H + 1, COL_HI);
}


static int pr_only = -1;                /* (note_box: only this note's row, -1: all) */
static void note_box_f(int idx, uint32_t tick, int note, int vel, int dur)
{
    if (pr_only < 0 || note == pr_only) note_box(idx, tick, note, vel, dur);
}

static void pr_beats(int y0, int y1)
{
    for (uint32_t b = 0; ; b++) {
        int x = PR_X + (int)(b * SONG_PPQ / 6);
        if (x >= SCREEN_W) break;
        gfx_vspan(x, y0, y1, (b * SONG_PPQ + pr_t0) % bar_len() ? COL_PANEL2 : COL_LINE);
    }
}

static void pr_line(void)               /* under the roll: the cursor or the chosen note */
{
    char t[64], *o = t;
    static const char nm[] = "C-C#D-D#E-F-F#G-G#A-A#B-";
    *o++ = nm[pr_note % 12 * 2]; *o++ = nm[pr_note % 12 * 2 + 1]; o = fmt_num(o, pr_note / 12 - 1);
    *o++ = ' ';
    pos_text(o, pr_sel_idx >= 0 ? pr_sel_tick : pr_cur);
    o += strlen(o);
    if (pr_sel_idx >= 0) {
        o = put_str(o, "  VELOCITY ");
        o = fmt_num(o, pr_sel_vel);
        o = put_str(o, "  LENGTH ");
        o = fmt_num(o, pr_sel_dur / 6);
        o = put_str(o, "/16");
    }
    *o = 0;
    gfx_rect(0, PR_Y + PR_N * PR_H, SCREEN_W, 212 - (PR_Y + PR_N * PR_H), COL_BG);
    gfx_text(4, PR_Y + PR_N * PR_H + 2, t, COL_TEXT);
    footer(pr_sel_idx >= 0 ? "HOLD A: MOVE IT  HOLD B: LENGTH, VELOCITY  B: DELETE"
                           : "A: A NOTE HERE  X/Y: NEXT/LAST NOTE  C: BACK");
}

static void draw_roll(void)
{
    char t[48], nm[17], *o;
    gfx_clear(COL_BG);
    track_name(sq_sel, nm);
    o = put_str(t, "TRACK ");
    o = fmt_num(o, sq_sel + 1);
    o = put_str(o, ": ");
    strcpy(o, nm[0] ? nm : kind_names[tr_kind[sq_sel] % TK_COUNT]);
    header(t);
    sq_draw_bar();
    sq_shown_mark = (int)(sq_pos / 24);
    sq_draw_ruler(PR_X, PR_W, pr_t0, (int)(bar_len() / 6));
    for (int r = 0; r < PR_N; r++) pr_row_bg(r);
    pr_beats(PR_Y, PR_Y + PR_N * PR_H - 1);
    pr_keys_text();
    pr_select();
    pr_only = -1;
    pr_notes(pr_t0, PR_SPAN, note_box_f);
    pr_cursor_box();
    if (FAST_SCREEN && sq_pos >= pr_t0 && sq_pos < pr_t0 + PR_SPAN)
        gfx_vspan(PR_X + (int)((sq_pos - pr_t0) / 6), PR_Y, PR_Y + PR_N * PR_H - 1, (sq_flags & SQS_REC) ? COL_RED : COL_GREEN);
    pr_line();
    tk_bar_dirty = 0;
}

static int sq_present;                  /* (drawn in part: to the screen) */

static void pr_redraw_note_rows(int n1, int n2) /* the rows of two notes (the cursor moved) */
{
    int ns[2] = { n1, n2 };
    pr_select();
    for (int k = 0; k < 2; k++) {
        int n = ns[k];
        if (n < pr_low || n >= pr_low + PR_N || (k == 1 && n == n1)) continue;
        int r = n - pr_low, y = PR_Y + (PR_N - 1 - r) * PR_H;
        pr_row_bg(r);
        pr_beats(y, y + PR_H - 1);
        pr_only = n;
        pr_notes(pr_t0, PR_SPAN, note_box_f);
        pr_only = -1;
        if (n % 12 == 0) { char t[4] = { 'C', n < 12 ? '-' : (char)('0' + n / 12 - 1), 0, 0 }; if (y - 5 >= PR_Y) gfx_text(2, y - 5, t, COL_BG); }
        if (n % 12 == 11 && y + PR_H - 1 >= PR_Y) {     /* (the C below's name reaches up here) */
            int yc = y + PR_H;
            char t[4] = { 'C', n + 1 < 12 ? '-' : (char)('0' + (n + 1) / 12 - 1), 0, 0 };
            if (n + 1 < pr_low + PR_N) gfx_text(2, yc - 5, t, COL_BG);
        }
    }
    pr_cursor_box();
    pr_line();
    sq_present = 1;
}

/* ---- editing a note: BUF_DATA = track, tick (3), note, velocity, length (2) */
static int note_put(int op, int idx, uint32_t tick, int note, int vel, int dur)
{
    uint8_t b[8] = { (uint8_t)sq_sel, (uint8_t)(tick >> 16), (uint8_t)(tick >> 8), (uint8_t)tick,
                     (uint8_t)note, (uint8_t)vel, (uint8_t)(dur >> 8), (uint8_t)dur };
    put_buf(BUF_DATA, b, 8);
    unsigned e = sq(op, 0, idx, 0);
    if (e) { say("THE SONG MEMORY IS FULL"); return -1; }
    sq(SQ_STATE, 0, 0, 0);
    sq_edits = md_p(C_P3);              /* (drawn here: not all again for it) */
    return 0;
}

static int sq_heard = -1;
static void hear(int note, int vel)     /* a note on the track's sound while A is held */
{
    if (sq_heard >= 0) { sq(SQ_KEY, 0, 0x8000 | sq_heard, 0); sq_heard = -1; }
    if (note <= 0 || (sq_flags & SQS_PLAY)) return;
    sq(SQ_KEY, 0, 0x9000 | note, vel);
    sq_heard = note;
}

/* ---- the screens' moves */
static void roll_view(void)             /* the cursor in view (else the view jumps) */
{
    int again = 0;
    if (pr_cur < pr_t0 || pr_cur >= pr_t0 + PR_SPAN - 24) { pr_t0 = pr_cur / bar_len() * bar_len(); again = 1; }
    if (pr_note < pr_low) { pr_low = pr_note - pr_note % 12; again = 1; }
    if (pr_note >= pr_low + PR_N) { pr_low = pr_note - PR_N + 12 - pr_note % 12; if (pr_low > 127 - PR_N + 1) pr_low = 128 - PR_N; again = 1; }
    if (pr_low < 0) pr_low = 0;
    if (again) dirty = 1;
}

static uint32_t nb_best;
static int nb_note, nb_dir;
static void near_note(int idx, uint32_t tick, int note, int vel, int dur)
{
    (void)idx; (void)vel; (void)dur;
    uint32_t c = pr_cur + (nb_dir > 0 ? 23 : 0);
    if (nb_dir > 0 ? (tick > c && (nb_note < 0 || tick < nb_best)) : (tick < pr_cur && (nb_note < 0 || tick > nb_best))) {
        nb_best = tick;
        nb_note = note;
    }
}

static void roll_jump(int dir)          /* X / Y: the next or the last note */
{
    nb_note = -1;
    nb_dir = dir;
    uint32_t span = 16 * bar_len();
    if (dir > 0) pr_notes(pr_cur, span, near_note);
    else pr_notes(pr_cur > span ? pr_cur - span : 0, pr_cur > span ? span : pr_cur, near_note);
    if (nb_note < 0) { say(dir > 0 ? "NO MORE NOTES AFTER THIS" : "NO NOTES BEFORE THIS"); return; }
    int old = pr_note;
    pr_cur = nb_best / 24 * 24;
    pr_note = nb_note;
    roll_view();
    if (!dirty) pr_redraw_note_rows(old, pr_note);
}

static void transport(int rec)
{
    if (sq_flags & SQS_PLAY) sq(SQ_STOP, 0, 0, 0);
    else sq(SQ_PLAY, 0, 0, rec);
    sq_poll();
    tk_bar_dirty = 1;
    dirty = 1;
}

static void roll_input(uint16_t p)
{
    int a = (held_now & PAD_A) != 0, b = (held_now & PAD_B) != 0, c = (held_now & PAD_C) != 0;
    int a_edge = a && !sq_a_down, b_edge = b && !sq_b_down, b_up = !b && sq_b_down;
    int c_edge = c && !sq_c_down, c_up = !c && sq_c_down;
    sq_a_down = a; sq_b_down = b; sq_c_down = c;
    if (b_edge) sq_b_used = 0;
    if (c_edge) sq_c_used = 0;
    int dx = (p & PAD_RIGHT) ? 1 : (p & PAD_LEFT) ? -1 : 0, dy = (p & PAD_UP) ? 1 : (p & PAD_DOWN) ? -1 : 0;
    if (a) {                            /* A held: a note (a new one on an empty place), moved */
        if (a_edge) {
            pr_select();
            if (pr_sel_idx < 0) {
                if (note_put(SQ_ADD, 0, pr_cur, pr_note, pr_vel, pr_len)) return;
                pr_select();
                pr_redraw_note_rows(pr_note, pr_note);
            }
            hear(pr_note, pr_sel_vel ? pr_sel_vel : pr_vel);
        } else if ((dx || dy) && pr_sel_idx >= 0) {
            int32_t t = (int32_t)pr_sel_tick + dx * 24;
            int n = pr_note + dy;
            if (t < 0 || n < 0 || n > 127) return;
            if (note_put(SQ_SET, pr_sel_idx, (uint32_t)t, n, pr_sel_vel, pr_sel_dur)) return;
            int old = pr_note;
            pr_cur = (uint32_t)t / 24 * 24;
            pr_note = n;
            roll_view();
            if (!dirty) pr_redraw_note_rows(old, n);
            hear(n, pr_sel_vel);
        }
        return;
    }
    if (sq_heard >= 0) hear(0, 0);
    if (b) {                            /* B held: its length (left/right), velocity (up/down) */
        if ((dx || dy) && pr_sel_idx >= 0) {
            sq_b_used = 1;
            int dur = pr_sel_dur + dx * 24, vel = pr_sel_vel + dy * 8;
            if (dur < 6) dur = 6;
            if (vel < 1) vel = 1;
            if (vel > 127) vel = 127;
            if (note_put(SQ_SET, pr_sel_idx, pr_sel_tick, pr_note, vel, dur)) return;
            pr_len = dur;
            pr_vel = vel;
            pr_redraw_note_rows(pr_note, pr_note);
        }
        return;
    }
    if (b_up && !sq_b_used && pr_sel_idx >= 0) {        /* B alone: away with it */
        sq(SQ_DEL, 0, pr_sel_idx, 0);
        sq(SQ_STATE, 0, 0, 0);
        sq_edits = md_p(C_P3);
        pr_redraw_note_rows(pr_note, pr_note);
        return;
    }
    if (c) {                            /* C held: up/down an octave, left/right a bar */
        if (dx || dy) {
            sq_c_used = 1;
            int old = pr_note;
            if (dy) { pr_note += dy * 12; if (pr_note < 0) pr_note += 12; if (pr_note > 127) pr_note -= 12; }
            if (dx < 0) pr_cur = pr_cur >= bar_len() ? pr_cur - bar_len() : 0;
            if (dx > 0) pr_cur += bar_len();
            roll_view();
            if (!dirty) pr_redraw_note_rows(old, pr_note);
        }
        return;
    }
    if (c_up && !sq_c_used) { screen = SCR_ARRANGE; dirty = 1; return; }
    if (p & PAD_START) { transport(0); return; }
    if (p & PAD_X) { roll_jump(1); return; }
    if (p & PAD_Y) { roll_jump(-1); return; }
    if (dx || dy) {
        int old = pr_note;
        if (dx < 0) pr_cur = pr_cur >= 24 ? pr_cur - 24 : 0;
        if (dx > 0) pr_cur += 24;
        pr_note += dy;
        if (pr_note < 0) pr_note = 0;
        if (pr_note > 127) pr_note = 127;
        roll_view();
        if (!dirty) pr_redraw_note_rows(old, pr_note);
    }
}

/* ---- the songs in MIDI_SONGS, listed instead of the patches for a moment */
static int sl_pick, sl_top;
static int songs_list(void)
{
    lib_put_name("");
    md_cmd(CMD_CD, 0, 0, 0);
    lib_songs = 1;
    int n = lib_scan();
    lib_songs = 0;
    return n;
}

static void patches_again(void)         /* (the patch list as it was) */
{
    char p[PATH_MAX_];
    strcpy(p, lib_path);
    lib_cd(p);
    if (cursor >= lib_count) cursor = lib_count ? lib_count - 1 : 0;
    if (top > cursor) top = cursor;
}

static void sq_new_name(char *out, const char *ext)
{
    for (int n = 1; n < 1000; n++) {
        char *o = put_str(out, "song");
        o = put_n(o, n, 3);
        *o++ = '.';
        strcpy(o, ext);
        if (lib_find(out) < 0) return;
    }
}

static void sq_name_song(const char *file)      /* the song's own name: its file's */
{
    char nm[16];
    int k = 0;
    while (k < 15 && file[k] && file[k] != '.') { nm[k] = file[k]; k++; }
    while (k < 16) nm[k++] = 0;
    for (int i = 0; i < 16; i += 2) song_poke(SO_NAME + i, 2, (uint8_t)nm[i] << 8 | (uint8_t)nm[i + 1]);
}

static void sq_save(int as_new)
{
    char name[24];
    int n = songs_list();
    if (n < 0) { patches_again(); say(-n == ERR_NO_ED ? "NO EVERDRIVE: CANNOT SAVE" : "SD CARD ERROR"); return; }
    if (as_new || !sq_file[0]) sq_new_name(name, "sng");
    else strcpy(name, sq_file);
    patches_again();
    sq_name_song(name);
    lib_put_name(name);
    unsigned e = sq(SQ_SAVE, 0, 0, 0);
    if (e) { say_err("COULD NOT SAVE THE SONG:", (int)e); return; }
    strcpy(sq_file, name);
    char t[48] = "SAVED MIDI_SONGS/";
    strcat(t, name);
    say(t);
}

static void sq_export(void)
{
    char name[24];
    int n = songs_list();
    if (n < 0) { patches_again(); say("NO SD CARD: CANNOT EXPORT"); return; }
    if (sq_file[0]) {
        strcpy(name, sq_file);
        char *dot = name;
        while (*dot && *dot != '.') dot++;
        strcpy(dot, ".mid");
    } else {
        sq_new_name(name, "mid");
    }
    patches_again();
    lib_put_name(name);
    unsigned e = sq(SQ_EXPORT, 0, 0, 0);
    sq(SQ_LIVE, 0, 1, 0);               /* (the export let go of the keyboard) */
    if (e) { say_err("COULD NOT WRITE THE MIDI FILE:", (int)e); return; }
    char t[48] = "WROTE MIDI_SONGS/";
    strcat(t, name);
    say(t);
}

static void sq_open(int i)
{
    char name[NAME_MAX + 1];
    strncpy(name, lib_name(i), NAME_MAX);
    name[NAME_MAX] = 0;
    patches_again();
    lib_put_name(name);
    unsigned e = sq(SQ_LOAD, 0, 0, 0);
    sq_fetch();
    if (e) { say_err(e == ERR_RANGE ? "NOT A SONG OF THIS VERSION:" : "COULD NOT OPEN THE SONG:", (int)e); return; }
    strncpy(sq_file, name, sizeof sq_file - 1);
    sq_file[sizeof sq_file - 1] = 0;
    sq_init_done = 1;
    x32_sounds();
    ar_t0 = 0;
    say("SONG OPEN");
}

static void draw_songs(void)
{
    panel(40, 30, 240, 170);
    gfx_text(50, 37, "OPEN A SONG (MIDI_SONGS)", COL_HI);
    for (int i = 0; i < 14 && sl_top + i < lib_count; i++) {
        int k = sl_top + i, y = 52 + i * 10;
        if (k == sl_pick) gfx_rect(46, y - 2, 228, 10, COL_CURSOR);
        gfx_text_n(50, y, lib_name(k), 36, k == sl_pick ? COL_HI : COL_TEXT);
    }
    gfx_text(50, 192, "A: OPEN   B: BACK", COL_DIM);
}

static void songs_input(uint16_t p)
{
    if (p & PAD_UP) sl_pick--;
    if (p & PAD_DOWN) sl_pick++;
    if (sl_pick >= lib_count) sl_pick = lib_count - 1;
    if (sl_pick < 0) sl_pick = 0;
    if (sl_pick < sl_top) sl_top = sl_pick;
    if (sl_pick >= sl_top + 14) sl_top = sl_pick - 13;
    if (p & (PAD_UP | PAD_DOWN)) ov_dirty = 1;
    if (p & PAD_A) { overlay = OV_NONE; sq_open(sl_pick); dirty = 1; }
    else if (p & (PAD_B | PAD_START | PAD_C)) { overlay = OV_NONE; patches_again(); dirty = 1; }
}

/* ---- in and out */
static void tk_enter(void)
{
    if (settings[SET_MODE]) {           /* (each FM channel its own: the song's voices) */
        settings[SET_MODE] = (uint8_t)md_cmd(CMD_SETTING, SET_MODE, 0, 0);
        say("THE SONG PLAYS IN GENMDM MODE");
    }
    if (tk_from_lib && target == track_target(sq_sel)) {        /* (back from choosing: edits too) */
        int slot = target >= PWM_T && target < PWM_T + 4 ? target - 1 : target;
        if (IS_PSG(target) ? psg_edited[target - PSG_T] : chan_edited[slot]) sound_from(target, sq_sel, 0);
    }
    tk_from_lib = 0;
    if (!sq_init_done) { sq_init_done = 1; sounds_from_home(); }
    sq(SQ_LIVE, 0, 1, 0);
    screen = SCR_ARRANGE;
    sq_fetch();
    sq_poll();
    dirty = 1;
}

static void tk_leave(void)              /* home: the keyboard plays as before */
{
    hear(0, 0);
    sq(SQ_STOP, 0, 0, 0);
    sq(SQ_LIVE, 0, 0, 0);
    screen = SCR_LIBRARY;
    dirty = 1;
}

static void choose_sound(void)          /* the library, for the selected track */
{
    int t = track_target(sq_sel);
    if (t < 0) {
        say(tr_kind[sq_sel] == TK_DRUMS ? "DRUMS PLAY THE KIT IN MIDI_DRUMS"
            : tr_kind[sq_sel] == TK_MIDI ? "A MIDI TRACK: IT ONLY GOES OUT TO THE COMPUTER"
                                         : "NO 32X HERE: IT ONLY GOES OUT AS MIDI");
        return;
    }
    hear(0, 0);
    target = t;
    uint8_t b[SOUND_MAX];
    int n = (int)sq(SQ_SOUND, sq_sel, 0xFFFF, 0);  /* (the track's sound, there to see and change) */
    if (n > 0) {
        if (IS_PSG(t)) {
            md_cmd(CMD_PSG_SET, (uint16_t)(t - PSG_T), (uint16_t)n, 0);
            psg_from[t - PSG_T] = 0;
            psg_edited[t - PSG_T] = 0;
        } else if (n >= FM_SOUND) {
            lib_get(BUF_DATA, b, FM_SOUND);
            fm_patch p;
            blob_patch(b, &p);
            slot_apply(shown_ch(), &p);
            chan_edited[shown_ch()] = 0;
        }
    }
    slot_read(shown_ch(), &voice);
    if (IS_PSG(target)) lib_psg_get(target - PSG_T, &psg_shown);
    tk_from_lib = 1;
    screen = SCR_LIBRARY;
    dirty = 1;
    say("CHOOSE A SOUND. START: BACK TO THE SONG");
}

/* ---- the arrange view's buttons */
static void arrange_input(uint16_t p)
{
    int c = (held_now & PAD_C) != 0, c_edge = c && !sq_c_down, c_up = !c && sq_c_down;
    sq_c_down = c;
    if (c_edge) sq_c_used = 0;
    if (c_up && !sq_c_used) { overlay = OV_TKMENU; sq_confirm = -1; ov_dirty = 1; return; }
    if (p & PAD_START) { transport(0); return; }
    if (p & PAD_X) { transport(!(sq_flags & SQS_PLAY)); return; }
    if (p & (PAD_UP | PAD_DOWN)) {
        int old = sq_sel;
        sq_sel = (sq_sel + ((p & PAD_DOWN) ? 1 : SONG_TRACKS - 1)) % SONG_TRACKS;
        song_poke(SO_SEL, 1, sq_sel);
        sq(SQ_STATE, 0, 0, 0);
        sq_edits = md_p(C_P3);
        ar_redraw_rows(old, sq_sel);
        sq_present = 1;
    }
    if (p & (PAD_LEFT | PAD_RIGHT)) {
        uint32_t bl = bar_len(), t = sq_pos / bl * bl;
        if (p & PAD_LEFT) t = t >= bl ? (sq_pos % bl ? t : t - bl) : 0;
        else t += bl;
        sq(SQ_LOCATE, (int)(t >> 16), (int)(t & 0xFFFF), 0);
        sq_poll();
        tk_bar_dirty = 1;
    }
    if (p & PAD_A) {                    /* the piano roll */
        screen = SCR_ROLL;
        pr_cur = sq_pos / 24 * 24;
        pr_t0 = sq_pos / bar_len() * bar_len();
        roll_view();
        dirty = 1;
    }
    if (p & PAD_Y) choose_sound();
    if (p & PAD_B) {                    /* mute */
        tr_flags[sq_sel] ^= TF_MUTE;
        song_poke(SO_TRACKS + sq_sel * TR_BYTES + TR_FLAGS, 1, tr_flags[sq_sel]);
        dirty = 1;
    }
    if (p & PAD_Z) { sq(SQ_UNDO, 0, 0, 0); say("THE LAST TAKE: TAKEN OUT"); dirty = 1; }
}

/* ---- the menu (C) */
enum { SM_BPM, SM_BEATS, SM_CYCLE, SM_CYC_A, SM_CYC_B, SM_CLICK, SM_COUNTIN, SM_KIND, SM_CH, SM_SOLO,
       SM_QUANT, SM_CLEAR, SM_UNDO, SM_OUT, SM_NEW, SM_OPEN, SM_SAVE, SM_SAVENEW, SM_EXPORT, SM_EXIT, NSM };
static const char *const sm_names[NSM] = {
    "TEMPO", "BEATS A BAR", "CYCLE (LOOP)", "CYCLE FROM BAR", "CYCLE TO BAR", "CLICK", "COUNT-IN",
    "THIS TRACK PLAYS", "ITS MIDI CHANNEL", "SOLO IT", "QUANTIZE IT", "CLEAR ITS NOTES",
    "UNDO THE LAST TAKE", "MIDI OUT TO COMPUTER", "NEW SONG", "OPEN A SONG...", "SAVE",
    "SAVE AS A NEW FILE", "EXPORT A MIDI FILE (.MID)", "BACK TO THE PATCHES" };
static int sm_row;

static int sm_old = -1;                 /* (only rows sm_old and sm_row to draw again) */

static void draw_sm_row(int i)
{
    static const char *const qn[Q_COUNT] = { "OFF (AS PLAYED)", "1/4", "1/8", "1/8 TRIPLETS", "1/16", "1/16 TRIPLETS", "1/32" };
    char t[32];
    {
        int y = 29 + i * 9;
        gfx_rect(24, y - 1, 272, 9, i == sm_row ? COL_CURSOR : COL_PANEL2);
        gfx_text(28, y, sm_names[i], i == sm_row ? COL_HI : COL_TEXT);
        t[0] = 0;
        int k = sq_sel;
        switch (i) {
        case SM_BPM: *fmt_num(t, sq_bpm) = 0; break;
        case SM_BEATS: *fmt_num(t, sq_beats) = 0; break;
        case SM_CYCLE: strcpy(t, sq_sflags & SF_CYCLE ? "ON" : "OFF"); break;
        case SM_CYC_A: *fmt_num(t, sq_cyc_a + 1) = 0; break;
        case SM_CYC_B: *fmt_num(t, sq_cyc_b) = 0; break;
        case SM_CLICK: strcpy(t, sq_sflags & SF_CLICK_PLAY ? "ALWAYS" : sq_sflags & SF_CLICK_REC ? "RECORDING" : "OFF"); break;
        case SM_COUNTIN: strcpy(t, sq_sflags & SF_COUNTIN ? "A BAR" : "OFF"); break;
        case SM_KIND: strcpy(t, kind_names[tr_kind[k] % TK_COUNT]); break;
        case SM_CH: *fmt_num(t, tr_ch[k] + 1) = 0; break;
        case SM_SOLO: strcpy(t, tr_flags[k] & TF_SOLO ? "ON" : "OFF"); break;
        case SM_QUANT: strcpy(t, qn[tr_quant[k] % Q_COUNT]); break;
        case SM_OUT: strcpy(t, settings[SET_MIDIOUT] == 2 ? "SONG + KEYS" : settings[SET_MIDIOUT] ? "THE SONG" : "OFF"); break;
        case SM_SAVE: strncpy(t, sq_file[0] ? sq_file : "(NEW FILE)", 16); t[16] = 0; break;
        }
        if (i == sq_confirm) strcpy(t, "A AGAIN: SURE?");
        gfx_text(196, y, t, i == sm_row ? COL_HI : COL_HEAD);
    }
}

static void draw_tkmenu(void)
{
    if (sm_old >= 0) {                  /* (the cursor moved: two rows) */
        draw_sm_row(sm_old);
        draw_sm_row(sm_row);
        sm_old = -1;
        return;
    }
    panel(20, 14, 280, 196);
    gfx_text(28, 18, "SONG MENU", COL_HI);
    for (int i = 0; i < NSM; i++) draw_sm_row(i);
}

static void tr_poke(int f, int v)
{
    song_poke(SO_TRACKS + sq_sel * TR_BYTES + f, 1, v);
}

static void tkmenu_input(uint16_t p)
{
    int was = sm_row;
    if (p & PAD_UP) sm_row = (sm_row + NSM - 1) % NSM;
    if (p & PAD_DOWN) sm_row = (sm_row + 1) % NSM;
    if (p & (PAD_UP | PAD_DOWN)) {
        if (!ov_dirty) sm_old = sq_confirm >= 0 ? -1 : was;     /* (a whole draw already due: that) */
        sq_confirm = -1;
        ov_dirty = 1;
    }
    int d = (p & PAD_RIGHT) ? 1 : (p & PAD_LEFT) ? -1 : 0, a = (p & PAD_A) != 0;
    if (p & (PAD_B | PAD_C | PAD_START)) { overlay = OV_NONE; sq_confirm = -1; sq_c_used = 1; dirty = 1; return; }
    if (!d && !a) return;
    int k = sq_sel, dd = d ? d : 1;
    ov_dirty = 1;
    sm_old = -1;                        /* (the values: all of it) */
    if (!dirty && sm_row != SM_QUANT && sm_row < SM_CLEAR) sm_old = sm_row;    /* (one row's value) */
    switch (sm_row) {
    case SM_BPM: { int v = sq_bpm + dd; if (v < 20) v = 20; if (v > 300) v = 300; song_poke(SO_BPM, 2, v); break; }
    case SM_BEATS: { int v = sq_beats + dd; if (v < 1) v = 16; if (v > 16) v = 1; song_poke(SO_BEATS, 1, v); break; }
    case SM_CYCLE: song_poke(SO_FLAGS, 1, sq_sflags ^ SF_CYCLE); break;
    case SM_CYC_A: { int v = sq_cyc_a + dd; if (v < 0) v = 0; if (v >= sq_cyc_b) v = sq_cyc_b - 1; song_poke(SO_CYC_A, 2, v); break; }
    case SM_CYC_B: { int v = sq_cyc_b + dd; if (v <= sq_cyc_a) v = sq_cyc_a + 1; if (v > 999) v = 999; song_poke(SO_CYC_B, 2, v); break; }
    case SM_CLICK: {
        int c = sq_sflags & SF_CLICK_PLAY ? 2 : sq_sflags & SF_CLICK_REC ? 1 : 0;
        c = (c + dd + 3) % 3;
        song_poke(SO_FLAGS, 1, (sq_sflags & ~(SF_CLICK_REC | SF_CLICK_PLAY)) | (c == 2 ? SF_CLICK_REC | SF_CLICK_PLAY : c ? SF_CLICK_REC : 0));
        break;
    }
    case SM_COUNTIN: song_poke(SO_FLAGS, 1, sq_sflags ^ SF_COUNTIN); break;
    case SM_KIND: {
        int v = (tr_kind[k] + dd + TK_COUNT) % TK_COUNT;
        tr_poke(TR_KIND, v);
        if (v == TK_X32 && (tr_ch[k] < 10 || tr_ch[k] > 13)) tr_poke(TR_CH, 10);
        if (v == TK_DRUMS) tr_poke(TR_CH, 9);
        break;
    }
    case SM_CH: {
        int v = tr_ch[k] + dd;
        if (tr_kind[k] == TK_X32) v = 10 + ((v - 10 + 4) & 3);
        else v = (v + 16) % 16;
        tr_poke(TR_CH, v);
        break;
    }
    case SM_SOLO: tr_poke(TR_FLAGS, tr_flags[k] ^ TF_SOLO); break;
    case SM_QUANT: {
        int v = (tr_quant[k] + dd + Q_COUNT) % Q_COUNT;
        sq(SQ_QUANT, k, v, 0);
        break;
    }
    case SM_OUT: {
        int v = (settings[SET_MIDIOUT] + dd + 3) % 3;
        settings[SET_MIDIOUT] = (uint8_t)md_cmd(CMD_SETTING, SET_MIDIOUT, (uint16_t)v, 0);
        if (v && (hw_flags() & FL_LINK) != LINK_MIDI) say("MIDI OUT NEEDS THE NEW TEENSY FIRMWARE");
        break;
    }
    default:
        if (!a) break;
        if ((sm_row == SM_NEW || sm_row == SM_CLEAR) && sq_confirm != sm_row) { sq_confirm = sm_row; break; }
        sq_confirm = -1;
        overlay = OV_NONE;
        dirty = 1;
        switch (sm_row) {
        case SM_CLEAR: sq(SQ_CLEAR, k, 0, 0); say("THE TRACK'S NOTES: CLEARED"); break;
        case SM_UNDO: sq(SQ_UNDO, 0, 0, 0); say("THE LAST TAKE: TAKEN OUT"); break;
        case SM_NEW:
            sq(SQ_NEW, 0, 0, 0);
            sq_file[0] = 0;
            sounds_from_home();
            ar_t0 = 0;
            say("A NEW SONG: THE SOUNDS YOU HAD LOADED");
            break;
        case SM_OPEN: {
            int n = songs_list();
            if (n <= 0) {
                patches_again();
                say(n < 0 ? "NO SD CARD (EVERDRIVE)" : "NO SONGS IN MIDI_SONGS YET");
                break;
            }
            sl_pick = sl_top = 0;
            overlay = OV_SONGS;
            break;
        }
        case SM_SAVE: sq_save(0); break;
        case SM_SAVENEW: sq_save(1); break;
        case SM_EXPORT: sq_export(); break;
        case SM_EXIT: tk_leave(); break;
        }
    }
    sq_fetch();
}

static void draw_tk_help(void)
{
    static const char *const lines[] = {
        "THE SONG",
        "ARRANGE: UP/DOWN A TRACK (YOUR KEYBOARD PLAYS IT)",
        " LEFT/RIGHT A BAR   START PLAY/STOP",
        " X RECORD (A BAR OF CLICKS, THEN PLAY FREELY)",
        " A ITS PIANO ROLL   Y ITS SOUND   B MUTE",
        " Z UNDO THE LAST TAKE   C THE MENU",
        "",
        "PIANO ROLL: THE D-PAD MOVES THE CURSOR",
        " A ADD A NOTE (HOLD: MOVE IT, YOU HEAR IT)",
        " B DELETE (HOLD: LEFT/RIGHT LENGTH,",
        "   UP/DOWN VELOCITY)   X/Y NEXT/LAST NOTE",
        " HOLD C: AN OCTAVE / A BAR   C BACK",
        "",
        "QUANTIZE (MENU): NOTES TO A GRID; OFF PUTS",
        "THEM BACK WHERE YOU PLAYED THEM",
        "",
        "PRESS ANY BUTTON TO CLOSE",
    };
    int n = (int)(sizeof lines / sizeof lines[0]);
    panel(14, 18, 292, n * CHAR_H + 10);
    for (int i = 0; i < n; i++)
        gfx_text(22, 24 + i * CHAR_H, lines[i], i == 0 || i == n - 1 ? COL_HI : COL_TEXT);
}

/* ------------------------------------------------------------ the loop */

void ui_draw(void)
{
    part_dirty = 0;
    ov_dirty = 0;
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
    else if (screen == SCR_ARRANGE) draw_arrange();
    else if (screen == SCR_ROLL) draw_roll();
    else draw_editor();
    if (overlay == OV_HELP) { if (screen >= SCR_ARRANGE) draw_tk_help(); else draw_help(); }
    else if (overlay == OV_TKMENU) draw_tkmenu();
    else if (overlay == OV_SONGS) draw_songs();
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
    } else if (screen == SCR_ARRANGE) {
        arrange_input(p);
    } else if (screen == SCR_ROLL) {
        roll_input(p);
    } else {
        editor_input(p);
    }
    if (screen == SCR_ARRANGE || screen == SCR_ROLL) sq_poll();    /* the 68000 plays: where it is */
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
    if (screen == SCR_ARRANGE || screen == SCR_ROLL || tk_from_lib) last_act = act;    /* (the song's voices) */
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
    else if (ov_dirty && (overlay == OV_TKMENU || overlay == OV_SONGS)) {
        if (overlay == OV_TKMENU) draw_tkmenu(); else draw_songs();
        ov_dirty = 0;
        gfx_present();
    } else if ((screen == SCR_ARRANGE || screen == SCR_ROLL) && overlay == OV_NONE && (tk_bar_dirty || sq_present)) {
        if (tk_bar_dirty) {
            sq_draw_bar();
            if (screen == SCR_ARRANGE) sq_draw_ruler(AR_X, AR_COLS * AR_COLW, ar_t0, AR_COLW * (sq_beats ? sq_beats : 4));
            else sq_draw_ruler(PR_X, PR_W, pr_t0, (int)(bar_len() / 6));
            tk_bar_dirty = 0;
        }
        sq_present = 0;
        gfx_present();
    } else if (part_dirty && screen == SCR_LIBRARY && overlay == OV_NONE)
        draw_library_parts();
    else if (part_dirty)
        ui_draw();
}
