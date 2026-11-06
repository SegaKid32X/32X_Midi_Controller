/* comm.h - how the master SH-2 and the 68000 talk: the 32X COMM registers.
 *
 * Both CPUs include this file. COMM is 8 words; each side writes only its own
 * words:
 *
 *   C_CMD    SH-2:  pad ack << 8 | seq << 5 | command (5 bits)
 *   C_P1..3  SH-2 writes the parameters before C_CMD; the 68000 may put
 *            reply data there before it answers (the SH-2 reads them after)
 *   C_REPLY  68000: seq << 8 | result (0 = fine, else an error)
 *   C_FLAGS  68000: FL_* bits
 *   C_PAD    68000: pad 1, held | pressed since the last ack (PAD_* bits)
 *   C_ACT    68000: counts sound-chip writes from the GenMDM / MIDI side, so
 *            the screen knows when to look again (wraps)
 *
 * Only the 68000 touches the YM2612 and the PSG, the controller ports and the
 * EverDrive; the SH-2s draw the screen and decide what to load.
 */
#ifndef COMM_H
#define COMM_H

#define C_CMD       0x0
#define C_P1        0x2
#define C_P2        0x4
#define C_P3        0x6
#define C_REPLY     0x8
#define C_FLAGS     0xA
#define C_PAD       0xC
#define C_ACT       0xE

/* C_FLAGS */
#define FL_ED       0x0001      /* a Mega EverDrive answered */
#define FL_VFS      0x0002      /* patches built into the ROM (test builds) */
#define FL_PAD3     0x0004      /* pad 1 is a 3-button pad */
#define FL_BUSY     0x0008      /* the 68000 is on the EverDrive (port not read) */
#define FL_LINK     0x0030      /* what is on port 2: */
#define LINK_NONE   0x0000      /*   nothing heard yet */
#define LINK_STOCK  0x0010      /*   GenMDM with its own firmware (register stream) */
#define LINK_MIDI   0x0020      /*   our Teensy firmware (MIDI bytes) */

/* Commands (5 bits). Unless noted the reply is 0 or an error. */
#define CMD_NOP         0
#define CMD_YM0         1   /* up to three YM2612 writes, part I (channels 1-3,
                             * globals): P1..P3 = reg << 8 | value (reg 0: none) */
#define CMD_YM1         2   /* the same for part II (channels 4-6) */
#define CMD_PSG         3   /* P1 low byte: one byte to the PSG */
#define CMD_SHADOW      4   /* P1 = first index: 6 bytes of the register shadow
                             * into P1..P3 (high byte first). Index 0-255 part I,
                             * 256-511 part II, 512-527 PSG (see SH_PSG) */
#define CMD_SCAN        5   /* look for patch files on the SD card -> count
                             * (0xFE: no EverDrive, 0xFF: SD error); count in P1 too */
#define CMD_NAME        6   /* P1 = entry index, P2 = offset: 6 name bytes into P1..P3
                             * (0 after the end); reply = the name's length
                             * (0-127) | 0x80 if it is a folder */
#define CMD_LOAD        7   /* P1 = file index: read the file into the 68000's
                             * buffer -> 0 or error; P1 = its size */
#define CMD_BUF_GET     8   /* P1 = offset: 6 buffer bytes into P1..P3 */
#define CMD_BUF_PUT     9   /* P1 = offset, P2..P3 = 4 bytes into the buffer */
#define CMD_SAVE        10  /* write the buffer: the path inside MIDI_PATCHES
                             * (zero-terminated) at BUF_NAME, P1 bytes of data at
                             * BUF_DATA; create or replace -> 0 or error */
#define CMD_DELETE      11  /* delete the file at BUF_NAME (inside MIDI_PATCHES) */
#define CMD_SILENCE     12  /* all notes off, PSG quiet */
#define CMD_SETTING     13  /* P1 = SET_*, P2 = the value (0xFFFF: only read it)
                             * -> the value (the MIDI engine's settings) */
#define SET_MODE        0   /* 0 GenMDM (MIDI ch 1-6 = FM 1-6), 1 POLY */
#define SET_POLY_CH     1   /* POLY: its MIDI channel, 0-15 */
#define SET_REGION      2   /* tuning: 0 NTSC, 1 PAL */
#define SET_VELOCITY    3   /* 1: velocity sets the loudness */
#define SET_SPREAD      4   /* POLY: stereo spread */
#define SET_COUNT       5
#define CMD_EVENTS      16  /* P1 = the first event wanted: up to two 3-byte MIDI
                             * messages for the 32X's voices into P1..P3 -> count.
                             * C_FLAGS' high byte: the next event's number */
#define CMD_DRUMS       17  /* load the kit at BUF_NAME ("" = MIDI_DRUMS) -> count */
#define CMD_DRUM        18  /* P1 = sample, P2 = 0xFFFF: P1 note << 8 | cartridge
                             * address bits 16-23, P2 its low 16 bits, P3 the
                             * size in 64-byte units;
                             * else 6 name bytes from offset P2 -> the name's length */
#define CMD_CD          15  /* the folder to list: BUF_NAME, a path inside
                             * MIDI_PATCHES ("" = MIDI_PATCHES itself) */
#define CMD_MIDI        14  /* P1 = count << 8 | byte 1, P2 = byte 2 << 8 | byte 3:
                             * MIDI bytes into the engine (count 1-3) */
#define CMD_CRASH       31  /* (written by a crashing SH-2, see crt0.s) */

#define BUF_NAME        0       /* 320 bytes: a path inside MIDI_PATCHES */
#define BUF_DATA        320
#define BUF_SIZE        2048    /* largest patch file we read */

/* Register shadow layout */
#define SH_PART2        256
#define SH_PSG          512     /* 16 bytes: per PSG channel tone (10 bits, 2
                                 * bytes high first) at +0..7, volume (0-15,
                                 * 15 = off) at +8..11, noise control at +12 */

/* Pad bits (C_PAD) */
#define PAD_UP    0x01
#define PAD_DOWN  0x02
#define PAD_LEFT  0x04
#define PAD_RIGHT 0x08
#define PAD_B     0x10
#define PAD_C     0x20
#define PAD_A     0x40
#define PAD_START 0x80
#define PAD_Z     0x100
#define PAD_Y     0x200
#define PAD_X     0x400
#define PAD_MODE  0x800

/* Errors */
#define ERR_NO_ED       0xFE
#define ERR_SD          0xFF
#define ERR_RANGE       0xF0
#define ERR_TOO_BIG     0xF1

#endif
