/* hw.h (Genesis-only build) - what the screens (src/32x/ui.c ...) ask of the
 * hardware, with no 32X: the commands that went to the 68000 through the
 * COMM registers are plain calls (src/md/md_main.c's run_command), and the
 * COMM registers are a few words of RAM. */
#ifndef MDONLY_HW_H
#define MDONLY_HW_H

#include <stdint.h>
#include "comm.h"

#define SCREEN_W    320
#define SCREEN_H    224

int      md_cmd(int cmd, uint16_t p1, uint16_t p2, uint16_t p3);  /* the result */
extern void (*poll_hook)(void);         /* run before each command and in long loops */
uint16_t md_p(int which);               /* reply data: C_P1 / C_P2 / C_P3 */
uint16_t hw_pad(void);                  /* held or pressed since the last call */
uint16_t hw_flags(void);
uint16_t hw_activity(void);

/* the patch list itself (src/md/md_files.c): no second copy of the names */
uint16_t md_vblanks(void);
const char *file_name(int i);
int file_is_dir(int i);

#endif
