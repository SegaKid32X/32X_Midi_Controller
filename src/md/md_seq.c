/* md_seq.c - the song (tracks of notes) and the sequencer, on the 68000.
 *
 * The song is a pool of bytes laid out as src/common/song.h says (the same
 * bytes are the file on the SD card). The 32X build keeps it in the 68000's
 * work RAM; the Genesis-only build, whose work RAM holds the screen, in the
 * Z80's 8 KB (the Z80 is never run: its bus is ours), a byte at a time.
 *
 * Time: the YM2612's timer A overflows every ~1 ms (polled, seq_poll); a
 * tick is 60 s / (BPM x 96). A frame that saw too few overflows (the polls
 * were late: the Genesis-only build draws on this CPU) gets the missing ones.
 *
 * Voices: a track plays as many notes at once as its kind has voices. An FM
 * track takes any free YM2612 channel (one that has its sound already, else
 * any, else the oldest note gives way) and loads its sound there; PSG
 * tracks the same over the three tone channels. The notes then go through
 * the MIDI engine (md_midi.c) on that channel, so velocity works as from a
 * keyboard. The 32X's tracks play their channel (11-14, 16): the 32X shares
 * its voices out itself. With MIDI OUT on, every note also goes to the
 * computer on the track's channel, with MIDI clock.
 *
 * Recording: the keyboard plays the selected track (SQS_LIVE). Recording,
 * each note is kept with the tick it began on and its length (the sustain
 * pedal holds them on) in a take, which plays along at once (a cycle plays
 * it back the next time round) and is merged into the song when it stops;
 * its notes stay marked until the next take, so it can be taken out again.
 */
#include <stddef.h>
#include "md.h"
#include "song.h"

#if defined(MD_ONLY) && !defined(MD_HOST)
typedef volatile uint8_t pb_t;          /* (the Z80's RAM: bytes only) */
#define SONG_BYTES  7424
#define POOL        ((pb_t *)0xA00000)
#define WORK        ((pb_t *)0xA01D00)  /* 256 bytes: the notes sounding, the keys held */
#define TAKE        ((pb_t *)0xA01E00)
#define TAKE_MAX    64
#else
typedef uint8_t pb_t;
#ifndef SONG_BYTES
#define SONG_BYTES  24576
#endif
#define TAKE_MAX    128
static uint8_t pool_mem[SONG_BYTES], work_mem[256], take_mem[TAKE_MAX * EV_BYTES];
#define POOL        pool_mem
#define WORK        work_mem
#define TAKE        take_mem
#endif

#define ACT_MAX     20                  /* notes sounding: WORK + i x 8 */
#define AF_USED     0x80
#define AF_LIVE     0x01                /* the keyboard's: no end yet */
#define AF_SUST     0x02                /* let go, the pedal holds it */
#define REC_AT      160                 /* keys held while recording: WORK + 160 + i x 8 */
#define REC_MAX     12
#define NO_END      0xFFFFFFu

/* ------------------------------------------------------------ the pool */

static unsigned p8(unsigned o) { return POOL[o]; }
static void w8(unsigned o, unsigned v) { POOL[o] = (uint8_t)v; }
static unsigned p16(unsigned o) { return (unsigned)(POOL[o] << 8 | POOL[o + 1]); }
static void w16(unsigned o, unsigned v) { POOL[o] = (uint8_t)(v >> 8); POOL[o + 1] = (uint8_t)v; }
static unsigned k8(unsigned o) { return WORK[o]; }
static void kw8(unsigned o, unsigned v) { WORK[o] = (uint8_t)v; }
static uint32_t k24(unsigned o) { return (uint32_t)WORK[o] << 16 | (uint32_t)WORK[o + 1] << 8 | WORK[o + 2]; }
static void kw24(unsigned o, uint32_t v) { WORK[o] = (uint8_t)(v >> 16); WORK[o + 1] = (uint8_t)(v >> 8); WORK[o + 2] = (uint8_t)v; }

static void pmove(unsigned dst, unsigned src, unsigned n)   /* (overlaps fine) */
{
    if (!n || dst == src) return;
#if !defined(MD_ONLY) || defined(MD_HOST)
    extern void *memmove(void *, const void *, size_t);
    memmove(POOL + dst, POOL + src, n);
#else
    if (dst < src) for (unsigned i = 0; i < n; i++) POOL[dst + i] = POOL[src + i];
    else for (unsigned i = n; i-- > 0; ) POOL[dst + i] = POOL[src + i];
#endif
}

typedef struct { uint32_t tick; uint16_t dur; uint8_t track, note, vel; int8_t qoff; } ev_t;

static void ev_rd(pb_t *b, unsigned a, ev_t *e)
{
    uint32_t t = (uint32_t)b[a] << 16 | (uint32_t)b[a + 1] << 8 | b[a + 2];
    e->tick = t >> 4;
    e->track = (uint8_t)(t & 15);
    e->qoff = (int8_t)b[a + 3];
    e->note = b[a + 4];
    e->vel = b[a + 5];
    e->dur = (uint16_t)(b[a + 6] << 8 | b[a + 7]);
}

static void ev_wr(pb_t *b, unsigned a, const ev_t *e)
{
    uint32_t t = e->tick << 4 | (e->track & 15);
    b[a] = (uint8_t)(t >> 16); b[a + 1] = (uint8_t)(t >> 8); b[a + 2] = (uint8_t)t;
    b[a + 3] = (uint8_t)e->qoff;
    b[a + 4] = e->note;
    b[a + 5] = e->vel;
    b[a + 6] = (uint8_t)(e->dur >> 8); b[a + 7] = (uint8_t)e->dur;
}

static uint32_t tick_at(pb_t *b, unsigned a)
{
    return ((uint32_t)b[a] << 16 | (uint32_t)b[a + 1] << 8 | b[a + 2]) >> 4;
}

static uint16_t edits;                  /* every change (the screen draws again) */
static uint8_t full;                    /* notes did not fit */
static unsigned ntake;

