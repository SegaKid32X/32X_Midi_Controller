/* md_main.c - the 68000's main loop.
 *
 * Boot: set up the sound chips and port 2, wait for the 32X boot ROMs'
 * "M_OK"/"S_OK" and release the master SH-2. Then, forever:
 *   - read port 2 (the GenMDM interface) - between every other step, since a
 *     stock GenMDM gives no handshake;
 *   - at each vblank, read pad 1 a step at a time (presses are latched until
 *     the SH-2 acknowledges them);
 *   - carry out the master SH-2's commands (src/common/comm.h).
 */
#include "md.h"

uint16_t md_flags;
static uint16_t latched, last_pad, last_ack;
static int pad6;

#define VDP_STATUS_VB   0x0008

void set_flag(uint16_t f, int on)
{
    md_flags = on ? (md_flags | f) : (md_flags & (uint16_t)~f);
    COMM16(C_FLAGS) = md_flags;
}

/* Pad 1, 3- or 6-button, with the port 2 receiver run between the steps.
 * Bits: U1 D2 L4 R8 B10 C20 A40 START80, 6-button Z100 Y200 X400 MODE800. */
static uint16_t pad_read(void)
{
    PAD1_DATA = 0x40; rx_poll(); rx_poll();
    uint8_t hi = (uint8_t)~PAD1_DATA;
    PAD1_DATA = 0x00; rx_poll(); rx_poll();
    uint8_t lo = (uint8_t)~PAD1_DATA;
    PAD1_DATA = 0x40; rx_poll();
    PAD1_DATA = 0x00; rx_poll();
    PAD1_DATA = 0x40; rx_poll();
    PAD1_DATA = 0x00; rx_poll(); rx_poll();
    uint8_t id = (uint8_t)~PAD1_DATA;
    PAD1_DATA = 0x40; rx_poll(); rx_poll();
    uint8_t ext = (uint8_t)~PAD1_DATA;
    PAD1_DATA = 0x00; rx_poll();
    PAD1_DATA = 0x40;
    uint16_t v = (uint16_t)((hi & 0x3F) | ((lo & 0x30) << 2));
    pad6 = (id & 0x0F) == 0x0F;
    if (pad6)
        v |= (uint16_t)((ext & 0x0F) << 8);
    return v;
}

static void vdp_init(void)
{
    VDP_CTRL = 0x8004;      /* no HINT */
    VDP_CTRL = 0x8144;      /* display on, mode 5, no VINT */
    VDP_CTRL = 0x8230;      /* plane A at 0xC000 */
    VDP_CTRL = 0x8407;      /* plane B at 0xE000 */
    VDP_CTRL = 0x8578;      /* sprites at 0xF000 */
    VDP_CTRL = 0x8700;      /* backdrop: palette 0 colour 0 */
    VDP_CTRL = 0x8C81;      /* H40 */
    VDP_CTRL = 0x8F02;      /* auto-increment 2 */
    VDP_CTRL32 = 0x40000000u;               /* clear VRAM: nothing in front of the 32X */
    for (int i = 0; i < 0x8000; i++) VDP_DATA = 0;
    VDP_CTRL32 = 0xC0000000u;               /* backdrop black */
    VDP_DATA = 0;
}

void md_fatal(int vector)
{
    VDP_CTRL32 = 0xC0000000u;               /* backdrop red: a 68000 crash */
    VDP_DATA = 0x000E;
    volatile uint16_t *t = (volatile uint16_t *)0xFFFF00;
    t[1] = 0xDEAD;
    t[2] = (uint16_t)vector;
    for (;;)
        ;
}

/* ------------------------------------------------------------ commands */

static void put6(const uint8_t *p)
{
    COMM16(C_P1) = (uint16_t)(p[0] << 8 | p[1]);
    COMM16(C_P2) = (uint16_t)(p[2] << 8 | p[3]);
    COMM16(C_P3) = (uint16_t)(p[4] << 8 | p[5]);
}

