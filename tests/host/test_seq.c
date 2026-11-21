/* test_seq.c - the song and the sequencer (src/md/md_seq.c) on the PC:
 * tracks and their sounds, notes in time order, playing (the voices each
 * note gets, their timing, the sounds loaded), MIDI out and its clock,
 * recording from the keyboard (count-in, click, pedal, the cycle), undo,
 * quantize and putting it back, editing, files, the standard MIDI file. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "md.h"
#include "song.h"

/* ---- what md_seq.c calls ---- */
uint8_t host_pad2_data, host_pad2_ctrl, host_pad1;
uint16_t host_vdp, host_comm[8];
uint16_t md_flags;
midi_settings_t ms = { MODE_GENMDM, 0, 0, 1, 0, 1 };
uint8_t file_buf[BUF_SIZE];
void set_flag(uint16_t f, int on) { md_flags = on ? (md_flags | f) : (md_flags & (uint16_t)~f); }
void rx_poll(void) {}

typedef struct { uint8_t st, a, b; uint32_t t; } msg_t;
static msg_t sent[8192];
static int nsent;
static uint32_t now_us;
void midi_play(uint8_t st, uint8_t a, uint8_t b)
{
    if (nsent < 8192) { sent[nsent].st = st; sent[nsent].a = a; sent[nsent].b = b; sent[nsent].t = now_us; nsent++; }
}
static uint8_t outb[16384];
static int nout;
void midi_out(uint8_t b) { if (nout < 16384) outb[nout++] = b; }
static int timer_on, timer_flag;
void ym_timer_run(int on) { timer_on = on; }
int ym_timer_tick(void) { int f = timer_flag; timer_flag = 0; return f; }
static uint8_t ymreg[2][256];
static int nym;
void ym_write(int part, uint8_t reg, uint8_t val) { ymreg[part][reg] = val; nym++; }
static int nclick;
void psg_write(uint8_t b) { if (b == 0xE4 || b == 0xE5) nclick++; }
static uint8_t psg_inst[4][160];
static int psg_len[4];
int midi_psg_set(int c, const uint8_t *in, int len) { memcpy(psg_inst[c], in, (size_t)len); psg_len[c] = len; return 0; }

/* the "SD card": one file */
static uint8_t disk[65536];
static uint32_t disk_len;
static char path_buf[64];
const char *files_song_path(void) { strcpy(path_buf, "/MIDI_SONGS/T.SNG"); return path_buf; }
void files_song_mkdir(void) {}
int files_song_read(uint8_t *dst, int max, uint16_t *len)
{
    if (disk_len > (uint32_t)max) return ERR_TOO_BIG;
    memcpy(dst, disk, disk_len);
    *len = (uint16_t)disk_len;
    return 0;
}
int ed_write_file(const char *p, const uint8_t *src, int len) { (void)p; memcpy(disk, src, (size_t)len); disk_len = (uint32_t)len; return 0; }
static uint32_t stream_want;
int ed_stream_open(const char *p, uint32_t len) { (void)p; disk_len = 0; stream_want = len; return 0; }
int ed_stream_put(const uint8_t *src, int len) { memcpy(disk + disk_len, src, (size_t)len); disk_len += (uint32_t)len; return 0; }
int ed_stream_close(void) { return disk_len == stream_want ? 0 : 99; }

/* md_seq.c's test hooks */
uint8_t *song_pool(void);
unsigned song_used(void);
unsigned song_ntake(void);
uint32_t seq_now(void);
int seq_voice(int v, int *track, int *note);
typedef struct { uint32_t tick; uint16_t dur; uint8_t track, note, vel; int8_t qoff; } ev_t;
int song_note(unsigned i, ev_t *e);
unsigned song_count(void);

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static unsigned cmd(int op, int a, unsigned p2, unsigned p3) { return seq_command((uint16_t)(op << 8 | a), (uint16_t)p2, (uint16_t)p3); }
static void run_ms(int ms_) { for (int i = 0; i < ms_ * 1000 / 995; i++) { now_us += 995; timer_flag = 1; seq_poll(); if (i % 16 == 15) seq_frame(); } }
static void live(int st, int a, int b) { if (!seq_live((uint8_t)st, (uint8_t)a, (uint8_t)b)) midi_play((uint8_t)st, (uint8_t)a, (uint8_t)b); }
static void poke(int off, int n, int v) { cmd(SQ_POKE, n, (unsigned)off, (unsigned)v); }