static unsigned ev0(void) { return SO_SOUNDS + p16(SO_PLEN); }
static unsigned nev(void) { return p16(SO_NEV); }
static unsigned ev_addr(unsigned i) { return ev0() + i * EV_BYTES; }
static unsigned used_bytes(void) { return ev0() + nev() * EV_BYTES; }
static unsigned trk(int t, int f) { return SO_TRACKS + (unsigned)t * TR_BYTES + (unsigned)f; }
static int kind_of(int t) { unsigned k = p8(trk(t, TR_KIND)); return k < TK_COUNT ? (int)k : TK_MIDI; }
static int ch_of(int t) { return p8(trk(t, TR_CH)) & 15; }
static unsigned bar_ticks(void) { unsigned b = p8(SO_BEATS); return (b ? b : 4) * SONG_PPQ; }

/* the first note at or after tick t (pool), and the same in the take */
static unsigned seek(uint32_t t)
{
    unsigned lo = 0, hi = nev(), e = ev0();
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (tick_at(POOL, e + mid * EV_BYTES) < t) lo = mid + 1; else hi = mid;
    }
    return lo;
}

static unsigned take_seek(uint32_t t)
{
    unsigned i = 0;
    while (i < ntake && tick_at(TAKE, i * EV_BYTES) < t) i++;
    return i;
}

/* ------------------------------------------------------------ the sounds */

static unsigned sound_at(int t)         /* its length byte */
{
    unsigned o = SO_SOUNDS;
    for (int i = 0; i < t; i++) o += 1 + p8(o);
    return o;
}

/* ------------------------------------------------------------ the song */

static const uint8_t def_kind[SONG_TRACKS] = { TK_FM, TK_FM, TK_FM, TK_FM, TK_PSG, TK_NOISE,
    TK_X32, TK_X32, TK_X32, TK_X32, TK_DRUMS, TK_FM, TK_FM, TK_PSG, TK_MIDI, TK_MIDI };
static const uint8_t def_ch[SONG_TRACKS] = { 0, 1, 2, 3, 4, 5, 10, 11, 12, 13, 9, 6, 7, 8, 14, 15 };

static void voices_forget(void);
static void tempo_set(void);

void song_new(void)
{
    for (unsigned o = 0; o < SO_SOUNDS + SONG_TRACKS; o++) w8(o, 0);
    const char *m = SONG_MAGIC;
    for (int i = 0; i < 4; i++) w8(SO_MAGIC + i, (uint8_t)m[i]);
    w8(SO_VER, SONG_VERSION);
    w8(SO_FLAGS, SF_CLICK_REC | SF_COUNTIN);
    w16(SO_BPM, 120);
    w8(SO_BEATS, 4);
    w16(SO_CYC_A, 0);
    w16(SO_CYC_B, 4);
    const char *nm = "NEW SONG";
    for (int i = 0; nm[i]; i++) w8(SO_NAME + i, (uint8_t)nm[i]);
    for (int t = 0; t < SONG_TRACKS; t++) {
        w8(trk(t, TR_KIND), def_kind[t]);
        w8(trk(t, TR_CH), def_ch[t]);
    }
    w16(SO_PLEN, SONG_TRACKS);          /* (each sound: a length of 0) */
    w16(SO_NEV, 0);
    ntake = 0;
    full = 0;
    voices_forget();
    tempo_set();
    edits++;
}

int song_sound_set(int t, const uint8_t *src, int len)
{
    if (t < 0 || t >= SONG_TRACKS || len < 0 || len > SOUND_MAX) return ERR_RANGE;
    unsigned o = sound_at(t), old = p8(o), end = used_bytes();
    if (end - old + (unsigned)len > SONG_BYTES) return ERR_TOO_BIG;
    pmove(o + 1 + (unsigned)len, o + 1 + old, end - (o + 1 + old));
    w8(o, (unsigned)len);
    for (int i = 0; i < len; i++) w8(o + 1 + (unsigned)i, src[i]);
    w16(SO_PLEN, p16(SO_PLEN) - old + (unsigned)len);
    voices_forget();
    edits++;
    return 0;
}

int song_sound_get(int t, uint8_t *dst)
{
    if (t < 0 || t >= SONG_TRACKS) return 0;
    unsigned o = sound_at(t), n = p8(o);
    for (unsigned i = 0; i < n; i++) dst[i] = (uint8_t)p8(o + 1 + i);
    return (int)n;
}

/* a note into the song, in time order (after those at the same tick) -> its index */
int song_add(const ev_t *e)
{
    if (used_bytes() + EV_BYTES > SONG_BYTES) { full = 1; return -1; }
    unsigned i = seek(e->tick + 1), a = ev_addr(i);
    pmove(a + EV_BYTES, a, used_bytes() - a);
    ev_wr(POOL, a, e);
    w16(SO_NEV, nev() + 1);
    edits++;
    return (int)i;
}

static void song_del(unsigned i)
{
    if (i >= nev()) return;
    unsigned a = ev_addr(i);
    pmove(a, a + EV_BYTES, used_bytes() - a - EV_BYTES);
    w16(SO_NEV, nev() - 1);
    edits++;
}

int song_note(unsigned i, ev_t *e)      /* (the host tests) */
{
    if (i >= nev()) return -1;
    ev_rd(POOL, ev_addr(i), e);
    return 0;
}

unsigned song_count(void) { return nev(); }

static void song_clear(int t)
{
    unsigned n = nev(), e = ev0(), k = 0;
    for (unsigned i = 0; i < n; i++) {
        unsigned a = e + i * EV_BYTES;
        if ((p8(a + 2) & 15) == (unsigned)t) continue;
        if (k != i) pmove(e + k * EV_BYTES, a, EV_BYTES);
        k++;
    }
    w16(SO_NEV, k);
    edits++;
}

static void undo_take(void)
{
    unsigned n = nev(), e = ev0(), k = 0;
    for (unsigned i = 0; i < n; i++) {
        unsigned a = e + i * EV_BYTES;
        if (p8(a + 5) & 0x80) continue;
        if (k != i) pmove(e + k * EV_BYTES, a, EV_BYTES);
        k++;
    }
    w16(SO_NEV, k);
    edits++;
}

/* by tick, keeping the order of equal ones (the list is nearly in order) */
static void song_sort(void)
{
    unsigned n = nev(), e = ev0();
    for (unsigned i = 1; i < n; i++) {
        uint32_t t = tick_at(POOL, e + i * EV_BYTES);
        unsigned j = i;
        while (j > 0 && tick_at(POOL, e + (j - 1) * EV_BYTES) > t) j--;
        if (j == i) continue;
        uint8_t keep[EV_BYTES];
        for (int b = 0; b < EV_BYTES; b++) keep[b] = (uint8_t)p8(e + i * EV_BYTES + (unsigned)b);
        pmove(e + (j + 1) * EV_BYTES, e + j * EV_BYTES, (i - j) * EV_BYTES);
        for (int b = 0; b < EV_BYTES; b++) w8(e + j * EV_BYTES + (unsigned)b, keep[b]);
    }
}

