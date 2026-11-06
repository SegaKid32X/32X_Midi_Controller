/* synth.c - the 32X's own voices, through its PWM: the drum kit (and, next,
 * software FM).
 *
 * MIDI channels 11-16 come here instead of the Mega Drive's chips: the
 * 68000's engine queues their messages, the master SH-2 fetches them
 * (synth_poll: cheap while there are none, the queue's head is in C_FLAGS)
 * and hands them to the slave SH-2 through a ring in SDRAM. The slave mixes
 * the voices at 22050 Hz and keeps the PWM's ring a few blocks ahead
 * (audio.c), so a note sounds about 15 ms after it arrives.
 *
 * Drums: MIDI channel 16. WAV files from the SD card's MIDI_DRUMS folder,
 * copied by the EverDrive into cartridge memory and played from there (8 or
 * 16 bits, mono or stereo, any rate). One-shot; a closed or pedal hi-hat
 * stops an open one.
 */
#include <string.h>
#include "hw.h"
#include "audio.h"
#include "synth.h"

#define RATE        22050
#define BLOCK       64
#define AHEAD       320         /* samples kept queued (about 15 ms) */
#define DRUM_CH     15          /* MIDI channel 16 */
#define NVOICE      12

typedef struct {
    uint32_t data;              /* the sample frames (a cached cartridge address) */
    uint32_t frames;
    uint32_t step;              /* frames per output sample, 20.12 */
    uint8_t  bits, chans, note, ok;
} sample_t;

typedef struct {
    volatile uint8_t q[256][3];
    volatile uint32_t head;     /* master */
    volatile uint32_t tail;     /* slave */
    sample_t smp[MAX_SAMPLES];
    volatile int nsmp;
    volatile int8_t by_note[128];
    volatile int volume;        /* 0-10 */
    volatile int reload;        /* the master changed the samples: purge the cache */
} shared_t;

static shared_t shared_storage __attribute__((aligned(16)));
#define SH ((shared_t *)SDRAM_THROUGH(&shared_storage))

char synth_names[MAX_SAMPLES][24];
int synth_count;

/* ------------------------------------------------------------ the master */

static uint8_t ev_tail;
static int polling;

void synth_poll(void)
{
    if (polling) return;
    polling = 1;
    while ((uint8_t)(hw_flags() >> 8) != ev_tail) {
        int n = md_cmd(CMD_EVENTS, ev_tail, 0, 0);
        uint16_t w[3] = { md_p(C_P1), md_p(C_P2), md_p(C_P3) };
        uint8_t b[6];
        for (int k = 0; k < 6; k++) b[k] = (uint8_t)(k & 1 ? w[k >> 1] : w[k >> 1] >> 8);
        shared_t *s = SH;
        for (int i = 0; i < n; i++) {
            uint32_t h = s->head;
            if (h - s->tail >= 256) break;          /* (full: the slave is stuck) */
            for (int k = 0; k < 3; k++) s->q[h & 255][k] = b[i * 3 + k];
            s->head = h + 1;
        }
        ev_tail = (uint8_t)(ev_tail + n);
        if (!n) break;
    }
    polling = 0;
}

