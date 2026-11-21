/* md_seq.c - the tracker's song and its sequencer, on the 68000.
 *
 * The song is a pool of bytes laid out as src/common/song.h says (the same
 * bytes are the file on the SD card). The 32X build keeps it in the 68000's
 * work RAM; the Genesis-only build, whose work RAM holds the screen, in the
 * Z80's 8 KB (the Z80 is never run: its bus is ours, src/md/md_snd.c), a
 * byte at a time.
 *
 * Playing: the YM2612's timer A overflows every ~1 ms (polled, seq_poll);
 * each row is 60 s / (BPM x rows a beat). A row's notes go into the MIDI
 * engine as MIDI messages (md_midi.c), exactly as if they came from the
 * keyboard: channels 1-10 to the YM2612 and the PSG, 11-14 and 16 on to the
 * 32X. With MIDI OUT on they also go out to the computer (md_rx.c), with
 * MIDI clock. A frame without enough timer ticks (the polls were late: the
 * Genesis-only build draws its screens on this CPU) gets the missing ones,
 * so the tempo holds.
 *
 * The keyboard: on the tracker screen it plays the cursor lane's part
 * (SQS_FOLLOW), a chord over that part's lanes. Stopped, with step entry
 * on (SQS_EDIT), each note goes in at the cursor, which moves down a step
 * when the keys are let go. Playing and armed (SQS_REC), notes are recorded
 * at the nearest row into a small take, merged into the song in one pass
 * (when it stops, or when the take fills); the take plays while it waits.
 */
#include <stddef.h>
#include "md.h"
#include "song.h"

#ifndef SONG_BYTES
#ifdef MD_ONLY
#define SONG_BYTES  7680                /* Z80 RAM: 8 KB, the take in the last 512 */
#else
#define SONG_BYTES  24576
#endif
#endif
#define TAKE_MAX    100                 /* recorded events waiting (5 bytes each) */

#if defined(MD_ONLY) && !defined(MD_HOST)
typedef volatile uint8_t pb_t;          /* (the Z80's RAM: bytes only) */
#define POOL        ((pb_t *)0xA00000)
#define TAKE        ((pb_t *)0xA01E00)
#else
typedef uint8_t pb_t;
static uint8_t pool_mem[SONG_BYTES];
static uint8_t take_mem[TAKE_MAX * 5];
#define POOL        pool_mem
#define TAKE        take_mem
#endif

/* ------------------------------------------------------------ the pool */

static unsigned p8(unsigned o) { return POOL[o]; }
static void w8(unsigned o, unsigned v) { POOL[o] = (uint8_t)v; }
static unsigned p16(unsigned o) { return (unsigned)(POOL[o] << 8 | POOL[o + 1]); }
static void w16(unsigned o, unsigned v) { POOL[o] = (uint8_t)(v >> 8); POOL[o + 1] = (uint8_t)v; }

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

static uint16_t edits;                  /* every change to the song (the screen redraws) */
static uint8_t full;                    /* a note did not fit */

static unsigned pcnt(int p) { return p16(SO_PCNT + 2 * p); }
static unsigned plen(int p) { unsigned n = p8(SO_PLEN + p); return n ? n : 64; }
static unsigned nord(void) { unsigned n = p8(SO_NORD); return n ? n : 1; }
static int lane_ch(int l) { return p8(SO_LANE + l) & 15; }

static unsigned pat_start(int p)
{
    unsigned n = 0;
    for (int i = 0; i < p; i++) n += pcnt(i);
    return SO_EVENTS + n * EV_BYTES;
}

static unsigned used_bytes(void) { return pat_start(SONG_PATS); }

static int find_cell(int p, int row, int lane)      /* event index in the pattern, or -1 */
{
    unsigned a = pat_start(p), n = pcnt(p);
    for (unsigned i = 0; i < n; i++, a += EV_BYTES)
        if (p8(a) == (unsigned)row && p8(a + 1) == (unsigned)lane) return (int)i;
    return -1;
}

static void remove_event(int p, unsigned i)
{
    unsigned a = pat_start(p) + i * EV_BYTES, end = used_bytes();
    pmove(a, a + EV_BYTES, end - a - EV_BYTES);
    w16(SO_PCNT + 2 * p, pcnt(p) - 1);
}