static const uint8_t q_ticks[Q_COUNT] = { 0, 96, 48, 32, 24, 16, 12 };

void song_quantize(int t, int q)
{
    if (q < 0 || q >= Q_COUNT) return;
    unsigned n = nev(), g = q_ticks[q];
    for (unsigned i = 0; i < n; i++) {
        unsigned a = ev_addr(i);
        ev_t e;
        ev_rd(POOL, a, &e);
        if (e.track != t) continue;
        int32_t orig = (int32_t)e.tick - e.qoff, to = orig;
        if (g) to = (orig + (int32_t)g / 2) / (int32_t)g * (int32_t)g;
        e.tick = (uint32_t)to;
        e.qoff = (int8_t)(to - orig);
        ev_wr(POOL, a, &e);
    }
    w8(trk(t, TR_QUANT), (unsigned)q);
    song_sort();
    edits++;
}

/* ------------------------------------------------------------ the take */

/* merge it into the song (its notes marked as the last take) */
static void take_merge(void)
{
    if (!ntake) return;
    unsigned n = nev(), m = ntake, e = ev0();
    while (m && used_bytes() + m * EV_BYTES > SONG_BYTES) { m--; full = 1; }
    for (unsigned i = 0; i < n; i++) {              /* (the take before: no longer the last) */
        unsigned a = e + i * EV_BYTES + 5;
        if (p8(a) & 0x80) w8(a, p8(a) & 0x7F);
    }
    int i = (int)n - 1, j = (int)m - 1, k = (int)(n + m) - 1;
    while (j >= 0) {
        if (i >= 0 && tick_at(POOL, e + (unsigned)i * EV_BYTES) > tick_at(TAKE, (unsigned)j * EV_BYTES)) {
            pmove(e + (unsigned)k * EV_BYTES, e + (unsigned)i * EV_BYTES, EV_BYTES);
            i--;
        } else {
            for (int b = 0; b < EV_BYTES; b++)
                w8(e + (unsigned)k * EV_BYTES + (unsigned)b, TAKE[(unsigned)j * EV_BYTES + (unsigned)b]);
            w8(e + (unsigned)k * EV_BYTES + 5, p8(e + (unsigned)k * EV_BYTES + 5) | 0x80);
            j--;
        }
        k--;
    }
    w16(SO_NEV, n + m);
    ntake = 0;
    edits++;
}

/* ------------------------------------------------------------ voices */

#define NVOICE  10                      /* YM2612 1-6, PSG 1-3, noise: MIDI channels 1-10 */
static uint8_t v_track[NVOICE], v_note[NVOICE], v_on[NVOICE], v_loaded[NVOICE];
static uint16_t v_age[NVOICE], age_now;

static void voices_forget(void)         /* (the sounds changed: load them again) */
{
    for (int v = 0; v < NVOICE; v++) v_loaded[v] = 0xFF;
}

static void load_sound(int v, int t)
{
    unsigned o = sound_at(t), len = p8(o++);
    if (v < 6) {
        if (len >= FM_SOUND) {
            int part = v / 3, c = v % 3;
            for (int i = 0; i < 28; i++)
                ym_write(part, (uint8_t)(0x30 + (i >> 2) * 0x10 + (i & 3) * 4 + c), (uint8_t)p8(o + (unsigned)i));
            ym_write(part, (uint8_t)(0xB0 + c), (uint8_t)p8(o + 28));
            ym_write(part, (uint8_t)(0xB4 + c), (uint8_t)p8(o + 29));
        }
    } else {
        uint8_t b[SOUND_MAX];
        for (unsigned i = 0; i < len && i < SOUND_MAX; i++) b[i] = (uint8_t)p8(o + i);
        midi_psg_set(v - 6, b, (int)len);
    }
    v_loaded[v] = (uint8_t)t;
}

static void out3(uint8_t st, uint8_t a, uint8_t b)
{
    midi_out(st);
    midi_out(a);
    midi_out(b);
}

static void act_drop_voice(int v);

/* a voice for track t: free with its sound, free, else the oldest note */
static int voice_get(int t, int kind)
{
    int lo = kind == TK_FM ? 0 : kind == TK_PSG ? 6 : 9, hi = kind == TK_FM ? 6 : kind == TK_PSG ? 9 : 10;
    int best = -1, pass;
    for (pass = 0; pass < 3 && best < 0; pass++)
        for (int v = lo; v < hi; v++) {
            if (pass == 0 && (v_on[v] || v_loaded[v] != t)) continue;
            if (pass == 1 && v_on[v]) continue;
            if (best < 0 || (uint16_t)(age_now - v_age[v]) > (uint16_t)(age_now - v_age[best])) best = v;
        }
    if (v_on[best]) {                   /* it gives way */
        act_drop_voice(best);
        midi_play((uint8_t)(0x80 | best), v_note[best], 0);
        v_on[best] = 0;
    }
    return best;
}

/* a note of track t -> how to end it: a voice (0-9), 0x80 | a channel, 0xFF */
static int note_on(int t, int note, int vel, int out)
{
    int kind = kind_of(t), h = 0xFF;
    if (kind <= TK_NOISE) {
        int v = voice_get(t, kind);
        if (v_loaded[v] != t) load_sound(v, t);
        midi_play((uint8_t)(0x90 | v), (uint8_t)note, (uint8_t)vel);
        v_on[v] = 1; v_track[v] = (uint8_t)t; v_note[v] = (uint8_t)note; v_age[v] = ++age_now;
        h = v;
    } else if (kind == TK_X32 || kind == TK_DRUMS) {
        int c = kind == TK_DRUMS ? 15 : 10 + ((ch_of(t) - 10) & 3);   /* (11-14) */
        midi_play((uint8_t)(0x90 | c), (uint8_t)note, (uint8_t)vel);
        h = 0x80 | c;
    }
    if (out && ms.midi_out) out3((uint8_t)(0x90 | ch_of(t)), (uint8_t)note, (uint8_t)vel);
    return h;
}

