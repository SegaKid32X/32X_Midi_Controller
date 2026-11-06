/* fm.c - software FM on the slave SH-2: the 32X's own FM voices for MIDI
 * channels 11-14, playing the same patches (TFI/DMP/Y12) as the YM2612.
 *
 * Built the way the YM2612 works (and so close in sound, though cleaner: no
 * 9-bit DAC):
 *   - four sine operators, the YM2612's eight algorithms; a modulator moves
 *     the next operator's phase by up to +-4 periods, operator 1 can feed
 *     back on itself (the average of its last two outputs);
 *   - envelopes in the chip's units (1024 steps of 0.09375 dB): attack
 *     (exponential), first decay to the sustain level, second decay, release,
 *     at the chip's rates (rate 2 x R + key scaling), applied every 16
 *     samples;
 *   - total level in 0.75 dB steps, velocity on the carriers;
 *   - MUL (0 = a half), detune (a few cents), pan, pitch bend, CC 7, the
 *     sustain pedal. (Not yet: SSG-EG, the LFO.)
 * 6 voices shared by the four channels; the oldest gives way.
 */
#include <string.h>
#include "fm.h"
#include "synth_tables.h"

#define NV      6               /* (the slave's time: raise once measured on a console) */
#define SUB     16              /* samples between envelope steps */

enum { EG_ATTACK, EG_DECAY1, EG_DECAY2, EG_RELEASE, EG_OFF };

typedef struct {
    uint32_t phase, inc;
    int32_t att;                /* 16.16, 0 (loud) .. 1023 */
    int stage;
    uint32_t r_ar, r_d1, r_d2, r_rr;   /* per sample, 16.16 */
    int32_t sl;                 /* the sustain level, 16.16 */
    int tl;                     /* TL x 8 (+ velocity on carriers) */
    int gain;                   /* x4096, for the next SUB samples */
} op_t;

typedef struct {
    op_t op[4];
    int alg, fb, on, held, ch, note, vel, age, ver;
    int out1[2];                /* operator 1's last two outputs (feedback) */
    int gl, gr;                 /* pan x volume, x256 */
} fmv_t;

static fmv_t v[NV];
static int age_clock;
static int bend[4], bend_range = 2;
static uint8_t volume[4] = { 100, 100, 100, 100 }, pan[4] = { 64, 64, 64, 64 }, pedal[4];
static const uint8_t carriers[8] = { 0x8, 0x8, 0x8, 0x8, 0xA, 0xE, 0xE, 0xF };

static uint32_t note_inc(int note, int ch)
{
    /* the note plus the bend, in 1/64 semitones */
    int u = note * 64 + bend[ch] * bend_range / 128;
    if (u < 0) u = 0;
    if (u > 127 * 64) u = 127 * 64;
    int n = u >> 6, f = u & 63;
    uint32_t a = fm_note_inc[n], b = n < 127 ? fm_note_inc[n + 1] : a;
    return a + (uint32_t)(((uint64_t)(b - a) * (uint32_t)f) >> 6);
}

/* (re)derive a voice's operators from its channel's patch, keeping their
 * phases and envelopes */
static void derive(fmv_t *x, const fm_patch *p)
{
    int kc = (x->note - 24) / 3;
    if (kc < 0) kc = 0;
    if (kc > 31) kc = 31;
    x->alg = p->alg & 7;
    x->fb = p->fb & 7;
    uint32_t base = note_inc(x->note, x->ch);
    for (int i = 0; i < 4; i++) {
        const fm_op *o = &p->op[i];
        op_t *q = &x->op[i];
        uint32_t inc = o->mul ? base * o->mul : base >> 1;
        int dt = o->dt & 3;
        if (dt) {
            uint32_t d = (uint32_t)(((uint64_t)inc * (uint32_t)(dt * 23)) >> 15);
            inc = (o->dt & 4) ? inc - d : inc + d;
        }
        q->inc = inc;
        int ks = kc >> (3 - (o->rs & 3));
        int r;
        r = o->ar ? 2 * o->ar + ks : 0;   q->r_ar = fm_eg_inc[r > 63 ? 63 : r];
        r = o->d1r ? 2 * o->d1r + ks : 0; q->r_d1 = fm_eg_inc[r > 63 ? 63 : r];
        r = o->d2r ? 2 * o->d2r + ks : 0; q->r_d2 = fm_eg_inc[r > 63 ? 63 : r];
        r = 4 * o->rr + 2 + ks;           q->r_rr = fm_eg_inc[r > 63 ? 63 : r];
        q->sl = (o->sl == 15 ? 1023 : o->sl * 32) << 16;
        int tl = (o->tl & 127) * 8;
        if (carriers[x->alg] >> i & 1) tl += fm_vel_att[x->vel] * 8;
        q->tl = tl;
    }
}

