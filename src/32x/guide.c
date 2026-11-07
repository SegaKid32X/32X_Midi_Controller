/* guide.c - the guide (START on the home screen): a page at a time, each
 * with a drawing of the pad. Pages that teach a button light it up and wait
 * for you to press it; then they tick and move on. LEFT/RIGHT turn the
 * pages, B leaves. */
#include <string.h>
#include "hw.h"
#include "gfx.h"
#include "guide.h"

typedef struct {
    const char *title;
    const char *lines[9];
    uint16_t try_btn;           /* the button to try, or 0 */
    const char *try_text;
} page_t;

static const page_t pages[] = {
    { "WELCOME", {
#ifdef MD_ONLY
        "THIS CARTRIDGE MAKES THE MEGA DRIVE THE",
        "SCREEN FOR YOUR GENMDM: CHOOSE FM SOUNDS",
        "FROM THE SD CARD, SHAPE THEM, SAVE THEM.",
#else
        "THIS CARTRIDGE TURNS THE 32X INTO THE",
        "SCREEN FOR YOUR GENMDM: CHOOSE FM SOUNDS",
        "FROM THE SD CARD, SHAPE THEM, SAVE THEM.",
#endif
        "",
        "THIS GUIDE SHOWS EVERYTHING, A PAGE AT A",
        "TIME. LIT BUTTONS ON THE PAD ARE THE ONES",
        "TO TRY.",
        "",
        "RIGHT: NEXT PAGE   LEFT: BACK   B: LEAVE" },
      PAD_RIGHT, "PRESS RIGHT TO START" },
    { "SETTING UP", {
        "1  GENMDM INTERFACE IN CONTROLLER PORT 2",
        "2  THIS PAD IN PORT 1 (6 BUTTONS IS BEST)",
        "3  KEYBOARD OR COMPUTER INTO THE GENMDM'S",
        "   MIDI IN OR USB, AS ALWAYS",
        "4  PATCH FILES (.TFI .DMP .Y12) IN THE",
        "   SD CARD'S MIDI_PATCHES FOLDER",
        "",
        "THE TOP RIGHT CORNER SAYS GENMDM: STOCK",
        "ONCE THE FIRST NOTE ARRIVES." },
      0, 0 },
    { "CHANNELS", {
        "THE GENMDM PLAYS MIDI CHANNELS 1-6 ON",
        "THE SIX FM CHANNELS, AND 7-10 ON THE PSG.",
        "",
        "SET YOUR KEYBOARD TO MIDI CHANNEL 1 TO",
        "PLAY FM CHANNEL 1, AND SO ON.",
        "",
        "EACH FM CHANNEL HAS ITS OWN SOUND (A",
        "PATCH). YOU CHOOSE WHICH CHANNEL A PATCH",
        "GOES TO: THE TARGET." },
      0, 0 },
    { "CHOOSING A SOUND", {
        "ON THE HOME SCREEN, UP AND DOWN MOVE",
        "THROUGH THE PATCH FILES.",
        "",
        "STOP ON ONE AND IT LOADS INTO THE TARGET",
        "CHANNEL: PLAY A KEY AND YOU HEAR IT.",
        "",
        "LEFT AND RIGHT JUMP A PAGE OF FILES.",
        "FOLDERS (BLUE, AT THE TOP): A OPENS ONE,",
        "B GOES BACK UP. FOLDERS IN FOLDERS TOO." },
      PAD_DOWN, "TRY IT: PRESS DOWN" },
    { "THE TARGET CHANNEL", {
        "C CHANGES THE TARGET: CH 1 ... CH 6, THEN",
        "ALL (A PATCH GOES TO ALL SIX CHANNELS).",
        "",
        "THE PANEL ON THE RIGHT SHOWS THE TARGET'S",
        "SOUND: ITS ALGORITHM (HOW THE FOUR",
        "OPERATORS CONNECT) AND EACH OPERATOR'S",
        "ENVELOPE (ITS VOLUME OVER TIME).",
        "",
        "" },
      PAD_C, "TRY IT: PRESS C" },
    { "EDITING A SOUND", {
        "A OPENS THE EDITOR FOR THE TARGET.",
        "",
        "D-PAD   CHOOSE A SETTING",
        "A / B   VALUE UP / DOWN (HOLD: FASTER)",
        "X / Z   UP / DOWN BY 10",
        "C       NEXT CHANNEL",
        "Y       BACK HOME",
        "",
        "YOU HEAR EVERY CHANGE AT ONCE." },
      PAD_A, "TRY IT: PRESS A" },
    { "WHAT THE SETTINGS DO", {
        "ALG  HOW OPERATORS FEED EACH OTHER. BLUE",
        "     ONES (CARRIERS) MAKE THE SOUND",
        "FB   OPERATOR 1 FEEDS ITSELF: GRIT",
        "TL   AN OPERATOR'S LEVEL (0 = LOUDEST)",
        "MUL  ITS PITCH: 1 = THE NOTE, 2 = OCTAVE..",
        "DT   A SLIGHT DETUNE",
        "AR D1R SL D2R RR   THE ENVELOPE: ATTACK,",
        "     DECAY, SUSTAIN LEVEL, SUSTAIN, RELEASE",
        "AM AMS FMS LFO  WOBBLE   SSG  LOOPS" },
      0, 0 },
    { "SAVING", {
        "IN THE EDITOR, START SAVES TO THE SD",
        "CARD:",
        "",
        "  OVERWRITE THE FILE IT CAME FROM, OR",
        "  A NEW FILE: NEWPATCH001.TFI, .DMP, .Y12",
        "",
        "UP/DOWN CHOOSE, A SAVES, B CANCELS.",
        "",
        "DON'T PLAY WHILE SAVING OR LOADING." },
      PAD_START, "TRY IT: PRESS START" },
    { "DELETING", {
        "ON THE HOME SCREEN, Y DELETES THE PATCH",
        "FILE UNDER THE CURSOR FROM THE SD CARD.",
        "",
        "YOU ARE ASKED FIRST: A DELETES, B KEEPS",
        "IT. A DELETED FILE CANNOT COME BACK.",
        "",
        "",
        "",
        "" },
      PAD_Y, "TRY IT: PRESS Y (NOTHING IS DELETED)" },
    { "HEAR IT, SETTINGS", {
        "X ON THE HOME SCREEN PLAYS MIDDLE C ON THE",
        "TARGET CHANNEL WHILE YOU HOLD IT.",
        "",
        "START ON THE HOME SCREEN: THE MENU, WITH",
        "SETTINGS: GENMDM MODE (MIDI CH 1-6 PLAY",
        "FM 1-6) OR POLY (ONE MIDI CHANNEL PLAYS",
        "ALL SIX, LIKE GENAJAM), VELOCITY, STEREO",
        "SPREAD AND THE TUNING.",
        "" },
      PAD_X, "TRY IT: PRESS X" },
#ifndef MD_ONLY
    { "THE 32X'S OWN FM VOICES", {
        "MIDI CHANNELS 11-14 PLAY THROUGH THE",
        "32X'S OWN SOUND (PWM) WITH SOFTWARE FM:",
        "6 MORE VOICES, THE SAME PATCH FILES.",
        "C TO 32X 11 ... 32X 14 ON THE HOME",
        "SCREEN: LOAD, EDIT, SAVE AND X HEAR AS",
        "FOR THE MEGA DRIVE'S CHANNELS. EACH OF",
        "THOSE CHANNELS PLAYS CHORDS.",
        "NEEDS THE NEW TEENSY FIRMWARE.",
        "(NOT YET: SSG-EG AND THE LFO HERE.)" },
      0, 0 },
    { "DRUMS ON THE 32X", {
        "PUT .WAV DRUM SAMPLES IN A FOLDER CALLED",
        "MIDI_DRUMS ON THE SD CARD. THEY PLAY ON",
        "MIDI CHANNEL 16, THROUGH THE 32X'S OWN",
        "SOUND (PWM), WITH THE NEW FIRMWARE.",
        "NOTES: A NAME STARTING WITH A NUMBER",
        "(36 KICK.WAV) USES IT; ELSE KICK SNARE",
        "HAT TOM CRASH RIDE... PICK THE USUAL ONE.",
        "C TO THE DRUMS TARGET SHOWS THE KIT,",
        "X PLAYS EACH ONE. VOLUME: SETTINGS." },
      0, 0 },
    #endif
    { "THE NEW TEENSY FIRMWARE", {
        "WITH GENMDM'S OWN FIRMWARE THE TEENSY",
        "DECIDES HOW MIDI PLAYS. OURS SENDS THE",
        "MIDI HERE: POLY MODE, VELOCITY AND THE",
        "SETTINGS WORK, NOTHING IS EVER LOST.",
        "TOP RIGHT THEN SAYS GENMDM: MIDI.",
        "",
        "TEENSY LOADER (PJRC.COM): OPEN",
        "MIDI32X.HEX, PRESS THE TEENSY'S BUTTON.",
        "THE OLD .HEX PUTS IT BACK THE SAME WAY." },
      0, 0 },
    { "GOOD TO KNOW", {
        "- WHAT YOUR COMPUTER CHANGES THROUGH THE",
        "  GENMDM SHOWS ON SCREEN AS IT HAPPENS.",
        "- AN EDITED SOUND HAS A * BY ITS NAME",
        "  UNTIL YOU SAVE IT.",
        "- THE 16 LIGHTS UP TOP: MIDI COMING IN,",
        "  ONE PER CHANNEL. START, MIDI MONITOR:",
        "  THE NOTES AND VELOCITY ON EACH ONE.",
        "- MODE (3-BUTTON PAD: B): THE BUTTONS.",
        "B: BACK TO THE HOME SCREEN" },
      0, 0 },
};
#define NPAGES ((int)(sizeof pages / sizeof pages[0]))

