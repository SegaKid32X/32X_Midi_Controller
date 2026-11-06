/* hw.h - the 32X hardware layer (master and slave SH-2). */
#ifndef HW_H
#define HW_H

#include <stdint.h>
#include "comm.h"

/* SH-2 side 32X registers (cache-through) */
#define MARS_ADAPTER    (*(volatile uint16_t *)0x20004000)
#define MARS_INTMASK    (*(volatile uint8_t *)0x20004001)
#define MARS_COMM(n)    (*(volatile uint16_t *)(0x20004020 + (n)))
#define MARS_PWM_CTRL   (*(volatile uint16_t *)0x20004030)
#define MARS_PWM_CYCLE  (*(volatile uint16_t *)0x20004032)
#define MARS_PWM_LEFT   (*(volatile uint16_t *)0x20004034)
#define MARS_PWM_RIGHT  (*(volatile uint16_t *)0x20004036)
#define MARS_PWM_MONO   (*(volatile uint16_t *)0x20004038)
#define MARS_VDP_MODE   (*(volatile uint16_t *)0x20004100)
#define MARS_VDP_SHIFT  (*(volatile uint16_t *)0x20004102)
#define MARS_VDP_FBCTL  (*(volatile uint16_t *)0x2000410A)
#define MARS_CRAM       ((volatile uint16_t *)0x20004200)
#define MARS_FRAMEBUF   ((volatile uint16_t *)0x24000000)
#define FB_BYTES        ((uint8_t *)0x24000000)

#define VDP_MODE_BLANK  0x0000
#define VDP_MODE_PACKED 0x0001
#define VDP_MODE_PAL    0x8000  /* read: a PAL console */
#define FBCTL_VBLK      0x8000
#define FBCTL_FEN       0x0002  /* frame buffer busy */
#define FBCTL_FS        0x0001

#define SDRAM_THROUGH(p) ((void *)((uintptr_t)(p) | 0x20000000u))

#define SCREEN_W    320
#define SCREEN_H    224

void hw_init(void);
/* 256 colours, 320 x 224, both buffers cleared */
void hw_video_init(void);
uint8_t *hw_row(int r);                 /* row r of the buffer the SH-2 can reach now */
void hw_flip(void);                     /* show it from the next vblank, wait for that */
int  hw_in_vblank(void);
void hw_wait_vblank(void);
void hw_load_palette(const uint16_t *pal, int n);

/* ---- the 68000 ---- */
int      md_cmd(int cmd, uint16_t p1, uint16_t p2, uint16_t p3);  /* blocking; the result */
uint16_t md_p(int which);               /* reply data: C_P1 / C_P2 / C_P3 */
uint16_t hw_pad(void);                  /* held or pressed since the last call */
uint16_t hw_flags(void);
uint16_t hw_activity(void);

/* ---- the slave SH-2 ---- */
typedef void (*slave_fn)(int arg);
void hw_slave_start(slave_fn fn, int arg);
int  hw_slave_wait(void);
void hw_cache_purge(void);

/* ---- timing: the free-running timer, 128 clocks (5.56 us) a tick ---- */
void     hw_timer_init(void);
uint16_t hw_ticks(void);

#endif