static void pan_gain(fmv_t *x)
{
    int vol = volume[x->ch], pn = pan[x->ch];
    const fm_patch *p = fm_patch_of(x->ch);
    int l = (p->lr & 2) ? 1 : 0, r = (p->lr & 1) ? 1 : 0;
    x->gl = l * vol * 2 * (pn < 64 ? 64 : 127 - pn) / 64;
    x->gr = r * vol * 2 * (pn > 64 ? 64 : pn) / 64;
}

void fm_note_on(int ch, int note, int vel)
{
    int k = -1;
    for (int i = 0; i < NV && k < 0; i++)          /* the same note again */
        if ((v[i].on || v[i].held) && v[i].ch == ch && v[i].note == note) k = i;
    for (int i = 0; i < NV && k < 0; i++)          /* a silent voice */
        if (!v[i].on && !v[i].held && v[i].op[0].stage == EG_OFF && v[i].op[1].stage == EG_OFF
            && v[i].op[2].stage == EG_OFF && v[i].op[3].stage == EG_OFF) k = i;
    for (int i = 0; i < NV && k < 0; i++)          /* a released one, the oldest */
        if (!v[i].on && !v[i].held && (k < 0 || v[i].age < v[k].age)) k = i;
    if (k < 0) {
        k = 0;
        for (int i = 1; i < NV; i++) if (v[i].age < v[k].age) k = i;
    }
    fmv_t *x = &v[k];
    memset(x, 0, sizeof *x);
    x->ch = ch;
    x->note = note;
    x->vel = vel;
    x->on = 1;
    x->age = ++age_clock;
    x->ver = fm_patch_version(ch);
    derive(x, fm_patch_of(ch));
    pan_gain(x);
    for (int i = 0; i < 4; i++) {
        x->op[i].att = 1023 << 16;
        x->op[i].stage = EG_ATTACK;
    }
}

static void release(fmv_t *x)
{
    x->on = 0;
    x->held = 0;
    for (int i = 0; i < 4; i++)
        if (x->op[i].stage != EG_OFF) x->op[i].stage = EG_RELEASE;
}

void fm_note_off(int ch, int note)
{
    for (int i = 0; i < NV; i++)
        if (v[i].on && v[i].ch == ch && v[i].note == note) {
            if (pedal[ch]) { v[i].on = 0; v[i].held = 1; }
            else release(&v[i]);
        }
}

void fm_control(int ch, int cc, int val)
{
    switch (cc) {
    case 7: volume[ch] = (uint8_t)val; break;
    case 10: pan[ch] = (uint8_t)val; break;
    case 64:
        pedal[ch] = val >= 64;
        if (!pedal[ch])
            for (int i = 0; i < NV; i++) if (v[i].held && v[i].ch == ch) release(&v[i]);
        return;
    case 81: bend_range = val * 11 >> 7; return;
    case 120: case 123:
        for (int i = 0; i < NV; i++) if (v[i].ch == ch) release(&v[i]);
        return;
    default: return;
    }
    for (int i = 0; i < NV; i++) if (v[i].ch == ch) pan_gain(&v[i]);
}

void fm_bend(int ch, int value)
{
    bend[ch] = value - 8192;
    for (int i = 0; i < NV; i++)
        if (v[i].ch == ch && (v[i].on || v[i].held)) derive(&v[i], fm_patch_of(ch));
}