/* note 0: clear; -> 0 or ERR_TOO_BIG */
int song_cell_set(int p, int row, int lane, int note, int vel)
{
    if (p < 0 || p >= SONG_PATS || lane < 0 || lane >= SONG_LANES || row < 0 || row >= SONG_ROWS_MAX)
        return ERR_RANGE;
    if (vel <= 0 || vel > 127) vel = 100;
    int i = find_cell(p, row, lane);
    edits++;
    if (!note) {
        if (i >= 0) remove_event(p, (unsigned)i);
        return 0;
    }
    if (i >= 0) {
        unsigned a = pat_start(p) + (unsigned)i * EV_BYTES;
        w8(a + 2, (unsigned)note);
        w8(a + 3, (unsigned)vel);
        return 0;
    }
    unsigned end = used_bytes();
    if (end + EV_BYTES > SONG_BYTES) { full = 1; return ERR_TOO_BIG; }
    unsigned a = pat_start(p) + pcnt(p) * EV_BYTES;
    pmove(a + EV_BYTES, a, end - a);
    w8(a, (unsigned)row); w8(a + 1, (unsigned)lane); w8(a + 2, (unsigned)note); w8(a + 3, (unsigned)vel);
    w16(SO_PCNT + 2 * p, pcnt(p) + 1);
    return 0;
}

int song_cell_get(int p, int row, int lane, int *vel)
{
    int i = find_cell(p, row, lane);
    if (i < 0) { *vel = 0; return 0; }
    unsigned a = pat_start(p) + (unsigned)i * EV_BYTES;
    *vel = (int)p8(a + 3);
    return (int)p8(a + 2);
}

/* ------------------------------------------------------------ instruments */

static unsigned path_at(int k)          /* where part k's path starts */
{
    unsigned o = SO_PATHS;
    for (int i = 0; i < k; i++) {
        while (o < SO_PATHS + SONG_PATHS && p8(o)) o++;
        o++;
    }
    return o;
}

static unsigned paths_end(void) { return path_at(SONG_PARTS); }

int song_path_get(int k, char *out, int max)
{
    unsigned o = path_at(k);
    int n = 0;
    while (o < SO_PATHS + SONG_PATHS && p8(o) && n < max - 1) out[n++] = (char)p8(o++);
    out[n] = 0;
    return n;
}

int song_path_set(int k, const char *s)
{
    int len = 0;
    while (s[len]) len++;
    unsigned at = path_at(k), old = 0;
    while (p8(at + old)) old++;
    unsigned end = paths_end();
    if (end - old + (unsigned)len > SO_PATHS + SONG_PATHS) return ERR_TOO_BIG;
    pmove(at + (unsigned)len, at + old, end - at - old);   /* (the zero and the paths after) */
    for (int i = 0; i < len; i++) w8(at + (unsigned)i, (uint8_t)s[i]);
    for (unsigned o = end - old + (unsigned)len; o < SO_PATHS + SONG_PATHS; o++) w8(o, 0);
    edits++;
    return 0;
}

/* ------------------------------------------------------------ a new song */

void song_new(void)
{
    static const uint8_t lanes[SONG_LANES] = SONG_DEFAULT_LANES;
    for (unsigned o = 0; o < SO_EVENTS; o++) w8(o, 0);
    const char *m = SONG_MAGIC;
    for (int i = 0; i < 4; i++) w8(SO_MAGIC + i, (uint8_t)m[i]);
    w8(SO_VER, SONG_VERSION);
    w8(SO_RPB, 4);
    w16(SO_BPM, 120);
    w8(SO_NORD, 1);
    w8(SO_NPAT, SONG_PATS);
    const char *nm = "NEW SONG";
    for (int i = 0; nm[i]; i++) w8(SO_NAME + i, (uint8_t)nm[i]);
    for (int l = 0; l < SONG_LANES; l++) w8(SO_LANE + l, lanes[l]);
    for (int p = 0; p < SONG_PATS; p++) w8(SO_PLEN + p, 64);
    edits++;
}

static int song_valid(unsigned len)
{
    const char *m = SONG_MAGIC;
    for (int i = 0; i < 4; i++) if (p8(SO_MAGIC + i) != (uint8_t)m[i]) return 0;
    if (p8(SO_VER) != SONG_VERSION || len < SO_EVENTS) return 0;
    if (used_bytes() != len) return 0;
    for (int p = 0; p < SONG_PATS; p++) if (plen(p) > SONG_ROWS_MAX) return 0;
    if (p8(SO_NORD) > SONG_ORDS) return 0;
    return 1;
}

/* ------------------------------------------------------------ pattern tools */

static void clear_lane(int p, int lane)         /* lane < 0: all */
{
    unsigned a = pat_start(p);
    for (unsigned i = 0; i < pcnt(p); ) {
        if (lane < 0 || p8(a + i * EV_BYTES + 1) == (unsigned)lane) remove_event(p, i);
        else i++;
    }
}

