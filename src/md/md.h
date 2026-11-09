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

#ifdef MD_HOST                  /* the host tests (tests/host/test_link.c) */
#include "host_md.h"
#else
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
#ifdef MD_ONLY                  /* no 32X: the COMM words are RAM (src/mdonly/hw_md.c) */
extern volatile uint16_t comm_regs[8];
#define COMM16(o)   (comm_regs[(o) >> 1])
#else
#define COMM16(o)   (*(volatile uint16_t *)(0xA15120 + (o)))
#define COMM32(o)   (*(volatile uint32_t *)(0xA15120 + (o)))
#endif
#define IRQ_OFF()   __asm__ volatile ("move.w #0x2700,%%sr" ::: "memory")
#define IRQ_ON()    __asm__ volatile ("move.w #0x2100,%%sr" ::: "memory")
#endif

/* md_main.c */
extern uint16_t md_flags;
void set_flag(uint16_t f, int on);
void md_fatal(int vector);
#ifdef MD_ONLY
unsigned md_run_command(int cmd, uint16_t p1, uint16_t p2, uint16_t p3);
void rx_poll(void);
extern void (*poll_hook)(void);
void ui_start(void);
void ui_frame(void);
#endif

/* md_snd.c: the sound chips and their shadow */
extern uint8_t shadow[SH_PSG + 16];
extern volatile uint16_t act_count;
void snd_init(void);
void ym_write(int part, uint8_t reg, uint8_t val);      /* the chip and the shadow */
void ym_chip(int part, uint8_t reg, uint8_t val);       /* the chip only */
void psg_write(uint8_t b);
void snd_silence(void);
void act_bump(void);            /* tell the screen the sound changed */
void flags_publish(void);       /* md_flags -> C_FLAGS */
extern uint16_t mon_voices;     /* voices that started a note (CMD_MONITOR) */

/* md_midi.c: the MIDI engine (with our Teensy firmware) */
#define MODE_GENMDM 0
#define MODE_POLY   1
typedef struct {
    uint8_t mode;               /* MODE_GENMDM / MODE_POLY */
    uint8_t poly_ch;            /* POLY: the MIDI channel (0-15) */
    uint8_t region;             /* tuning: 0 NTSC, 1 PAL */
    uint8_t velocity;           /* 1: velocity sets the loudness */
    uint8_t spread;             /* POLY: each note left, right or both */
    uint8_t reserved;
} midi_settings_t;
extern midi_settings_t ms;
void midi_init(void);
void midi_byte(uint8_t b);
void midi_reset_voices(void);
void midi_frame(void);          /* once a frame: the PSG instruments' sequences */
int  midi_psg_set(int c, const uint8_t *in, int len);  /* CMD_PSG_SET */
int  midi_psg_get(int c, uint8_t *out);                /* its compact form: the length */
extern uint16_t rx_bytes, rx_errors;   /* md_rx.c: for the monitor */
extern uint8_t rx_pins[6];
extern int midi_quiet;          /* 1: leave these bytes out of the monitor */
extern uint16_t mon_chans, mon_count;
extern uint8_t mon_last[6], mon_ch[16][3];
#ifdef MD_ONLY
extern uint8_t ev_ring[1][3];
#else
extern uint8_t ev_ring[256][3];
#endif

/* md_drums.c: the drum kit (WAVs in MIDI_DRUMS, into cartridge memory) */
#define MAX_DRUMS   64
int  drums_scan(void);          /* the kit at BUF_NAME -> count */
unsigned drum_info(int i, int off);
int  ed_load_to_cart(const char *path, uint32_t cart_addr, uint32_t max, uint32_t *len);

/* md_rx.c: port 2 (the GenMDM interface) */
extern uint8_t rx_ring[256];
extern volatile uint8_t rx_head;
void rx_init(void);
void rx_poll(void);             /* call as often as possible */
void rx_frame(void);            /* once a frame */
extern volatile uint8_t rx_custom;
void rx_take(uint8_t v);

/* md_files.c: the patch list and file transfers */
#ifdef MD_ONLY
#define MAX_FILES   500         /* (64 KB of RAM holds the screen too) */
#else
#define MAX_FILES   999
#endif
extern uint8_t file_buf[BUF_SIZE];
int  files_scan(void);          /* count, or ERR_NO_ED / ERR_SD */
int  file_name_len(int i);
const char *file_name(int i);
int  file_load(int i, uint16_t *size);  /* into file_buf + BUF_DATA */
int  file_save(uint16_t len);
int  file_delete(void);
int  file_is_dir(int i);
void files_cd(void);            /* the folder at BUF_NAME (inside MIDI_PATCHES) */
extern int files_skipped;

/* md_ed.c: the Mega EverDrive (runs from work RAM with RV = 1) */
int  ed_detect(void);                   /* 1 if it answers */
int  ed_dir_open(const char *path);     /* sorted listing -> entry count, or -1 */
#define ED_DIR_BATCH 12         /* entries per ed_dir_batch (they must fit the 2 KB FIFO) */
#define ED_NAME_MAX  128        /* longest name asked for (longer: cut, then left out) */
int  ed_dir_batch(int first, int n);    /* then n x ed_dir_next() */
int  ed_dir_next(char *name, int max, int *is_dir);
int  ed_dir_entry(int i, char *name, int max, int *is_dir);   /* 0 or -1; *is_dir -1: hidden */
int  junk_name(const char *n);  /* a file or folder computers leave behind (md_files.c) */
int  ed_read_file(const char *path, uint8_t *dst, int max, uint16_t *len);
int  ed_write_file(const char *path, const uint8_t *src, int len);
int  ed_delete(const char *path);
int  ed_mkdir(const char *path);

/* md_vfs.c: patches built into the ROM (tests, no EverDrive) */
int  vfs_scan(void);                    /* count, 0 if none */
const uint8_t *vfs_file(int i, const char **name, uint16_t *len);

#endif
