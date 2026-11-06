/* guide.h - the guide (guide.c) */
#ifndef GUIDE_H
#define GUIDE_H
#include <stdint.h>
void guide_open(void);
void guide_draw(void);
int  guide_input(uint16_t pressed, int *redraw);    /* 0 when it closes */
#endif
