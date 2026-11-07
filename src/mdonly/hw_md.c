/* hw_md.c - the Genesis-only build's stand-in for the 32X's COMM registers
 * (see hw.h). */
#include "md.h"
#include "hw.h"

volatile uint16_t comm_regs[8];
void (*poll_hook)(void);
static uint8_t pad_ack;

int md_cmd(int cmd, uint16_t p1, uint16_t p2, uint16_t p3)
{
    if (poll_hook) poll_hook();
    COMM16(C_P1) = p1;
    COMM16(C_P2) = p2;
    COMM16(C_P3) = p3;
    return (int)(md_run_command(cmd, p1, p2, p3) & 0xFF);
}

uint16_t md_p(int which)
{
    return COMM16(which);
}

/* What is held, plus the next press from md_main.c's queue. A press of a
 * button the screens still think is held comes after a frame without it,
 * so they see it as a press. */
extern uint16_t pad_q[8], pad_held;
extern uint8_t pad_qh, pad_qt;
static uint16_t last_ret;

uint16_t hw_pad(void)
{
    uint16_t v = pad_held;
    if (pad_qt != pad_qh) {
        uint16_t e = pad_q[pad_qt & 7];
        if (e & last_ret) {
            last_ret = v & (uint16_t)~e;
            return last_ret & 0x0FFF;
        }
        pad_qt++;
        v |= e;
    }
    last_ret = v;
    pad_ack++;
    COMM16(C_CMD) = (uint16_t)(pad_ack << 8);
    return v & 0x0FFF;
}

uint16_t hw_flags(void)
{
    return COMM16(C_FLAGS);
}

uint16_t hw_activity(void)
{
    return COMM16(C_ACT);
}
