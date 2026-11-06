/* md.h - the 68000 program: shared definitions.
 *
 * The 68000 is the sound engine's front end. It owns the YM2612 and the PSG,
 * the controller ports (pad 1: the player; port 2: the GenMDM interface) and
 * the Mega EverDrive's SD card. It keeps a shadow of every sound register so
 * the 32X can show and save what is playing. The master SH-2 asks for things
 * through the COMM registers (src/common/comm.h); the SH-2s draw the screen.
 */
#ifndef MD_H
#define MD_H

#include <stdint.h>
#include "comm.h"

#define VDP_DATA    (*(volatile uint16_t *)0xC00000)
#define VDP_CTRL    (*(volatile uint16_t *)0xC00004)
#define VDP_CTRL32  (*(volatile uint32_t *)0xC00004)
#define PSG_PORT    (*(volatile uint8_t *)0xC00011)
#define PAD1_DATA   (*(volatile uint8_t *)0xA10003)
#define PAD2_DATA   (*(volatile uint8_t *)0xA10005)
#define PAD1_CTRL   (*(volatile uint8_t *)0xA10009)
#define PAD2_CTRL   (*(volatile uint8_t *)0xA1000B)
#define Z80_BUSREQ  (*(volatile uint16_t *)0xA11100)
#define Z80_RESET   (*(volatile uint16_t *)0xA11200)
#define YM_A0       (*(volatile uint8_t *)0xA04000)
#define YM_D0       (*(volatile uint8_t *)0xA04001)
#define YM_A1       (*(volatile uint8_t *)0xA04002)
#define YM_D1       (*(volatile uint8_t *)0xA04003)

/* 32X system registers, 68000 side */
#define MARS8(o)    (*(volatile uint8_t *)(0xA15100 + (o)))
#define MARS16(o)   (*(volatile uint16_t *)(0xA15100 + (o)))
#define COMM16(o)   (*(volatile uint16_t *)(0xA15120 + (o)))
#define COMM32(o)   (*(volatile uint32_t *)(0xA15120 + (o)))

/* md_main.c */
extern uint16_t md_flags;
void set_flag(uint16_t f, int on);
void md_fatal(int vector);

/* md_snd.c: the sound chips and their shadow */
extern uint8_t shadow[SH_PSG + 16];
extern volatile uint16_t act_count;
void snd_init(void);
void ym_write(int part, uint8_t reg, uint8_t val);
void psg_write(uint8_t b);
void snd_silence(void);

/* md_rx.c: port 2 (the GenMDM interface) */
void rx_init(void);
void rx_poll(void);             /* call as often as possible */

/* md_files.c: the patch list and file transfers */
#define MAX_FILES   999
extern uint8_t file_buf[BUF_SIZE];
int  files_scan(void);          /* count, or ERR_NO_ED / ERR_SD */
int  file_name_len(int i);
const char *file_name(int i);
int  file_load(int i, uint16_t *size);  /* into file_buf + BUF_DATA */
int  file_save(uint16_t len);
int  file_delete(void);

/* md_ed.c: the Mega EverDrive (runs from work RAM with RV = 1) */
int  ed_detect(void);                   /* 1 if it answers */
int  ed_dir_open(const char *path);     /* sorted listing -> entry count, or -1 */
int  ed_dir_entry(int i, char *name, int max, int *is_dir);   /* 0 or -1 */
int  ed_read_file(const char *path, uint8_t *dst, int max, uint16_t *len);
int  ed_write_file(const char *path, const uint8_t *src, int len);
int  ed_delete(const char *path);

/* md_vfs.c: patches built into the ROM (tests, no EverDrive) */
int  vfs_scan(void);                    /* count, 0 if none */
const uint8_t *vfs_file(int i, const char **name, uint16_t *len);

#endif