#ifdef MD_ONLY
unsigned md_run_command(int cmd, uint16_t p1, uint16_t p2, uint16_t p3)
#else
static unsigned run_command(int cmd, uint16_t p1, uint16_t p2, uint16_t p3)
#endif
{
    uint8_t b[6];
    switch (cmd) {
    case CMD_NOP:
        return 0;
    case CMD_YM0:
    case CMD_YM1: {
        uint16_t w[3] = { p1, p2, p3 };
        for (int k = 0; k < 3; k++)
            if (w[k] >> 8)
                ym_write(cmd == CMD_YM1, (uint8_t)(w[k] >> 8), (uint8_t)w[k]);
        return 0;
    }
    case CMD_PSG:
        psg_write((uint8_t)p1);
        return 0;
    case CMD_SHADOW:
        for (int k = 0; k < 6; k++)
            b[k] = (unsigned)(p1 + k) < sizeof shadow ? shadow[p1 + k] : 0;
        put6(b);
        return 0;
    case CMD_SCAN: {
        int n = files_scan();
        if (n == ERR_NO_ED || n == ERR_SD) {
            COMM16(C_P1) = 0;
            return (unsigned)n;
        }
        COMM16(C_P1) = (uint16_t)n;
        COMM16(C_P2) = (uint16_t)files_skipped;
        return 0;
    }
    case CMD_NAME: {
        const char *n = file_name(p1);
        int len = file_name_len(p1);
        for (int k = 0; k < 6; k++)
            b[k] = p2 + k < len ? (uint8_t)n[p2 + k] : 0;
        put6(b);
        return (unsigned)((len > 127 ? 127 : len) | (file_is_dir(p1) ? 0x80 : 0));
    }
#ifndef MD_ONLY
    case CMD_EVENTS: {
        uint8_t head = (uint8_t)(md_flags >> 8), at = (uint8_t)p1;
        int n = 0;
        for (; n < 2 && at != head; n++, at++)
            for (int k = 0; k < 3; k++) b[n * 3 + k] = ev_ring[at][k];
        for (int k = n * 3; k < 6; k++) b[k] = 0;
        put6(b);
        return (unsigned)n;
    }
    case CMD_DRUMS: {
        int n = drums_scan();
        return (unsigned)(n > 255 ? 255 : n);
    }
    case CMD_DRUM:
        return drum_info(p1, p2);
#endif
    case CMD_CD:
        files_cd();
        return 0;
    case CMD_LOAD: {
        uint16_t size = 0;
        int e = file_load(p1, &size);
        COMM16(C_P1) = size;
        return (unsigned)e;
    }
    case CMD_BUF_GET:
        for (int k = 0; k < 6; k++)
            b[k] = p1 + k < BUF_SIZE ? file_buf[p1 + k] : 0;
        put6(b);
        return 0;
    case CMD_BUF_PUT:
        if (p1 + 4 > BUF_SIZE) return ERR_RANGE;
        file_buf[p1] = (uint8_t)(p2 >> 8);
        file_buf[p1 + 1] = (uint8_t)p2;
        file_buf[p1 + 2] = (uint8_t)(p3 >> 8);
        file_buf[p1 + 3] = (uint8_t)p3;
        return 0;
    case CMD_SAVE:
        return (unsigned)file_save(p1);
    case CMD_DELETE:
        return (unsigned)file_delete();
    case CMD_SILENCE:
        snd_silence();
        midi_reset_voices();
        return 0;
    case CMD_MIDI:              /* MIDI bytes from the 32X (the preview note) */
        midi_quiet = 1;
        for (int k = 0; k < (p1 >> 8) && k < 3; k++)
            midi_byte((uint8_t)(k == 0 ? p1 : k == 1 ? p2 >> 8 : p2));
        midi_quiet = 0;
        return 0;
    case CMD_MONITOR:
        if (p1 == 0) {
            COMM16(C_P1) = mon_chans;
            COMM16(C_P2) = mon_voices;
            COMM16(C_P3) = mon_count;
            mon_chans = mon_voices = 0;
        } else if (p1 == 1) {
            put6(mon_last);     /* (bytes 4-5: whatever follows; unused) */
        } else if (p1 == 11) {
            put6(rx_pins);
        } else if (p1 == 10) {
            COMM16(C_P1) = rx_bytes;
            COMM16(C_P2) = rx_errors;
            COMM16(C_P3) = 0;
        } else if (p1 < 10) {
            int c = (p1 - 2) * 2;
            for (int k = 0; k < 3; k++) { b[k] = mon_ch[c][k]; b[3 + k] = mon_ch[c + 1][k]; }
            put6(b);
        } else {
            return ERR_RANGE;
        }
        return 0;
    case CMD_PSG_SET:
        if (p2 > BUF_SIZE - BUF_DATA) return ERR_RANGE;
        return midi_psg_set(p1, file_buf + BUF_DATA, p2) ? ERR_RANGE : 0;
    case CMD_PSG_GET:
        return (unsigned)midi_psg_get(p1, file_buf + BUF_DATA);
    case CMD_SETTING: {
        uint8_t *s = (uint8_t *)&ms;
        if (p1 >= SET_COUNT) return ERR_RANGE;
        if (p2 != 0xFFFF) {
            if (p1 == SET_MODE || p1 == SET_POLY_CH) midi_reset_voices();
            s[p1] = (uint8_t)p2;
        }
        return s[p1];
    }
    }
    return ERR_RANGE;
}

/* ------------------------------------------------------------ main */

static int was_vb;
static uint16_t vblanks;
#ifdef MD_ONLY
volatile uint16_t vint_count;           /* frames: the vertical interrupt counts them */
#endif
#ifdef MD_ONLY
/* (no 32X: the screens can be busy for frames, so presses queue up rather
 * than merge; src/mdonly/hw_md.c hands them over one at a time) */
uint16_t pad_q[8], pad_held;
uint8_t pad_qh, pad_qt;
#endif
uint16_t md_vblanks(void) { return vblanks; }
#ifdef MD_ONLY
extern uint16_t gfx_draw_frames, gfx_present_frames;
#endif