static void note_off(int h, int t, int note, int out)
{
    if (h < NVOICE) {
        if (v_on[h] && v_track[h] == t && v_note[h] == note) {
            midi_play((uint8_t)(0x80 | h), (uint8_t)note, 0);
            v_on[h] = 0;
            v_age[h] = ++age_now;
        }
    } else if (h != 0xFF) {
        midi_play((uint8_t)(0x80 | (h & 15)), (uint8_t)note, 0);
    }
    if (out && ms.midi_out) out3((uint8_t)(0x80 | ch_of(t)), (uint8_t)note, 0);
}

/* ------------------------------------------------------------ notes sounding (WORK) */

static uint8_t seq_flags;
static uint32_t now;                    /* the tick played next */

static unsigned act_new(void)
{
    unsigned best = 0;
    for (unsigned i = 0; i < ACT_MAX; i++) {
        unsigned a = i * 8;
        if (!(k8(a + 3) & AF_USED)) return a;
        if (!(k8(a + 3) & AF_LIVE) && k24(a + 4) < k24(best * 8 + 4)) best = i;
    }
    unsigned a = best * 8;              /* full: the one ending first ends now */
    note_off(k8(a + 2), k8(a), k8(a + 1), !(k8(a + 3) & AF_LIVE) || ms.midi_out >= 2);
    return a;
}

static void act_add(int t, int note, int h, uint32_t end, int flags)
{
    unsigned a = act_new();
    kw8(a, (unsigned)t); kw8(a + 1, (unsigned)note); kw8(a + 2, (unsigned)h);
    kw8(a + 3, AF_USED | (unsigned)flags);
    kw24(a + 4, end);
}

static void act_end(unsigned a)
{
    int out = !(k8(a + 3) & AF_LIVE) || ms.midi_out >= 2;
    note_off(k8(a + 2), k8(a), k8(a + 1), out);
    kw8(a + 3, 0);
}

static void act_drop_voice(int v)       /* (a voice given to another note) */
{
    for (unsigned i = 0; i < ACT_MAX; i++) {
        unsigned a = i * 8;
        if ((k8(a + 3) & AF_USED) && k8(a + 2) == (unsigned)v) {
            if (ms.midi_out && (!(k8(a + 3) & AF_LIVE) || ms.midi_out >= 2))
                out3((uint8_t)(0x80 | ch_of((int)k8(a))), (uint8_t)k8(a + 1), 0);
            kw8(a + 3, 0);
        }
    }
}

static void act_all_off(int live_too)
{
    for (unsigned i = 0; i < ACT_MAX; i++) {
        unsigned a = i * 8;
        if (!(k8(a + 3) & AF_USED)) continue;
        if (!live_too && (k8(a + 3) & AF_LIVE)) continue;
        act_end(a);
    }
}

/* ------------------------------------------------------------ the clock */

static uint32_t acc_us, tick_len;       /* tick_len: a tick in 1/16 microseconds */
static uint16_t timer_us;
static uint8_t frame_ticks, click_left;
static uint32_t pre_ticks;              /* the count-in still to go */
static unsigned pi, ti;                 /* the next note to play: in the song, in the take */

static void tempo_set(void)
{
    unsigned bpm = p16(SO_BPM);
    if (bpm < 20) bpm = 20;
    if (bpm > 300) bpm = 300;
    tick_len = (60000000u / SONG_PPQ * 16) / bpm;
    timer_us = ms.region ? 1004 : 995;  /* timer A: 53 x 144 / the YM2612's clock */
}

static void click(int accent)
{
    psg_write((uint8_t)(accent ? 0xE4 : 0xE5));     /* the noise: white, high */
    psg_write((uint8_t)(accent ? 0xF0 : 0xF3));
    click_left = 2;                                 /* frames */
}

static int audible(int t)
{
    unsigned f = p8(trk(t, TR_FLAGS));
    if (f & TF_MUTE) return 0;
    for (int k = 0; k < SONG_TRACKS; k++)
        if (p8(trk(k, TR_FLAGS)) & TF_SOLO) return (f & TF_SOLO) != 0;
    return 1;
}

static uint32_t cyc_a(void) { return p16(SO_CYC_A) * bar_ticks(); }
static uint32_t cyc_b(void) { return p16(SO_CYC_B) * bar_ticks(); }
static int cycling(void) { return (p8(SO_FLAGS) & SF_CYCLE) && p16(SO_CYC_B) > p16(SO_CYC_A); }

static void spp_out(void)               /* where we are, for the computer */
{
    uint32_t s = now / (SONG_PPQ / 4);
    if (s > 16383) s = 16383;
    midi_out(0xF2);
    midi_out((uint8_t)(s & 127));
    midi_out((uint8_t)(s >> 7));
}

static void play_ev(const ev_t *e)
{
    if (!audible(e->track)) return;
    int h = note_on(e->track, e->note, e->vel & 0x7F, 1);
    act_add(e->track, e->note, h, now + (e->dur ? e->dur : 1), 0);
}

static void tick_step(void)
{
    if (pre_ticks) {                    /* the count-in */
        if (pre_ticks % SONG_PPQ == 0) click(pre_ticks == bar_ticks());
        if (!--pre_ticks && ms.midi_out) { spp_out(); midi_out(0xFB); }
        return;
    }
    for (unsigned i = 0; i < ACT_MAX; i++) {        /* notes that end */
        unsigned a = i * 8;
        if ((k8(a + 3) & (AF_USED | AF_LIVE)) == AF_USED && k24(a + 4) <= now) act_end(a);
    }
    if (cycling() && now >= cyc_b()) {              /* round again */
        act_all_off(0);
        now = cyc_a();
        pi = seek(now);
        ti = take_seek(now);
        if (ms.midi_out) { spp_out(); }
    }
    unsigned f = p8(SO_FLAGS);
    if (now % SONG_PPQ == 0 && (f & ((seq_flags & SQS_REC) ? SF_CLICK_REC : SF_CLICK_PLAY)))
        click(now % bar_ticks() == 0);
    if (ms.midi_out && now % (SONG_PPQ / 24) == 0) midi_out(0xF8);
    ev_t e;
    unsigned n = nev(), base = ev0();
    while (pi < n && tick_at(POOL, base + pi * EV_BYTES) <= now) {
        ev_rd(POOL, base + pi * EV_BYTES, &e);
        play_ev(&e);
        pi++;
    }
    while (ti < ntake && tick_at(TAKE, ti * EV_BYTES) <= now) {
        ev_rd(TAKE, ti * EV_BYTES, &e);
        play_ev(&e);
        ti++;
    }
    now++;
}