int song_patop(int op, int p, int other, int arg)
{
    if (p < 0 || p >= SONG_PATS) return ERR_RANGE;
    int lane = arg & 0xFF;
    if (lane == 0xFF) lane = -1;
    edits++;
    switch (op) {
    case PO_CLEAR:
        clear_lane(p, lane);
        return 0;
    case PO_COPY: {
        if (other < 0 || other >= SONG_PATS || other == p) return ERR_RANGE;
        unsigned n = pcnt(p);
        clear_lane(other, -1);
        if (used_bytes() + n * EV_BYTES > SONG_BYTES) { full = 1; return ERR_TOO_BIG; }
        unsigned dst = pat_start(other), end = used_bytes();
        pmove(dst + n * EV_BYTES, dst, end - dst);      /* a gap for them */
        w16(SO_PCNT + 2 * other, n);
        unsigned src = pat_start(p);                    /* (moved if after the gap) */
        for (unsigned i = 0; i < n * EV_BYTES; i++) w8(dst + i, p8(src + i));
        w8(SO_PLEN + other, plen(p));
        return 0;
    }
    case PO_INSERT:
    case PO_DELETE: {
        unsigned row = (unsigned)(arg >> 8) & 0xFF, len = plen(p);
        unsigned a = pat_start(p);
        for (unsigned i = 0; i < pcnt(p); ) {
            unsigned e = a + i * EV_BYTES, r = p8(e);
            if ((lane >= 0 && p8(e + 1) != (unsigned)lane) || r < row) { i++; continue; }
            if (op == PO_DELETE && r == row) { remove_event(p, i); continue; }
            unsigned nr = op == PO_INSERT ? r + 1 : r - 1;
            if (nr >= len) { remove_event(p, i); continue; }
            w8(e, nr);
            i++;
        }
        return 0;
    }
    case PO_TRANSPOSE: {
        int d = (int8_t)(arg >> 8);
        unsigned a = pat_start(p);
        for (unsigned i = 0; i < pcnt(p); i++) {
            unsigned e = a + i * EV_BYTES, n = p8(e + 2);
            if ((lane >= 0 && p8(e + 1) != (unsigned)lane) || n == NOTE_OFF) continue;
            int m = (int)n + d;
            if (m < 1) m = 1;
            if (m > 127) m = 127;
            w8(e + 2, (unsigned)m);
        }
        return 0;
    }
    }
    return ERR_RANGE;
}

/* ------------------------------------------------------------ the take */

static uint8_t ntake;

static void take_merge(void);

static void take_add(int p, int row, int lane, int note, int vel)
{
    for (unsigned i = 0; i < ntake; i++) {
        pb_t *t = TAKE + i * 5;
        if (t[0] == p && t[1] == row && t[2] == lane) { t[3] = (uint8_t)note; t[4] = (uint8_t)vel; return; }
    }
    if (ntake >= TAKE_MAX) take_merge();
    pb_t *t = TAKE + ntake * 5;
    t[0] = (uint8_t)p; t[1] = (uint8_t)row; t[2] = (uint8_t)lane; t[3] = (uint8_t)note; t[4] = (uint8_t)vel;
    ntake++;
    edits++;
}

/* the take into the song in one pass: the cells it replaces in place, then
 * the patterns moved up (the last first) with the new events at their ends */
static void take_merge(void)
{
    if (!ntake) return;
    uint8_t add[SONG_PATS];
    for (int p = 0; p < SONG_PATS; p++) add[p] = 0;
    for (unsigned i = 0; i < ntake; i++) {
        pb_t *t = TAKE + i * 5;
        int c = find_cell(t[0], t[1], t[2]);
        if (c >= 0) {
            unsigned a = pat_start(t[0]) + (unsigned)c * EV_BYTES;
            w8(a + 2, t[3]); w8(a + 3, t[4]);
            t[2] = 0xFF;                            /* (done) */
        } else {
            add[t[0]]++;
        }
    }
    unsigned grow = 0;
    for (int p = 0; p < SONG_PATS; p++) grow += add[p];
    while (used_bytes() + grow * EV_BYTES > SONG_BYTES && grow) {     /* no room: drop the latest */
        for (int i = (int)ntake - 1; i >= 0; i--) {
            pb_t *t = TAKE + i * 5;
            if (t[2] != 0xFF) { add[t[0]]--; t[2] = 0xFF; grow--; full = 1; break; }
        }
    }
    unsigned shift = grow;
    for (int p = SONG_PATS - 1; p >= 0 && shift; p--) {
        shift -= add[p];
        unsigned old = pat_start(p), n = pcnt(p), dst = old + shift * EV_BYTES;
        pmove(dst, old, n * EV_BYTES);
        unsigned e = dst + n * EV_BYTES;
        for (unsigned i = 0; i < ntake; i++) {
            pb_t *t = TAKE + i * 5;
            if (t[2] == 0xFF || t[0] != p) continue;
            w8(e, t[1]); w8(e + 1, t[2]); w8(e + 2, t[3]); w8(e + 3, t[4]);
            e += EV_BYTES;
        }
        w16(SO_PCNT + 2 * p, n + add[p]);
    }
    ntake = 0;
    edits++;
}

