/* song.h - the tracker's song: its layout in memory and on the SD card
 * (both the same bytes), the lanes and parts, the sequencer's commands.
 *
 * A song is a pool of bytes (the 68000 keeps it; src/md/md_seq.c):
 *
 *   a header         tempo, rows a beat, the order list, each pattern's
 *                    length and event count, which MIDI channel (part)
 *                    each lane plays, the lanes muted, the song's name
 *   the instruments  each part's file (a patch inside MIDI_PATCHES, or for
 *                    part 16 the drum kit's folder): 16 zero-terminated
 *                    paths packed one after another
 *   the events       pattern 0's, then pattern 1's ... 4 bytes each:
 *                    row, lane, note (1-127, or NOTE_OFF), velocity (1-127)
 *
 * Numbers of two bytes are big-endian. A lane is one column of the tracker:
 * one note at a time (a new note, or NOTE_OFF, ends the one before). Each
 * lane plays a part: a MIDI channel and the instrument loaded there, as on
 * the home screen (1-6 FM, 7-9 PSG, 10 noise, 11-14 the 32X's FM voices, 15
 * MIDI out only, 16 the 32X's drum kit). Several lanes on one part play
 * chords (the 32X's FM voices are shared by channels 11-14).
 */
#ifndef SONG_H
#define SONG_H

#define SONG_LANES      22      /* every voice at once: YM 6, PSG 3 + noise, the 32X's FM 6, drums 6 */
#define SONG_PATS       64
#define SONG_ORDS       128
#define SONG_ROWS_MAX   128
#define SONG_PARTS      16
#define NOTE_OFF        0xFF
#define SONG_MAGIC      "MSNG"
#define SONG_VERSION    1

/* header offsets */
#define SO_MAGIC        0       /* "MSNG" */
#define SO_VER          4
#define SO_RPB          5       /* rows a beat, 1-16 */
#define SO_BPM          6       /* 2 bytes: 20-300 */
#define SO_NORD         8       /* order list length, 1-128 */
#define SO_LOOP         9       /* where the order list loops back to */
#define SO_NPAT         10      /* (patterns are always SONG_PATS) */
#define SO_FLAGS        11
#define SO_NAME         12      /* 24 bytes, zero-padded */
#define SO_LANE         36      /* 24 bytes: each lane's MIDI channel, 0-15 */
#define SO_ORD          60      /* 128 pattern numbers */
#define SO_PLEN         188     /* 64 pattern lengths, 1-128 rows */
#define SO_PCNT         252     /* 64 event counts, 2 bytes each */
#define SO_MUTE         380     /* 4 bytes: lanes muted (bit n: lane n) */
#define SO_PATHS        384     /* 1024 bytes: 16 paths, each zero-terminated */
#define SONG_PATHS      1024
#define SO_EVENTS       1408
#define EV_BYTES        4

/* the default lanes: FM 1-6, PSG 1-3, noise, the 32X's six FM voices
 * (channels 11-14, then 11 and 12 again: chords), six drum lanes (the kit's
 * samples ring on: twelve voices) */
#define SONG_DEFAULT_LANES { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 10, 11, 15, 15, 15, 15, 15, 15 }

/* CMD_SEQ (comm.h): P1 = SQ_* << 8 | a byte, P2, P3; the reply below */
#define SQ_STATE        0       /* -> flags (SQS_*); P1 = order pos << 8 | row,
                                 * P2 = pattern << 8 | rows in it, P3 = edits so far */
#define SQS_PLAY        0x01
#define SQS_REC         0x02    /* armed */
#define SQS_EDIT        0x04    /* step entry from the keyboard (stopped) */
#define SQS_FOLLOW      0x08    /* the keyboard plays the cursor lane's part */
#define SQS_FULL        0x10    /* a note did not fit (the song memory is full) */
#define SQ_PEEK         1       /* P2 = offset: 6 bytes of the song into P1..P3 */
#define SQ_POKE         2       /* P2 = offset, P3 = 2 bytes; byte (P1 low): 1 or 2 */
#define SQ_CELL         3       /* P1 low = lane, P2 = pattern << 8 | row,
                                 * P3 = note << 8 | velocity (note 0: clear) */
#define SQ_ROWS         4       /* P1 low = lane0, P2 = pattern << 8 | row0,
                                 * P3 = rows << 8 | lanes: the cells into the
                                 * buffer at BUF_DATA, [row][lane] note, velocity */
#define SQ_PLAY         5       /* P1 low = 1: from the cursor (P2 = order pos << 8 | row),
                                 * 0: from the top; P3 = 1 armed (record) */
#define SQ_STOP         6
#define SQ_CURSOR       7       /* P1 low = lane, P2 = order pos << 8 | row,
                                 * P3 = flags (SQS_EDIT, SQS_FOLLOW) | step << 8 */
#define SQ_NEW          8       /* an empty song */
#define SQ_PATOP        9       /* P1 low = PO_*, P2 = pattern << 8 | other, P3 = lane (0xFF all) */
#define PO_CLEAR        0       /* the pattern (or one lane of it) */
#define PO_COPY         1       /* pattern -> other (all lanes) */
#define PO_INSERT       2       /* a blank row at P3 high byte (row), lane P3 low */
#define PO_DELETE       3       /* remove that row */
#define PO_TRANSPOSE    4       /* P3 = lane | semitones (signed) << 8 */
#define SQ_PATH         10      /* P1 low = part; P2 = 0: its path into BUF_NAME,
                                 * 1: set it from BUF_NAME -> 0 or ERR_TOO_BIG */
#define SQ_SAVE         11      /* write the song to MIDI_SONGS/<BUF_NAME> */
#define SQ_LOAD         12      /* read MIDI_SONGS/<BUF_NAME> -> 0 or an error */
#define SQ_EXPORT       13      /* a standard MIDI file MIDI_SONGS/<BUF_NAME> */
#define SQ_FREE         14      /* -> 0; P1 = events free, P2 = events used */
#define SQ_MUTE         15      /* P1 low = lane, P2 = 1 muted (0xFFFF: only read) */

#define SONG_DIR        "/MIDI_SONGS"

#endif