static int add(int t, uint32_t tick, int note, int vel, int dur)
{
    uint8_t *d = file_buf + BUF_DATA;
    d[0] = (uint8_t)t; d[1] = (uint8_t)(tick >> 16); d[2] = (uint8_t)(tick >> 8); d[3] = (uint8_t)tick;
    d[4] = (uint8_t)note; d[5] = (uint8_t)vel; d[6] = (uint8_t)(dur >> 8); d[7] = (uint8_t)dur;
    return cmd(SQ_ADD, 0, 0, 0) ? -1 : host_comm[C_P1 >> 1];
}

static int count_msg(int st, int note)
{
    int n = 0;
    for (int i = 0; i < nsent; i++) if (sent[i].st == st && sent[i].a == note && ((st & 0xF0) != 0x90 || sent[i].b)) n++;
    return n;
}

static const msg_t *find(int st, int note, int nth)
{
    for (int i = 0; i < nsent; i++)
        if (sent[i].st == st && sent[i].a == note && ((st & 0xF0) != 0x90 || sent[i].b) && nth-- == 0) return &sent[i];
    return 0;
}

static const msg_t *find_fm(int note, int nth)  /* on any YM2612 channel */
{
    for (int i = 0; i < nsent; i++)
        if ((sent[i].st & 0xF0) == 0x90 && (sent[i].st & 15) < 6 && sent[i].a == note && sent[i].b && nth-- == 0) return &sent[i];
    return 0;
}

static int count_fm(int note)
{
    int n = 0;
    while (find_fm(note, n)) n++;
    return n;
}

static void reset_log(void) { nsent = 0; nout = 0; now_us = 0; nym = 0; nclick = 0; }

static void fm_sound(int t, int alg)    /* a made-up FM sound: register bytes */
{
    uint8_t *d = file_buf + BUF_DATA;
    for (int i = 0; i < FM_SOUND; i++) d[i] = (uint8_t)(0x10 + i + t);
    d[28] = (uint8_t)alg;
    cmd(SQ_SOUND, t, FM_SOUND, 0);
}

