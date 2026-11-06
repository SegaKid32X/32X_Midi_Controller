/* host_md.h - the 68000's hardware, faked for the host tests */
#include <stdint.h>
extern uint8_t host_pad2_data, host_pad2_ctrl, host_pad1;
extern uint16_t host_vdp, host_comm[8];
#define VDP_DATA    host_vdp
#define VDP_CTRL    host_vdp
#define VDP_CTRL32  host_vdp
#define PSG_PORT    host_pad1
#define PAD1_DATA   host_pad1
#define PAD2_DATA   host_pad2_data
#define PAD1_CTRL   host_pad1
#define PAD2_CTRL   host_pad2_ctrl
#define COMM16(o)   host_comm[(o) / 2]
#define IRQ_OFF()   ((void)0)
#define IRQ_ON()    ((void)0)
