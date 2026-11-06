/* hw.c - the 32X hardware layer (from SegaTube's, which came from ZERO
 * SECTOR's, both tested on a console). */
#include "hw.h"

#define SPIN_LIMIT 4000000

/* ------------------------------------------------------------ frame buffers */

static void wait_fb_ready(void)
{
    for (int i = 0; i < SPIN_LIMIT && (MARS_VDP_FBCTL & FBCTL_FEN); i++)
        ;
}

void hw_wait_vblank(void)
{
    int i = 0;
    while (i++ < SPIN_LIMIT && (MARS_VDP_FBCTL & FBCTL_VBLK))
        ;
    while (i++ < SPIN_LIMIT && !(MARS_VDP_FBCTL & FBCTL_VBLK))
        ;
}

int hw_in_vblank(void)
{
    return (MARS_VDP_FBCTL & FBCTL_VBLK) != 0;
}

/* Show the other buffer from the next vblank; return at the start of it. */
void hw_flip(void)
{
    uint16_t want = (uint16_t)((MARS_VDP_FBCTL & FBCTL_FS) ^ 1);
    MARS_VDP_FBCTL = want;
    for (int i = 0; i < SPIN_LIMIT && (MARS_VDP_FBCTL & FBCTL_FS) != want; i++)
        ;
    wait_fb_ready();
}

uint8_t *hw_row(int r)
{
    return FB_BYTES + 0x200 + r * SCREEN_W;
}

void hw_video_init(void)
{
    MARS_VDP_MODE = VDP_MODE_BLANK;
    for (int k = 0; k < 2; k++) {
        volatile uint16_t *fb = MARS_FRAMEBUF;
        wait_fb_ready();
        for (int line = 0; line < 256; line++)          /* line table: row L at 0x200 + L*320 */
            fb[line] = (uint16_t)(0x100 + (line < SCREEN_H ? line : SCREEN_H - 1) * (SCREEN_W / 2));
        volatile uint32_t *px = (volatile uint32_t *)(void *)(FB_BYTES + 0x200);
        for (int i = 0; i < SCREEN_W * SCREEN_H / 4; i++)
            px[i] = 0;
        hw_flip();
    }
    MARS_VDP_SHIFT = 0;
    MARS_VDP_MODE = VDP_MODE_PACKED;
}

void hw_load_palette(const uint16_t *pal, int n)
{
    for (int i = 0; i < n; i++)
        MARS_CRAM[i] = pal[i];
}

void hw_init(void)
{
    MARS_VDP_MODE = VDP_MODE_BLANK;
    volatile uint32_t *dmaor = (volatile uint32_t *)0xFFFFFFB0;
    *dmaor = 1;                                 /* DMAC on */
    hw_timer_init();
}

/* ------------------------------------------------------------ the 68000 */

static uint16_t cmd_seq, pad_ack;

int md_cmd(int cmd, uint16_t p1, uint16_t p2, uint16_t p3)
{
    cmd_seq = (uint16_t)((cmd_seq + 1) & 7);
    MARS_COMM(C_P1) = p1;
    MARS_COMM(C_P2) = p2;
    MARS_COMM(C_P3) = p3;
    MARS_COMM(C_CMD) = (uint16_t)(pad_ack << 8 | cmd_seq << 5 | (cmd & 31));
    for (;;) {
        uint16_t r = MARS_COMM(C_REPLY);
        if (((r >> 8) & 7) == cmd_seq)
            return r & 0xFF;
    }
}

uint16_t md_p(int which)
{
    return MARS_COMM(which);
}

uint16_t hw_pad(void)
{
    uint16_t v = MARS_COMM(C_PAD);
    pad_ack = (uint16_t)((pad_ack + 1) & 0xFF);
    MARS_COMM(C_CMD) = (uint16_t)(pad_ack << 8 | (MARS_COMM(C_CMD) & 0xFF));
    return v & 0x0FFF;
}

uint16_t hw_flags(void)
{
    return MARS_COMM(C_FLAGS);
}

uint16_t hw_activity(void)
{
    return MARS_COMM(C_ACT);
}

/* ------------------------------------------------------------ the timer */

#define FRT_FTCSR   (*(volatile uint8_t *)0xFFFFFE11)
#define FRT_FRCH    (*(volatile uint8_t *)0xFFFFFE12)
#define FRT_FRCL    (*(volatile uint8_t *)0xFFFFFE13)
#define FRT_TCR     (*(volatile uint8_t *)0xFFFFFE16)

void hw_timer_init(void)
{
    FRT_TCR = 0x02;             /* clock / 128 */
    FRT_FTCSR = 0x00;           /* count freely (crt0 set it to clear at OCRA = 2) */
}

uint16_t hw_ticks(void)
{
    uint8_t h = FRT_FRCH;       /* (the high byte first: it latches the low one) */
    return (uint16_t)(h << 8 | FRT_FRCL);
}

/* ------------------------------------------------------------ caches */

void hw_cache_purge(void)
{
    volatile uint8_t *ccr = (volatile uint8_t *)0xFFFFFE92;
    *ccr = 0x10;    /* purge + disable */
    *ccr = 0x11;    /* purge + enable */
}

/* ------------------------------------------------------------ the slave */

typedef struct {
    volatile slave_fn fn;
    volatile int      arg;
    volatile int      busy;
} mailbox_t;

static mailbox_t mailbox_storage __attribute__((aligned(16)));
#define MAILBOX ((mailbox_t *)SDRAM_THROUGH(&mailbox_storage))

void hw_slave_start(slave_fn fn, int arg)
{
    mailbox_t *mb = MAILBOX;
    hw_slave_wait();
    mb->arg = arg;
    mb->busy = 1;
    mb->fn = fn;
}

int hw_slave_wait(void)
{
    mailbox_t *mb = MAILBOX;
    int i = 0;
    for (; i < SPIN_LIMIT * 4 && mb->busy; i++)
        ;
    return i;
}

/* Slave SH-2 entry (crt0.s): run the jobs the master posts. */
void slave_main(void)
{
    mailbox_t *mb = MAILBOX;
    for (;;) {
        slave_fn fn = mb->fn;
        if (!fn) {
            for (int d = 0; d < 64; d++)    /* keep off the bus while idle */
                __asm__ volatile ("nop");
            continue;
        }
        mb->fn = 0;
        hw_cache_purge();
        fn(mb->arg);
        mb->busy = 0;
    }
}