static uint32_t rd32le(const uint8_t *p) { return p[0] | p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16le(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

/* the WAV's format and its data, from cartridge memory */
static int parse_wav(uint32_t cart, uint32_t size, sample_t *s)
{
    const uint8_t *p = (const uint8_t *)(0x22000000u + cart);
    if (size < 44 || memcmp(p, "RIFF", 4) || memcmp(p + 8, "WAVE", 4)) return -1;
    uint32_t at = 12, rate = 0;
    int have_fmt = 0;
    while (at + 8 <= size) {
        uint32_t len = rd32le(p + at + 4);
        if (!memcmp(p + at, "fmt ", 4) && len >= 16) {
            if (rd16le(p + at + 8) != 1) return -1;         /* PCM only */
            s->chans = (uint8_t)rd16le(p + at + 10);
            rate = rd32le(p + at + 12);
            s->bits = (uint8_t)rd16le(p + at + 22);
            have_fmt = 1;
        } else if (!memcmp(p + at, "data", 4) && have_fmt) {
            if ((s->bits != 8 && s->bits != 16) || s->chans < 1 || s->chans > 2 || !rate) return -1;
            if (len > size - at - 8) len = size - at - 8;
            s->data = 0x02000000u + cart + at + 8;
            s->frames = len / (s->chans * (s->bits / 8));
            s->step = (uint32_t)(((uint64_t)rate << 12) / RATE);
            s->ok = 1;
            return 0;
        }
        at += 8 + ((len + 1) & ~1u);
    }
    return -1;
}

int synth_load_drums(const char *kit)
{
    shared_t *sh = SH;
    sh->nsmp = 0;
    for (int i = 0; i < 128; i++) sh->by_note[i] = -1;
    synth_count = 0;
    /* the kit's folder (inside MIDI_DRUMS) */
    char k[64];
    memset(k, 0, sizeof k);
    strncpy(k, kit ? kit : "", sizeof k - 1);
    for (int off = 0; off < 64; off += 4)
        md_cmd(CMD_BUF_PUT, (uint16_t)(BUF_NAME + off), (uint16_t)(k[off] << 8 | (uint8_t)k[off + 1]),
               (uint16_t)((uint8_t)k[off + 2] << 8 | (uint8_t)k[off + 3]));
    int n = md_cmd(CMD_DRUMS, 0, 0, 0);
    if (n > MAX_SAMPLES) n = MAX_SAMPLES;
    for (int i = 0; i < n; i++) {
        md_cmd(CMD_DRUM, (uint16_t)i, 0xFFFF, 0);
        uint16_t w1 = md_p(C_P1);
        uint8_t note = (uint8_t)(w1 >> 8);
        uint32_t cart = (uint32_t)(w1 & 0xFF) << 16 | md_p(C_P2), size = (uint32_t)md_p(C_P3) << 6;
        int len = 0;
        for (int off = 0; off < 22; off += 6) {
            int total = md_cmd(CMD_DRUM, (uint16_t)i, (uint16_t)off, 0);
            uint16_t w[3] = { md_p(C_P1), md_p(C_P2), md_p(C_P3) };
            for (int b = 0; b < 6 && off + b < total && len < 23; b++)
                synth_names[synth_count][len++] = (char)(b & 1 ? w[b >> 1] : w[b >> 1] >> 8);
            if (off + 6 >= total) break;
        }
        synth_names[synth_count][len] = 0;
        sample_t s;
        memset(&s, 0, sizeof s);
        s.note = note;
        if (parse_wav(cart, size, &s)) continue;
        sh->smp[synth_count] = s;
        sh->by_note[note] = (int8_t)synth_count;
        synth_count++;
    }
    sh->nsmp = synth_count;
    sh->reload = 1;
    return synth_count;
}

int synth_note_of(int i)
{
    return i >= 0 && i < synth_count ? SH->smp[i].note : -1;
}

void synth_set_volume(int v)
{
    SH->volume = v < 0 ? 0 : v > 10 ? 10 : v;
}

/* ------------------------------------------------------------ the slave */

typedef struct {
    int smp;                    /* -1: free */
    uint32_t pos;               /* frames, 20.12 */
    int gain_l, gain_r;         /* 0-4096 */
    uint8_t note;
} voice_t;

static voice_t voice[NVOICE];
static uint8_t ch_volume[16], ch_pan[16];
static int16_t mix[BLOCK * 2];

static void drum_on(int note, int vel)
{
    shared_t *sh = SH;
    int si = sh->by_note[note & 127];
    if (si < 0) return;
    if (note == 42 || note == 44)                   /* a closed hat stops the open one */
        for (int i = 0; i < NVOICE; i++)
            if (voice[i].smp >= 0 && voice[i].note == 46) voice[i].smp = -1;
    int v = -1;
    for (int i = 0; i < NVOICE; i++)                /* the same drum again: restart it */
        if (voice[i].smp >= 0 && voice[i].note == note) v = i;
    for (int i = 0; i < NVOICE && v < 0; i++)
        if (voice[i].smp < 0) v = i;
    if (v < 0) {                                    /* else the one furthest along */
        uint32_t far = 0;
        for (int i = 0; i < NVOICE; i++)
            if (voice[i].pos >= far) { far = voice[i].pos; v = i; }
    }
    int g = vel * vel * ch_volume[DRUM_CH] / 127 * 4096 / (127 * 127);   /* 0-4096 */
    int pan = ch_pan[DRUM_CH];                      /* 0 left .. 127 right */
    voice[v].gain_l = g * (pan < 64 ? 64 : 127 - pan) / 64;
    voice[v].gain_r = g * (pan > 64 ? 64 : pan) / 64;
    voice[v].note = (uint8_t)note;
    voice[v].pos = 0;
    voice[v].smp = si;
}

static void event(const volatile uint8_t *e)
{
    int st = e[0] & 0xF0, ch = e[0] & 15;
    if (st == 0x90 && e[2] && ch == DRUM_CH) drum_on(e[1], e[2]);
    else if (st == 0xB0) {
        if (e[1] == 7) ch_volume[ch] = e[2];
        else if (e[1] == 10) ch_pan[ch] = e[2];
        else if (e[1] == 120 || e[1] == 123)
            for (int i = 0; i < NVOICE; i++) voice[i].smp = -1;
    }
}

static void render(void)
{
    shared_t *sh = SH;
    int32_t acc[BLOCK * 2];
    memset(acc, 0, sizeof acc);
    for (int i = 0; i < NVOICE; i++) {
        voice_t *v = &voice[i];
        if (v->smp < 0) continue;
        const sample_t *s = &sh->smp[v->smp];
        const uint8_t *d = (const uint8_t *)s->data;
        uint32_t pos = v->pos, step = s->step, end = s->frames << 12;
        int gl = v->gain_l, gr = v->gain_r, frame = (s->bits / 8) * s->chans;
        for (int k = 0; k < BLOCK; k++) {
            if (pos >= end) { v->smp = -1; break; }
            const uint8_t *f = d + (pos >> 12) * frame;
            int x;
            if (s->bits == 16) {
                x = (int16_t)(f[0] | f[1] << 8);
                if (s->chans == 2) x = (x + (int16_t)(f[2] | f[3] << 8)) >> 1;
            } else {
                x = (f[0] - 128) << 8;
                if (s->chans == 2) x = (x + ((f[1] - 128) << 8)) >> 1;
            }
            acc[2 * k] += x * gl >> 12;
            acc[2 * k + 1] += x * gr >> 12;
            pos += step;
        }
        v->pos = pos;
    }
    int vol = sh->volume;                           /* 0-10 */
    for (int k = 0; k < BLOCK * 2; k++) {
        int32_t x = acc[k] * vol / 10;
        mix[k] = (int16_t)(x > 32767 ? 32767 : x < -32768 ? -32768 : x);
    }
}

static void synth_job(int arg)
{
    (void)arg;
    shared_t *sh = SH;
    for (int i = 0; i < NVOICE; i++) voice[i].smp = -1;
    for (int c = 0; c < 16; c++) { ch_volume[c] = 100; ch_pan[c] = 64; }
    for (;;) {
        if (sh->reload) {                           /* new samples in the cartridge */
            sh->reload = 0;
            for (int i = 0; i < NVOICE; i++) voice[i].smp = -1;
            hw_cache_purge();
        }
        while (sh->tail != sh->head) {
            event(sh->q[sh->tail & 255]);
            sh->tail = sh->tail + 1;
        }
        if (audio_queued() < AHEAD) {
            render();
            audio_write(mix, BLOCK);
        }
    }
}

void synth_start(void)
{
    SH->volume = 8;
    SH->head = SH->tail = 0;
    for (int i = 0; i < 128; i++) SH->by_note[i] = -1;
    audio_init(RATE);
    audio_pause(0);
    hw_slave_start(synth_job, 0);                   /* (it runs for good) */
}
