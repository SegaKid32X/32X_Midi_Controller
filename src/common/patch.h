/* patch.h - FM instruments: one voice of the YM2612, and the files that
 * hold one (TFI, DMP, Y12). Shared by the 32X program and the host tests.
 *
 * Operators are numbered as people number them (OP1-OP4). The chip's
 * registers and all three file formats list them in the order S1 S3 S2 S4.
 */
#ifndef PATCH_H
#define PATCH_H

#include <stdint.h>

typedef struct {
    uint8_t mul;        /* 0-15 */
    uint8_t dt;         /* 0-7, the register's encoding: 0-3 = 0..+3, 4-7 = -0..-3 */
    uint8_t tl;         /* 0-127 (0 = loudest) */
    uint8_t rs;         /* 0-3 */
    uint8_t ar;         /* 0-31 */
    uint8_t d1r;        /* 0-31 */
    uint8_t d2r;        /* 0-31 */
    uint8_t sl;         /* 0-15 */
    uint8_t rr;         /* 0-15 */
    uint8_t ssg;        /* 0-15: bit 3 on, bits 2-0 the shape */
    uint8_t am;         /* 0-1 */
} fm_op;

typedef struct {
    uint8_t alg;        /* 0-7 */
    uint8_t fb;         /* 0-7 */
    uint8_t ams;        /* 0-3 */
    uint8_t fms;        /* 0-7 */
    uint8_t lr;         /* bit 1 left, bit 0 right */
    fm_op   op[4];      /* OP1..OP4 */
} fm_patch;

enum { FMT_TFI, FMT_DMP, FMT_Y12, FMT_UNKNOWN };

#define PATCH_FILE_MAX  128     /* the largest file we write */

int  patch_format_of(const char *name);         /* by the extension */
/* 0, or -1 if the data is not a patch of that format */
int  patch_parse(fm_patch *p, int fmt, const uint8_t *data, int len);
/* the file's bytes into out (PATCH_FILE_MAX); the length */
int  patch_write(const fm_patch *p, int fmt, const char *name, uint8_t *out);

/* The registers of channel ch (0-5) for this voice: up to 30 pairs of
 * (register, value) in regs, for part ch / 3; the count. Pan kept as given. */
int  patch_regs(const fm_patch *p, int ch, uint8_t regs[][2]);
/* The voice that channel ch plays now, from a register shadow (part I at
 * 0-255, part II at 256-511) */
void patch_from_regs(fm_patch *p, int ch, const uint8_t *shadow);
void patch_default(fm_patch *p);

/* The register offset of operator n (0-3, OP1..OP4) */
extern const uint8_t patch_op_reg[4];

#endif
