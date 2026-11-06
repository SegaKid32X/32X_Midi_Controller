/* lib.h - the patch files and the sound chip, from the master SH-2 (all
 * through the 68000: src/common/comm.h). */
#ifndef LIB_H
#define LIB_H

#include <stdint.h>
#include "patch.h"

#define LIB_MAX     999
#define NAME_MAX    127

#define PATH_MAX_   160

extern int lib_count;
extern int lib_skipped;                 /* patch files the 68000 left out */
extern char lib_path[PATH_MAX_];        /* the folder shown, inside MIDI_PATCHES ("" = the top) */
int  lib_scan(void);                    /* count (>= 0) or -ERR_* */
int  lib_cd(const char *path);          /* show another folder (and scan it) */
int  lib_enter(int i);                  /* into folder i */
int  lib_up(void);                      /* up a folder; 0 if at the top already */
const char *lib_name(int i);
int  lib_is_dir(int i);
void lib_rel(int i, char *out);         /* entry i's path inside MIDI_PATCHES */
const char *base_name(const char *rel);
int  lib_load(int i, fm_patch *p);      /* 0 or an error */
int  lib_save(const char *name, const fm_patch *p);    /* 0 or an error */
int  lib_delete(const char *name);
int  lib_find(const char *name);        /* index, or -1 */
void lib_new_name(char *out, const char *ext);         /* "newpatch001.tfi" ... */

/* the sound chip */
void ym_set(int ch, uint8_t reg, uint8_t val);       /* a channel's register (reg as for part I) */
void ym_global(uint8_t reg, uint8_t val);            /* part I only (LFO 0x22 ...) */
void voice_apply(int ch, const fm_patch *p);          /* ch 0-5 */
void voice_read(int ch, fm_patch *p);                 /* what channel ch plays now */
uint8_t shadow_byte(int index);
void shadow_refresh(void);                            /* fetch the whole shadow */
extern uint8_t shadow_copy[528];

#endif
