/* song.h - a song: its layout in memory and on the SD card (the same bytes),
 * and the sequencer's commands (CMD_SEQ, src/md/md_seq.c).
 *
 * A song is up to 16 tracks of notes, as in a DAW: each track is one
 * instrument and plays as many notes at once as the hardware has voices
 * for it. Notes keep the time they were played at (96 ticks a beat) and
 * their length; quantizing is something done afterwards, and can be undone.
 *
 *   the header       tempo, beats a bar, the cycle, the click, the name
 *   16 track heads   what each plays (TK_*), its MIDI channel (for MIDI out
 *                    and the MIDI file; the 32X's FM tracks play that channel,
 *                    11-14), mute/solo, quantize, the instrument's name
 *   the sounds       each track's instrument itself: a byte of length, then
 *                    the bytes (FM: 30 register values; PSG: psg_pack's), so
 *                    a song needs no patch files to play
 *   the notes        8 bytes each, in time order:
 *                      0-2  tick << 4 | track    (tick: 20 bits, 96 a beat)
 *                      3    quantize offset: the played tick = tick - this
 *                      4    note (0-127)
 *                      5    velocity (1-127), bit 7: from the last take
 *                      6-7  length in ticks
 *
 * Numbers of two bytes or more are big-endian.
 */
#ifndef SONG_H
#define SONG_H

#define SONG_TRACKS     16
#define SONG_PPQ        96              /* ticks a beat */
#define SONG_MAGIC      "MSNG"
#define SONG_VERSION    2

/* the header */
#define SO_MAGIC        0
#define SO_VER          4
#define SO_FLAGS        5       /* SF_* */
#define SF_CYCLE        0x01    /* play (and record) the cycle over and over */
#define SF_CLICK_REC    0x02    /* the click while recording */
#define SF_CLICK_PLAY   0x04    /* and while playing */
#define SF_COUNTIN      0x08    /* a bar of clicks before recording */
#define SO_BPM          6       /* 2 bytes: 20-300 */
#define SO_BEATS        8       /* beats a bar, 1-16 */
#define SO_SEL          9       /* the selected track (the keyboard plays it, recording goes there) */
#define SO_CYC_A        10      /* 2 bytes: the cycle's first bar (from 0) */
#define SO_CYC_B        12      /* 2 bytes: the bar after its last */
#define SO_NAME         14      /* 16 bytes, zero-padded */
#define SO_NEV          30      /* 2 bytes: how many notes */
#define SO_PLEN         32      /* 2 bytes: the bytes of the sounds */
#define SO_TRACKS       40      /* SONG_TRACKS x TR_BYTES */
#define TR_BYTES        24
#define TR_KIND         0       /* TK_* */
#define TR_CH           1       /* its MIDI channel, 0-15 */
#define TR_FLAGS        2       /* TF_* */
#define TR_QUANT        3       /* the quantize last applied (Q_*) */
#define TR_NAME         4       /* 16 bytes: the instrument's name */
#define TF_MUTE         0x01
#define TF_SOLO         0x02
#define SO_SOUNDS       (SO_TRACKS + SONG_TRACKS * TR_BYTES)    /* 424 */
#define SOUND_MAX       152     /* (PSG_PACKED_MAX, patch.h) */
#define FM_SOUND        30      /* 0x30-0x90 for OP slots +0 +4 +8 +12, then 0xB0, 0xB4 */
#define EV_BYTES        8

/* what a track plays */
#define TK_FM           0       /* the YM2612: its six channels, each note on a free one */
#define TK_PSG          1       /* the PSG's three tone channels */
#define TK_NOISE        2       /* the PSG's noise */
#define TK_X32          3       /* the 32X's FM voices: channel TR_CH (10-13) */
#define TK_DRUMS        4       /* the 32X's drum kit (MIDI_DRUMS) */
#define TK_MIDI         5       /* nothing here: only MIDI out */
#define TK_COUNT        6

/* quantize grids (TR_QUANT): OFF puts the notes back where they were played */
#define Q_OFF           0
#define Q_4             1       /* 96 ticks */
#define Q_8             2       /* 48 */
#define Q_8T            3       /* 32 */
#define Q_16            4       /* 24 */
#define Q_16T           5       /* 16 */
#define Q_32            6       /* 12 */
#define Q_COUNT         7

/* CMD_SEQ (comm.h): P1 = SQ_* << 8 | a byte, P2, P3 -> the reply below */
#define SQ_STATE        0       /* -> SQS_*; P1 = tick >> 16, P2 = tick & 0xFFFF, P3 = edits */
#define SQS_PLAY        0x01
#define SQS_REC         0x02
#define SQS_LIVE        0x04    /* the keyboard plays the selected track */
#define SQS_COUNTIN     0x08
#define SQS_FULL        0x10    /* notes did not fit */
#define SQ_PEEK         1       /* P2 = offset: 6 bytes into P1..P3 */
#define SQ_POKE         2       /* P1 low = 1 or 2 bytes, P2 = offset (the header and the
                                 * track heads, not SO_NEV / SO_PLEN), P3 = the value */
#define SQ_PLAY         3       /* P3 = 1: record (on the selected track) */
#define SQ_STOP         4
#define SQ_LOCATE       5       /* P1 low = tick >> 16, P2 = tick & 0xFFFF */
#define SQ_LIVE         6       /* P2 = 1: the keyboard plays the selected track */
#define SQ_NEW          7
#define SQ_SOUND        8       /* P1 low = track; P2 = 0xFFFF: its sound into BUF_DATA
                                 * -> its length; else P2 bytes from BUF_DATA become it */
#define SQ_NOTES        9       /* P1 low = track, P2 = from (in 16ths), P3 = how long (16ths):
                                 * into BUF_DATA a count (2 bytes), then NOTE_REC bytes each:
                                 * index (2, 0x8000+: still in the take), tick (3), note,
                                 * velocity (bit 7: the last take), length (2) */
#define NOTE_REC        9
#define NOTES_MAX       ((BUF_SIZE - BUF_DATA - 2) / NOTE_REC)
#define SQ_OVERVIEW     10      /* P2 = from (16ths), P3 = a column (16ths): 32 columns, into
                                 * BUF_DATA 4 bytes a track (bit n of byte n / 8: column n
                                 * has a note sounding) */
#define SQ_ADD          11      /* BUF_DATA: track, tick (3), note, velocity, length (2) -> 0
                                 * or ERR_TOO_BIG; P1 = its index */
#define SQ_SET          12      /* P2 = index, BUF_DATA as for SQ_ADD -> P1 = its new index */
#define SQ_DEL          13      /* P2 = index */
#define SQ_QUANT        14      /* P1 low = track, P2 = Q_* */
#define SQ_UNDO         15      /* take the last take out */
#define SQ_CLEAR        16      /* P1 low = track: all its notes */
#define SQ_SAVE         17      /* MIDI_SONGS/<BUF_NAME> */
#define SQ_LOAD         18
#define SQ_EXPORT       19      /* a standard MIDI file (type 1: a track each) */
#define SQ_FREE         20      /* P1 = notes free, P2 = notes used */
#define SQ_KEY          21      /* P2 = status << 8 | note, P3 = velocity: as the keyboard,
                                 * on the selected track (to hear a note being edited) */

#define SONG_DIR        "/MIDI_SONGS"

#endif