static int page, done_timer;
static uint16_t lit_ok;                 /* the button just tried */

void guide_open(void)
{
    page = 0;
    done_timer = 0;
    lit_ok = 0;
}

/* ---- the pad drawing ---- */

typedef struct { uint16_t btn; int x, y, w, h; const char *label; } key_t;
static const key_t keys[] = {
    { PAD_UP,    34, 18, 10, 10, 0 }, { PAD_DOWN, 34, 38, 10, 10, 0 },
    { PAD_LEFT,  24, 28, 10, 10, 0 }, { PAD_RIGHT, 44, 28, 10, 10, 0 },
    { PAD_X,     98, 14, 12, 10, "X" }, { PAD_Y, 114, 12, 12, 10, "Y" }, { PAD_Z, 130, 10, 12, 10, "Z" },
    { PAD_A,     94, 30, 14, 12, "A" }, { PAD_B, 112, 28, 14, 12, "B" }, { PAD_C, 130, 26, 14, 12, "C" },
    { PAD_START, 64, 40, 20, 7, "S" }, { PAD_MODE, 120, 46, 22, 7, "M" },
};

static void draw_pad(int ox, int oy, uint16_t lit, uint16_t ok, uint16_t held)
{
    gfx_rect(ox + 6, oy + 4, 150, 54, COL_PANEL2);
    gfx_rect(ox + 2, oy + 12, 158, 38, COL_PANEL2);
    gfx_rect(ox + 34, oy + 28, 10, 10, COL_DIM);        /* the D-pad's middle */
    for (unsigned i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        const key_t *k = &keys[i];
        uint8_t c = (ok & k->btn) ? COL_GREEN : (lit & k->btn) ? COL_HI
                  : (held & k->btn) ? COL_TEXT : COL_DIM;
        gfx_rect(ox + k->x, oy + k->y, k->w, k->h, c);
        if (k->label)
            gfx_text(ox + k->x + (k->w - 5) / 2, oy + k->y + (k->h - 7) / 2, k->label,
                     c == COL_DIM ? COL_BG : COL_BG);
    }
    gfx_text(ox + 60, oy + 50, "START", COL_DIM);
    gfx_text(ox + 112, oy + 56, "MODE", COL_DIM);
}

