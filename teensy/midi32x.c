/*
 * midi32x.c - firmware for the GenMDM interface's Teensy 2.0 (ATmega32U4)
 * for the 32X MIDI Controller.
 *
 * The stock GenMDM firmware turns MIDI into finished sound-chip register
 * writes. This one passes the MIDI itself through to the console, where the
 * 32X MIDI Controller's engine plays it (GenMDM's whole CC map, plus POLY
 * mode, the 32X's own voices, ...), with a handshake so nothing is lost.
 *
 * MIDI in: USB (the Teensy shows up as "GenMDM 32X") and the MIDI jack
 * (serial, 31250 baud, on the Teensy's pin 7 / PD2 as with stock firmware
 * 1.02 and later).
 *
 * The wiring is GenMDM's (from its 1.03 firmware):
 *   Teensy pins 0-3 (PB0-3)  the four data lines (DB9 pins 1-4)
 *   pin 4 (PB7)              bit 4, TL (DB9 pin 6)
 *   pin 5 (PD0)              TH, the strobe (DB9 pin 7)
 *   pin 6 (PD1)              bit 5, TR (DB9 pin 9): an INPUT here - the console
 *                            drives it to answer
 * The LED (pin 11, PD6) is on while the console answers, and blinks off for
 * MIDI.
 *
 * Hello: until the console answers, every 20 ms we send four nibbles the
 * way the stock firmware does (TH high 20 us, low 20 us each): bit 4 low with
 * 9, high with F, low with D, high with F. To the GenMDM cartridge that is
 * nothing (an address F). The 32X MIDI Controller recognises it and pulls TR
 * low: linked.
 *
 * Linked: each MIDI byte goes as two nibbles, the high one first (bit 4 set),
 * the low one with bit 4 clear. For each nibble we set the lines, then turn
 * TH over; the console takes it and sets TR to the level TH now has; then
 * the next. No answer for 2 s: back to the hello. Nothing to send for 300 ms:
 * an Active Sensing byte (FE), which the console ignores, so it knows we are
 * still here.
 */
#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/wdt.h>
#include <util/delay.h>
#include <stdint.h>
#include "usb_private.h"

/* ---- PJRC's usb.c asks for these two (from Teensyduino's pins_teensy.c) ---- */

static void disable_peripherals(void)
{
    EIMSK = 0; PCICR = 0; SPCR = 0; ACSR = 0; EECR = 0; ADCSRA = 0;
    TIMSK0 = 0; TIMSK1 = 0; TIMSK3 = 0; TIMSK4 = 0; UCSR1B = 0; TWCR = 0;
    DDRB = 0; DDRC = 0; DDRD = 0; DDRE = 0; DDRF = 0;
    PORTB = 0; PORTC = 0; PORTD = 0; PORTE = 0; PORTF = 0;
}

/* Teensy Loader asks for this to program us without the button */
void _reboot_Teensyduino_(void)
{
    cli();
    MCUSR &= ~(1 << WDRF);
    WDTCSR |= (1 << WDCE);
    WDTCSR = 0;
    _delay_ms(5);
    UDCON = 1;
    USBCON = (1 << FRZCLK);
    _delay_ms(15);
    disable_peripherals();
    __asm__ volatile ("jmp 0x7E00");
    for (;;) ;
}

void _restart_Teensyduino_(void)
{
    cli();
    disable_peripherals();
    _delay_ms(15);
    __asm__ volatile ("jmp 0");
    for (;;) ;
}

/* ---- time: timer 1 at 250 kHz (4 us a tick) ---- */

static uint16_t t_now(void) { return TCNT1; }
static uint16_t t_since(uint16_t t) { return (uint16_t)(TCNT1 - t); }
#define MS(n)   ((uint16_t)((n) * 250u))        /* (up to 262 ms) */

/* ---- the MIDI waiting to go ---- */

#define RING 1024
static uint8_t ring[RING];
static uint16_t r_head, r_tail;

static void push(uint8_t b)
{
    uint16_t next = (r_head + 1) & (RING - 1);
    if (next == r_tail) return;                 /* full: drop the newest */
    ring[r_head] = b;
    r_head = next;
}

static uint8_t led_off;                         /* 30 ms steps the LED stays off (activity) */

/* ---- the pin watch (a diagnostic): every pin we do not use, to see which
 * one the MIDI jack moves. Reported to the console as 0xF4, port << 3 | bit
 * (port 0-4: B C D E F); its MIDI monitor shows them. */
static const uint8_t watch_mask[5] = { 0x70, 0xC0, 0xBC, 0x40, 0xF3 };
static uint8_t pin_prev[5], pin_moved[5];

static void pins_watch(void)
{
    uint8_t v[5] = { PINB, PINC, PIND, PINE, PINF };
    for (uint8_t k = 0; k < 5; k++) {
        pin_moved[k] |= (uint8_t)((v[k] ^ pin_prev[k]) & watch_mask[k]);
        pin_prev[k] = v[k];
    }
}