/* ------------------------------------------------------------ the clock */

static uint8_t seq_flags;
static uint8_t cur_lane, cur_ord, cur_row, step_rows = 1;
static uint8_t pl_ord, pl_row;          /* the next row to play */
static uint8_t at_ord, at_row;          /* the row played last */
static uint32_t acc_us, row_us, clk_acc, clk_us;
static uint16_t tick_us;
static uint8_t frame_ticks;
static uint8_t lane_note[SONG_LANES];
static uint32_t lane_live;              /* lanes the keyboard sounded ahead of the take */
static uint8_t live_note[SONG_LANES];   /* (which note) */

static void tempo_set(void)
{
    unsigned bpm = p16(SO_BPM), rpb = p8(SO_RPB);
    if (bpm < 20) bpm = 20;
    if (bpm > 300) bpm = 300;
    if (rpb < 1) rpb = 1;
    if (rpb > 16) rpb = 16;
    row_us = 60000000u / (bpm * rpb);
    clk_us = 2500000u / bpm;            /* 24 a beat */
    tick_us = ms.region ? 1004 : 995;   /* timer A: 53 x 144 / the YM2612's clock */
}

/* ------------------------------------------------------------ sending */

static void send(uint8_t st, uint8_t a, uint8_t b, int out)
{
    midi_play(st, a, b);
    if (out && ms.midi_out) {
        midi_out(st);
        midi_out(a);
        if ((st & 0xE0) != 0xC0) midi_out(b);
    }
}

static void lane_off(int l)
{
    if (!lane_note[l]) return;
    send((uint8_t)(0x80 | lane_ch(l)), lane_note[l], 0, 1);
    lane_note[l] = 0;
}

static int muted(int l)
{
    return (p8(SO_MUTE + (l >> 3)) >> (l & 7)) & 1;
}

static void play_event(int l, int note, int vel)
{
    uint32_t bit = (uint32_t)1 << l;
    if (note == NOTE_OFF) {
        lane_off(l);
        lane_live &= ~bit;
        return;
    }
    if ((lane_live & bit) && live_note[l] == note) {    /* (the keyboard played it a moment ago) */
        lane_live &= ~bit;
        return;
    }
    if (muted(l)) return;
    lane_off(l);
    send((uint8_t)(0x90 | lane_ch(l)), (uint8_t)note, (uint8_t)(vel ? vel : 100), 1);
    lane_note[l] = (uint8_t)note;
}

static int next_ord(int o)
{
    if (o + 1 < (int)nord()) return o + 1;
    int lp = (int)p8(SO_LOOP);
    return lp < (int)nord() ? lp : 0;
}

static void play_row(void)
{
    int p = p8(SO_ORD + pl_ord) % SONG_PATS;
    if (pl_row >= plen(p)) { pl_row = 0; pl_ord = (uint8_t)next_ord(pl_ord); p = p8(SO_ORD + pl_ord) % SONG_PATS; }
    uint32_t took = 0;                  /* lanes the take has an event for in this row */
    for (unsigned i = 0; i < ntake; i++) {
        pb_t *t = TAKE + i * 5;
        if (t[0] == p && t[1] == pl_row && t[2] < SONG_LANES) took |= (uint32_t)1 << t[2];
    }
    unsigned a = pat_start(p), n = pcnt(p);
    for (unsigned i = 0; i < n; i++, a += EV_BYTES) {
        if (p8(a) != pl_row) continue;
        unsigned l = p8(a + 1);
        if (l < SONG_LANES && !(took >> l & 1)) play_event((int)l, (int)p8(a + 2), (int)p8(a + 3));
    }
    for (unsigned i = 0; i < ntake; i++) {
        pb_t *t = TAKE + i * 5;
        if (t[0] == p && t[1] == pl_row && t[2] < SONG_LANES) play_event(t[2], t[3], t[4]);
    }
    at_ord = pl_ord;
    at_row = pl_row;
    if (++pl_row >= plen(p)) { pl_row = 0; pl_ord = (uint8_t)next_ord(pl_ord); }
}

static void advance(uint32_t us)
{
    if (ms.midi_out) {
        clk_acc += us;
        while (clk_acc >= clk_us) { clk_acc -= clk_us; midi_out(0xF8); }
    }
    acc_us += us;
    while (acc_us >= row_us) {
        acc_us -= row_us;
        play_row();
    }
}

