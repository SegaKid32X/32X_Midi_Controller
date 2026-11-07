/* md_midi.c - the MIDI engine: what the GenMDM's Teensy does with the stock
 * firmware, done here instead (our firmware passes the MIDI through), plus
 * a polyphonic mode.
 *
 * GENMDM mode (the default, as GenMDM): MIDI channels 1-6 play FM channels
 *   1-6, 7-9 the PSG's tone channels, 10 its noise. Every GenMDM CC works
 *   (the map: github.com/2xAA/genmdm-editor, src/renderer/src/genmdm-mapping.js,
 *   MIT; the values are scaled the same way), so editors and DAW projects
 *   made for GenMDM behave the same. With channel 3 in special mode, MIDI
 *   channels 11, 12, 13 and 3 give operators 1-4 their own pitches.
 * POLY mode (as GENajam): one MIDI channel plays all six FM channels, a
 *   voice each; the oldest note gives way. Sustain pedal, stereo spread
 *   (each note to the left, the right or both), the mod wheel turns the LFO
 *   on (from 64 up). Its CCs reach all six channels.
 *
 * Velocity lowers the carriers' levels (a piano-like curve) without
 * touching the patch: the shadow (and so the screen and saved files) keeps
 * the voice's own levels.
 */
#include "md.h"
#include "midi_tables.h"

midi_settings_t ms = { MODE_GENMDM, 0, 0, 1, 0, 2 };

/* ------------------------------------------------------------ chip helpers */

static const uint8_t ch_code[6] = { 0, 1, 2, 4, 5, 6 };    /* key on/off */
static const uint8_t op_reg[4] = { 0x0, 0x8, 0x4, 0xC };   /* OP1..OP4 */
static const uint8_t carriers[8] = { 0x8, 0x8, 0x8, 0x8, 0xA, 0xE, 0xE, 0xF };  /* bit n = OP n+1 */

static uint8_t reg_get(int ch, uint8_t reg)
{
    return shadow[(ch / 3) * SH_PART2 + reg + ch % 3];
}

static void reg_set(int ch, uint8_t reg, uint8_t val)       /* chip and shadow */
{
    ym_write(ch / 3, (uint8_t)(reg + ch % 3), val);
}

static void reg_chip(int ch, uint8_t reg, uint8_t val)      /* chip only */
{
    ym_chip(ch / 3, (uint8_t)(reg + ch % 3), val);
}

static void changed(void)
{
    act_bump();
}

/* ------------------------------------------------------------ voices */

typedef struct {
    uint8_t note, vel, on, held;        /* held: off, but the pedal is down */
    uint16_t age;
} voice_t;

static voice_t fm[6];
static uint16_t clock_age;
static int16_t bend[16];                /* -8192..8191 */
static uint8_t sustain[16];
static uint8_t bend_range = 2;
static int8_t transpose;
static uint8_t tet = 12;
static uint8_t special3;                /* channel 3 special mode */
static uint8_t sp_keys;                 /* its operators keyed on (0x28's slot bits) */
static const uint8_t sp_slot[4] = { 0x10, 0x40, 0x20, 0x80 };  /* OP1..OP4 */
static uint8_t psg_note[4], psg_vel[4];
static uint16_t rnd = 0xACE1;

/* pitch in 1/16 semitones above middle C */
static int pitch_of(int note, int midi_ch)
{
    int u = ((note - 69 + transpose) * 192) / tet + 9 * 16;
    u += (bend[midi_ch] * bend_range) / 512;       /* 8192 / 16 */
    return u;
}

static void fm_pitch(int ch, int note, int midi_ch, uint8_t reg_hi, uint8_t reg_lo)
{
    int u = pitch_of(note, midi_ch) + 192 * 8;      /* keep it positive */
    int block = u / 192 - 8 + 4, idx = u % 192;
    unsigned fnum = ym_fnum[ms.region][idx];
    if (block < 0) { fnum >>= -block; block = 0; }
    if (block > 7) { block = 7; fnum = 2047; }
    reg_set(ch, reg_hi, (uint8_t)(block << 3 | fnum >> 8));
    reg_set(ch, reg_lo, (uint8_t)fnum);
}