static void poll_inputs(void)
{
    /* the MIDI jack */
    while (UCSR1A & (1 << RXC1)) {
        uint8_t st = UCSR1A, b = UDR1;      /* (the status first) */
        push(st & ((1 << FE1) | (1 << DOR1)) ? 0xF9 : b);   /* 0xF9: a damaged byte */
        led_off = 5;
    }
    /* USB: 4-byte event packets (cable/code index, then up to 3 bytes) */
    if (!usb_configuration) return;
    for (int n = 0; n < 16; n++) {
        uint8_t sreg = SREG;
        cli();
        UENUM = MIDI_RX_ENDPOINT;
        uint8_t c = UEINTX;
        if (!(c & (1 << RWAL))) {
            if (c & (1 << RXOUTI)) UEINTX = 0x6B;
            SREG = sreg;
            return;
        }
        uint8_t b0 = UEDATX, b1 = UEDATX, b2 = UEDATX, b3 = UEDATX;
        if (!(UEINTX & (1 << RWAL))) UEINTX = 0x6B;
        SREG = sreg;
        static const uint8_t len[16] = { 0, 0, 2, 3, 3, 1, 2, 3, 3, 3, 3, 3, 2, 2, 3, 1 };
        uint8_t k = len[b0 & 15];
        if (k >= 1) push(b1);
        if (k >= 2) push(b2);
        if (k >= 3) push(b3);
        led_off = 5;
    }
}

/* ---- the port ---- */

static uint8_t th;                              /* TH's level now */

static void lines(uint8_t nibble, uint8_t bit4)
{
    PORTB = (uint8_t)((PORTB & 0x70) | (nibble & 15) | (bit4 ? 0x80 : 0));
}

static void th_set(uint8_t level)
{
    if (level) PORTD |= 0x01; else PORTD &= (uint8_t)~0x01;
    th = level;
}

static uint8_t tr(void) { return (PIND & 0x02) ? 1 : 0; }

static void hello(void)
{
    static const uint8_t nib[4] = { 0x9, 0xF, 0xD, 0xF }, b4[4] = { 0, 1, 0, 1 };
    for (int i = 0; i < 4; i++) {
        lines(nib[i], b4[i]);
        _delay_us(2);
        th_set(1);
        _delay_us(20);
        th_set(0);
        _delay_us(20);
    }
}

int main(void)
{
    CLKPR = 0x80;                               /* full speed (16 MHz) */
    CLKPR = 0x00;
    DDRB = 0x8F;                                /* PB0-3, PB7 out */
    PORTB = 0x00;
    DDRD = 0x41;                                /* PD0 (TH), PD6 (LED) out */
    PORTD = 0x02 | 0xBC;                        /* pull-ups: PD1 (TR), PD2 (MIDI in), the rest watched */
    PORTB |= 0x70; PORTC |= 0xC0; PORTE |= 0x40; PORTF |= 0xF3;     /* (pull-ups: the pin watch) */
    UBRR1 = 31;                                 /* 16 MHz / 16 / 32 = 31250 baud */
    UCSR1A = 0;
    UCSR1C = 0x06;                              /* 8N1 */
    UCSR1B = (1 << RXEN1);
    TCCR1A = 0;
    TCCR1B = 0x03;                              /* timer 1: clock / 64 */
    usb_init();
    sei();

    uint8_t linked = 0, waiting = 0, have_low = 0, low = 0;
    uint16_t t_hello = t_now(), t_ack = 0, t_idle = t_now();
    uint8_t ack_ms = 0;                         /* (2 s counted in steps of 200 ms) */
    uint16_t t_led = t_now();
    uint8_t pin_steps = 0;
    pins_watch();
    for (uint8_t k = 0; k < 5; k++) pin_moved[k] = 0;

    for (;;) {
        poll_inputs();
        pins_watch();

        if (t_since(t_led) > MS(30)) {          /* the LED: on when linked, off a moment for MIDI */
            t_led = t_now();
            if (linked && !led_off) PORTD |= 0x40; else PORTD &= (uint8_t)~0x40;
            if (led_off) led_off--;
            if (++pin_steps >= 33) {            /* about once a second: the pins that moved */
                pin_steps = 0;
                for (uint8_t k = 0; k < 5; k++) {
                    for (uint8_t b = 0; b < 8; b++)
                        if (linked && (pin_moved[k] >> b & 1)) { push(0xF4); push((uint8_t)(k << 3 | b)); }
                    pin_moved[k] = 0;
                }
            }
        }

        if (!linked) {
            if (t_since(t_hello) < MS(20)) continue;
            t_hello = t_now();
            hello();
            for (int i = 0; i < 100 && tr(); i++) _delay_us(20);   /* up to 2 ms for an answer */
            if (!tr()) {
                linked = 1;
                waiting = 0;
                have_low = 0;
                r_tail = r_head;                /* what came before the link is stale */
                t_idle = t_now();
            }
            continue;
        }

        if (waiting) {
            if (tr() == th) {
                waiting = 0;
            } else if (t_since(t_ack) > MS(200)) {
                t_ack = t_now();
                if (++ack_ms >= 10) {           /* 2 s without an answer: gone */
                    linked = 0;
                    th_set(0);
                    PORTD &= (uint8_t)~0x40;
                    t_hello = t_now();
                }
            }
            continue;
        }

        uint8_t nibble, bit4;
        if (have_low) {
            nibble = low;
            bit4 = 0;
            have_low = 0;
        } else if (r_tail != r_head) {
            uint8_t b = ring[r_tail];
            r_tail = (r_tail + 1) & (RING - 1);
            nibble = b >> 4;
            bit4 = 1;
            low = b & 15;
            have_low = 1;
            t_idle = t_now();
        } else {
            if (t_since(t_idle) > MS(250)) push(0xFE);     /* still here */
            continue;
        }
        lines(nibble, bit4);
        _delay_us(1);
        th_set(!th);
        waiting = 1;
        t_ack = t_now();
        ack_ms = 0;
    }
}