void seq_poll(void)
{
    if (!(seq_flags & SQS_PLAY) || !ym_timer_tick()) return;
    if (frame_ticks < 255) frame_ticks++;
    advance(tick_us);
}

void seq_frame(void)
{
    if (!(seq_flags & SQS_PLAY)) return;
    unsigned want = ms.region ? 19 : 16;    /* (timer ticks a frame: 19.9 PAL, 16.8 NTSC) */
    if (frame_ticks + 1u < want)            /* polled too late to see them all */
        advance((uint32_t)(want - frame_ticks) * tick_us);
    frame_ticks = 0;
}

/* ------------------------------------------------------------ transport */

void seq_stop(void)
{
    if (!(seq_flags & SQS_PLAY)) return;
    seq_flags &= (uint8_t)~SQS_PLAY;
    ym_timer_run(0);
    for (int l = 0; l < SONG_LANES; l++) lane_off(l);
    lane_live = 0;
    if (ms.midi_out) midi_out(0xFC);
    take_merge();
    cur_ord = at_ord;                   /* (the cursor where it stopped) */
    cur_row = at_row;
}

void seq_play(int from_cursor, int ord, int row, int rec)
{
    seq_stop();
    tempo_set();
    pl_ord = (uint8_t)(from_cursor && ord < (int)nord() ? ord : 0);
    pl_row = (uint8_t)(from_cursor ? row : 0);
    at_ord = pl_ord;
    at_row = pl_row;
    acc_us = 0;
    clk_acc = 0;
    frame_ticks = 0;
    for (int l = 0; l < SONG_LANES; l++) lane_note[l] = 0;
    lane_live = 0;
    if (rec) seq_flags |= SQS_REC; else seq_flags &= (uint8_t)~SQS_REC;
    seq_flags |= SQS_PLAY;
    if (ms.midi_out) midi_out(from_cursor && (ord || row) ? 0xFB : 0xFA);
    ym_timer_run(1);
    play_row();
}

int seq_playing(void) { return (seq_flags & SQS_PLAY) != 0; }

/* ------------------------------------------------------------ the keyboard */

#define HELD 8
static uint8_t held_note[HELD], held_lane[HELD], held_ord[HELD], held_row[HELD], nheld;
static uint8_t keys_down;

static int lane_free(int l)
{
    for (int i = 0; i < nheld; i++) if (held_lane[i] == l) return 0;
    return 1;
}

/* the lane for a new key: the cursor lane, else the next lane of the same part */
static int pick_lane(void)
{
    int ch = lane_ch(cur_lane);
    for (int k = 0; k < SONG_LANES; k++) {
        int l = (cur_lane + k) % SONG_LANES;
        if (lane_ch(l) == ch && lane_free(l)) return l;
    }
    return -1;
}

/* the nearest row: the one played last, or (more than half way on) the next */
static void rec_pos(int *o, int *r, int *ahead)
{
    *ahead = acc_us * 2 >= row_us;
    *o = *ahead ? pl_ord : at_ord;
    *r = *ahead ? pl_row : at_row;
    int p = p8(SO_ORD + *o) % SONG_PATS;
    if (*r >= (int)plen(p)) { *r = 0; *o = next_ord(*o); }
}

static void step_on(int lane, int note, int vel)
{
    int p = p8(SO_ORD + cur_ord) % SONG_PATS;
    if (cur_row >= plen(p)) cur_row = 0;
    if (song_cell_set(p, cur_row, lane, note, vel) == 0) keys_down++;
}

static void step_release(void)
{
    if (keys_down && --keys_down) return;
    int p = p8(SO_ORD + cur_ord) % SONG_PATS;
    unsigned r = cur_row + step_rows;
    if (r >= plen(p)) {                 /* on into the next pattern of the order list */
        r -= plen(p);
        if (cur_ord + 1 < (int)nord()) cur_ord++;
        if (r >= plen(p8(SO_ORD + cur_ord) % SONG_PATS)) r = 0;
    }
    cur_row = (uint8_t)r;
    edits++;
}