static void fm_levels(int ch, int vel)
{
    uint8_t alg = reg_get(ch, 0xB0) & 7;
    int att = ms.velocity ? vel_att[vel] : 0;
    for (int i = 0; i < 4; i++) {
        if (!(carriers[alg] >> i & 1)) continue;
        uint8_t r = (uint8_t)(0x40 + op_reg[i]);
        int tl = (reg_get(ch, r) & 127) + att;
        reg_chip(ch, r, (uint8_t)(tl > 127 ? 127 : tl));
    }
}

static void fm_on(int ch, int note, int vel, int midi_ch)
{
    ym_write(0, 0x28, ch_code[ch]);                 /* (retrigger) */
    fm_pitch(ch, note, midi_ch, 0xA4, 0xA0);
    fm_levels(ch, vel);
    if (ms.mode == MODE_POLY && ms.spread) {
        rnd = (uint16_t)(rnd * 25173 + 13849);
        static const uint8_t pans[3] = { 0x80, 0x40, 0xC0 };
        reg_chip(ch, 0xB4, (uint8_t)(pans[(rnd >> 8) % 3] | (reg_get(ch, 0xB4) & 0x3F)));
    }
    ym_write(0, 0x28, (uint8_t)(0xF0 | ch_code[ch]));
    fm[ch].note = (uint8_t)note;
    fm[ch].vel = (uint8_t)vel;
    fm[ch].on = 1;
    fm[ch].held = 0;
    fm[ch].age = ++clock_age;
    changed();
}

static void fm_off(int ch)
{
    ym_write(0, 0x28, ch_code[ch]);
    fm[ch].on = 0;
    fm[ch].held = 0;
    changed();
}

/* ---- the PSG ---- */

static void psg_tone_set(int c, int note, int midi_ch)
{
    int u = pitch_of(note, midi_ch) - 192 + 192 * 8;    /* the table starts an octave up */
    int oct = u / 192 - 8, idx = u % 192;
    unsigned n = psg_tone[ms.region][idx];
    if (oct < 0) { n <<= -oct; if (n > 1023) n = 1023; }
    else n >>= oct;
    if (n < 1) n = 1;
    psg_write((uint8_t)(0x80 | c << 5 | (n & 15)));
    psg_write((uint8_t)(n >> 4));
}

static void psg_volume(int c, int vel)
{
    int v = vel ? 15 - (vel >> 3) : 15;
    psg_write((uint8_t)(0x90 | c << 5 | v));
}

static void psg_on(int c, int note, int vel, int midi_ch)
{
    if (c < 3) {
        psg_tone_set(c, note, midi_ch);
    } else {
        /* noise: the note picks the kind (white from C, periodic from C an
         * octave up) and the rate (the last of each four follows tone 3) */
        psg_write((uint8_t)(0xE0 | ((note / 12) & 1 ? 0 : 4) | (note % 4)));
    }
    psg_volume(c, vel);
    psg_note[c] = (uint8_t)note;
    psg_vel[c] = (uint8_t)vel;
    changed();
}

static void psg_off(int c)
{
    psg_volume(c, 0);
    psg_vel[c] = 0;
    changed();
}

/* ------------------------------------------------------------ notes */

static int poly_voice(void)
{
    int best = -1;
    for (int c = 0; c < 6; c++)             /* a free channel, the longest free */
        if (!fm[c].on && !fm[c].held && (best < 0 || fm[c].age < fm[best].age))
            best = c;
    if (best >= 0) return best;
    for (int c = 0; c < 6; c++)             /* else the oldest note */
        if (best < 0 || fm[c].age < fm[best].age)
            best = c;
    return best;
}

static void note_on(int mch, int note, int vel)
{
    if (ms.mode == MODE_POLY && mch == ms.poly_ch) {
        fm_on(poly_voice(), note, vel, mch);
        return;
    }
    if (special3 && (mch == 2 || (mch >= 10 && mch <= 12))) {
        /* channel 3 special: an operator each (OP1 11, OP2 12, OP3 13, OP4 3) */
        static const uint8_t hi[4] = { 0xAD, 0xAE, 0xAC, 0xA6 }, lo[4] = { 0xA9, 0xAA, 0xA8, 0xA2 };
        int op = mch == 2 ? 3 : mch - 10;
        fm_pitch(0, note, mch, hi[op], lo[op]);     /* (absolute registers: channel 0) */
        int tl = (reg_get(2, (uint8_t)(0x40 + op_reg[op])) & 127) + (ms.velocity ? vel_att[vel] : 0);
        reg_chip(2, (uint8_t)(0x40 + op_reg[op]), (uint8_t)(tl > 127 ? 127 : tl));
        sp_keys = (uint8_t)(sp_keys | sp_slot[op]);
        ym_write(0, 0x28, (uint8_t)(sp_keys | 2));
        changed();
        return;
    }
    if (mch < 6) fm_on(mch, note, vel, mch);
    else if (mch < 10) psg_on(mch - 6, note, vel, mch);
}