int main(void)
{
    seq_init();
    uint8_t *pool = song_pool();

    /* ---- a new song ---- */
    CHECK(!memcmp(pool, "MSNG", 4) && pool[SO_VER] == SONG_VERSION, "the magic, version 2");
    CHECK((pool[SO_BPM] << 8 | pool[SO_BPM + 1]) == 120 && pool[SO_BEATS] == 4, "120 BPM, 4 beats a bar");
    CHECK(pool[SO_TRACKS + TR_KIND] == TK_FM && pool[SO_TRACKS + 4 * TR_BYTES + TR_KIND] == TK_PSG
          && pool[SO_TRACKS + 6 * TR_BYTES + TR_KIND] == TK_X32 && pool[SO_TRACKS + 6 * TR_BYTES + TR_CH] == 10
          && pool[SO_TRACKS + 10 * TR_BYTES + TR_KIND] == TK_DRUMS, "the default tracks");
    CHECK(song_used() == SO_SOUNDS + SONG_TRACKS, "no sounds, no notes (%u)", song_used());

    /* ---- sounds ---- */
    fm_sound(0, 4);
    fm_sound(1, 7);
    uint8_t psg[20] = { 0, 3, 0, 15, 10, 5, 0, 0, 0, 0 };
    memcpy(file_buf + BUF_DATA, psg, 20);
    cmd(SQ_SOUND, 4, 20, 0);
    memset(file_buf + BUF_DATA, 0, 64);
    CHECK(cmd(SQ_SOUND, 1, 0xFFFF, 0) == FM_SOUND && file_buf[BUF_DATA] == 0x11 && file_buf[BUF_DATA + 28] == 7, "track 2's sound back");
    CHECK(cmd(SQ_SOUND, 4, 0xFFFF, 0) == 20 && file_buf[BUF_DATA + 3] == 15, "the PSG track's sound back");
    CHECK(cmd(SQ_SOUND, 2, 0xFFFF, 0) == 0, "track 3: none");

    /* ---- notes in time order ---- */
    add(0, 192, 64, 100, 48);
    add(0, 0, 60, 100, 96);
    add(1, 96, 40, 90, 24);
    add(0, 0, 67, 80, 96);
    CHECK(song_count() == 4, "4 notes");
    ev_t e;
    uint32_t last = 0;
    int ordered = 1;
    for (unsigned i = 0; i < song_count(); i++) { song_note(i, &e); if (e.tick < last) ordered = 0; last = e.tick; }
    CHECK(ordered, "in time order");
    song_note(0, &e);
    CHECK(e.tick == 0 && e.note == 60 && e.dur == 96 && e.track == 0, "the first (added second)");
    cmd(SQ_NOTES, 0, 0, 16);            /* track 1, a bar */
    int cnt = file_buf[BUF_DATA] << 8 | file_buf[BUF_DATA + 1];
    CHECK(cnt == 3, "track 1 has 3 notes in bar 1 (%d)", cnt);
    cmd(SQ_OVERVIEW, 0, 0, 4);          /* columns of a beat */
    CHECK(file_buf[BUF_DATA] == 0x05 && file_buf[BUF_DATA + 4] == 0x02, "the overview: track 1 beats 1 and 3, track 2 beat 2 (%02X %02X)",
          file_buf[BUF_DATA], file_buf[BUF_DATA + 4]);

    /* ---- playing: a chord on one FM track takes several channels, each
     * loaded with the track's sound; timing at 120 BPM (500 ms a beat) ---- */
    reset_log();
    memset(ymreg, 0, sizeof ymreg);
    cmd(SQ_LOCATE, 0, 0, 0);
    cmd(SQ_PLAY, 0, 0, 0);
    CHECK(timer_on, "the timer runs");
    run_ms(5);
    const msg_t *c1 = find(0x90, 60, 0), *c2 = find(0x91, 67, 0);
    CHECK(c1 && c2, "the chord: on FM 1 and FM 2 (%d sent)", nsent);
    if (!c1 || !c2) { printf("%d failed (stopping)\n", fails); return 1; }
    CHECK(ymreg[0][0xB0] == 4 && ymreg[0][0xB1] == 4 && ymreg[0][0x30] == 0x10 && ymreg[0][0x31] == 0x10,
          "both channels have track 1's sound");
    run_ms(500);
    const msg_t *b = find(0x92, 40, 0);
    CHECK(b && b->t >= 495000 && b->t <= 510000, "track 2 a beat later, on FM 3 (%u)", b ? b->t : 0);
    CHECK(ymreg[0][0xB2] == 7, "with track 2's sound");
    CHECK(find(0x80, 60, 0) && find(0x80, 60, 0)->t >= 495000 && find(0x80, 60, 0)->t <= 510000, "the chord ends after its beat");
    run_ms(500);
    const msg_t *bo = find(0x82, 40, 0);
    CHECK(bo && bo->t - b->t >= 120000 && bo->t - b->t <= 130000, "a 16th: 125 ms (%d)", bo ? (int)(bo->t - b->t) : -1);
    const msg_t *n3 = find(0x90, 64, 0);
    CHECK(n3, "beat 3 on a free channel that has the sound (FM 1)");
    int clocks = 0, fa = 0;
    for (int i = 0; i < nout; i++) { if (outb[i] == 0xF8) clocks++; if (outb[i] == 0xFA) fa = 1; }
    CHECK(fa && clocks >= 47 && clocks <= 50, "MIDI out: start, then 24 clocks a beat (%d in ~1 s)", clocks);
    int outn = 0;
    for (int i = 0; i + 2 < nout; i++) if (outb[i] == 0x91 && outb[i + 1] == 40) outn++;
    CHECK(outn == 1, "MIDI out: track 2's note on its channel (2)");
    cmd(SQ_STOP, 0, 0, 0);
    CHECK(!timer_on && outb[nout - 1] == 0xFC, "stopped: the timer off, MIDI stop");

    /* ---- a big chord: 6 FM channels, the 7th note takes the oldest ---- */
    cmd(SQ_NEW, 0, 0, 0);
    fm_sound(0, 1);
    for (int k = 0; k < 7; k++) add(0, (uint32_t)k, 60 + k, 100, 400);
    reset_log();
    cmd(SQ_PLAY, 0, 0, 0);
    run_ms(60);
    int t, n, on = 0, has66 = 0, has60 = 0;
    for (int v = 0; v < 6; v++) if (seq_voice(v, &t, &n)) { on++; has66 |= n == 66; has60 |= n == 60; }
    CHECK(on == 6 && has66 && !has60, "six sound: the 7th took the oldest's channel (%d)", on);
    int off60 = 0;
    for (int i = 0; i < nsent; i++) if ((sent[i].st & 0xF0) == 0x80 && sent[i].a == 60) off60++;
    CHECK(off60 == 1, "the first note gave way");
    cmd(SQ_STOP, 0, 0, 0);
    on = 0;
    for (int v = 0; v < 10; v++) on += seq_voice(v, &t, &n);
    CHECK(on == 0, "stopped: all off");

    /* ---- PSG, the 32X, drums, MIDI-only, mute ---- */
    cmd(SQ_NEW, 0, 0, 0);
    memcpy(file_buf + BUF_DATA, psg, 20);
    cmd(SQ_SOUND, 4, 20, 0);
    add(4, 0, 72, 100, 48); add(4, 0, 76, 100, 48); add(4, 0, 79, 100, 48);
    add(6, 0, 48, 100, 48);             /* 32X CH 11 */
    add(10, 0, 36, 127, 12);            /* drums */
    add(14, 0, 50, 100, 12);            /* MIDI only (CH 15) */
    add(2, 0, 55, 100, 12);
    poke(SO_TRACKS + 2 * TR_BYTES + TR_FLAGS, 1, TF_MUTE);
    reset_log();
    memset(psg_len, 0, sizeof psg_len);
    cmd(SQ_PLAY, 0, 0, 0);
    run_ms(10);
    CHECK(find(0x96, 72, 0) && find(0x97, 76, 0) && find(0x98, 79, 0), "the PSG chord on PSG 1-3");
    CHECK(psg_len[0] == 20 && psg_len[1] == 20 && psg_len[2] == 20, "each with the track's instrument");
    CHECK(find(0x9A, 48, 0) && find(0x9F, 36, 0), "the 32X: CH 11, the drums CH 16");
    CHECK(!find(0x9E, 50, 0), "MIDI only: nothing here");
    int o15 = 0;
    for (int i = 0; i + 1 < nout; i++) if (outb[i] == 0x9E && outb[i + 1] == 50) o15++;
    CHECK(o15 == 1, "MIDI only: out on CH 15");
    CHECK(count_msg(0x90, 55) + count_msg(0x91, 55) + count_msg(0x92, 55) == 0, "muted: silent");
    cmd(SQ_STOP, 0, 0, 0);

    /* ---- recording: a bar of clicks first, then notes as played ---- */
    cmd(SQ_NEW, 0, 0, 0);
    fm_sound(0, 2);
    cmd(SQ_LIVE, 0, 1, 0);
    reset_log();
    cmd(SQ_PLAY, 0, 0, 1);
    CHECK(cmd(SQ_STATE, 0, 0, 0) & SQS_COUNTIN, "counting in");
    run_ms(1000);
    live(0x90, 60, 100);                /* during the count-in: heard, not recorded */
    live(0x80, 60, 0);
    run_ms(1000);
    CHECK(nclick == 4 && !(cmd(SQ_STATE, 0, 0, 0) & SQS_COUNTIN), "4 clicks, then recording (%d)", nclick);
    run_ms(100);
    live(0x90, 62, 90);
    CHECK(find_fm(62, 0) != 0, "the key sounds at once");
    run_ms(250);
    live(0x80, 62, 0);
    live(0x90, 64, 80);
    live(0x90, 67, 80);                 /* a chord: two channels */
    CHECK(find_fm(64, 0) && find_fm(67, 0) && (find_fm(64, 0)->st != find_fm(67, 0)->st), "the chord on two channels");
    int c64 = find_fm(64, 0)->st & 15, c67 = find_fm(67, 0)->st & 15;
    live(0xB0, 64, 127);                /* the pedal */
    run_ms(100);
    live(0x80, 64, 0);
    live(0x80, 67, 0);
    CHECK(!find(0x80 | c64, 64, 0), "the pedal holds it");
    run_ms(200);
    live(0xB0, 64, 0);
    CHECK(find(0x80 | c64, 64, 0) && find(0x80 | c67, 67, 0), "the pedal up: they end");
    CHECK(song_ntake() == 3, "3 notes in the take (%u)", song_ntake());
    cmd(SQ_STOP, 0, 0, 0);
    CHECK(song_count() == 3 && song_ntake() == 0, "merged (%u)", song_count());
    song_note(0, &e);
    CHECK(e.note == 62 && e.tick >= 18 && e.tick <= 21 && e.dur >= 46 && e.dur <= 50 && (e.vel & 0x80),
          "62 at ~100 ms (tick %u), 250 ms long (%u), marked", e.tick, e.dur);
    song_note(1, &e);
    CHECK(e.dur >= 55 && e.dur <= 60, "held by the pedal: 300 ms (%u)", e.dur);

    /* ---- undo the take, record another over a cycle (it plays the next time round) ---- */
    cmd(SQ_UNDO, 0, 0, 0);
    CHECK(song_count() == 0, "undone");
    add(0, 0, 48, 100, 24);             /* (a note of the song, not of the take) */
    poke(SO_FLAGS, 1, SF_CYCLE);        /* no click, no count-in */
    poke(SO_CYC_A, 2, 0);
    poke(SO_CYC_B, 2, 1);               /* the first bar: 2 s */
    reset_log();
    cmd(SQ_LOCATE, 0, 0, 0);
    cmd(SQ_PLAY, 0, 0, 1);
    run_ms(500);
    live(0x90, 70, 100);
    run_ms(100);
    live(0x80, 70, 0);
    int heard = count_fm(70);
    run_ms(1500);                       /* round again */
    run_ms(700);
    CHECK(count_fm(70) == heard + 1, "the take plays the next time round (%d, %d)", count_fm(70), heard);
    CHECK(count_fm(48) == 2, "the song's note twice (%d)", count_fm(48));
    CHECK(seq_now() < 384, "back in the first bar (%u)", seq_now());
    cmd(SQ_STOP, 0, 0, 0);
    CHECK(song_count() == 2, "the song's note and the take (%u)", song_count());
    cmd(SQ_UNDO, 0, 0, 0);
    CHECK(song_count() == 1, "undo: only the take goes");

    /* ---- quantize, and back ---- */
    cmd(SQ_NEW, 0, 0, 0);
    add(0, 5, 60, 100, 20); add(0, 50, 62, 100, 20); add(0, 100, 64, 100, 20); add(1, 7, 40, 100, 20);
    cmd(SQ_QUANT, 0, Q_16, 0);
    uint32_t tk[4];
    for (int i = 0; i < 4; i++) { song_note((unsigned)i, &e); tk[i] = e.tick | (uint32_t)e.track << 24; }
    CHECK(tk[0] == 0 && tk[1] == (7 | 1u << 24) && tk[2] == 48 && tk[3] == 96, "to 16ths, track 2 left (%u %u %u %u)",
          tk[0], tk[1] & 0xFFFF, tk[2], tk[3]);
    cmd(SQ_QUANT, 0, Q_4, 0);
    uint32_t at62 = 0;
    for (unsigned i = 0; i < song_count(); i++) { song_note(i, &e); if (e.note == 62) at62 = e.tick; }
    CHECK(at62 == 96, "to beats: 50 -> 96 (from where it was played, not the 16th: %u)", at62);
    cmd(SQ_QUANT, 0, Q_OFF, 0);
    for (int i = 0; i < 4; i++) { song_note((unsigned)i, &e); tk[i] = e.tick; }
    CHECK(tk[0] == 5 && tk[1] == 7 && tk[2] == 50 && tk[3] == 100, "OFF: where they were played");

    /* ---- editing ---- */
    uint8_t *d = file_buf + BUF_DATA;
    d[0] = 0; d[1] = 0; d[2] = 1; d[3] = 0; d[4] = 65; d[5] = 70; d[6] = 0; d[7] = 30;
    cmd(SQ_SET, 0, 0, 0);               /* note 0 -> tick 256 */
    song_note(song_count() - 1, &e);
    CHECK(e.tick == 256 && e.note == 65 && e.dur == 30, "moved to the end");
    cmd(SQ_DEL, 0, 1, 0);               /* (the one at 50) */
    CHECK(song_count() == 3, "deleted");
    cmd(SQ_CLEAR, 0, 0, 0);
    CHECK(song_count() == 1, "track 1 cleared");

    /* ---- the song full ---- */
    cmd(SQ_NEW, 0, 0, 0);
    int k = 0;
    while (add(k & 15, (uint32_t)k * 3, 60, 100, 2) >= 0) k++;
    cmd(SQ_FREE, 0, 0, 0);
    CHECK(k > 3000 && host_comm[C_P1 >> 1] == 0, "full after %d notes", k);
    CHECK(cmd(SQ_STATE, 0, 0, 0) & SQS_FULL, "and says so");

    /* ---- files ---- */
    cmd(SQ_NEW, 0, 0, 0);
    fm_sound(3, 5);
    add(3, 10, 60, 100, 50); add(3, 10, 64, 90, 50); add(6, 96, 48, 80, 200); add(10, 0, 36, 127, 10);
    poke(SO_BPM, 2, 140);
    uint8_t saved[1024];
    unsigned used = song_used();
    memcpy(saved, pool, used);
    CHECK(cmd(SQ_SAVE, 0, 0, 0) == 0 && disk_len == used, "saved (%u bytes)", disk_len);
    cmd(SQ_NEW, 0, 0, 0);
    CHECK(cmd(SQ_LOAD, 0, 0, 0) == 0 && song_used() == used && !memcmp(pool, saved, used), "loaded back the same");
    disk[SO_VER] = 1;
    CHECK(cmd(SQ_LOAD, 0, 0, 0) == ERR_RANGE && song_count() == 0, "an old tracker song: refused");
    memcpy(disk, saved, used);
    disk_len = used - 3;
    CHECK(cmd(SQ_LOAD, 0, 0, 0) == ERR_RANGE, "cut short: refused");
    memcpy(pool, saved, used);
    disk_len = used;
    cmd(SQ_LOAD, 0, 0, 0);

    /* ---- the MIDI file: type 1, 96 a beat, a track each ---- */
    CHECK(cmd(SQ_EXPORT, 0, 0, 0) == 0, "exported (%u bytes)", disk_len);
    CHECK(!memcmp(disk, "MThd", 4) && disk[9] == 1 && disk[11] == 4 && disk[13] == 96, "type 1, 4 tracks, 96 a beat");
    uint32_t at = 14;
    int tracks = 0, ons = 0, offs = 0, ok = 1;
    while (at + 8 <= disk_len && !memcmp(disk + at, "MTrk", 4)) {
        uint32_t len = (uint32_t)disk[at + 4] << 24 | disk[at + 5] << 16 | disk[at + 6] << 8 | disk[at + 7];
        uint32_t p = at + 8, end = p + len, tick = 0;
        while (p < end) {
            uint32_t dt = 0;
            do dt = dt << 7 | (disk[p] & 0x7F); while (disk[p++] & 0x80);
            tick += dt;
            uint8_t s = disk[p++];
            if (s == 0xFF) { p++; uint32_t l = disk[p++]; p += l; continue; }
            uint8_t nn = disk[p++], v = disk[p++];
            if ((s & 0xF0) == 0x90 && v) {
                ons++;
                if (s == 0x93 && tick != 10) ok = 0;
                if (s == 0x9A && (tick != 96 || nn != 48)) ok = 0;
                if (s == 0x99 && nn != 36) ok = 0;      /* (drums: GM channel 10) */
            } else {
                offs++;
                if (s == 0x83 && tick != 60) ok = 0;
                if (s == 0x8A && tick != 296) ok = 0;
            }
        }
        if (p != end) ok = 0;
        tracks++;
        at = end;
    }
    CHECK(tracks == 4 && at == disk_len, "4 chunks, all of the file (%d)", tracks);
    CHECK(ons == 4 && offs == 4 && ok, "the notes on their channels at their ticks (%d on, %d off)", ons, offs);

    if (fails) { printf("%d failed\n", fails); return 1; }
    printf("ok   song: tracks, voices, playing, recording, cycle, undo, quantize, editing, files, MIDI file\n");
    return 0;
}
