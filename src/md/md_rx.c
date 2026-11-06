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
volatile uint8_t rx_custom;             /* 1: our firmware (the interrupt calls rx_take) */
static uint8_t rx_tail;
static uint8_t last_port;               /* the port at the last direct read */
static uint8_t last;                    /* the last stock nibble decoded */
static uint8_t d_lo, d_hi, a_lo;
static uint8_t part;
static int seen;
static uint8_t hist[4];                 /* the last four stock nibbles (the hello) */
static uint8_t expect;                  /* ours: the strobe level of the next nibble */
static uint8_t hi_nib, have_hi;
static uint16_t quiet;                  /* ours: frames without a nibble */

static void set_link(uint16_t link)
{
    md_flags = (uint16_t)((md_flags & ~FL_LINK) | link);
    COMM16(C_FLAGS) = md_flags;
}

void rx_init(void)
{
    PAD2_CTRL = 0x80;                   /* all inputs, TH raises the external interrupt */
    VDP_CTRL = 0x8B08;                  /* VDP: external interrupts on (IE2) */
    last_port = PAD2_DATA;              /* whatever is there now is old */
    last = 0xFF;
    rx_tail = rx_head;
    rx_custom = 0;
    seen = 0;
}

/* ---- our firmware: a MIDI byte as two nibbles (bit 4 set: the high one).
 * Each nibble turns the strobe (TH) over; we answer on TR with the level we
 * took, and the Teensy waits for that before the next. ---- */

static void start_custom(void)
{
    expect = 0x40;                      /* the Teensy left TH low: the first nibble raises it */
    have_hi = 0;
    quiet = 0;
    PAD2_DATA = 0x00;                   /* TR low: "heard you" */
    PAD2_CTRL = 0xA0;                   /* TR an output, TH's interrupt on */
    rx_custom = 1;
    midi_init();
    set_link(LINK_MIDI);
}

static void stop_custom(void)
{
    rx_custom = 0;
    PAD2_CTRL = 0x80;                   /* let go of TR */
    midi_reset_voices();
    last = 0xFF;
    set_link(LINK_NONE);
    seen = 0;
}

/* from the interrupt (md_crt.s) and from rx_poll with interrupts off */
void rx_take(uint8_t v)
{
    if ((v & 0x40) != expect)
        return;
    rx_ring[rx_head++] = v;
    expect ^= 0x40;
    PAD2_DATA = (v & 0x40) ? 0x20 : 0x00;
}

uint16_t rx_bytes, rx_errors;           /* for the monitor: bytes from the Teensy
                                         * (not its keepalives), damaged ones */
uint8_t rx_pins[6];                     /* the Teensy's pins it saw move (its pin watch:
                                         * 0xF4, port << 3 | bit; ports B C D E F) */
static uint8_t pin_next;

static void custom_nibble(uint8_t v)
{
    quiet = 0;
    if (v & 0x10) {
        hi_nib = v & 15;
        have_hi = 1;
    } else if (have_hi) {
        have_hi = 0;
        uint8_t b = (uint8_t)(hi_nib << 4 | (v & 15));
        if (pin_next) {
            pin_next = 0;
            if (b < 40) rx_pins[b >> 3] |= (uint8_t)(1u << (b & 7));
            return;
        }
        if (b == 0xF4) { pin_next = 1; return; }
        if (b == 0xF9) rx_errors++;     /* (our firmware: the MIDI jack got a bad byte) */
        else if (b != 0xFE) rx_bytes++;
        midi_byte(b);
    }
}

/* ---- the stock GenMDM stream ---- */

static void nibble(uint8_t v)
{
    v |= 0x40;
    if (v == last)
        return;
    last = v;
    hist[0] = hist[1]; hist[1] = hist[2]; hist[2] = hist[3]; hist[3] = v;
    if (hist[0] == 0x69 && hist[1] == 0x7F && hist[2] == 0x6D && hist[3] == 0x7F) {
        start_custom();                 /* our firmware's hello */
        return;
    }
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
            set_link(LINK_STOCK);
        }
        break;
    }
}

void rx_poll(void)
{
    if (rx_custom) {
        IRQ_OFF();
        rx_take(PAD2_DATA);
        IRQ_ON();
        while (rx_tail != rx_head)
            custom_nibble(rx_ring[rx_tail++]);
        return;
    }
    while (rx_tail != rx_head && !rx_custom)
        nibble(rx_ring[rx_tail++]);
    uint8_t v = PAD2_DATA;
    if (v != last_port) {
        last_port = v;
        if (v & 0x40)
            nibble(v);
    }
}

/* once a frame */
void rx_frame(void)
{
    if (rx_custom && ++quiet > 120)     /* 2 s without a byte: the Teensy is gone */
        stop_custom();
}