/* one envelope step (SUB samples) */
static void envelope(op_t *q)
{
    int32_t a = q->att;
    switch (q->stage) {
    case EG_ATTACK:
        if (q->r_ar >= fm_eg_inc[62]) { a = 0; }
        else a -= (int32_t)(((int64_t)a * (q->r_ar * SUB >> 4)) >> 16);
        if (a <= 0) { a = 0; q->stage = EG_DECAY1; }
        break;
    case EG_DECAY1:
        a += (int32_t)(q->r_d1 * SUB);
        if (a >= q->sl) { a = q->sl; q->stage = EG_DECAY2; }
        break;
    case EG_DECAY2:
        a += (int32_t)(q->r_d2 * SUB);
        break;
    case EG_RELEASE:
        a += (int32_t)(q->r_rr * SUB);
        break;
    }
    if (a >= 1023 << 16) {
        a = 1023 << 16;
        if (q->stage == EG_RELEASE) q->stage = EG_OFF;
    }
    q->att = a;
    int t = (a >> 16) + q->tl;
    q->gain = q->stage == EG_OFF || t > FM_GAIN_MAX ? 0 : fm_gain[t];
}

#define OP(q, m) ((fm_sine[(((q)->phase >> 22) + ((m) >> 1)) & 1023] * (q)->gain) >> 12)

/* Add SUB samples of every voice into acc (stereo, interleaved) */
static void render_sub(int32_t *acc)
{
    for (int i = 0; i < NV; i++) {
        fmv_t *x = &v[i];
        op_t *o = x->op;
        if (o[0].stage == EG_OFF && o[1].stage == EG_OFF && o[2].stage == EG_OFF && o[3].stage == EG_OFF)
            continue;
        if (x->ver != fm_patch_version(x->ch)) {    /* the patch was edited: hear it now */
            x->ver = fm_patch_version(x->ch);
            derive(x, fm_patch_of(x->ch));
            pan_gain(x);
        }
        for (int k = 0; k < 4; k++) envelope(&o[k]);
        int fb = x->fb, alg = x->alg, gl = x->gl, gr = x->gr;
        int p0 = x->out1[0], p1 = x->out1[1];
        for (int s = 0; s < SUB; s++) {
            for (int k = 0; k < 4; k++) o[k].phase += o[k].inc;
            int fbm = fb ? (p0 + p1) >> (10 - fb) : 0;
            int o1 = (fm_sine[((o[0].phase >> 22) + fbm) & 1023] * o[0].gain) >> 12;
            p1 = p0; p0 = o1;
            int o2, o3, out;
            switch (alg) {
            case 0: o2 = OP(&o[1], o1); o3 = OP(&o[2], o2); out = OP(&o[3], o3); break;
            case 1: o2 = OP(&o[1], 0); o3 = OP(&o[2], o1 + o2); out = OP(&o[3], o3); break;
            case 2: o2 = OP(&o[1], 0); o3 = OP(&o[2], o2); out = OP(&o[3], o1 + o3); break;
            case 3: o2 = OP(&o[1], o1); o3 = OP(&o[2], 0); out = OP(&o[3], o2 + o3); break;
            case 4: out = OP(&o[1], o1) + OP(&o[3], OP(&o[2], 0)); break;
            case 5: out = OP(&o[1], o1) + OP(&o[2], o1) + OP(&o[3], o1); break;
            case 6: out = OP(&o[1], o1) + OP(&o[2], 0) + OP(&o[3], 0); break;
            default: out = o1 + OP(&o[1], 0) + OP(&o[2], 0) + OP(&o[3], 0); break;
            }
            acc[2 * s] += out * gl >> 8;
            acc[2 * s + 1] += out * gr >> 8;
        }
        x->out1[0] = p0;
        x->out1[1] = p1;
    }
}

void fm_render(int32_t *acc, int n)
{
    for (int s = 0; s + SUB <= n; s += SUB)
        render_sub(acc + 2 * s);
}

void fm_reset(void)
{
    memset(v, 0, sizeof v);
    for (int i = 0; i < NV; i++)
        for (int k = 0; k < 4; k++) v[i].op[k].stage = EG_OFF;
}
