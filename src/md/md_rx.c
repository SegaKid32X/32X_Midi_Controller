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
 * Bit 6 (TH) is the strobe: the Teensy sets the nibble, raises TH for 18 us,
 * lowers it for 18 us, then sets the next one; a nibble counts while TH is
 * high (from the GenMDM firmware 1.03 itself: pins 0-3 data, 4 and 6 the
 * nibble kind, 5 the strobe, a 18 us delay). There is no handshake.
 *
 * TH also raises the 68000's external interrupt (level 2): its handler
 * (md_crt.s) copies the port into a ring at each strobe, so a nibble is caught
 * even while the main loop is busy. rx_poll() decodes the ring, and also
 * reads the port itself (the emulators do not raise that interrupt for a
 * pad). The decoder ignores a value equal to the one before it, which is how
 * a nibble caught twice counts once: two nibbles in a row always differ in
 * their kind bits. (The protocol was first learned from MiGeRA's commented
 * disassembly of the GenMDM cartridge, genMDM-newbuild; this is our own code.)
 */
#include "md.h"

uint8_t rx_ring[256];
volatile uint8_t rx_head;
static uint8_t rx_tail;
static uint8_t last_port;               /* the port at the last direct read */
static uint8_t last;                    /* the last nibble decoded */
static uint8_t d_lo, d_hi, a_lo;
static uint8_t part;
static int seen;

void rx_init(void)
{
    PAD2_CTRL = 0x80;                   /* all inputs, TH raises the external interrupt */
    VDP_CTRL = 0x8B08;                  /* VDP: external interrupts on (IE2) */
    last_port = PAD2_DATA;              /* whatever is there now is old */
    last = 0xFF;
    rx_tail = rx_head;
    seen = 0;
}

static void nibble(uint8_t v)
{
    v |= 0x40;
    if (v == last)
        return;
    last = v;
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

void rx_poll(void)
{
    while (rx_tail != rx_head)
        nibble(rx_ring[rx_tail++]);
    uint8_t v = PAD2_DATA;
    if (v != last_port) {
        last_port = v;
        if (v & 0x40)
            nibble(v);
    }
}
