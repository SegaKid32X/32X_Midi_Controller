/* md_snd.c - the YM2612 and the PSG, and a shadow of every register.
 *
 * The 68000 holds the Z80 off the bus and writes the YM2612 itself (as the
 * GenMDM cartridge does). Every write also lands in `shadow`, so the 32X can
 * show the voices and save them as patch files: the YM2612's registers
 * cannot be read back.
 */
#include "md.h"

uint8_t shadow[SH_PSG + 16];
volatile uint16_t act_count;
static uint8_t psg_latch;           /* the last latch byte: channel and kind */

static inline void ym_wait(void)
{
    for (int i = 0; i < 256 && (YM_A0 & 0x80); i++)
        ;
}

/* the voices that started a note since the screen last asked (CMD_MONITOR):
 * bits 0-5 FM 1-6, 6-8 PSG tone 1-3, 9 noise */
uint16_t mon_voices;
/* for the visualizer (CMD_VIZ): FM channels keyed on now, and those keyed on
 * since it last asked (bit n: channel n + 1) */
uint8_t viz_keyed, viz_keyev;

void ym_write(int part, uint8_t reg, uint8_t val)
{
    if (!part && reg == 0x28 && (val & 3) != 3) {
        uint8_t bit = (uint8_t)(1u << ((val & 3) + (val & 4 ? 3 : 0)));
        if (val & 0xF0) {                                           /* a key on */
            mon_voices |= bit;
            viz_keyed |= bit;
            viz_keyev |= bit;
        } else {
            viz_keyed &= (uint8_t)~bit;
        }
    }
    ym_wait();
    if (part) {
        YM_A1 = reg;
        ym_wait();
        YM_D1 = val;
        shadow[SH_PART2 + reg] = val;
    } else {
        YM_A0 = reg;
        ym_wait();
        YM_D0 = val;
        shadow[reg] = val;
    }
}

void ym_chip(int part, uint8_t reg, uint8_t val)
{
    ym_wait();
    if (part) {
        YM_A1 = reg;
        ym_wait();
        YM_D1 = val;
    } else {
        YM_A0 = reg;
        ym_wait();
        YM_D0 = val;
    }
}

void psg_write(uint8_t b)
{
    PSG_PORT = b;
    if (b & 0x80)
        psg_latch = b;
    int ch = (psg_latch >> 5) & 3;
    uint8_t *p = shadow + SH_PSG;
    if (psg_latch & 0x10) {                 /* volume */
        if ((b & 15) != 15 && (b & 15) < p[8 + ch])     /* louder: a note */
            mon_voices |= (uint16_t)(1u << (6 + ch));
        p[8 + ch] = b & 15;
    } else if (ch == 3) {                   /* noise control */
        p[12] = b & 7;
    } else if (b & 0x80) {                  /* tone, low 4 bits */
        uint16_t t = (uint16_t)((p[2 * ch] << 8 | p[2 * ch + 1]) & 0x3F0) | (b & 15);
        p[2 * ch] = (uint8_t)(t >> 8);
        p[2 * ch + 1] = (uint8_t)t;
    } else {                                /* tone, high 6 bits */
        uint16_t t = (uint16_t)((b & 0x3F) << 4) | ((p[2 * ch] << 8 | p[2 * ch + 1]) & 15);
        p[2 * ch] = (uint8_t)(t >> 8);
        p[2 * ch + 1] = (uint8_t)t;
    }
}

void flags_publish(void)
{
    COMM16(C_FLAGS) = md_flags;
}

void act_bump(void)
{
    COMM16(C_ACT) = ++act_count;
}

void snd_silence(void)
{
    for (int ch = 0; ch < 7; ch++)
        if (ch != 3)
            ym_write(0, 0x28, (uint8_t)ch);         /* key off */
    for (int ch = 0; ch < 4; ch++)
        psg_write((uint8_t)(0x9F | ch << 5));
}

/* A plain electric-piano-like voice on every channel, so a key played before
 * any patch is loaded makes a sound (op order in registers: S1 S3 S2 S4). */
static const uint8_t default_voice[][2] = {
    { 0x30, 0x71 }, { 0x34, 0x0D }, { 0x38, 0x33 }, { 0x3C, 0x01 },     /* DT/MUL */
    { 0x40, 0x23 }, { 0x44, 0x2D }, { 0x48, 0x26 }, { 0x4C, 0x00 },     /* TL */
    { 0x50, 0x5F }, { 0x54, 0x99 }, { 0x58, 0x5F }, { 0x5C, 0x94 },     /* RS/AR */
    { 0x60, 0x05 }, { 0x64, 0x05 }, { 0x68, 0x05 }, { 0x6C, 0x07 },     /* AM/D1R */
    { 0x70, 0x02 }, { 0x74, 0x02 }, { 0x78, 0x02 }, { 0x7C, 0x02 },     /* D2R */
    { 0x80, 0x11 }, { 0x84, 0x11 }, { 0x88, 0x11 }, { 0x8C, 0xA6 },     /* SL/RR */
    { 0x90, 0x00 }, { 0x94, 0x00 }, { 0x98, 0x00 }, { 0x9C, 0x00 },     /* SSG-EG */
    { 0xB0, 0x32 }, { 0xB4, 0xC0 },                                     /* FB/ALG, L R */
};

void snd_init(void)
{
    /* the Z80 stays in reset with its bus handed to us */
    Z80_RESET = 0x0000;
    Z80_BUSREQ = 0x0100;
    Z80_RESET = 0x0100;
    for (int i = 0; i < 1000 && (Z80_BUSREQ & 0x0100); i++)
        ;
    for (unsigned i = 0; i < sizeof shadow; i++)
        shadow[i] = 0;
    ym_write(0, 0x22, 0x00);                /* LFO off */
    ym_write(0, 0x27, 0x00);                /* channel 3 normal, timers off */
    ym_write(0, 0x2B, 0x00);                /* DAC off */
    for (int ch = 0; ch < 6; ch++) {
        int part = ch / 3, c = ch % 3;
        for (unsigned k = 0; k < sizeof default_voice / sizeof default_voice[0]; k++)
            ym_write(part, (uint8_t)(default_voice[k][0] + c), default_voice[k][1]);
        ym_write(part, (uint8_t)(0xA4 + c), 0x22);  /* a note's frequency */
        ym_write(part, (uint8_t)(0xA0 + c), 0x69);
    }
    for (int i = 0; i < 6; i++)
        shadow[SH_PSG + i] = 0;
    for (int ch = 0; ch < 4; ch++)
        psg_write((uint8_t)(0x9F | ch << 5));
    snd_silence();
}