static void advance(uint32_t us)
{
    acc_us += us * 16;
    while (acc_us >= tick_len) {
        acc_us -= tick_len;
        tick_step();
    }
}

void seq_poll(void)
{
    if (!(seq_flags & SQS_PLAY) || !ym_timer_tick()) return;
    if (frame_ticks < 255) frame_ticks++;
    advance(timer_us);
}

void seq_frame(void)
{
    if (click_left && !--click_left) psg_write(0xFF);   /* (the click: off) */
    if (!(seq_flags & SQS_PLAY)) return;
    unsigned want = ms.region ? 19 : 16;    /* (timer ticks a frame: 19.9 PAL, 16.8 NTSC) */
    if (frame_ticks + 1u < want)            /* polled too late to see them all */
        advance((uint32_t)(want - frame_ticks) * timer_us);
    frame_ticks = 0;
}

/* ------------------------------------------------------------ recording */

static uint8_t pedal;

static void rec_finish(unsigned a)      /* a key let go: its note into the take */
{
    ev_t e;
    uint32_t start = k24(a + 2), d = now > start ? now - start : 1;
    e.tick = start;
    e.track = (uint8_t)(p8(SO_SEL) & 15);
    e.note = (uint8_t)k8(a);
    e.vel = (uint8_t)(k8(a + 1) & 0x7F);
    e.dur = (uint16_t)(d > 65535 ? 65535 : d);
    e.qoff = 0;
    kw8(a + 5, 0);
    if (ntake >= TAKE_MAX) {            /* (full: into the song now) */
        take_merge();
        pi = seek(now);
        ti = 0;
    }
    unsigned i = ntake;
    while (i > 0 && tick_at(TAKE, (i - 1) * EV_BYTES) > e.tick) {
        for (int b = 0; b < EV_BYTES; b++) TAKE[i * EV_BYTES + (unsigned)b] = TAKE[(i - 1) * EV_BYTES + (unsigned)b];
        i--;
    }
    ev_wr(TAKE, i * EV_BYTES, &e);
    ntake++;
    if (i <= ti) ti++;                  /* (it sounded already: not again this time round) */
    edits++;
}

static void rec_on(int note, int vel)
{
    unsigned a = REC_AT, oldest = REC_AT;
    for (unsigned i = 0; i < REC_MAX; i++, a += 8) {
        if (!(k8(a + 5) & AF_USED)) break;
        if (k24(a + 2) < k24(oldest + 2)) oldest = a;
    }
    if (a >= REC_AT + REC_MAX * 8) { rec_finish(oldest); a = oldest; }
    kw8(a, (unsigned)note);
    kw8(a + 1, (unsigned)vel);
    kw24(a + 2, now);
    kw8(a + 5, AF_USED);
}

static void rec_off(int note)           /* -> the pedal holds it, or it ends */
{
    for (unsigned a = REC_AT; a < REC_AT + REC_MAX * 8; a += 8)
        if ((k8(a + 5) & AF_USED) && !(k8(a + 5) & AF_SUST) && k8(a) == (unsigned)note) {
            if (pedal) kw8(a + 5, AF_USED | AF_SUST);
            else rec_finish(a);
            return;
        }
}

static void rec_all(int only_sust)
{
    for (unsigned a = REC_AT; a < REC_AT + REC_MAX * 8; a += 8)
        if ((k8(a + 5) & AF_USED) && (!only_sust || (k8(a + 5) & AF_SUST))) rec_finish(a);
}

static int recording(void) { return (seq_flags & (SQS_PLAY | SQS_REC)) == (SQS_PLAY | SQS_REC) && !pre_ticks; }

/* ------------------------------------------------------------ transport */

void seq_stop(void)
{
    if (!(seq_flags & SQS_PLAY)) return;
    if (recording()) rec_all(0);
    seq_flags &= (uint8_t)~(SQS_PLAY | SQS_REC | SQS_COUNTIN);
    pre_ticks = 0;
    ym_timer_run(0);
    act_all_off(0);
    take_merge();
    if (ms.midi_out) midi_out(0xFC);
    if (click_left) { click_left = 0; psg_write(0xFF); }
}

void seq_play(int rec)
{
    if (seq_flags & SQS_PLAY) return;
    ms.mode = MODE_GENMDM;              /* (each YM2612 channel its own: the voices are ours) */
    if (cycling() && (now >= cyc_b() || (rec && now < cyc_a()))) now = cyc_a();
    ntake = 0;
    for (unsigned a = REC_AT; a < REC_AT + REC_MAX * 8; a += 8) kw8(a + 5, 0);
    pi = seek(now);
    ti = 0;
    acc_us = 0;
    frame_ticks = 0;
    pre_ticks = rec && (p8(SO_FLAGS) & SF_COUNTIN) ? bar_ticks() : 0;
    seq_flags = (uint8_t)((seq_flags & SQS_LIVE) | SQS_PLAY | (rec ? SQS_REC : 0));
    tempo_set();
    if (ms.midi_out && !pre_ticks) {
        if (now) { spp_out(); midi_out(0xFB); } else midi_out(0xFA);
    }
    ym_timer_run(1);
    tick_step();                        /* (the first tick at once: its notes, the first click) */
}

int seq_playing(void) { return (seq_flags & SQS_PLAY) != 0; }

void seq_locate(uint32_t t)
{
    if (t > 0xFFFFF) t = 0xFFFFF;
    if (seq_flags & SQS_PLAY) {
        if (recording()) rec_all(0);
        act_all_off(0);
        take_merge();
        now = t;
        pi = seek(now);
        ti = 0;
        if (ms.midi_out) spp_out();
    } else {
        now = t;
    }
}

/* ------------------------------------------------------------ the keyboard */