/* At each vblank: pad 1 (presses latched until the screens take them), the
 * receiver's frame count, the test telemetry. 1: a new frame. */
static int frame_tick(void)
{
    int vb = (VDP_CTRL & VDP_STATUS_VB) != 0;
    int edge = vb && !was_vb;
    was_vb = vb;
#ifdef MD_ONLY
    /* (while the screens draw, the vblank flag - 2.4 ms a frame - can pass
     * between two looks: the vertical interrupt counts every frame,
     * src/mdonly/crt.s) */
    static uint16_t seen;
    uint16_t now = vint_count;
    edge = now != seen;
    seen = now;
    (void)vb;
#endif
    if (!edge) return 0;
    uint16_t pad = pad_read();
    uint16_t ack = COMM16(C_CMD) >> 8;
    if (ack != last_ack) {      /* the screens took the latched presses */
        last_ack = ack;
        latched = 0;
    }
    latched |= pad & ~last_pad;
#ifdef MD_ONLY
    uint16_t nw = pad & ~last_pad;
    if (nw && (uint8_t)(pad_qh - pad_qt) < 8) pad_q[pad_qh++ & 7] = nw;
    pad_held = pad;
#endif
    last_pad = pad;
    COMM16(C_PAD) = pad | latched;
    if (!!(md_flags & FL_PAD3) != !pad6)
        set_flag(FL_PAD3, !pad6);
#ifdef MD_ONLY
    vblanks = vint_count;
#else
    vblanks++;
#endif
    rx_frame();
    midi_frame();
    /* test telemetry (tests/harness.c reads work RAM 0xFFFF00) */
    volatile uint16_t *t = (volatile uint16_t *)0xFFFF00;
    t[0] = 0x5354;
    t[1] = COMM16(C_CMD);
    t[2] = COMM16(C_P1);
    t[3] = COMM16(C_P2);
#ifdef MD_ONLY
    t[2] = gfx_draw_frames;                 /* (frames the last redraw took: drawing, */
    t[3] = gfx_present_frames;              /* showing) */
#endif
    t[4] = pad;
    t[5] = vblanks;
    t[6] = md_flags;
    t[7] = act_count;
    return 1;
}

#ifdef MD_ONLY
/* (the screens' poll_hook: while they draw, the port and the pad go on) */
static void md_poll(void)
{
    rx_poll();
    frame_tick();
}
#endif

#define M_OK 0x4D5F4F4Bu
#define S_OK 0x535F4F4Bu

void md_main(int boot_err)
{
    __asm__ volatile ("move.w #0x2700,%sr");
    PAD1_CTRL = 0x40;
    PAD1_DATA = 0x40;
    vdp_init();
#ifndef MD_ONLY
    MARS16(0x04) = 0;           /* cartridge bank 0 in the 0x900000 window */
    MARS8(0x07) = 0;            /* RV = 0: the SH-2s may read the cartridge */
#endif
    snd_init();
    if (VDP_CTRL & 1)           /* a PAL console: PAL tuning */
        ms.region = 1;
    midi_init();
    rx_init();

    md_flags = 0;
    if (vfs_scan() > 0)
        md_flags |= FL_VFS;
    COMM16(C_FLAGS) = md_flags;
    COMM16(C_REPLY) = 0;
    COMM16(C_ACT) = 0;

#ifndef MD_ONLY
    /* the 32X boot ROMs post M_OK / S_OK once they have loaded us */
    while (COMM32(0x0) != M_OK || COMM32(0x4) != S_OK)
        rx_poll();
    COMM32(0x0) = 0;            /* release the master: its first command is seq 1 */
#endif
    __asm__ volatile ("move.w #0x2100,%sr");   /* the GenMDM's strobe interrupt (level 2) */
#ifdef MD_ONLY
    /* no 32X: the screens run here too (src/32x/ui.c over src/mdonly/), at
     * each vblank; their commands are calls (md_run_command) */
    poll_hook = md_poll;
    ui_start();
#endif

    last_ack = 0;
    unsigned last_seq = 0;
    for (;;) {
        rx_poll();
#ifdef MD_ONLY
        if (frame_tick())
            ui_frame();
#else
        frame_tick();
#endif
#ifdef MD_ONLY
        (void)last_seq;
#else

        rx_poll();
        uint16_t c = COMM16(C_CMD);
        unsigned seq = (c >> 5) & 7, cmd = c & 31;
        if (cmd == CMD_CRASH) {
            VDP_CTRL32 = 0xC0000000u;   /* backdrop purple: an SH-2 crash */
            VDP_DATA = 0x0E0E;
            for (;;)
                ;
        }
        if (seq != last_seq) {
            last_seq = seq;
            unsigned r = run_command((int)cmd, COMM16(C_P1), COMM16(C_P2), COMM16(C_P3));
            COMM16(C_REPLY) = (uint16_t)(seq << 8 | (r & 0xFF));
        }
#endif
    }
}