static void note_off(int mch, int note)
{
    if (ms.mode == MODE_POLY && mch == ms.poly_ch) {
        for (int c = 0; c < 6; c++)
            if (fm[c].on && fm[c].note == note) {
                if (sustain[mch]) { fm[c].on = 0; fm[c].held = 1; }
                else fm_off(c);
            }
        return;
    }
    if (special3 && (mch == 2 || (mch >= 10 && mch <= 12))) {
        int op = mch == 2 ? 3 : mch - 10;
        sp_keys = (uint8_t)(sp_keys & ~sp_slot[op]);
        ym_write(0, 0x28, (uint8_t)(sp_keys | 2));
        changed();
        return;
    }
    if (mch < 6) {
        if (fm[mch].on && fm[mch].note == note) {
            if (sustain[mch]) { fm[mch].on = 0; fm[mch].held = 1; }
            else fm_off(mch);
        }
    } else if (mch < 10) {
        int c = mch - 6;
        if (psg_vel[c] && psg_note[c] == note) psg_off(c);
    }
}

static void pedal(int mch, int down)
{
    sustain[mch] = (uint8_t)down;
    if (down) return;
    for (int c = 0; c < 6; c++) {
        int owner = ms.mode == MODE_POLY ? ms.poly_ch : c;
        if (owner == mch && fm[c].held) fm_off(c);
    }
}

static void bend_to(int mch, int value)
{
    bend[mch] = (int16_t)(value - 8192);
    if (ms.mode == MODE_POLY && mch == ms.poly_ch) {
        for (int c = 0; c < 6; c++)
            if (fm[c].on || fm[c].held) fm_pitch(c, fm[c].note, mch, 0xA4, 0xA0);
    } else if (mch < 6) {
        if (fm[mch].on || fm[mch].held) fm_pitch(mch, fm[mch].note, mch, 0xA4, 0xA0);
    } else if (mch < 9 && psg_vel[mch - 6]) {
        psg_tone_set(mch - 6, psg_note[mch - 6], mch);
    }
    changed();
}

/* ------------------------------------------------------------ the CC map */

static unsigned scale(int v, int n) { return (unsigned)(v * n) >> 7; }  /* 0..127 -> 0..n-1 */

