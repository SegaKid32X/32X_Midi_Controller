/* test_link.c - our Teensy firmware's link to the console, simulated: the
 * console's real receiver (src/md/md_rx.c) against a model of the Teensy
 * that follows teensy/midi32x.c step by step. Checks that every MIDI byte
 * arrives once and in order however the console's interrupt and polling
 * fall, and that the stock GenMDM stream still decodes. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "md.h"

uint8_t host_pad2_data, host_pad2_ctrl, host_pad1;
uint16_t host_vdp, host_comm[8];
uint16_t md_flags;
uint8_t shadow[SH_PSG + 16];
volatile uint16_t act_count;
midi_settings_t ms;

static uint8_t got[4096];
static int ngot;
void midi_byte(uint8_t b) { if (b != 0xFE && b < 0xF8) got[ngot++] = b; else if (b == 0xFE) {} }
void midi_init(void) {}
void midi_reset_voices(void) {}
static int ym_writes, last_reg, last_val;
void ym_write(int part, uint8_t reg, uint8_t val) { ym_writes++; last_reg = part << 8 | reg; last_val = val; }
void psg_write(uint8_t b) { (void)b; }
void set_flag(uint16_t f, int on) { md_flags = on ? (md_flags | f) : (md_flags & ~f); }

/* ---- the lines ---- */
static uint8_t t_lines;         /* the Teensy's: bits 0-4, and TH at bit 6 */
static uint8_t c_tr = 1;        /* the console's TR output (when driven) */
static int stock_tr = -1;       /* the stock firmware drives TR (bit 5) itself */

static int t_drive = 1;         /* the Teensy drives the data lines (bits 0-3) */
static uint8_t c_data;          /* the console's data lines, when it drives them */
static int clashes;             /* both drove them at once */

static uint8_t port_value(void)
{
    uint8_t tr = stock_tr >= 0 ? (uint8_t)stock_tr
               : (host_pad2_ctrl & 0x20) ? c_tr : 1;     /* not driven: pulled up */
    int c_drive = (host_pad2_ctrl & 0x0F) != 0;
    if (t_drive && c_drive) clashes++;
    uint8_t data = t_drive ? (t_lines & 0x0F) : c_drive ? c_data : 0x0F;
    return (uint8_t)((t_lines & 0x50) | data | (tr << 5));
}

static void console_wrote(uint8_t before)
{
    if (host_pad2_data != before || (host_pad2_ctrl & 0x0F)) {
        c_tr = (host_pad2_data >> 5) & 1;
        c_data = host_pad2_data & 0x0F;
    }
}

/* the console reads the port; a write to it sets TR */
static void console_isr(void)
{
    host_pad2_data = port_value();
    uint8_t before = host_pad2_data;
    if (!rx_custom) {
        rx_ring[rx_head++] = host_pad2_data;
    } else {
        rx_take(host_pad2_data);
    }
    console_wrote(before);
}

static void console_poll(void)
{
    host_pad2_data = port_value();
    uint8_t before = host_pad2_data;
    rx_poll();
    if (host_pad2_ctrl & 0x20) console_wrote(before);
}

static int edge_mode;           /* interrupts on: 0 rising, 1 falling, 2 none */
static int poll_every;          /* the console polls once in this many steps */
static unsigned rnd = 1;
static int chance(int n) { rnd = rnd * 1103515245u + 12345u; return (int)((rnd >> 16) % (unsigned)n) == 0; }

static void th_set(int level)
{
    int old = (t_lines >> 6) & 1;
    t_lines = (uint8_t)((t_lines & ~0x40) | (level << 6));
    if (old != level && ((edge_mode == 0 && level) || (edge_mode == 1 && !level)))
        console_isr();
}

static void step(void)
{
    if (chance(poll_every)) console_poll();
}

static void teensy_lines(int nib, int bit4) { t_lines = (uint8_t)((t_lines & 0x40) | nib | (bit4 << 4)); }

static int console_tr(void)
{
    return (host_pad2_ctrl & 0x20) ? c_tr : 1;
}

/* the Teensy, as teensy/midi32x.c: hello until answered, then the bytes */
static int send_all(const uint8_t *b, int n, int max_steps)
{
    int linked = 0, i = 0, th = 0, steps = 0;
    while (steps < max_steps) {
        if (!linked) {
            static const int nib[4] = { 9, 15, 13, 15 }, b4[4] = { 0, 1, 0, 1 };
            for (int k = 0; k < 4; k++) {
                teensy_lines(nib[k], b4[k]);
                th_set(1); for (int s = 0; s < 3; s++) step();
                th_set(0); for (int s = 0; s < 3; s++) step();
            }
            th = 0;
            for (int w = 0; w < 100 && console_tr(); w++) { step(); steps++; }
            if (!console_tr()) linked = 1;
            for (int w = 0; w < 50 && !linked; w++) step();
            continue;
        }
        if (i >= 2 * n) return 1;
        int byte = b[i / 2], hi = !(i & 1);
        teensy_lines(hi ? byte >> 4 : byte & 15, hi);
        th = !th;
        th_set(th);
        int w = 0;
        while (console_tr() != th && w < 100000) { step(); w++; steps++; }
        if (console_tr() != th) return 0;
        i++;
    }
    return 0;
}

/* MIDI out: the Teensy reads what the console has for it in windows (0xF5,
 * the lines let go, the count, the nibbles, the lines back), one after
 * every few bytes it sends and more at the end. The console's main loop
 * hands it bytes as it goes. */
static uint8_t out_got[4096];
static int nout_got, out_sent;
static const uint8_t *out_src;
static int out_n;

