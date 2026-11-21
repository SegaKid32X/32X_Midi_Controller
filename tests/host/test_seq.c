/* test_seq.c - the tracker's song and sequencer (src/md/md_seq.c) on the PC:
 * editing, the pattern tools, playing (the messages and their timing),
 * recording and step entry from the keyboard, the take's merge, files and
 * the standard MIDI file. */
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
static msg_t sent[4096];
static int nsent;
static uint32_t now_us;
void midi_play(uint8_t st, uint8_t a, uint8_t b)
{
    if (nsent < 4096) { sent[nsent].st = st; sent[nsent].a = a; sent[nsent].b = b; sent[nsent].t = now_us; nsent++; }
}
static uint8_t outb[8192];
static int nout;
void midi_out(uint8_t b) { if (nout < 8192) outb[nout++] = b; }
static int timer_on, timer_flag;
void ym_timer_run(int on) { timer_on = on; }
int ym_timer_tick(void) { int f = timer_flag; timer_flag = 0; return f; }

/* the "SD card": one file */
static uint8_t disk[65536];
static uint32_t disk_len;
static int have_ed = 1;
static char path_buf[64];
const char *files_song_path(void) { if (!have_ed) return 0; strcpy(path_buf, "/MIDI_SONGS/T.SNG"); return path_buf; }
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
void song_take_merge(void);
unsigned song_ntake(void);
int seq_pos(int *o, int *r);
void seq_advance_us(uint32_t us);

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static unsigned cmd(int op, int a, unsigned p2, unsigned p3) { return seq_command((uint16_t)(op << 8 | a), (uint16_t)p2, (uint16_t)p3); }
static void run_ms(int ms_) { for (int i = 0; i < ms_ * 1000 / 995; i++) { now_us += 995; timer_flag = 1; seq_poll(); } }
static void live(int st, int a, int b) { if (!seq_live((uint8_t)st, (uint8_t)a, (uint8_t)b)) midi_play((uint8_t)st, (uint8_t)a, (uint8_t)b); }
static int cell(int p, int r, int l) { int v; return song_cell_get(p, r, l, &v); }

static int count_on(int ch, int note)
{
    int n = 0;
    for (int i = 0; i < nsent; i++) if (sent[i].st == (0x90 | ch) && sent[i].a == note && sent[i].b) n++;
    return n;
}