/* a message from the keyboard (md_midi.c): 1 if it was taken here */
int seq_live(uint8_t st, uint8_t a, uint8_t b)
{
    if (!(seq_flags & SQS_FOLLOW)) return 0;
    uint8_t type = st & 0xF0;
    int out = ms.midi_out >= 2;
    if (type == 0x90 && b) {
        int l = pick_lane();
        if (l < 0 || nheld >= HELD) return 1;
        int ch = lane_ch(l);
        lane_note[l] = 0;               /* (the lane's own note gives way) */
        send((uint8_t)(0x90 | ch), a, b, out);
        lane_note[l] = a;
        int o = cur_ord, r = cur_row, ahead = 0;
        if (seq_flags & SQS_PLAY) {
            if (seq_flags & SQS_REC) {
                rec_pos(&o, &r, &ahead);
                take_add(p8(SO_ORD + o) % SONG_PATS, r, l, a, b);
                if (ahead) { lane_live |= (uint32_t)1 << l; live_note[l] = a; }
            }
        } else if (seq_flags & SQS_EDIT) {
            step_on(l, a, b);
        }
        held_note[nheld] = a; held_lane[nheld] = (uint8_t)l;
        held_ord[nheld] = (uint8_t)o; held_row[nheld] = (uint8_t)r;
        nheld++;
        return 1;
    }
    if (type == 0x80 || type == 0x90) {
        int i = 0;
        while (i < nheld && held_note[i] != a) i++;
        if (i == nheld) return 1;
        int l = held_lane[i], o0 = held_ord[i], r0 = held_row[i];
        nheld--;
        held_note[i] = held_note[nheld]; held_lane[i] = held_lane[nheld];
        held_ord[i] = held_ord[nheld]; held_row[i] = held_row[nheld];
        if (lane_note[l] == a) {
            send((uint8_t)(0x80 | lane_ch(l)), a, 0, out);
            lane_note[l] = 0;
        }
        if ((seq_flags & SQS_PLAY) && (seq_flags & SQS_REC)) {
            int o, r, ahead;
            rec_pos(&o, &r, &ahead);
            if (o == o0 && r == r0) {           /* at least a row long */
                int p = p8(SO_ORD + o) % SONG_PATS;
                if (++r >= (int)plen(p)) { r = 0; o = next_ord(o); }
            }
            take_add(p8(SO_ORD + o) % SONG_PATS, r, l, NOTE_OFF, 0);
        } else if (!(seq_flags & SQS_PLAY) && (seq_flags & SQS_EDIT)) {
            step_release();
        }
        return 1;
    }
    if (type == 0xB0 || type == 0xE0 || type == 0xD0 || type == 0xA0) {
        send((uint8_t)(type | lane_ch(cur_lane)), a, b, out);
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------ files */

int song_save(void)
{
    const char *path = files_song_path();
    if (!path) return ERR_NO_ED;
    take_merge();
    files_song_mkdir();
#if defined(MD_ONLY) && !defined(MD_HOST)
    return ed_write_file(path, (const uint8_t *)POOL, (int)used_bytes());
#else
    return ed_write_file(path, POOL, (int)used_bytes());
#endif
}

int song_load(void)
{
    seq_stop();
    uint16_t len = 0;
    int e = files_song_read((uint8_t *)POOL, SONG_BYTES, &len);
    if (!e && !song_valid(len)) e = ERR_RANGE;
    if (e) song_new();
    ntake = 0;
    edits++;
    return e;
}

/* ------------------------------------------------------------ a standard MIDI file
 *
 * Format 0, one track, 24 ticks a row (rows a beat x 24 a quarter note):
 * the tempo, the song's name, then each row of the order list once through,
 * a lane's note ended by its next one (or a note-off). Made twice: once to
 * count the bytes (the EverDrive wants the size first), once to write them,
 * a kilobyte at a time through file_buf. */

static uint32_t smf_count;
static int smf_writing, smf_err;
static unsigned smf_fill;
#define SMF_BLOCK   1024
#define SMF_BUF     (file_buf + BUF_DATA)

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

static uint32_t smf_track(void)         /* the track's events; -> its length */
{
    uint32_t start = smf_count, wait = 0;
    uint8_t note[SONG_LANES];
    for (int l = 0; l < SONG_LANES; l++) note[l] = 0;
    unsigned bpm = p16(SO_BPM) ? p16(SO_BPM) : 120;
    uint32_t qus = 60000000u / bpm;
    smf_var(0); smf_byte(0xFF); smf_byte(0x51); smf_byte(3);
    smf_byte((uint8_t)(qus >> 16)); smf_byte((uint8_t)(qus >> 8)); smf_byte((uint8_t)qus);
    int nl = 0;
    while (nl < 24 && p8(SO_NAME + nl)) nl++;
    smf_var(0); smf_byte(0xFF); smf_byte(0x03); smf_var((uint32_t)nl);
    for (int i = 0; i < nl; i++) smf_byte((uint8_t)p8(SO_NAME + i));
    uint8_t rn[SONG_LANES], rv[SONG_LANES];
    for (unsigned o = 0; o < nord(); o++) {
        int p = p8(SO_ORD + o) % SONG_PATS;
        unsigned a0 = pat_start(p), n = pcnt(p);
        for (unsigned r = 0; r < plen(p); r++) {
            for (int l = 0; l < SONG_LANES; l++) rn[l] = 0;
            unsigned a = a0;
            for (unsigned i = 0; i < n; i++, a += EV_BYTES) {     /* this row's cells, by lane */
                unsigned l = p8(a + 1);
                if (p8(a) == r && l < SONG_LANES) { rn[l] = (uint8_t)p8(a + 2); rv[l] = (uint8_t)p8(a + 3); }
            }
            for (int l = 0; l < SONG_LANES; l++) {
                if (!rn[l] || muted(l)) continue;
                int ch = lane_ch(l);
                if (note[l]) {
                    smf_var(wait); wait = 0;
                    smf_byte((uint8_t)(0x80 | ch)); smf_byte(note[l]); smf_byte(0);
                    note[l] = 0;
                }
                if (rn[l] != NOTE_OFF) {
                    smf_var(wait); wait = 0;
                    smf_byte((uint8_t)(0x90 | ch)); smf_byte(rn[l]); smf_byte(rv[l] ? rv[l] : 100);
                    note[l] = rn[l];
                }
            }
            wait += 24;
            rx_poll();
        }
    }
    for (int l = 0; l < SONG_LANES; l++)
        if (note[l]) {
            smf_var(wait); wait = 0;
            smf_byte((uint8_t)(0x80 | lane_ch(l))); smf_byte(note[l]); smf_byte(0);
        }
    smf_var(wait); smf_byte(0xFF); smf_byte(0x2F); smf_byte(0);
    return smf_count - start;
}

static void smf_all(uint32_t track_len)
{
    smf_byte('M'); smf_byte('T'); smf_byte('h'); smf_byte('d');
    smf_32(6);
    smf_byte(0); smf_byte(0);           /* format 0 */
    smf_byte(0); smf_byte(1);           /* one track */
    unsigned rpb = p8(SO_RPB) ? p8(SO_RPB) : 4;
    smf_byte((uint8_t)((24 * rpb) >> 8)); smf_byte((uint8_t)(24 * rpb));
    smf_byte('M'); smf_byte('T'); smf_byte('r'); smf_byte('k');
    smf_32(track_len);
    smf_track();
}

int song_export(void)
{
    const char *path = files_song_path();
    if (!path) return ERR_NO_ED;
    seq_stop();
    smf_writing = 0;
    smf_count = 0;
    smf_all(0);                         /* (the length: its track is the rest) */
    uint32_t total = smf_count, track = total - 22;
    files_song_mkdir();
    int e = ed_stream_open(path, total);
    if (e) return e;
    smf_writing = 1;
    smf_err = 0;
    smf_fill = 0;
    smf_count = 0;
    smf_all(track);
    if (smf_fill && !smf_err) smf_err = ed_stream_put(SMF_BUF, (int)smf_fill);
    smf_writing = 0;
    e = ed_stream_close();
    return smf_err ? smf_err : e;
}

/* ------------------------------------------------------------ the commands */

void seq_init(void)
{
    song_new();
    tempo_set();
}

static void put6(unsigned o)
{
    uint8_t b[6];
    for (int k = 0; k < 6; k++) b[k] = o + (unsigned)k < SONG_BYTES ? (uint8_t)p8(o + (unsigned)k) : 0;
    COMM16(C_P1) = (uint16_t)(b[0] << 8 | b[1]);
    COMM16(C_P2) = (uint16_t)(b[2] << 8 | b[3]);
    COMM16(C_P3) = (uint16_t)(b[4] << 8 | b[5]);
}

unsigned seq_command(uint16_t p1, uint16_t p2, uint16_t p3)
{
    int op = p1 >> 8, a = p1 & 0xFF;
    switch (op) {
    case SQ_STATE: {
        int playing = seq_flags & SQS_PLAY;
        int o = playing ? at_ord : cur_ord, r = playing ? at_row : cur_row;
        int p = p8(SO_ORD + o) % SONG_PATS;
        COMM16(C_P1) = (uint16_t)(o << 8 | r);
        COMM16(C_P2) = (uint16_t)(p << 8 | (plen(p) & 0xFF));
        COMM16(C_P3) = (uint16_t)(edits + ntake);
        return (unsigned)(seq_flags | (full ? SQS_FULL : 0));
    }
    case SQ_PEEK:
        put6(p2);
        return 0;
    case SQ_POKE:
        if (p2 < SO_EVENTS && p2 + (unsigned)a <= SO_EVENTS && p2 >= SO_RPB) {
            if (a == 2) w16(p2, p3); else w8(p2, p3 & 0xFF);
            edits++;
            tempo_set();
            return 0;
        }
        return ERR_RANGE;
    case SQ_CELL:
        if (seq_flags & SQS_PLAY) take_merge();
        return (unsigned)song_cell_set(p2 >> 8, p2 & 0xFF, a, p3 >> 8, p3 & 0xFF);
    case SQ_ROWS: {
        int p = p2 >> 8, r0 = p2 & 0xFF, rows = p3 >> 8, lanes = p3 & 0xFF;
        if (rows * lanes * 2 > BUF_SIZE - BUF_DATA || p >= SONG_PATS) return ERR_RANGE;
        uint8_t *d = file_buf + BUF_DATA;
        for (int i = 0; i < rows * lanes * 2; i++) d[i] = 0;
        unsigned e = pat_start(p), n = pcnt(p);
        for (unsigned i = 0; i < n; i++, e += EV_BYTES) {
            int r = (int)p8(e) - r0, l = (int)p8(e + 1) - a;
            if (r < 0 || r >= rows || l < 0 || l >= lanes) continue;
            d[(r * lanes + l) * 2] = (uint8_t)p8(e + 2);
            d[(r * lanes + l) * 2 + 1] = (uint8_t)p8(e + 3);
        }
        for (unsigned i = 0; i < ntake; i++) {     /* (the take, as it will be) */
            pb_t *t = TAKE + i * 5;
            int r = t[1] - r0, l = t[2] - a;
            if (t[0] != p || r < 0 || r >= rows || l < 0 || l >= lanes) continue;
            d[(r * lanes + l) * 2] = t[3];
            d[(r * lanes + l) * 2 + 1] = t[4];
        }
        return 0;
    }
    case SQ_PLAY:
        seq_play(a, p2 >> 8, p2 & 0xFF, p3 & 1);
        return 0;
    case SQ_STOP:
        seq_stop();
        return 0;
    case SQ_CURSOR:
        cur_lane = (uint8_t)(a < SONG_LANES ? a : 0);
        if (!(seq_flags & SQS_PLAY)) { cur_ord = (uint8_t)(p2 >> 8); cur_row = (uint8_t)p2; }
        seq_flags = (uint8_t)((seq_flags & (SQS_PLAY | SQS_REC)) | (p3 & (SQS_EDIT | SQS_FOLLOW)));
        step_rows = (uint8_t)(p3 >> 8);
        if (!(seq_flags & SQS_FOLLOW)) nheld = keys_down = 0;
        return 0;
    case SQ_NEW:
        seq_stop();
        song_new();
        ntake = 0;
        full = 0;
        return 0;
    case SQ_PATOP:
        seq_stop();
        return (unsigned)song_patop(a, p2 >> 8, p2 & 0xFF, p3);
    case SQ_PATH: {
        char *n = (char *)file_buf + BUF_NAME;
        if (a >= SONG_PARTS) return ERR_RANGE;
        if (p2 == 0) { song_path_get(a, n, BUF_DATA - BUF_NAME); return 0; }
        file_buf[BUF_DATA - 1] = 0;
        return (unsigned)song_path_set(a, n);
    }
    case SQ_SAVE:
        return (unsigned)song_save();
    case SQ_LOAD:
        return (unsigned)song_load();
    case SQ_EXPORT:
        return (unsigned)song_export();
    case SQ_FREE: {
        unsigned used = (used_bytes() - SO_EVENTS) / EV_BYTES;
        COMM16(C_P1) = (uint16_t)((SONG_BYTES - used_bytes()) / EV_BYTES);
        COMM16(C_P2) = (uint16_t)used;
        return 0;
    }
    case SQ_MUTE: {
        if (a >= SONG_LANES) return ERR_RANGE;
        unsigned o = SO_MUTE + (a >> 3), bit = 1u << (a & 7);
        if (p2 != 0xFFFF) {
            w8(o, p2 ? (p8(o) | bit) : (p8(o) & ~bit));
            if (p2) lane_off(a);
            edits++;
        }
        return (p8(o) & bit) ? 1 : 0;
    }
    }
    return ERR_RANGE;
}

#ifdef MD_HOST
/* (tests/host: the pool itself) */
uint8_t *song_pool(void) { return (uint8_t *)POOL; }
unsigned song_used(void) { return used_bytes(); }
void song_take_merge(void) { take_merge(); }
unsigned song_ntake(void) { return ntake; }
int seq_pos(int *o, int *r) { *o = at_ord; *r = at_row; return seq_flags; }
void seq_advance_us(uint32_t us) { advance(us); }
#endif