/* a message from the keyboard (md_midi.c): 1 if it was taken here */
int seq_live(uint8_t st, uint8_t a, uint8_t b)
{
    if (!(seq_flags & SQS_LIVE)) return 0;
    uint8_t type = st & 0xF0;
    int sel = p8(SO_SEL) & 15;
    if (type == 0x90 && b) {
        int h = note_on(sel, a, b, ms.midi_out >= 2);
        act_add(sel, a, h, NO_END, AF_LIVE);
        if (recording()) rec_on(a, b);
        return 1;
    }
    if (type == 0x80 || type == 0x90) {
        for (unsigned i = 0; i < ACT_MAX; i++) {
            unsigned q = i * 8;
            if ((k8(q + 3) & (AF_USED | AF_LIVE | AF_SUST)) == (AF_USED | AF_LIVE) && k8(q + 1) == a) {
                if (pedal) kw8(q + 3, AF_USED | AF_LIVE | AF_SUST);
                else act_end(q);
                break;
            }
        }
        if (recording()) rec_off(a);
        return 1;
    }
    if (type == 0xB0 && a == 64) {      /* the sustain pedal */
        pedal = b >= 64;
        if (!pedal) {
            for (unsigned i = 0; i < ACT_MAX; i++) {
                unsigned q = i * 8;
                if ((k8(q + 3) & (AF_USED | AF_SUST)) == (AF_USED | AF_SUST)) act_end(q);
            }
            if (recording()) rec_all(1);
        }
        if (ms.midi_out >= 2) out3((uint8_t)(0xB0 | ch_of(sel)), a, b);
        return 1;
    }
    return 1;                           /* (the rest: not into the voices the song uses) */
}

static void live_set(int on)
{
    if (on) { seq_flags |= SQS_LIVE; ms.mode = MODE_GENMDM; return; }
    seq_flags &= (uint8_t)~SQS_LIVE;
    for (unsigned i = 0; i < ACT_MAX; i++)
        if (k8(i * 8 + 3) & AF_LIVE) act_end(i * 8);
    pedal = 0;
}

/* ------------------------------------------------------------ files */

int song_save(void)
{
    const char *path = files_song_path();
    if (!path) return ERR_NO_ED;
    seq_stop();
    files_song_mkdir();
#if defined(MD_ONLY) && !defined(MD_HOST)
    return ed_write_file(path, (const uint8_t *)POOL, (int)used_bytes());
#else
    return ed_write_file(path, POOL, (int)used_bytes());
#endif
}

static int song_valid(unsigned len)
{
    const char *m = SONG_MAGIC;
    for (int i = 0; i < 4; i++) if (p8(SO_MAGIC + i) != (uint8_t)m[i]) return 0;
    if (p8(SO_VER) != SONG_VERSION || len < SO_SOUNDS + SONG_TRACKS) return 0;
    if (p16(SO_PLEN) > SONG_BYTES || SO_SOUNDS + p16(SO_PLEN) > len) return 0;
    if (sound_at(SONG_TRACKS) != ev0() || used_bytes() != len) return 0;
    return 1;
}

int song_load(void)
{
    seq_stop();
    uint16_t len = 0;
#if defined(MD_ONLY) && !defined(MD_HOST)
    int e = files_song_read((uint8_t *)POOL, SONG_BYTES, &len);
#else
    int e = files_song_read(POOL, SONG_BYTES, &len);
#endif
    if (!e && !song_valid(len)) e = ERR_RANGE;
    if (e) song_new();
    ntake = 0;
    full = 0;
    now = 0;
    voices_forget();
    tempo_set();
    edits++;
    return e;
}

/* ------------------------------------------------------------ a standard MIDI file
 *
 * Type 1, 96 ticks a beat: a first track with the tempo, the bars and the
 * name, then one for each track that has notes (its instrument's name, its
 * notes on its channel). Made twice: once to count the bytes (the EverDrive
 * wants the size first), once to write them, a kilobyte at a time through
 * file_buf; each track is counted again just before it is written (its
 * header holds its length). */

static uint32_t smf_count;
static int smf_writing, smf_err;
static unsigned smf_fill;
#define SMF_BLOCK   1024
#define SMF_BUF     (file_buf + BUF_DATA)
#define PEND_MAX    16                  /* notes waiting for their end (WORK, 4 bytes each) */

static void smf_byte(uint8_t b)
{
    smf_count++;
    if (!smf_writing) return;
    SMF_BUF[smf_fill++] = b;
    if (smf_fill == SMF_BLOCK) {
        if (!smf_err) smf_err = ed_stream_put(SMF_BUF, SMF_BLOCK);
        smf_fill = 0;
    }
}

static void smf_var(uint32_t v)
{
    uint8_t b[5];
    int n = 0;
    b[n++] = (uint8_t)(v & 0x7F);
    while (v >>= 7) b[n++] = (uint8_t)(0x80 | (v & 0x7F));
    while (n--) smf_byte(b[n]);
}

static void smf_32(uint32_t v) { smf_byte((uint8_t)(v >> 24)); smf_byte((uint8_t)(v >> 16)); smf_byte((uint8_t)(v >> 8)); smf_byte((uint8_t)v); }

static void smf_text(uint8_t type, unsigned at, int max)
{
    int n = 0;
    while (n < max && p8(at + (unsigned)n)) n++;
    smf_var(0); smf_byte(0xFF); smf_byte(type); smf_var((uint32_t)n);
    for (int i = 0; i < n; i++) smf_byte((uint8_t)p8(at + (unsigned)i));
}

static void smf_conductor(void)
{
    unsigned bpm = p16(SO_BPM) ? p16(SO_BPM) : 120;
    uint32_t qus = 60000000u / bpm;
    smf_text(0x03, SO_NAME, 16);
    smf_var(0); smf_byte(0xFF); smf_byte(0x51); smf_byte(3);
    smf_byte((uint8_t)(qus >> 16)); smf_byte((uint8_t)(qus >> 8)); smf_byte((uint8_t)qus);
    smf_var(0); smf_byte(0xFF); smf_byte(0x58); smf_byte(4);
    smf_byte((uint8_t)(bar_ticks() / SONG_PPQ)); smf_byte(2); smf_byte(24); smf_byte(8);
    smf_var(0); smf_byte(0xFF); smf_byte(0x2F); smf_byte(0);
}

static uint32_t smf_last;
static unsigned npend;

static void pend_flush(uint32_t upto, int ch)   /* the notes that end by then, first first */
{
    for (;;) {
        unsigned best = PEND_MAX;
        for (unsigned i = 0; i < npend; i++)
            if (k24(i * 4) <= upto && (best == PEND_MAX || k24(i * 4) < k24(best * 4))) best = i;
        if (best == PEND_MAX) return;
        uint32_t t = k24(best * 4);
        smf_var(t - smf_last); smf_last = t;
        smf_byte((uint8_t)(0x80 | ch)); smf_byte((uint8_t)k8(best * 4 + 3)); smf_byte(0);
        npend--;
        for (int b = 0; b < 4; b++) kw8(best * 4 + (unsigned)b, k8(npend * 4 + (unsigned)b));
    }
}