static void cc_channel(int ch, int cc, int v)
{
    uint8_t r;
    int op;
    switch (cc) {
    case 14: r = reg_get(ch, 0xB0); reg_set(ch, 0xB0, (uint8_t)((r & 0x38) | scale(v, 8))); return;
    case 15: r = reg_get(ch, 0xB0); reg_set(ch, 0xB0, (uint8_t)((r & 7) | scale(v, 8) << 3)); return;
    case 77: r = reg_get(ch, 0xB4); reg_set(ch, 0xB4, (uint8_t)((r & 0x3F) | (v >> 5) << 6)); return;
    case 76: r = reg_get(ch, 0xB4); reg_set(ch, 0xB4, (uint8_t)((r & 0xCF) | (v >> 5) << 4)); return;
    case 75: r = reg_get(ch, 0xB4); reg_set(ch, 0xB4, (uint8_t)((r & 0xF8) | scale(v, 8))); return;
    }
    if (cc >= 16 && cc <= 19) {                     /* total level: 127 = loudest */
        op = cc - 16;
        reg_set(ch, (uint8_t)(0x40 + op_reg[op]), (uint8_t)(127 - v));
        if (fm[ch].on) fm_levels(ch, fm[ch].vel);
    } else if (cc >= 20 && cc <= 23) {              /* multiple */
        op = cc - 20; r = (uint8_t)(0x30 + op_reg[op]);
        reg_set(ch, r, (uint8_t)((reg_get(ch, r) & 0x70) | scale(v, 16)));
    } else if (cc >= 24 && cc <= 27) {              /* detune (the register's own encoding) */
        op = cc - 24; r = (uint8_t)(0x30 + op_reg[op]);
        reg_set(ch, r, (uint8_t)((reg_get(ch, r) & 0x0F) | (v >> 4) << 4));
    } else if (cc >= 39 && cc <= 42) {              /* rate scaling */
        op = cc - 39; r = (uint8_t)(0x50 + op_reg[op]);
        reg_set(ch, r, (uint8_t)((reg_get(ch, r) & 0x1F) | scale(v, 4) << 6));
    } else if (cc >= 43 && cc <= 46) {              /* attack */
        op = cc - 43; r = (uint8_t)(0x50 + op_reg[op]);
        reg_set(ch, r, (uint8_t)((reg_get(ch, r) & 0xC0) | scale(v, 32)));
    } else if (cc >= 47 && cc <= 50) {              /* first decay */
        op = cc - 47; r = (uint8_t)(0x60 + op_reg[op]);
        reg_set(ch, r, (uint8_t)((reg_get(ch, r) & 0x80) | scale(v, 32)));
    } else if (cc >= 51 && cc <= 54) {              /* second decay */
        op = cc - 51;
        reg_set(ch, (uint8_t)(0x70 + op_reg[op]), (uint8_t)scale(v, 32));
    } else if (cc >= 55 && cc <= 58) {              /* sustain level */
        op = cc - 55; r = (uint8_t)(0x80 + op_reg[op]);
        reg_set(ch, r, (uint8_t)((reg_get(ch, r) & 0x0F) | scale(v, 16) << 4));
    } else if (cc >= 59 && cc <= 62) {              /* release */
        op = cc - 59; r = (uint8_t)(0x80 + op_reg[op]);
        reg_set(ch, r, (uint8_t)((reg_get(ch, r) & 0xF0) | scale(v, 16)));
    } else if (cc >= 70 && cc <= 73) {              /* AM on/off */
        op = cc - 70; r = (uint8_t)(0x60 + op_reg[op]);
        reg_set(ch, r, (uint8_t)((reg_get(ch, r) & 0x1F) | (v >= 64 ? 0x80 : 0)));
    } else if (cc >= 90 && cc <= 93) {              /* SSG-EG */
        op = cc - 90;
        reg_set(ch, (uint8_t)(0x90 + op_reg[op]), (uint8_t)(v >= 32 ? 8 | ((v >> 2) & 7) : 0));
    }
}

static void control(int mch, int cc, int v)
{
    switch (cc) {                                   /* global */
    case 64: pedal(mch, v >= 64); return;
    case 74: ym_write(0, 0x22, (uint8_t)((shadow[0x22] & 7) | (v >= 64 ? 8 : 0))); changed(); return;
    case 1:
        if (ms.mode == MODE_POLY && mch == ms.poly_ch)  /* GENajam: the mod wheel turns the LFO on */
            ym_write(0, 0x22, (uint8_t)((shadow[0x22] & 7) | (v >= 64 ? 8 : 0)));
        else
            ym_write(0, 0x22, (uint8_t)((shadow[0x22] & 8) | scale(v, 8)));
        changed();
        return;
    case 81: bend_range = (uint8_t)((v * 11) >> 7); return;
    case 83: ms.region = v < 64 ? 1 : 0; return;    /* (GenMDM: < 64 PAL) */
    case 84: tet = (uint8_t)(v + 1); return;
    case 85: transpose = (int8_t)(v - 64); return;
    case 80:
        special3 = v >= 64;
        ym_write(0, 0x27, special3 ? 0x40 : 0x00);
        changed();
        return;
    case 120: case 123:                             /* all sound / notes off */
        for (int c = 0; c < 6; c++) if (fm[c].on || fm[c].held) fm_off(c);
        for (int c = 0; c < 4; c++) psg_off(c);
        return;
    }
    if (ms.mode == MODE_POLY && mch == ms.poly_ch) {
        for (int c = 0; c < 6; c++) cc_channel(c, cc, v);
        changed();
    } else if (mch < 6) {
        cc_channel(mch, cc, v);
        changed();
    }
}

/* ------------------------------------------------------------ the 32X's voices */

