/* test_midi.c - the 68000's MIDI engine on the PC, against a fake sound chip:
 * the register writes for notes, CCs, bends, POLY and the pedal. */
#include <stdio.h>
#include <string.h>
#include "md.h"

uint8_t shadow[SH_PSG + 16];
volatile uint16_t act_count;
static uint8_t chip[2][256];
static uint8_t psg_last[64];
static int psg_n;

void ym_write(int part, uint8_t reg, uint8_t val) { chip[part][reg] = val; shadow[part * 256 + reg] = val; }
void ym_chip(int part, uint8_t reg, uint8_t val) { chip[part][reg] = val; }
void psg_write(uint8_t b) { if (psg_n < 64) psg_last[psg_n++] = b; }
void act_bump(void) { act_count++; }
uint16_t md_flags;
void flags_publish(void) {}

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static void send(int a, int b, int c)
{
    midi_byte((uint8_t)a);
    if (b >= 0) midi_byte((uint8_t)b);
    if (c >= 0) midi_byte((uint8_t)c);
}


static unsigned freq(int ch) { int p = ch / 3, c = ch % 3; return (unsigned)(chip[p][0xA4 + c] << 8 | chip[p][0xA0 + c]); }

int main(void)
{
    ms.region = 0;
    midi_init();
    /* a voice on channel 1: algorithm 4 (carriers OP2, OP4), TLs 30 20 10 0 */
    ym_write(0, 0xB0, 0x04);
    ym_write(0, 0x40, 30); ym_write(0, 0x48, 20); ym_write(0, 0x44, 10); ym_write(0, 0x4C, 0);

    /* middle C on MIDI channel 1 -> block 4, F-number 644 (NTSC) */
    send(0x90, 60, 127);
    CHECK(chip[0][0x28] == 0xF0, "key on ch1: %02X", chip[0][0x28]);
    CHECK(freq(0) == (4 << 11 | 644) || freq(0) == (4 << 11 | 643), "C4 freq %04X", freq(0));
    CHECK(chip[0][0x48] == 20 && chip[0][0x4C] == 0, "velocity 127: carriers as the patch (%d %d)", chip[0][0x48], chip[0][0x4C]);
    send(0x80, 60, 0);
    CHECK(chip[0][0x28] == 0x00, "key off");
    /* an octave up: block 5, same F-number */
    send(0x90, 72, 64);
    CHECK((freq(0) >> 11) == 5 && (freq(0) & 0x7FF) >= 643 && (freq(0) & 0x7FF) <= 644, "C5 %04X", freq(0));
    CHECK(chip[0][0x48] > 20 && chip[0][0x40] == 30, "velocity 64 lowers carriers only (%d %d)", chip[0][0x48], chip[0][0x40]);
    CHECK(shadow[0x48] == 20, "the shadow keeps the patch level");
    /* running status: note off as a note on with velocity 0 */
    midi_byte(60 + 12); midi_byte(0);
    CHECK(chip[0][0x28] == 0x00, "running status note off");

    /* channel 5 (part II, offset 1) */
    send(0x94, 69, 100);
    CHECK(chip[0][0x28] == 0xF5, "key on ch5: %02X", chip[0][0x28]);
    CHECK(chip[1][0xA5] != 0, "ch5 frequency in part II");
    send(0x84, 69, 0);

    /* CCs (GenMDM map) on MIDI channel 2 */
    send(0xB1, 14, 127);  CHECK((chip[0][0xB1] & 7) == 7, "CC14 algorithm 7");
    send(0xB1, 15, 64);   CHECK(((chip[0][0xB1] >> 3) & 7) == 4, "CC15 feedback 4");
    send(0xB1, 16, 127);  CHECK(chip[0][0x41] == 0, "CC16 TL op1 loudest");
    send(0xB1, 17, 0);    CHECK(chip[0][0x49] == 127, "CC17 TL op2 silent");
    send(0xB1, 20, 127);  CHECK((chip[0][0x31] & 15) == 15, "CC20 MUL op1");
    send(0xB1, 24, 73);   CHECK((chip[0][0x31] >> 4) == 4, "CC24 detune -0");
    send(0xB1, 43, 127);  CHECK((chip[0][0x51] & 31) == 31, "CC43 AR");
    send(0xB1, 39, 127);  CHECK((chip[0][0x51] >> 6) == 3 && (chip[0][0x51] & 31) == 31, "CC39 RS keeps AR");
    send(0xB1, 70, 127);  CHECK(chip[0][0x61] & 0x80, "CC70 AM on");
    send(0xB1, 90, 36);   CHECK(chip[0][0x91] == 9, "CC90 SSG shape 1");
    send(0xB1, 77, 40);   CHECK((chip[0][0xB5] >> 6) == 1, "CC77 pan");
    send(0xB1, 74, 127);  CHECK(chip[0][0x22] & 8, "CC74 LFO on");
    send(0xB1, 1, 127);   CHECK((chip[0][0x22] & 7) == 7 && (chip[0][0x22] & 8), "CC1 LFO speed");

    /* pitch bend: +2 semitones (range 2) moves the frequency up */
    send(0x90, 60, 100);
    unsigned f0 = freq(0);
    send(0xE0, 0x7F, 0x7F);
    CHECK(freq(0) > f0, "bend up %04X > %04X", freq(0), f0);
    send(0xE0, 0x00, 0x40);
    CHECK(freq(0) == f0, "bend back");
    send(0x80, 60, 0);

    /* PSG: MIDI channel 7 -> tone 1 */
    psg_n = 0;
    send(0x96, 69, 127);
    CHECK(psg_n >= 3 && (psg_last[0] & 0xF0) == 0x80 && (psg_last[2] & 0xF0) == 0x90 && (psg_last[2] & 15) == 0,
          "PSG tone + volume (%d writes)", psg_n);
    unsigned n = (psg_last[0] & 15) | (psg_last[1] & 0x3F) << 4;
    CHECK(n >= 252 && n <= 256, "A4 period %u", n);   /* 3579545 / 32 / 440 = 254 */

    /* POLY on MIDI channel 1: six notes take six channels, a seventh steals the oldest */
    ms.mode = MODE_POLY; ms.poly_ch = 0; ms.spread = 0;
    midi_reset_voices();
    int used = 0;
    for (int i = 0; i < 6; i++) { send(0x90, 60 + i, 100); used |= 1 << (chip[0][0x28] & 7); }
    CHECK(used == (1 << 0 | 1 << 1 | 1 << 2 | 1 << 4 | 1 << 5 | 1 << 6), "six voices %02X", used);
    send(0x90, 80, 100);
    CHECK((chip[0][0x28] & 0xF0) == 0xF0, "seventh note keys on");
    /* the pedal holds a released note */
    midi_reset_voices();
    send(0xB0, 64, 127);
    send(0x90, 62, 100);
    int code = chip[0][0x28] & 7;
    send(0x80, 62, 0);
    CHECK(chip[0][0x28] == (0xF0 | code), "held by the pedal");
    send(0xB0, 64, 0);
    CHECK(chip[0][0x28] == code, "released with the pedal");

    /* MIDI channels 11-16 go to the 32X's voices: queued, not played here */
    ms.mode = MODE_GENMDM;
    uint8_t h0 = (uint8_t)(md_flags >> 8);
    chip[0][0x28] = 0x55;
    send(0x9F, 36, 100);
    send(0xBA, 7, 90);
    CHECK((uint8_t)(md_flags >> 8) == (uint8_t)(h0 + 2), "two events queued");
    CHECK(ev_ring[h0][0] == 0x9F && ev_ring[h0][1] == 36 && ev_ring[h0][2] == 100, "the note");
    CHECK(ev_ring[(uint8_t)(h0 + 1)][0] == 0xBA && ev_ring[(uint8_t)(h0 + 1)][2] == 90, "the CC");
    CHECK(chip[0][0x28] == 0x55, "nothing on the YM2612");

    /* the monitor: channels heard, the last key per channel, the preview left out */
    midi_init();
    mon_chans = 0;
    send(0x92, 64, 90);
    CHECK(mon_chans == 1 << 2, "channel 3 heard: %04X", mon_chans);
    CHECK(mon_ch[2][0] == 64 && mon_ch[2][1] == 90 && mon_ch[2][2] == 1, "ch3 key %d %d %d",
          mon_ch[2][0], mon_ch[2][1], mon_ch[2][2]);
    CHECK(mon_last[0] == 0x92 && mon_last[1] == 64 && mon_last[2] == 90 && mon_last[3] == 3, "last message");
    send(0x82, 64, 0);
    CHECK(mon_ch[2][0] == 64 && mon_ch[2][1] == 0 && mon_ch[2][2] == 0, "ch3 let go");
    CHECK(mon_ch[0][0] == 0xFF, "ch1 nothing yet");
    mon_chans = 0;
    midi_quiet = 1;
    send(0x90, 60, 110);
    send(0x80, 60, 0);
    midi_quiet = 0;
    CHECK(mon_chans == 0 && mon_ch[0][0] == 0xFF, "the preview is not monitored");
    uint16_t n0 = mon_count;
    send(0xE5, 0, 64);
    CHECK(mon_count == n0 + 1 && mon_chans == 1 << 5 && mon_last[3] == 3, "bend counted");

    printf(fails ? "test_midi: %d failure(s)\n" : "ok   MIDI engine: notes, CCs, bend, PSG, POLY, pedal%.0d\n", fails);
    return fails != 0;
}
