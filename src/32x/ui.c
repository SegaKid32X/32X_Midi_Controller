/* ui.c - the screens: the patch library (home) and the FM editor.
 *
 * LIBRARY  UP/DOWN choose a patch (it loads into the target channel a moment
 *          after you stop: you hear it at once), LEFT/RIGHT a page,
 *          C the target channel (1-6 or ALL), A the editor, Y delete,
 *          START the guide, MODE (or B on a 3-button pad) the controls.
 * EDITOR   the D-pad moves, A/B change the value (held: faster), X/Z by 10,
 *          C the channel, START save, Y back, MODE the controls.
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

enum { SCR_LIBRARY, SCR_EDITOR };
enum { OV_NONE, OV_HELP, OV_DELETE, OV_SAVE, OV_GUIDE, OV_MENU, OV_SETTINGS };

#define ALL_CH      6
#define LIST_ROWS   19
#define LIST_Y      22

static int screen = SCR_LIBRARY, overlay = OV_NONE;
static int cursor, top, target;         /* target 0-5, or ALL_CH */
static int load_timer = -1;
static char chan_path[6][PATH_MAX_ + NAME_MAX + 2];   /* the file each channel came from ("": none) */
static int chan_edited[6];
static fm_patch voice;                  /* the voice shown (the editor's channel) */
static int ed_row, ed_col, save_choice, menu_choice, set_row;
static uint8_t settings[SET_COUNT];
static int preview_note = -1;           /* the note X is playing, or -1 */
static void menu_input(uint16_t p);
static void settings_input(uint16_t p);
static char message[60];
static int message_timer;
static int dirty = 1;
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

