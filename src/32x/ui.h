/* ui.h - the screens (ui.c) */
#ifndef UI_H
#define UI_H
void ui_start(void);
void ui_frame(void);            /* once a frame */
void ui_draw(void);
void ui_rescan(void);
extern int ui_continuous;        /* 1: call ui_frame() again at once (it waits itself) */
#endif