/* MIDI channels 11-16 belong to the 32X's PWM voices (drums, software FM):
 * their messages wait here for the master SH-2, which sees the head in
 * C_FLAGS' high byte and fetches them (CMD_EVENTS). 256 x 3 bytes. */
#ifdef MD_ONLY
uint8_t ev_ring[1][3];                  /* (no 32X: never used) */
#else
uint8_t ev_ring[256][3];
#endif

static void ev_push(uint8_t st, uint8_t a, uint8_t b)
{
    uint8_t h = (uint8_t)(md_flags >> 8);
    ev_ring[h][0] = st;
    ev_ring[h][1] = a;
    ev_ring[h][2] = b;
    md_flags = (uint16_t)((md_flags & 0xFF) | (uint8_t)(h + 1) << 8);
    flags_publish();
}

static int to_pwm(int mch)
{
#ifdef MD_ONLY
    (void)mch;
    return 0;                                   /* (no 32X: MIDI 11-16 play nothing) */
#endif
    if (mch < 10) return 0;
    if (special3 && mch <= 12) return 0;        /* (channel 3 special mode has 11-13) */
    return 1;
}

/* ------------------------------------------------------------ the monitor */

/* What came in, for the screen's MIDI monitor (CMD_MONITOR). The preview note
 * (X on the home screen) is left out: midi_quiet is set while it goes in. */
int midi_quiet;
uint16_t mon_chans, mon_count;
uint8_t mon_last[6];                    /* status, data 1, data 2, length */
uint8_t mon_ch[16][3];                  /* last note (0xFF: none yet), its velocity
                                         * (0: let go), keys down */

static void monitor(uint8_t st, uint8_t a, uint8_t b, int len)
{
    int mch = st & 15;
    mon_chans |= (uint16_t)(1u << mch);
    mon_count++;
    mon_last[0] = st;
    mon_last[1] = a;
    mon_last[2] = b;
    mon_last[3] = (uint8_t)len;
    uint8_t *c = mon_ch[mch];
    int on = (st & 0xF0) == 0x90 && b;
    if (on) {
        c[0] = a;
        c[1] = b;
        if (c[2] < 255) c[2]++;
    } else if ((st & 0xF0) == 0x80 || (st & 0xF0) == 0x90) {
        if (c[0] == a) c[1] = 0;
        if (c[2]) c[2]--;
    } else if ((st & 0xF0) == 0xB0 && (a == 123 || a == 120)) {   /* all notes off */
        c[1] = 0;
        c[2] = 0;
    }
}

/* ------------------------------------------------------------ the parser */

static uint8_t status, data[2], ndata, need;

void midi_init(void)
{
    status = 0;
    ndata = 0;
    for (int i = 0; i < 16; i++) { mon_ch[i][0] = 0xFF; mon_ch[i][1] = mon_ch[i][2] = 0; }
}

void midi_reset_voices(void)
{
    for (int c = 0; c < 6; c++) if (fm[c].on || fm[c].held) fm_off(c);
    for (int c = 0; c < 4; c++) if (psg_vel[c]) psg_off(c);
    for (int i = 0; i < 16; i++) { sustain[i] = 0; bend[i] = 0; }
}

void midi_byte(uint8_t b)
{
    if (b >= 0xF8) return;                          /* real time: nothing */
    if (b >= 0xF0) {                                /* system: forget the running status */
        status = 0;
        return;
    }
    if (b & 0x80) {
        status = b;
        ndata = 0;
        need = (b & 0xE0) == 0xC0 ? 1 : 2;          /* program change, pressure: one */
        return;
    }
    if (!status) return;
    data[ndata++] = b;
    if (ndata < need) return;
    ndata = 0;
    int mch = status & 15;
    if (!midi_quiet) monitor(status, data[0], need > 1 ? data[1] : 0, need + 1);
    if (to_pwm(mch)) {
        ev_push(status, data[0], need > 1 ? data[1] : 0);
        return;
    }
    switch (status & 0xF0) {
    case 0x80: note_off(mch, data[0]); break;
    case 0x90: if (data[1]) note_on(mch, data[0], data[1]); else note_off(mch, data[0]); break;
    case 0xB0: control(mch, data[0], data[1]); break;
    case 0xE0: bend_to(mch, data[0] | data[1] << 7); break;
    }
}
