/* md_rx.c - port 2: the GenMDM interface.
 *
 * GenMDM's own Teensy firmware turns MIDI into finished sound-chip register
 * writes and sends each one through the port's six data lines as four
 * nibbles. Bits 5-4 say which nibble the low four bits are:
 *
 *   00  data, low      01  data, high
 *   10  address, low   11  address, high - the write happens now:
 *       0-B  a YM2612 register (address 0x00-0xBF) in the selected part
 *       C    the data byte goes to the PSG
 *       D    select part I (channels 1-3)    E  select part II (4-6)
 *
 * Bit 6 (TH) is not driven and reads 1. There is no handshake: the Teensy
 * holds each nibble for a moment and the console must look often enough.
 * (Learned from the commented disassembly of the GenMDM cartridge in
 * MiGeRA/genMDM-newbuild; this is our own code.) The original cartridge reads
 * the port in a tight loop and repeats a write for as long as its last nibble
 * stays; we act once per change of the port, which needs fewer reads (other
 * work can happen between them) and gives the same result.
 */
#include "md.h"

static uint8_t last;
static uint8_t d_lo, d_hi, a_lo;
static uint8_t part;
static int seen;

void rx_init(void)
{
    PAD2_CTRL = 0x00;                   /* all inputs */
    last = PAD2_DATA;                   /* whatever is there now is old */
    seen = 0;
}

void rx_poll(void)
{
    uint8_t v = PAD2_DATA;
    if (v == last)
        return;
    last = v;
    if (!(v & 0x40))
        return;
    uint8_t nib = v & 15;
    switch ((v >> 4) & 3) {
    case 0: d_lo = nib; break;
    case 1: d_hi = (uint8_t)(nib << 4); break;
    case 2: a_lo = nib; break;
    default:
        if (nib <= 0xB) {
            ym_write(part, (uint8_t)(nib << 4 | a_lo), d_hi | d_lo);
        } else if (nib == 0xC) {
            psg_write(d_hi | d_lo);
        } else if (nib == 0xD) {
            part = 0;
            break;
        } else if (nib == 0xE) {
            part = 1;
            break;
        } else {
            break;                      /* F: nothing in the GenMDM stream */
        }
        COMM16(C_ACT) = ++act_count;
        if (!seen) {
            seen = 1;
            md_flags = (uint16_t)((md_flags & ~FL_LINK) | LINK_STOCK);
            COMM16(C_FLAGS) = md_flags;
        }
        break;
    }
}
