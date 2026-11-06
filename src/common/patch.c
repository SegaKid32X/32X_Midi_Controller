/* patch.c - FM instruments and their files (see patch.h).
 *
 * The layouts (facts about the formats, our own code):
 *   TFI (TFM Music Maker), 42 bytes: algorithm, feedback, then for S1 S3 S2
 *     S4: MUL, DT, TL, RS, AR, D1R, D2R, RR, SL, SSG-EG. DT is 0-6 with 3 =
 *     none.
 *   DMP (DefleMask), FM: a version byte, then (by version) system and mode
 *     bytes, then FMS, FB, ALG, AMS and for S1 S3 S2 S4: MUL, TL, AR, D1R,
 *     SL, RR, AM, RS, DT (0-6, 3 = none; high nibble YM2151 DT2), D2R,
 *     SSG-EG. We write version 11 (0x0B), Genesis, FM: 51 bytes.
 *   Y12 (Gens), 128 bytes: for S1 S3 S2 S4 16 bytes each holding the raw
 *     registers 30 40 50 60 70 80 90; then algorithm, feedback at 64, 65 and
 *     the name at 80 (16 bytes).
 */
#include <string.h>
#include "patch.h"

const uint8_t patch_op_reg[4] = { 0x0, 0x8, 0x4, 0xC };   /* OP1 OP2 OP3 OP4 */
static const uint8_t file_op[4] = { 0, 2, 1, 3 };          /* file slot k -> OP */

static const uint8_t dt_from_file[7] = { 7, 6, 5, 0, 1, 2, 3 };
static const uint8_t dt_to_file[8] = { 3, 4, 5, 6, 3, 2, 1, 0 };

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

int patch_format_of(const char *name)
{
    int n = (int)strlen(name);
    if (n < 4 || name[n - 4] != '.') return FMT_UNKNOWN;
    int a = lower(name[n - 3]), b = lower(name[n - 2]), c = lower(name[n - 1]);
    if (a == 't' && b == 'f' && c == 'i') return FMT_TFI;
    if (a == 'd' && b == 'm' && c == 'p') return FMT_DMP;
    if (a == 'y' && b == '1' && c == '2') return FMT_Y12;
    return FMT_UNKNOWN;
}

void patch_default(fm_patch *p)
{
    memset(p, 0, sizeof *p);
    p->alg = 4;
    p->fb = 5;
    p->lr = 3;
    for (int i = 0; i < 4; i++) {
        fm_op *o = &p->op[i];
        o->mul = 1;
        o->tl = (i & 1) ? 0 : 30;       /* OP2 and OP4 are the carriers of algorithm 4 */
        o->ar = 31;
        o->d1r = 8;
        o->d2r = 2;
        o->sl = 2;
        o->rr = 6;
    }
}

static uint8_t dtf(uint8_t v) { return v <= 6 ? dt_from_file[v] : 0; }

static int parse_tfi(fm_patch *p, const uint8_t *d, int len)
{
    if (len < 42) return -1;
    p->alg = d[0] & 7;
    p->fb = d[1] & 7;
    for (int k = 0; k < 4; k++) {
        const uint8_t *s = d + 2 + k * 10;
        fm_op *o = &p->op[file_op[k]];
        o->mul = s[0] & 15;
        o->dt = dtf(s[1]);
        o->tl = s[2] & 127;
        o->rs = s[3] & 3;
        o->ar = s[4] & 31;
        o->d1r = s[5] & 31;
        o->d2r = s[6] & 31;
        o->rr = s[7] & 15;
        o->sl = s[8] & 15;
        o->ssg = s[9] & 15;
    }
    return 0;
}

static int parse_dmp(fm_patch *p, const uint8_t *d, int len)
{
    if (len < 2) return -1;
    int head, data;
    switch (d[0]) {
    case 0x00:
        head = 1; data = 5;
        break;
    case 0x09:
        if (d[1] != 1) return -1;           /* FM only */
        head = 3; data = 7;
        break;
    case 0x0B:
        if (len < 3 || d[1] != 2 || d[2] != 1) return -1;   /* Genesis, FM */
        head = 3; data = 7;
        break;
    default:
        if (d[0] > 0x0B || d[1] != 1) return -1;
        head = 2; data = 6;
        break;
    }
    if (len < data + 44) return -1;
    p->fms = d[head] & 7;
    p->fb = d[head + 1] & 7;
    p->alg = d[head + 2] & 7;
    p->ams = d[head + 3] & 3;
    for (int k = 0; k < 4; k++) {
        const uint8_t *s = d + data + k * 11;
        fm_op *o = &p->op[file_op[k]];
        o->mul = s[0] & 15;
        o->tl = s[1] & 127;
        o->ar = s[2] & 31;
        o->d1r = s[3] & 31;
        o->sl = s[4] & 15;
        o->rr = s[5] & 15;
        o->am = s[6] ? 1 : 0;
        o->rs = s[7] & 3;
        o->dt = dtf(s[8] & 15);
        o->d2r = s[9] & 31;
        o->ssg = s[10] & 15;
    }
    return 0;
}

static int parse_y12(fm_patch *p, const uint8_t *d, int len)
{
    if (len < 128) return -1;
    for (int k = 0; k < 4; k++) {
        const uint8_t *s = d + k * 16;
        fm_op *o = &p->op[file_op[k]];
        o->dt = (s[0] >> 4) & 7;
        o->mul = s[0] & 15;
        o->tl = s[1] & 127;
        o->rs = s[2] >> 6;
        o->ar = s[2] & 31;
        o->am = s[3] >> 7;
        o->d1r = s[3] & 31;
        o->d2r = s[4] & 31;
        o->sl = s[5] >> 4;
        o->rr = s[5] & 15;
        o->ssg = s[6] & 15;
    }
    p->alg = d[64] & 7;
    p->fb = d[65] & 7;
    return 0;
}