static int shown_ch(void) { return target == ALL_CH ? 0 : target; }

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
        if (--repeat_timer <= 0) {
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

static const char *const ch_names[7] = { "CH 1", "CH 2", "CH 3", "CH 4", "CH 5", "CH 6", "ALL" };

static void header(const char *title)
{
    gfx_rect(0, 0, SCREEN_W, 12, COL_PANEL);
    gfx_text(4, 3, title, COL_HI);
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

static void set_origin(int c, const char *rel)
{
    strncpy(chan_path[c], rel, sizeof chan_path[c] - 1);
    chan_path[c][sizeof chan_path[c] - 1] = 0;
    chan_edited[c] = 0;
}

static void apply_to_target(const fm_patch *p, const char *rel)
{
    if (target == ALL_CH) {
        for (int c = 0; c < 6; c++) {
            voice_apply(c, p);
            set_origin(c, rel);
        }
    } else {
        voice_apply(target, p);
        set_origin(target, rel);
    }
}

static void load_cursor(void)
{
    if (lib_is_dir(cursor)) return;
    fm_patch p;
    int e = lib_load(cursor, &p);
    if (e) {
        say_err("CANNOT READ THAT FILE:", e);
        return;
    }
    char rel[PATH_MAX_ + NAME_MAX + 2];
    lib_rel(cursor, rel);
    apply_to_target(&p, rel);
    voice = p;
}

static void draw_library(void)
{
    gfx_clear(COL_BG);
    header("32X MIDI CONTROLLER");
    {                                           /* where we are */
        char where[PATH_MAX_ + 16] = "MIDI_PATCHES";
        if (lib_path[0]) { strcat(where, "/"); strcat(where, lib_path); }
        int n = (int)strlen(where), max = 31;
        gfx_text(6, 13, n > max ? where + n - max : where, COL_HEAD);
    }
    if (!lib_count && lib_path[0]) {
        gfx_text(16, 60, "THIS FOLDER HAS NO PATCH FILES", COL_HI);
        gfx_text(16, 76, "B: BACK UP A FOLDER", COL_TEXT);
    } else if (!lib_count) {
        gfx_text(16, 60, "NO PATCH FILES FOUND", COL_HI);
        gfx_text(16, 76, "PUT .TFI .DMP OR .Y12 FILES IN THE FOLDER", COL_TEXT);
        gfx_text(16, 88, "MIDI_PATCHES ON THE SD CARD.", COL_TEXT);
        gfx_text(16, 104, "A: LOOK AGAIN    START: THE GUIDE", COL_TEXT);
    }
    for (int r = 0; r < LIST_ROWS && top + r < lib_count; r++) {
        int i = top + r, y = LIST_Y + r * CHAR_H;
        if (i == cursor)
            gfx_rect(2, y - 1, 186, CHAR_H, COL_CURSOR);
        if (lib_is_dir(i)) {                    /* a folder: a tab and a name in blue */
            gfx_rect(6, y + 1, 4, 2, COL_HEAD);
            gfx_rect(6, y + 2, 9, 5, COL_HEAD);
            int n = gfx_text_n(18, y, lib_name(i), 27, i == cursor ? COL_HI : COL_HEAD);
            gfx_text(18 + n * CHAR_W, y, "/", COL_HEAD);
        } else {
            gfx_text_n(6, y, lib_name(i), 30, i == cursor ? COL_HI : COL_TEXT);
        }
    }
    if (lib_count > LIST_ROWS) {                /* the scroll bar */
        int h = LIST_ROWS * CHAR_H;
        int bar = h * LIST_ROWS / lib_count;
        if (bar < 6) bar = 6;
        int pos = (h - bar) * top / (lib_count - LIST_ROWS);
        gfx_rect(190, LIST_Y - 1, 3, h, COL_PANEL);
        gfx_rect(190, LIST_Y - 1 + pos, 3, bar, COL_DIM);
    }
    /* the target and its voice */
    int px = 200;
    gfx_rect(px - 4, 16, SCREEN_W - px + 4, SCREEN_H - 32, COL_PANEL);
    gfx_text(px, 20, "TARGET", COL_DIM);
    gfx_text(px + 42, 20, ch_names[target], COL_HI);
    gfx_text(px + 80, 20, "C", COL_DIM);
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
    if (lib_count && lib_is_dir(cursor))
        footer(lib_path[0] ? "A OPEN THE FOLDER  B UP  START MENU" : "A OPEN THE FOLDER  START MENU");
    else if (lib_count)
        footer(lib_path[0] ? "UP/DN PATCH  C CHAN  A EDIT  B UP  X HEAR"
                           : "UP/DN PATCH  C CHAN  A EDIT  X HEAR  START MENU");
    else
        footer(lib_path[0] ? "B: BACK UP A FOLDER" : "A: LOOK AGAIN   START: MENU");
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
    if (cursor != old) {
        load_timer = lib_is_dir(cursor) ? -1 : 10;  /* load once the cursor rests */
        dirty = 1;
    }
    if (cursor < top) top = cursor;
    if (cursor >= top + LIST_ROWS) top = cursor - LIST_ROWS + 1;
    if (p & PAD_C) {
        target = (target + 1) % 7;
        voice_read(shown_ch(), &voice);
        dirty = 1;
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
        load_timer = -1;
        dirty = 1;
        return;
    }
    if ((p & PAD_A) && !lib_count) {
        if (!lib_path[0]) ui_rescan();
        return;
    }
    if ((p & PAD_A) && lib_is_dir(cursor)) {
        if (lib_enter(cursor) < 0) say("CANNOT OPEN THAT FOLDER");
        cursor = top = 0;
        load_timer = -1;
        dirty = 1;
        return;
    }
    if (p & PAD_A) {
        screen = SCR_EDITOR;
        ed_row = ed_col = 0;
        voice_read(shown_ch(), &voice);
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
        if (sel) gfx_rect(x - 2, y - 2, 32, 21, COL_CURSOR);
        gfx_text(x, y, gname[i], COL_DIM);
        o = t;
        if (i == P_PAN) o = stpcpy(o, pans[gv[i] & 3]);
        else if (i == P_LFO && !gv[i]) o = stpcpy(o, "OFF");
        else o = fmt_num(o, i == P_LFO ? gv[i] - 1 : gv[i]);
        *o = 0;
        gfx_text(x, y + 10, t, sel ? COL_HI : COL_TEXT);
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
            if (sel) gfx_rect(tx + i * cw - 2, y - 1, cw - 4, CHAR_H, COL_CURSOR);
            cell_text(t, r, op_get(&voice.op[i], r));
            gfx_text(tx + i * cw, y, t, sel ? COL_HI : COL_TEXT);
        }
    }
    for (int i = 0; i < 4; i++) {
        int ex = 186 + (i & 1) * 66, ey = 84 + (i >> 1) * 52;
        draw_envelope(ex, ey, 62, 44, &voice.op[i],
                      (algs[voice.alg].carriers >> i & 1) ? COL_ORANGE : COL_GREEN);
        gfx_num(ex + 54, ey + 2, i + 1, COL_DIM);
    }
    gfx_text(186, 190, "ORANGE: CARRIER  GREEN: MOD", COL_DIM);
    footer("A/B VALUE  X/Z 10  C CHAN  START SAVE  Y BACK");
}

static void voice_changed(void)
{
    if (target == ALL_CH) {
        for (int c = 0; c < 6; c++) { voice_apply(c, &voice); chan_edited[c] = 1; }
    } else {
        voice_apply(target, &voice);
        chan_edited[target] = 1;
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
    if (p & PAD_UP) ed_row = ed_row > 0 ? ed_row - 1 : NOP_ROWS;
    if (p & PAD_DOWN) ed_row = ed_row < NOP_ROWS ? ed_row + 1 : 0;
    int ncols = ed_row == 0 ? NGLOBAL : 4;
    if (ed_col >= ncols) ed_col = ncols - 1;
    if (p & PAD_LEFT) ed_col = ed_col > 0 ? ed_col - 1 : ncols - 1;
    if (p & PAD_RIGHT) ed_col = ed_col < ncols - 1 ? ed_col + 1 : 0;
    if (p & (PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT)) dirty = 1;
    if (p & PAD_A) editor_change(+1);
    if (p & PAD_B) editor_change(-1);
    if (p & PAD_X) editor_change(+10);
    if (p & PAD_Z) editor_change(-10);
    if (p & PAD_C) {
        target = (target + 1) % 7;
        voice_read(shown_ch(), &voice);
        dirty = 1;
    }
    if (p & PAD_START) {
        overlay = OV_SAVE;
        save_choice = chan_path[shown_ch()][0] ? 0 : 1;
        dirty = 1;
    }
    if (p & PAD_Y) {
        screen = SCR_LIBRARY;
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
        "C           TARGET CHANNEL 1-6 / ALL",
        "A           EDIT THE TARGET'S VOICE",
        "Y           DELETE THE PATCH FILE",
        "X           HEAR IT (MIDDLE C, HOLD)",
        "START       MENU: GUIDE, SETTINGS",
        "",
        "EDITOR",
        "D-PAD       CHOOSE A SETTING",
        "A / B       VALUE UP / DOWN",
        "X / Z       UP / DOWN BY 10",
        "C           NEXT CHANNEL",
        "START       SAVE TO THE SD CARD",
        "Y           BACK TO THE LIBRARY",
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
    gfx_text(30 + 18 * CHAR_W, 78, ch_names[ch], COL_HI);
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
    case OV_DELETE:
        if (p & PAD_A) {
            char name[PATH_MAX_ + NAME_MAX + 2];
            lib_rel(cursor, name);
            int e = lib_delete(name);
            overlay = OV_NONE;
            if (e) { say_err("COULD NOT DELETE:", e); break; }
            for (int c = 0; c < 6; c++)
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

static const char *const menu_items[3] = { "THE GUIDE: HOW IT ALL WORKS", "SETTINGS", "LOOK FOR PATCHES AGAIN" };

static void draw_menu(void)
{
    panel(60, 70, 200, 62);
    gfx_text(70, 77, "MENU", COL_HI);
    for (int i = 0; i < 3; i++) {
        int y = 91 + i * 11;
        if (i == menu_choice) gfx_rect(66, y - 2, 188, 11, COL_CURSOR);
        gfx_text(70, y, menu_items[i], i == menu_choice ? COL_HI : COL_TEXT);
    }
    gfx_text(70, 124, "A: CHOOSE   B: BACK", COL_DIM);
}

static const char *const set_names[SET_COUNT] = { "MODE", "POLY MIDI CHANNEL", "TUNING", "VELOCITY", "STEREO SPREAD" };
static const uint8_t set_max[SET_COUNT] = { 1, 15, 1, 1, 1 };

static void settings_fetch(void)
{
    for (int i = 0; i < SET_COUNT; i++)
        settings[i] = (uint8_t)md_cmd(CMD_SETTING, (uint16_t)i, 0xFFFF, 0);
}

static void draw_settings(void)
{
    char t[24];
    panel(16, 30, 288, 160);
    gfx_text(26, 37, "SETTINGS", COL_HI);
    for (int i = 0; i < SET_COUNT; i++) {
        int y = 54 + i * 13;
        int v = settings[i];
        if (i == set_row) gfx_rect(22, y - 3, 276, 13, COL_CURSOR);
        gfx_text(28, y, set_names[i], COL_TEXT);
        switch (i) {
        case SET_MODE: strcpy(t, v ? "POLY (6 VOICES)" : "GENMDM (CH 1-6)"); break;
        case SET_POLY_CH: *fmt_num(t, v + 1) = 0; break;
        case SET_REGION: strcpy(t, v ? "PAL" : "NTSC"); break;
        default: strcpy(t, v ? "ON" : "OFF"); break;
        }
        gfx_text(190, y, t, i == set_row ? COL_HI : COL_TEXT);
    }
    static const char *const help[SET_COUNT] = {
        "GENMDM: MIDI CH 1-6 PLAY FM 1-6.  POLY: ONE",
        "THE CHANNEL YOUR KEYBOARD SENDS ON (POLY)",
        "MATCH YOUR CONSOLE SO NOTES ARE IN TUNE",
        "HARDER KEYS PLAY LOUDER",
        "EACH POLY NOTE LEFT, RIGHT OR CENTRE",
    };
    gfx_text(28, 128, help[set_row], COL_DIM);
    if (set_row == SET_MODE)
        gfx_text(28, 139, "CHANNEL PLAYS ALL SIX FM VOICES", COL_DIM);
    if ((hw_flags() & FL_LINK) != LINK_MIDI) {
        gfx_text(28, 156, "THESE NEED THE NEW TEENSY FIRMWARE:", COL_ORANGE);
        gfx_text(28, 166, "THE STOCK ONE DECIDES ALL THIS ITSELF", COL_ORANGE);
    }
    gfx_text(28, 178, "UP/DOWN CHOOSE  LEFT/RIGHT CHANGE  B BACK", COL_DIM);
}

static void menu_input(uint16_t p)
{
    if (p & PAD_UP) menu_choice = (menu_choice + 2) % 3;
    if (p & PAD_DOWN) menu_choice = (menu_choice + 1) % 3;
    if (p & (PAD_UP | PAD_DOWN)) dirty = 1;
    if (p & (PAD_A | PAD_START)) {
        overlay = OV_NONE;
        if (menu_choice == 0) { guide_open(); overlay = OV_GUIDE; }
        else if (menu_choice == 1) { settings_fetch(); set_row = 0; overlay = OV_SETTINGS; }
        else ui_rescan();
        dirty = 1;
    } else if (p & PAD_B) {
        overlay = OV_NONE;
        dirty = 1;
    }
}

static void settings_input(uint16_t p)
{
    if (p & PAD_UP) set_row = (set_row + SET_COUNT - 1) % SET_COUNT;
    if (p & PAD_DOWN) set_row = (set_row + 1) % SET_COUNT;
    int d = (p & (PAD_RIGHT | PAD_A)) ? 1 : (p & PAD_LEFT) ? -1 : 0;
    if (d) {
        int v = settings[set_row] + d;
        if (v < 0) v = set_max[set_row];
        if (v > set_max[set_row]) v = 0;
        settings[set_row] = (uint8_t)md_cmd(CMD_SETTING, (uint16_t)set_row, (uint16_t)v, 0);
        if (set_row == SET_MODE && v == 1) target = ALL_CH;     /* POLY: patches go to all six */
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
    if (screen != SCR_LIBRARY || overlay != OV_NONE) return;
    int down = (held_now & PAD_X) != 0;
    if (down && preview_note < 0) {
        int mch = settings[SET_MODE] ? settings[SET_POLY_CH] : (target == ALL_CH ? 0 : target);
        preview_note = 60 | mch << 8;
        md_cmd(CMD_MIDI, (uint16_t)(3 << 8 | (0x90 | mch)), (uint16_t)(60 << 8 | 110), 0);
    } else if (!down && preview_note >= 0) {
        md_cmd(CMD_MIDI, (uint16_t)(3 << 8 | (0x80 | preview_note >> 8)), (uint16_t)(60 << 8), 0);
        preview_note = -1;
    }
}

/* ------------------------------------------------------------ the loop */

void ui_draw(void)
{
    if (overlay == OV_GUIDE) {
        guide_draw();
        gfx_present();
        dirty = 0;
        return;
    }
    if (screen == SCR_LIBRARY) draw_library();
    else draw_editor();
    if (overlay == OV_HELP) draw_help();
    else if (overlay == OV_DELETE) draw_delete();
    else if (overlay == OV_SAVE) draw_save();
    else if (overlay == OV_MENU) draw_menu();
    else if (overlay == OV_SETTINGS) draw_settings();
    gfx_present();
    dirty = 0;
}

void ui_rescan(void)
{
    gfx_clear(COL_BG);
    header("32X MIDI CONTROLLER");
    gfx_text(16, 60, "LOOKING FOR PATCHES IN MIDI_PATCHES...", COL_TEXT);
    gfx_present();
    int n = lib_scan();
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
    } else {
        editor_input(p);
    }
    preview();
    if (load_timer > 0 && --load_timer == 0) {
        load_timer = -1;
        if (lib_count) load_cursor();
        dirty = 1;
    }
    /* the GenMDM changed something: show it (at most every few frames) */
    uint16_t act = hw_activity();
    if (act != last_act && ++act_timer >= 6) {
        act_timer = 0;
        last_act = act;
        fm_patch now;
        uint8_t lfo = shadow_byte(0x22);
        voice_read(shown_ch(), &now);
        if (memcmp(&now, &voice, sizeof now) || lfo != shadow_byte(0x22)) {
            voice = now;
            dirty = 1;
        }
    }
    if (message_timer > 0 && --message_timer == 0)
        dirty = 1;
    static uint16_t last_flags;
    if (hw_flags() != last_flags) {
        last_flags = hw_flags();
        dirty = 1;
    }
    if (dirty)
        ui_draw();
}