int main(void)
{
    seq_init();
    uint8_t *pool = song_pool();
    CHECK(!memcmp(pool, "MSNG", 4) && song_used() == SO_EVENTS, "a new song: header only (%u)", song_used());
    CHECK((pool[SO_BPM] << 8 | pool[SO_BPM + 1]) == 120 && pool[SO_RPB] == 4, "120 BPM, 4 rows a beat");

    /* ---- editing ---- */
    CHECK(song_cell_set(0, 0, 0, 60, 100) == 0 && cell(0, 0, 0) == 60, "a note");
    CHECK(song_cell_set(1, 4, 2, 64, 90) == 0 && song_cell_set(0, 8, 0, 62, 100) == 0, "more");
    CHECK(cell(1, 4, 2) == 64 && cell(0, 8, 0) == 62 && song_used() == SO_EVENTS + 12, "kept apart: %u", song_used());
    song_cell_set(0, 0, 0, 67, 100);
    CHECK(cell(0, 0, 0) == 67 && song_used() == SO_EVENTS + 12, "replaced in place");
    song_cell_set(0, 8, 0, 0, 0);
    CHECK(cell(0, 8, 0) == 0 && cell(1, 4, 2) == 64 && song_used() == SO_EVENTS + 8, "cleared");
    song_cell_set(0, 8, 0, 62, 100);

    /* ---- instruments ---- */
    CHECK(song_path_set(3, "Bass/Slap.tfi") == 0 && song_path_set(15, "Kit") == 0 && song_path_set(0, "Lead.y12") == 0, "paths");
    char t[160];
    song_path_get(3, t, sizeof t); CHECK(!strcmp(t, "Bass/Slap.tfi"), "path 3: %s", t);
    song_path_get(15, t, sizeof t); CHECK(!strcmp(t, "Kit"), "path 15: %s", t);
    song_path_get(0, t, sizeof t); CHECK(!strcmp(t, "Lead.y12"), "path 0: %s", t);
    song_path_set(3, "");
    song_path_get(3, t, sizeof t); CHECK(!t[0], "path 3 cleared");
    song_path_get(15, t, sizeof t); CHECK(!strcmp(t, "Kit"), "path 15 kept: %s", t);
    char longp[200]; memset(longp, 'x', 150); longp[150] = 0;
    int fit = 0; for (int k = 1; k < 16; k++) if (song_path_set(k, longp) == 0) fit++;
    CHECK(fit < 15 && fit >= 5, "paths: as many as fit (%d)", fit);
    for (int k = 1; k < 16; k++) song_path_set(k, "");

    /* ---- pattern tools ---- */
    song_patop(PO_COPY, 0, 5, 0);
    CHECK(cell(5, 0, 0) == 67 && cell(5, 8, 0) == 62 && cell(0, 0, 0) == 67, "copied");
    song_patop(PO_TRANSPOSE, 5, 0, 0xFF | (2 << 8));
    CHECK(cell(5, 0, 0) == 69, "transposed");
    song_patop(PO_INSERT, 5, 0, 0xFF | (4 << 8));
    CHECK(cell(5, 0, 0) == 69 && cell(5, 9, 0) == 64 && cell(5, 8, 0) == 0, "a row inserted");
    song_patop(PO_DELETE, 5, 0, 0xFF | (0 << 8));
    CHECK(cell(5, 0, 0) == 0 && cell(5, 8, 0) == 64, "a row deleted");
    song_patop(PO_CLEAR, 5, 0, 0xFF);
    CHECK(cell(5, 8, 0) == 0 && cell(1, 4, 2) == 64, "cleared, the others kept");

    /* ---- playing: 120 BPM, 4 rows a beat = 125 ms a row ---- */
    seq_command(SQ_NEW << 8, 0, 0);
    song_cell_set(0, 0, 0, 60, 100);        /* FM 1 */
    song_cell_set(0, 2, 0, 62, 100);
    song_cell_set(0, 3, 0, NOTE_OFF, 0);
    song_cell_set(0, 0, 6, 72, 80);         /* PSG 1 (lane 7 = MIDI 7) */
    song_cell_set(0, 1, 18, 36, 127);       /* drums (lane 19 = MIDI 16) */
    nsent = 0; now_us = 0; nout = 0;
    cmd(SQ_PLAY, 0, 0, 0);
    CHECK(timer_on, "the timer runs");
    CHECK(nsent == 2 && sent[0].st == 0x90 && sent[0].a == 60 && sent[1].st == 0x96 && sent[1].a == 72, "row 0 at once (%d)", nsent);
    CHECK(nout >= 7 && outb[0] == 0xFA && outb[1] == 0x90, "MIDI out: start, then the notes");
    run_ms(130);
    CHECK(nsent == 3 && sent[2].st == 0x9F && sent[2].a == 36 && sent[2].t >= 124000 && sent[2].t <= 127000, "row 1 at 125 ms (%u)", sent[2].t);
    run_ms(125);
    CHECK(nsent == 5 && sent[3].st == 0x80 && sent[3].a == 60 && sent[4].st == 0x90 && sent[4].a == 62, "row 2: the lane's note gives way");
    run_ms(125);
    CHECK(nsent == 6 && sent[5].st == 0x80 && sent[5].a == 62, "row 3: note off");
    int clocks = 0; for (int i = 0; i < nout; i++) if (outb[i] == 0xF8) clocks++;
    CHECK(clocks >= 17 && clocks <= 19, "MIDI clock: 24 a beat (%d in 380 ms)", clocks);
    run_ms(125 * 61 + 5);                   /* the pattern (64 rows) loops */
    CHECK(count_on(0, 60) == 2, "the order list loops (%d)", count_on(0, 60));
    cmd(SQ_STOP, 0, 0, 0);
    CHECK(!timer_on && sent[nsent - 1].b == 0 && outb[nout - 1] == 0xFC, "stopped: notes off, MIDI stop");
    /* late polls: a frame with few ticks gets the rest */
    nsent = 0; now_us = 0;
    cmd(SQ_PLAY, 0, 0, 0);
    for (int f = 0; f < 8; f++) { timer_flag = 1; seq_poll(); seq_frame(); }    /* one tick a frame */
    int o, r; seq_pos(&o, &r);
    CHECK(r == 1, "8 frames (133 ms) with one tick each: row 1 (%d)", r);
    cmd(SQ_STOP, 0, 0, 0);
    /* mute */
    cmd(SQ_MUTE, 6, 1, 0);
    nsent = 0;
    cmd(SQ_PLAY, 0, 0, 0);
    CHECK(count_on(6, 72) == 0 && count_on(0, 60) == 1, "a muted lane is quiet");
    cmd(SQ_STOP, 0, 0, 0);
    cmd(SQ_MUTE, 6, 0, 0);

    /* ---- the order list ---- */
    seq_command(SQ_NEW << 8, 0, 0);
    pool = song_pool();
    pool[SO_NORD] = 3; pool[SO_ORD] = 0; pool[SO_ORD + 1] = 2; pool[SO_ORD + 2] = 0; pool[SO_LOOP] = 1;
    pool[SO_PLEN] = 2; pool[SO_PLEN + 2] = 2;
    song_cell_set(0, 0, 0, 50, 100);
    song_cell_set(2, 0, 0, 52, 100);
    nsent = 0;
    cmd(SQ_PLAY, 0, 0, 0);
    run_ms(125 * 8 + 5);                    /* 9 rows: 0 0 2 2 0 0 | 2 2 0 */
    int seq50 = 0, seq52 = 0;
    for (int i = 0; i < nsent; i++) if (sent[i].b) { if (sent[i].a == 50) seq50++; if (sent[i].a == 52) seq52++; }
    CHECK(seq50 == 3 && seq52 == 2, "orders 0 2 0, looping to 1 (%d %d)", seq50, seq52);
    cmd(SQ_STOP, 0, 0, 0);

    /* ---- the keyboard: it plays the cursor lane's part ---- */
    seq_command(SQ_NEW << 8, 0, 0);
    cmd(SQ_CURSOR, 10, 0, SQS_FOLLOW | 1 << 8);     /* lane 11: MIDI 11 (the 32X) */
    nsent = 0;
    live(0x90, 60, 100);
    live(0x90, 64, 100);                    /* lane 15 is MIDI 11 too: a chord */
    CHECK(nsent == 2 && sent[0].st == 0x9A && sent[1].st == 0x9A, "keys play part 11 (%02X)", sent[0].st);
    live(0x80, 60, 0);
    live(0xB0, 1, 64);
    CHECK(sent[2].st == 0x8A && sent[3].st == 0xBA, "off, the mod wheel too");
    live(0x80, 64, 0);
    CHECK(song_used() == SO_EVENTS, "nothing written (not editing)");

    /* step entry: the cursor moves a step when the keys are let go */
    cmd(SQ_CURSOR, 0, 0 << 8 | 4, SQS_FOLLOW | SQS_EDIT | 2 << 8);
    live(0x90, 60, 90); live(0x80, 60, 0);
    live(0x90, 62, 91); live(0x80, 62, 0);
    CHECK(cell(0, 4, 0) == 60 && cell(0, 6, 0) == 62, "step entry, 2 rows a step");
    unsigned st = cmd(SQ_STATE, 0, 0, 0);
    CHECK((host_comm[C_P1 / 2] & 0xFF) == 8 && (st & SQS_EDIT), "the cursor moved on (%d)", host_comm[C_P1 / 2] & 0xFF);
    cmd(SQ_CURSOR, 10, 0 << 8 | 0, SQS_FOLLOW | SQS_EDIT | 1 << 8);
    live(0x90, 48, 100); live(0x90, 55, 100); live(0x80, 48, 0); live(0x80, 55, 0);
    CHECK(cell(0, 0, 10) == 48 && cell(0, 0, 14) == 55, "a chord: part 11's lanes");

    /* recording: the nearest row */
    seq_command(SQ_NEW << 8, 0, 0);
    cmd(SQ_CURSOR, 1, 0, SQS_FOLLOW | 1 << 8);
    nsent = 0; now_us = 0;
    cmd(SQ_PLAY, 0, 0, 1);
    run_ms(30); live(0x91, 60, 100);        /* a bit after row 0: row 0 */
    run_ms(150); live(0x81, 60, 0);         /* row 1 at 125, 180 ms: row 1 */
    run_ms(60); live(0x91, 62, 100);        /* 240 ms: nearer row 2 (250) */
    run_ms(5); live(0x81, 62, 0);           /* the same row: a row later */
    CHECK(song_ntake() == 4, "4 in the take (%u)", song_ntake());
    int before = nsent;
    run_ms(20);                             /* row 2: 62 is sounding already */
    CHECK(count_on(1, 62) == 1, "not played again (%d)", count_on(1, 62));
    (void)before;
    cmd(SQ_ROWS, 1, 0 << 8 | 0, 4 << 8 | 1);
    CHECK(file_buf[BUF_DATA] == 60 && file_buf[BUF_DATA + 2] == NOTE_OFF && file_buf[BUF_DATA + 4] == 62 && file_buf[BUF_DATA + 6] == NOTE_OFF,
          "the take shows (%d %d %d %d)", file_buf[BUF_DATA], file_buf[BUF_DATA + 2], file_buf[BUF_DATA + 4], file_buf[BUF_DATA + 6]);
    run_ms(125 * 64);                       /* the next time round it plays from the take */
    CHECK(count_on(1, 60) == 2, "played back (%d)", count_on(1, 60));
    cmd(SQ_STOP, 0, 0, 0);
    CHECK(song_ntake() == 0 && cell(0, 0, 1) == 60 && cell(0, 1, 1) == NOTE_OFF && cell(0, 2, 1) == 62 && cell(0, 3, 1) == NOTE_OFF,
          "merged when it stopped");

    /* ---- the merge against a model: random takes over random songs ---- */
    srand(7);
    for (int round = 0; round < 40; round++) {
        seq_command(SQ_NEW << 8, 0, 0);
        static uint8_t model[SONG_PATS][SONG_ROWS_MAX][SONG_LANES];
        memset(model, 0, sizeof model);
        for (int k = 0; k < 300; k++) {
            int p = rand() % 8, r = rand() % 64, l = rand() % SONG_LANES, n = 1 + rand() % 120;
            song_cell_set(p, r, l, n, 100);
            model[p][r][l] = (uint8_t)n;
        }
        cmd(SQ_CURSOR, 0, 0, SQS_FOLLOW);
        song_pool()[SO_NORD] = 8;
        for (int i = 0; i < 8; i++) song_pool()[SO_ORD + i] = (uint8_t)i;
        cmd(SQ_PLAY, 0, 0, 1);
        for (int k = 0; k < 150; k++) {
            run_ms(rand() % 200);
            int lane = rand() % 6, nt = 1 + rand() % 120;
            cmd(SQ_CURSOR, lane, 0, SQS_FOLLOW);
            int o2, r2;
            seq_pos(&o2, &r2);
            live(0x90, nt, 100);
            live(0x80, nt, 0);
        }
        cmd(SQ_STOP, 0, 0, 0);
        unsigned total = 0;
        for (int p = 0; p < SONG_PATS; p++) total += (unsigned)(song_pool()[SO_PCNT + 2 * p] << 8 | song_pool()[SO_PCNT + 2 * p + 1]);
        CHECK(song_used() == SO_EVENTS + total * 4, "counts add up");
        int lost = 0;
        for (int p = 0; p < 8; p++)
            for (int r = 0; r < 64; r++)
                for (int l = 6; l < SONG_LANES; l++)      /* (lanes 0-5 were recorded over) */
                    if (model[p][r][l] && cell(p, r, l) != model[p][r][l]) lost++;
        CHECK(!lost, "round %d: %d cells lost", round, lost);
    }

    /* ---- full ---- */
    seq_command(SQ_NEW << 8, 0, 0);
    int n = 0;
    while (song_cell_set(n / (64 * SONG_LANES), (n / SONG_LANES) % 64, n % SONG_LANES, 60, 100) == 0) n++;
    CHECK(n == (24576 - SO_EVENTS) / 4 && (cmd(SQ_STATE, 0, 0, 0) & SQS_FULL), "full at %d events", n);

    /* ---- files ---- */
    seq_command(SQ_NEW << 8, 0, 0);
    song_cell_set(0, 0, 0, 60, 100);
    song_cell_set(3, 5, 9, 70, 50);
    song_path_set(0, "Keys/EP.tfi");
    CHECK(cmd(SQ_SAVE, 0, 0, 0) == 0 && disk_len == SO_EVENTS + 8, "saved (%u bytes)", disk_len);
    seq_command(SQ_NEW << 8, 0, 0);
    CHECK(cmd(SQ_LOAD, 0, 0, 0) == 0 && cell(3, 5, 9) == 70, "loaded back");
    song_path_get(0, t, sizeof t); CHECK(!strcmp(t, "Keys/EP.tfi"), "its instrument");
    disk[0] = 'X';
    CHECK(cmd(SQ_LOAD, 0, 0, 0) != 0 && song_used() == SO_EVENTS, "not a song: refused, a new one");

    /* ---- a standard MIDI file ---- */
    seq_command(SQ_NEW << 8, 0, 0);
    song_pool()[SO_PLEN] = 4;
    song_cell_set(0, 0, 0, 60, 100);
    song_cell_set(0, 2, 0, 62, 90);
    song_cell_set(0, 0, 18, 36, 127);
    song_cell_set(0, 3, 18, NOTE_OFF, 0);
    CHECK(cmd(SQ_EXPORT, 0, 0, 0) == 0, "exported");
    CHECK(!memcmp(disk, "MThd", 4) && disk[9] == 0 && disk[11] == 1 && (disk[12] << 8 | disk[13]) == 96, "header: format 0, one track, 96 a beat");
    uint32_t tl = (uint32_t)disk[18] << 24 | disk[19] << 16 | disk[20] << 8 | disk[21];
    CHECK(!memcmp(disk + 14, "MTrk", 4) && tl + 22 == disk_len, "track length %u of %u", tl, disk_len);
    CHECK(disk[disk_len - 3] == 0xFF && disk[disk_len - 2] == 0x2F, "end of track");
    /* the events: walk them */
    uint32_t at = 22, tick = 0;
    int ons = 0, offs = 0, ok_times = 1;
    while (at < disk_len) {
        uint32_t d = 0;
        while (disk[at] & 0x80) d = d << 7 | (disk[at++] & 0x7F);
        d = d << 7 | disk[at++];
        tick += d;
        uint8_t s0 = disk[at++];
        if (s0 == 0xFF) { at++; uint32_t l = disk[at++]; at += l; continue; }
        uint8_t k = disk[at++], v = disk[at++];
        if ((s0 & 0xF0) == 0x90) {
            ons++;
            if (k == 60 && tick != 0) ok_times = 0;
            if (k == 62 && tick != 48) ok_times = 0;
            if (k == 36 && s0 != 0x9F) ok_times = 0;
        } else {
            offs++;
            if (k == 36 && tick != 72) ok_times = 0;
        }
        (void)v;
    }
    CHECK(ons == 3 && offs == 3 && ok_times, "3 notes at their ticks (%d on, %d off)", ons, offs);

    if (fails) { printf("%d failed\n", fails); return 1; }
    printf("ok   tracker: editing, playing, the keyboard, recording, files, MIDI file\n");
    return 0;
}