int patch_parse(fm_patch *p, int fmt, const uint8_t *data, int len)
{
    patch_default(p);
    memset(p->op, 0, sizeof p->op);
    p->ams = p->fms = 0;
    p->lr = 3;
    switch (fmt) {
    case FMT_TFI: return parse_tfi(p, data, len);
    case FMT_DMP: return parse_dmp(p, data, len);
    case FMT_Y12: return parse_y12(p, data, len);
    }
    return -1;
}

int patch_write(const fm_patch *p, int fmt, const char *name, uint8_t *out)
{
    memset(out, 0, PATCH_FILE_MAX);
    if (fmt == FMT_TFI) {
        out[0] = p->alg;
        out[1] = p->fb;
        for (int k = 0; k < 4; k++) {
            uint8_t *s = out + 2 + k * 10;
            const fm_op *o = &p->op[file_op[k]];
            s[0] = o->mul; s[1] = dt_to_file[o->dt & 7]; s[2] = o->tl; s[3] = o->rs;
            s[4] = o->ar; s[5] = o->d1r; s[6] = o->d2r; s[7] = o->rr; s[8] = o->sl;
            s[9] = o->ssg;
        }
        return 42;
    }
    if (fmt == FMT_DMP) {
        out[0] = 0x0B; out[1] = 0x02; out[2] = 0x01;
        out[3] = p->fms; out[4] = p->fb; out[5] = p->alg; out[6] = p->ams;
        for (int k = 0; k < 4; k++) {
            uint8_t *s = out + 7 + k * 11;
            const fm_op *o = &p->op[file_op[k]];
            s[0] = o->mul; s[1] = o->tl; s[2] = o->ar; s[3] = o->d1r; s[4] = o->sl;
            s[5] = o->rr; s[6] = o->am; s[7] = o->rs; s[8] = dt_to_file[o->dt & 7];
            s[9] = o->d2r; s[10] = o->ssg;
        }
        return 51;
    }
    if (fmt == FMT_Y12) {
        for (int k = 0; k < 4; k++) {
            uint8_t *s = out + k * 16;
            const fm_op *o = &p->op[file_op[k]];
            s[0] = (uint8_t)((o->dt & 7) << 4 | (o->mul & 15));
            s[1] = o->tl & 127;
            s[2] = (uint8_t)((o->rs & 3) << 6 | (o->ar & 31));
            s[3] = (uint8_t)((o->am & 1) << 7 | (o->d1r & 31));
            s[4] = o->d2r & 31;
            s[5] = (uint8_t)((o->sl & 15) << 4 | (o->rr & 15));
            s[6] = o->ssg & 15;
        }
        out[64] = p->alg;
        out[65] = p->fb;
        for (int i = 0; i < 16 && name && name[i] && name[i] != '.'; i++)
            out[80 + i] = (uint8_t)name[i];
        return 128;
    }
    return 0;
}

int patch_regs(const fm_patch *p, int ch, uint8_t regs[][2])
{
    int c = ch % 3, n = 0;
    for (int i = 0; i < 4; i++) {
        const fm_op *o = &p->op[i];
        uint8_t r = (uint8_t)(patch_op_reg[i] + c);
        regs[n][0] = 0x30 + r; regs[n++][1] = (uint8_t)((o->dt & 7) << 4 | (o->mul & 15));
        regs[n][0] = 0x40 + r; regs[n++][1] = o->tl & 127;
        regs[n][0] = 0x50 + r; regs[n++][1] = (uint8_t)((o->rs & 3) << 6 | (o->ar & 31));
        regs[n][0] = 0x60 + r; regs[n++][1] = (uint8_t)((o->am & 1) << 7 | (o->d1r & 31));
        regs[n][0] = 0x70 + r; regs[n++][1] = o->d2r & 31;
        regs[n][0] = 0x80 + r; regs[n++][1] = (uint8_t)((o->sl & 15) << 4 | (o->rr & 15));
        regs[n][0] = 0x90 + r; regs[n++][1] = o->ssg & 15;
    }
    regs[n][0] = (uint8_t)(0xB0 + c); regs[n++][1] = (uint8_t)((p->fb & 7) << 3 | (p->alg & 7));
    regs[n][0] = (uint8_t)(0xB4 + c);
    regs[n++][1] = (uint8_t)((p->lr & 3) << 6 | (p->ams & 3) << 4 | (p->fms & 7));
    return n;
}

void patch_from_regs(fm_patch *p, int ch, const uint8_t *shadow)
{
    const uint8_t *r = shadow + (ch / 3) * 256;
    int c = ch % 3;
    for (int i = 0; i < 4; i++) {
        fm_op *o = &p->op[i];
        int k = patch_op_reg[i] + c;
        o->dt = (r[0x30 + k] >> 4) & 7;
        o->mul = r[0x30 + k] & 15;
        o->tl = r[0x40 + k] & 127;
        o->rs = r[0x50 + k] >> 6;
        o->ar = r[0x50 + k] & 31;
        o->am = r[0x60 + k] >> 7;
        o->d1r = r[0x60 + k] & 31;
        o->d2r = r[0x70 + k] & 31;
        o->sl = r[0x80 + k] >> 4;
        o->rr = r[0x80 + k] & 15;
        o->ssg = r[0x90 + k] & 15;
    }
    p->alg = r[0xB0 + c] & 7;
    p->fb = (r[0xB0 + c] >> 3) & 7;
    p->lr = r[0xB4 + c] >> 6;
    p->ams = (r[0xB4 + c] >> 4) & 3;
    p->fms = r[0xB4 + c] & 7;
}