static int strobe(int *th)                  /* TH over, wait for the answer */
{
    *th = !*th;
    th_set(*th);
    int w = 0;
    while (console_tr() != *th && w < 100000) {
        step();
        if (out_sent < out_n && out_sent - nout_got < 200 && chance(7)) midi_out(out_src[out_sent++]);
        w++;
    }
    return console_tr() == *th;
}

static int send_byte(int b, int *th)
{
    teensy_lines(b >> 4, 1);
    if (!strobe(th)) return 0;
    teensy_lines(b & 15, 0);
    return strobe(th);
}

static int read_window(int *th)
{
    if (!send_byte(0xF5, th)) return -1;
    t_drive = 0;
    if (!strobe(th)) return -1;
    int n = port_value() & 15;
    if (n > 7) return -1;
    for (int k = 0; k < n; k++) {
        if (!strobe(th)) return -1;
        int hi = port_value() & 15;
        if (!strobe(th)) return -1;
        out_got[nout_got++] = (uint8_t)(hi << 4 | (port_value() & 15));
    }
    if (!strobe(th)) return -1;
    if (host_pad2_ctrl & 0x0F) return -1;       /* the console must have let go */
    t_drive = 1;
    return n;
}

static int both_ways(const uint8_t *in, int n)
{
    int th = 0;
    /* linked already (send_all's hello): carry on from TH low */
    for (int i = 0; i < n; i++) {
        if (!send_byte(in[i], &th)) return 0;
        if (i % 5 == 4 && read_window(&th) < 0) return 0;
        if (out_sent < out_n && out_sent - nout_got < 200 && chance(3)) midi_out(out_src[out_sent++]);
    }
    for (int k = 0; k < 2000 && nout_got < out_n; k++) {
        if (out_sent < out_n && out_sent - nout_got < 200) midi_out(out_src[out_sent++]);
        if (read_window(&th) < 0) return 0;
    }
    return 1;
}

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static void reset_console(void)
{
    rx_init();
    md_flags = 0;
    ngot = 0;
    c_tr = 1;
    t_lines = 0x40 | 0x1F;
}

int main(void)
{
    uint8_t msg[600];
    for (int i = 0; i < (int)sizeof msg; i++)
        msg[i] = (uint8_t)(i % 3 == 0 ? 0x90 | (i & 15) : (i * 37) & 0x7F);
    for (edge_mode = 0; edge_mode < 3; edge_mode++)
        for (poll_every = 1; poll_every <= 64; poll_every *= 4) {
            reset_console();
            int ok = send_all(msg, sizeof msg, 50000000);
            for (int k = 0; k < 20; k++) console_poll();
            CHECK(ok, "edge %d poll %d: the Teensy got no answer", edge_mode, poll_every);
            CHECK((md_flags & FL_LINK) == LINK_MIDI, "linked");
            CHECK(ngot == (int)sizeof msg && !memcmp(got, msg, sizeof msg),
                  "edge %d poll %d: %d of %d bytes, in order: %d", edge_mode, poll_every, ngot,
                  (int)sizeof msg, !memcmp(got, msg, (size_t)(ngot < (int)sizeof msg ? ngot : (int)sizeof msg)));
        }
    /* both ways: MIDI in as before, MIDI out read back in windows */
    for (edge_mode = 0; edge_mode < 3; edge_mode++)
        for (poll_every = 1; poll_every <= 64; poll_every *= 4) {
            reset_console();
            static const uint8_t none[1] = { 0x90 };
            int ok = send_all(none, 0, 50000000);
            CHECK(ok && rx_custom, "linked for both ways");
            uint8_t out[300];
            for (int i = 0; i < 300; i++) out[i] = (uint8_t)(i % 3 == 0 ? 0x80 | (i & 31) : (i * 13) & 0x7F);
            out_src = out; out_n = 300; out_sent = 0; nout_got = 0; clashes = 0;
            ngot = 0;
            ok = both_ways(msg, 300);
            for (int k = 0; k < 20; k++) console_poll();
            CHECK(ok, "edge %d poll %d: a window stalled", edge_mode, poll_every);
            CHECK(!clashes, "edge %d poll %d: both drove the data lines %d times", edge_mode, poll_every, clashes);
            CHECK(ngot == 300 && !memcmp(got, msg, 300), "edge %d poll %d: MIDI in %d of 300", edge_mode, poll_every, ngot);
            CHECK(nout_got == 300 && !memcmp(out_got, out, 300), "edge %d poll %d: MIDI out %d of 300", edge_mode, poll_every, nout_got);
        }
    /* the stock GenMDM stream still decodes: YM write 0x28 = 0xF0 */
    reset_console();
    edge_mode = 0;
    static const uint8_t stock[4] = { 0x40 | 0x00, 0x40 | 0x10 | 0xF, 0x40 | 0x20 | 0x8, 0x40 | 0x30 | 0x2 };
    for (int k = 0; k < 4; k++) {
        t_lines = (uint8_t)(stock[k] & 0x1F);
        stock_tr = (stock[k] >> 5) & 1;
        th_set(1); console_poll(); th_set(0); console_poll();
    }
    CHECK(ym_writes == 1 && last_reg == 0x28 && last_val == 0xF0, "stock write: %d writes, %03X=%02X",
          ym_writes, last_reg, last_val);
    CHECK((md_flags & FL_LINK) == LINK_STOCK, "stock link");
    CHECK(!(host_pad2_ctrl & 0x20), "TR left alone with the stock firmware");
    printf(fails ? "test_link: %d failure(s)\n" : "ok   Teensy link: every byte once, in order (both edges, slow polls), stock stream%.0d\n", fails);
    return fails != 0;
}
