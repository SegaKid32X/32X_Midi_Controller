/* test_patch.c - the patch formats round trip, and parse what we expect. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "patch.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static void random_patch(fm_patch *p, unsigned seed)
{
    srand(seed);
    p->alg = rand() & 7; p->fb = rand() & 7; p->ams = rand() & 3; p->fms = rand() & 7; p->lr = 3;
    for (int i = 0; i < 4; i++) {
        fm_op *o = &p->op[i];
        o->mul = rand() & 15; o->dt = rand() & 7; if (o->dt == 4) o->dt = 0;   /* -0 is +0 */
        o->tl = rand() & 127; o->rs = rand() & 3; o->ar = rand() & 31; o->d1r = rand() & 31;
        o->d2r = rand() & 31; o->sl = rand() & 15; o->rr = rand() & 15;
        o->ssg = (rand() & 1) ? 8 | (rand() & 7) : 0; o->am = rand() & 1;
    }
}

int main(void)
{
    static const char *names[3] = { "a.tfi", "b.DMP", "c.y12" };
    for (int fmt = 0; fmt < 3; fmt++) {
        CHECK(patch_format_of(names[fmt]) == fmt, "format of %s", names[fmt]);
        for (unsigned s = 1; s < 200; s++) {
            fm_patch a, b;
            uint8_t buf[PATCH_FILE_MAX];
            random_patch(&a, s);
            int n = patch_write(&a, fmt, "name.x", buf);
            CHECK(n == (fmt == FMT_TFI ? 42 : fmt == FMT_DMP ? 51 : 128), "size %d", n);
            CHECK(patch_parse(&b, fmt, buf, n) == 0, "parse fmt %d", fmt);
            if (fmt != FMT_DMP) { b.ams = a.ams; b.fms = a.fms; }   /* TFI, Y12: no LFO sensitivity */
            if (fmt == FMT_TFI) for (int i = 0; i < 4; i++) b.op[i].am = a.op[i].am;
            CHECK(!memcmp(&a, &b, sizeof a), "round trip fmt %d seed %u", fmt, s);
        }
    }
    /* registers: a voice to registers and back */
    for (unsigned s = 1; s < 50; s++) {
        fm_patch a, b;
        uint8_t shadow[512] = { 0 }, regs[32][2];
        random_patch(&a, s);
        a.lr = s & 3;
        for (int ch = 0; ch < 6; ch++) {
            int n = patch_regs(&a, ch, regs);
            CHECK(n == 30, "%d registers", n);
            for (int i = 0; i < n; i++) shadow[(ch / 3) * 256 + regs[i][0]] = regs[i][1];
            patch_from_regs(&b, ch, shadow);
            CHECK(!memcmp(&a, &b, sizeof a), "registers ch %d seed %u", ch, s);
        }
    }
    /* a known TFI: operator order S1 S3 S2 S4, detune 3 = none */
    uint8_t t[42] = { 4, 5 };
    for (int k = 0; k < 4; k++) { t[2 + k * 10] = (uint8_t)(k + 1); t[3 + k * 10] = (uint8_t)(k + 2); }
    fm_patch p;
    CHECK(!patch_parse(&p, FMT_TFI, t, 42), "parse tfi");
    CHECK(p.alg == 4 && p.fb == 5, "alg/fb");
    CHECK(p.op[0].mul == 1 && p.op[2].mul == 2 && p.op[1].mul == 3 && p.op[3].mul == 4, "op order");
    CHECK(p.op[0].dt == 5 && p.op[2].dt == 0 && p.op[1].dt == 1 && p.op[3].dt == 2, "detune");
    /* old DMP versions */
    uint8_t d[60] = { 0x08, 0x01, 3, 6, 2, 1 };
    CHECK(!patch_parse(&p, FMT_DMP, d, 50) && p.fms == 3 && p.fb == 6 && p.alg == 2 && p.ams == 1, "dmp v8");
    d[0] = 0x0C;
    CHECK(patch_parse(&p, FMT_DMP, d, 51), "dmp v12 refused");
    printf(fails ? "test_patch: %d failure(s)\n" : "ok   patch formats: TFI, DMP, Y12, registers%.0d\n", fails);
    return fails != 0;
}
