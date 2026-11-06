/* gfx.h - drawing on a 320 x 224, 256-colour canvas in SDRAM, shown with
 * gfx_present() (a copy into the 32X frame buffer, then a flip). */
#ifndef GFX_H
#define GFX_H

#include <stdint.h>

enum {
    COL_BG, COL_TEXT, COL_DIM, COL_HI, COL_HEAD, COL_PANEL, COL_RED, COL_GREEN,
    COL_ORANGE, COL_CURSOR, COL_LINE, COL_BLUE, COL_PANEL2, COL_COUNT
};

#define CHAR_W  6
#define CHAR_H  9
#define COLS    53
#define ROWS    24

void gfx_init(void);
void gfx_clear(uint8_t c);
void gfx_rect(int x, int y, int w, int h, uint8_t c);
void gfx_frame(int x, int y, int w, int h, uint8_t c);
void gfx_line(int x0, int y0, int x1, int y1, uint8_t c);
void gfx_text(int x, int y, const char *s, uint8_t c);      /* pixel position */
int  gfx_text_n(int x, int y, const char *s, int max, uint8_t c);
void gfx_num(int x, int y, int v, uint8_t c);
void gfx_present(void);

char *fmt_num(char *o, int v);          /* a decimal number, no terminator */

#endif
