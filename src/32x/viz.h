/* viz.h - the rainbow visualizer (viz.c), from the menu */
#ifndef VIZ_H
#define VIZ_H
void viz_open(void);            /* its colours; read the voices again */
void viz_draw(void);            /* the whole picture (then gfx_present) */
void viz_frame(int el);         /* once a frame: the wave, el frames on */
void viz_close(void);           /* the screens' colours back */
#endif