static void smf_trk(int t)
{
    int ch = ch_of(t);
    smf_last = 0;
    npend = 0;
    smf_text(0x03, trk(t, TR_NAME), 16);
    unsigned n = nev();
    ev_t e;
    for (unsigned i = 0; i < n; i++) {
        ev_rd(POOL, ev_addr(i), &e);
        if (e.track != t) continue;
        pend_flush(e.tick, ch);
        if (npend == PEND_MAX) {        /* (too many at once: the first to end, ends now) */
            unsigned best = 0;
            for (unsigned k = 1; k < npend; k++) if (k24(k * 4) < k24(best * 4)) best = k;
            kw24(best * 4, e.tick);
            pend_flush(e.tick, ch);
        }
        smf_var(e.tick - smf_last); smf_last = e.tick;
        smf_byte((uint8_t)(0x90 | ch)); smf_byte(e.note); smf_byte((uint8_t)((e.vel & 0x7F) ? (e.vel & 0x7F) : 1));
        kw24(npend * 4, e.tick + (e.dur ? e.dur : 1));
        kw8(npend * 4 + 3, e.note);
        npend++;
        rx_poll();
    }
    pend_flush(0xFFFFFFFFu, ch);
    smf_var(0); smf_byte(0xFF); smf_byte(0x2F); smf_byte(0);
}

static int has_notes(int t)
{
    unsigned n = nev();
    for (unsigned i = 0; i < n; i++) if ((p8(ev_addr(i) + 2) & 15) == (unsigned)t) return 1;
    return 0;
}

static uint32_t chunk_len(int t)        /* (counted: nothing written) */
{
    int w = smf_writing;
    uint32_t c = smf_count;
    smf_writing = 0;
    if (t < 0) smf_conductor(); else smf_trk(t);
    uint32_t len = smf_count - c;
    smf_count = c;
    smf_writing = w;
    return len;
}

static void smf_chunk(int t)
{
    uint32_t len = chunk_len(t);
    smf_byte('M'); smf_byte('T'); smf_byte('r'); smf_byte('k');
    smf_32(len);
    if (t < 0) smf_conductor(); else smf_trk(t);
}

static void smf_all(void)
{
    int ntr = 1;
    for (int t = 0; t < SONG_TRACKS; t++) ntr += has_notes(t);
    smf_byte('M'); smf_byte('T'); smf_byte('h'); smf_byte('d');
    smf_32(6);
    smf_byte(0); smf_byte(1);                       /* type 1 */
    smf_byte(0); smf_byte((uint8_t)ntr);
    smf_byte(0); smf_byte(SONG_PPQ);
    smf_chunk(-1);
    for (int t = 0; t < SONG_TRACKS; t++) if (has_notes(t)) smf_chunk(t);
}

int song_export(void)
{
    const char *path = files_song_path();
    if (!path) return ERR_NO_ED;
    seq_stop();
    live_set(0);
    smf_writing = 0;
    smf_count = 0;
    smf_all();
    uint32_t total = smf_count;
    files_song_mkdir();
    int e = ed_stream_open(path, total);
    if (e) return e;
    smf_writing = 1;
    smf_err = 0;
    smf_fill = 0;
    smf_count = 0;
    smf_all();
    if (smf_fill && !smf_err) smf_err = ed_stream_put(SMF_BUF, (int)smf_fill);
    smf_writing = 0;
    e = ed_stream_close();
    for (unsigned i = 0; i < 256; i++) kw8(i, 0);  /* (WORK held the ends: clear) */
    return smf_err ? smf_err : e;
}

/* ------------------------------------------------------------ the commands */

void seq_init(void)
{
    for (unsigned i = 0; i < 256; i++) kw8(i, 0);
    song_new();
}

static void put6(unsigned o)
{
    uint8_t b[6];
    for (int k = 0; k < 6; k++) b[k] = o + (unsigned)k < SONG_BYTES ? (uint8_t)p8(o + (unsigned)k) : 0;
    COMM16(C_P1) = (uint16_t)(b[0] << 8 | b[1]);
    COMM16(C_P2) = (uint16_t)(b[2] << 8 | b[3]);
    COMM16(C_P3) = (uint16_t)(b[4] << 8 | b[5]);
}

static void ev_from_buf(ev_t *e)        /* track, tick (3), note, velocity, length (2) */
{
    const uint8_t *d = file_buf + BUF_DATA;
    e->track = d[0] & 15;
    e->tick = ((uint32_t)d[1] << 16 | (uint32_t)d[2] << 8 | d[3]) & 0xFFFFF;
    e->note = d[4] & 127;
    e->vel = d[5] & 127;
    if (!e->vel) e->vel = 1;
    e->dur = (uint16_t)(d[6] << 8 | d[7]);
    if (!e->dur) e->dur = 1;
    e->qoff = 0;
}

static void notes_query(int t, uint32_t t0, uint32_t t1)
{
    uint8_t *d = file_buf + BUF_DATA + 2;
    unsigned cnt = 0, n = nev();
    ev_t e;
    for (unsigned i = seek(t0 > 6144 ? t0 - 6144 : 0); i < n && cnt < NOTES_MAX; i++) {
        unsigned a = ev_addr(i);
        if (tick_at(POOL, a) >= t1) break;
        ev_rd(POOL, a, &e);
        if (e.track != t || e.tick + e.dur <= t0) continue;
        d[0] = (uint8_t)(i >> 8); d[1] = (uint8_t)i;
        d[2] = (uint8_t)(e.tick >> 16); d[3] = (uint8_t)(e.tick >> 8); d[4] = (uint8_t)e.tick;
        d[5] = e.note; d[6] = e.vel; d[7] = (uint8_t)(e.dur >> 8); d[8] = (uint8_t)e.dur;
        d += NOTE_REC;
        cnt++;
    }
    for (unsigned i = 0; i < ntake && cnt < NOTES_MAX; i++) {
        ev_rd(TAKE, i * EV_BYTES, &e);
        if (e.track != t || e.tick >= t1 || e.tick + e.dur <= t0) continue;
        d[0] = (uint8_t)(0x80 | i >> 8); d[1] = (uint8_t)i;
        d[2] = (uint8_t)(e.tick >> 16); d[3] = (uint8_t)(e.tick >> 8); d[4] = (uint8_t)e.tick;
        d[5] = e.note; d[6] = (uint8_t)(e.vel | 0x80); d[7] = (uint8_t)(e.dur >> 8); d[8] = (uint8_t)e.dur;
        d += NOTE_REC;
        cnt++;
    }
    file_buf[BUF_DATA] = (uint8_t)(cnt >> 8);
    file_buf[BUF_DATA + 1] = (uint8_t)cnt;
}