void guide_draw(void)
{
    const page_t *p = &pages[page];
    gfx_clear(COL_BG);
    gfx_rect(0, 0, SCREEN_W, 12, COL_PANEL);
    gfx_text(4, 3, "GUIDE", COL_HI);
    gfx_text(40, 3, p->title, COL_TEXT);
    char t[16], *o = t;
    o = fmt_num(o, page + 1);
    *o++ = '/';
    o = fmt_num(o, NPAGES);
    *o = 0;
    gfx_text(SCREEN_W - 4 - (int)strlen(t) * CHAR_W, 3, t, COL_DIM);
    for (int i = 0; i < 9; i++)
        if (p->lines[i])
            gfx_text(14, 20 + i * 10, p->lines[i], COL_TEXT);
    int ok = done_timer > 0;
    draw_pad(80, 112, ok ? 0 : p->try_btn, ok ? lit_ok : 0, 0);
    if (p->try_btn) {
        gfx_rect(0, 180, SCREEN_W, 14, ok ? COL_GREEN : COL_PANEL);
        gfx_text(14, 184, ok ? "WELL DONE!" : p->try_text, ok ? COL_BG : COL_HI);
    }
    /* the page dots */
    for (int i = 0; i < NPAGES; i++)
        gfx_rect(SCREEN_W / 2 - NPAGES * 5 + i * 10, 200, 6, 6, i == page ? COL_HI : COL_PANEL2);
    gfx_rect(0, SCREEN_H - 12, SCREEN_W, 12, COL_PANEL);
    gfx_text(4, SCREEN_H - 9, "LEFT/RIGHT PAGES   B LEAVE", COL_DIM);
}

/* 1 while the guide stays open; *redraw set when the screen changed */
int guide_input(uint16_t pressed, int *redraw)
{
    const page_t *p = &pages[page];
    if (done_timer > 0) {
        if (--done_timer == 0) {
            if (page < NPAGES - 1) page++;
            *redraw = 1;
        }
        return 1;
    }
    if (p->try_btn && (pressed & p->try_btn)) {
        lit_ok = p->try_btn;
        done_timer = 40;
        *redraw = 1;
        return 1;
    }
    if (pressed & PAD_B) return 0;
    if ((pressed & PAD_RIGHT) && page < NPAGES - 1) { page++; *redraw = 1; }
    if ((pressed & PAD_LEFT) && page > 0) { page--; *redraw = 1; }
    if ((pressed & PAD_RIGHT) && page == NPAGES - 1 && !p->try_btn) return 0;
    return 1;
}
