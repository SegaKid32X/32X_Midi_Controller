/* fm.h - the 32X's software FM voices (fm.c, on the slave SH-2) */
#ifndef FM_H
#define FM_H
#include <stdint.h>
#include "patch.h"

#define FM_CHANNELS 4           /* MIDI channels 11-14 */

void fm_reset(void);
void fm_note_on(int ch, int note, int vel);     /* ch 0-3 */
void fm_note_off(int ch, int note);
void fm_control(int ch, int cc, int val);
void fm_bend(int ch, int value);                /* 0..16383 */
void fm_render(int32_t *acc, int n);            /* add n stereo samples (n: a multiple of 16) */

/* the channels' patches (synth.c: shared with the master) */
const fm_patch *fm_patch_of(int ch);
int fm_patch_version(int ch);

#endif