static void mark(uint8_t *d, const ev_t *e, uint32_t t0, uint32_t col)
{
    uint32_t a = e->tick, b = e->tick + (e->dur ? e->dur : 1);
    if (b <= t0) return;
    uint32_t c0 = a > t0 ? (a - t0) / col : 0, c1 = (b - 1 - t0) / col;
    for (uint32_t c = c0; c <= c1 && c < 32; c++) d[e->track * 4 + c / 8] |= (uint8_t)(1 << (c & 7));
}

static void overview(uint32_t t0, uint32_t col)
{
    uint8_t *d = file_buf + BUF_DATA;
    uint32_t t1 = t0 + col * 32;
    for (int i = 0; i < SONG_TRACKS * 4; i++) d[i] = 0;
    unsigned n = nev();
    ev_t e;
    for (unsigned i = seek(t0 > 6144 ? t0 - 6144 : 0); i < n; i++) {
        unsigned a = ev_addr(i);
        if (tick_at(POOL, a) >= t1) break;
        ev_rd(POOL, a, &e);
        mark(d, &e, t0, col);
    }
    for (unsigned i = 0; i < ntake; i++) {
        ev_rd(TAKE, i * EV_BYTES, &e);
        if (e.tick < t1) mark(d, &e, t0, col);
    }
}

static void after_edit(void)            /* (playing: the next note again) */
{
    if (seq_flags & SQS_PLAY) pi = seek(now);
}

unsigned seq_command(uint16_t p1, uint16_t p2, uint16_t p3)
{
    int op = p1 >> 8, a = p1 & 0xFF;
    switch (op) {
    case SQ_STATE:
        COMM16(C_P1) = (uint16_t)(now >> 16);
        COMM16(C_P2) = (uint16_t)now;
        COMM16(C_P3) = edits;
        return (unsigned)(seq_flags | (pre_ticks ? SQS_COUNTIN : 0) | (full ? SQS_FULL : 0));
    case SQ_PEEK:
        put6(p2);
        return 0;
    case SQ_POKE:
        if (p2 < SO_FLAGS || p2 + (unsigned)a > SO_SOUNDS || (p2 < SO_TRACKS && p2 + (unsigned)a > SO_NEV))
            return ERR_RANGE;
        if (a == 2) w16(p2, p3); else w8(p2, p3 & 0xFF);
        if (p2 >= SO_TRACKS) voices_forget();
        tempo_set();
        edits++;
        return 0;
    case SQ_PLAY:
        seq_play(p3 & 1);
        return 0;
    case SQ_STOP:
        seq_stop();
        return 0;
    case SQ_LOCATE:
        seq_locate((uint32_t)a << 16 | p2);
        return 0;
    case SQ_LIVE:
        live_set(p2 != 0);
        return 0;
    case SQ_NEW:
        seq_stop();
        song_new();
        now = 0;
        return 0;
    case SQ_SOUND:
        if (a >= SONG_TRACKS) return ERR_RANGE;
        if (p2 == 0xFFFF) return (unsigned)song_sound_get(a, file_buf + BUF_DATA);
        return (unsigned)song_sound_set(a, file_buf + BUF_DATA, p2);
    case SQ_NOTES:
        notes_query(a & 15, (uint32_t)p2 * 24, (uint32_t)p2 * 24 + (uint32_t)p3 * 24);
        return 0;
    case SQ_OVERVIEW:
        overview((uint32_t)p2 * 24, p3 ? (uint32_t)p3 * 24 : 24);
        return 0;
    case SQ_ADD: {
        ev_t e;
        ev_from_buf(&e);
        int i = song_add(&e);
        if (i < 0) return ERR_TOO_BIG;
        COMM16(C_P1) = (uint16_t)i;
        after_edit();
        return 0;
    }
    case SQ_SET: {
        if (p2 >= nev()) return ERR_RANGE;
        ev_t e;
        ev_from_buf(&e);
        song_del(p2);
        int i = song_add(&e);
        COMM16(C_P1) = (uint16_t)(i < 0 ? 0 : i);
        after_edit();
        return i < 0 ? ERR_TOO_BIG : 0;
    }
    case SQ_DEL:
        song_del(p2);
        after_edit();
        return 0;
    case SQ_QUANT:
        if (a >= SONG_TRACKS) return ERR_RANGE;
        if (seq_flags & SQS_PLAY) take_merge();
        song_quantize(a, p2);
        after_edit();
        return 0;
    case SQ_UNDO:
        seq_stop();
        undo_take();
        return 0;
    case SQ_CLEAR:
        if (a >= SONG_TRACKS) return ERR_RANGE;
        if (seq_flags & SQS_PLAY) take_merge();
        song_clear(a);
        after_edit();
        return 0;
    case SQ_SAVE:
        return (unsigned)song_save();
    case SQ_LOAD:
        return (unsigned)song_load();
    case SQ_EXPORT:
        return (unsigned)song_export();
    case SQ_FREE: {
        unsigned used = used_bytes();
        COMM16(C_P1) = (uint16_t)((SONG_BYTES - used) / EV_BYTES);
        COMM16(C_P2) = (uint16_t)nev();
        return 0;
    }
    }
    return ERR_RANGE;
}

#ifdef MD_HOST
/* (tests/host: the pool itself) */
uint8_t *song_pool(void) { return (uint8_t *)POOL; }
unsigned song_used(void) { return used_bytes(); }
unsigned song_ntake(void) { return ntake; }
uint32_t seq_now(void) { return now; }
void seq_advance_us(uint32_t us) { advance(us); }
int seq_voice(int v, int *track, int *note) { *track = v_track[v]; *note = v_note[v]; return v_on[v]; }
#endif
