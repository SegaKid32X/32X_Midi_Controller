/* md_vfs.c - patch files built into the ROM (test builds; the emulator has
 * no EverDrive). tools/mkvfs.py appends them after the program, inside the
 * first 512 KiB (which the 68000 sees at 0x880000), and writes "PATFS@" and
 * the offset into the ROM header at 0x1C8.
 *
 * Layout (big-endian): "PATFS1\0\0", u32 count, then count entries of 40
 * bytes: name[32] (zero-padded), u32 offset (from the start of the ROM),
 * u32 length.
 */
#include "md.h"

#define ROM     ((const uint8_t *)0x880000)

static const uint8_t *dir;
static int n;

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

int vfs_scan(void)
{
    static const char tag[] = "PATFS@";
    for (int i = 0; i < 6; i++)
        if (ROM[0x1C8 + i] != (uint8_t)tag[i])
            return 0;
    uint32_t at = rd32(ROM + 0x1CE);
    if (at >= 0x80000 || ROM[at] != 'P') return 0;
    dir = ROM + at;
    n = (int)rd32(dir + 8);
    return n;
}

const uint8_t *vfs_file(int i, const char **name, uint16_t *len)
{
    const uint8_t *e = dir + 12 + i * 40;
    *name = (const char *)e;
    *len = (uint16_t)rd32(e + 36);
    return ROM + rd32(e + 32);
}
