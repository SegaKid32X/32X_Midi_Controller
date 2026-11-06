/* lib.c - the patch files and the sound chip, through the 68000. */
#include <string.h>
#include "hw.h"
#include "lib.h"

int lib_count;
char lib_path[PATH_MAX_];
static char pool[40000];
static uint16_t name_at[LIB_MAX];
static uint8_t dir_of[LIB_MAX];
uint8_t shadow_copy[528];

int lib_scan(void)
{
    lib_count = 0;
    int r = md_cmd(CMD_SCAN, 0, 0, 0);
    if (r) return -r;
    int n = md_p(C_P1);
    if (n > LIB_MAX) n = LIB_MAX;
    int used = 0;
    for (int i = 0; i < n; i++) {
        int len = 0;
        char name[NAME_MAX + 1];
        int is_dir = 0;
        for (int off = 0; ; off += 6) {
            int total = md_cmd(CMD_NAME, (uint16_t)i, (uint16_t)off, 0);
            is_dir = total >> 7;
            total &= 127;
            uint16_t w[3] = { md_p(C_P1), md_p(C_P2), md_p(C_P3) };
            for (int k = 0; k < 6 && len < NAME_MAX; k++)
                if (off + k < total)
                    name[len++] = (char)(k & 1 ? w[k >> 1] : w[k >> 1] >> 8);
            if (off + 6 >= total || len >= NAME_MAX) break;
        }
        name[len] = 0;
        if (used + len + 1 > (int)sizeof pool) break;
        dir_of[lib_count] = (uint8_t)is_dir;
        name_at[lib_count++] = (uint16_t)used;
        memcpy(pool + used, name, (size_t)len + 1);
        used += len + 1;
    }
    return lib_count;
}

const char *lib_name(int i)
{
    return i >= 0 && i < lib_count ? pool + name_at[i] : "";
}

int lib_is_dir(int i)
{
    return i >= 0 && i < lib_count && dir_of[i];
}

void lib_rel(int i, char *out)
{
    out[0] = 0;
    if (lib_path[0]) {
        strcpy(out, lib_path);
        strcat(out, "/");
    }
    strcat(out, lib_name(i));
}

const char *base_name(const char *rel)
{
    const char *b = rel;
    for (; *rel; rel++) if (*rel == '/') b = rel + 1;
    return b;
}

static int put_name(const char *name);

int lib_cd(const char *path)
{
    if (strlen(path) >= PATH_MAX_) return ERR_RANGE;
    strcpy(lib_path, path);
    put_name(lib_path);
    md_cmd(CMD_CD, 0, 0, 0);
    return lib_scan();
}

int lib_enter(int i)
{
    char p[PATH_MAX_ + NAME_MAX + 2];
    lib_rel(i, p);
    if (strlen(p) >= PATH_MAX_) return ERR_RANGE;
    return lib_cd(p);
}

int lib_up(void)
{
    if (!lib_path[0]) return 0;
    char p[PATH_MAX_];
    strcpy(p, lib_path);
    char *slash = p;
    for (char *q = p; *q; q++) if (*q == '/') slash = q;
    *slash = 0;
    lib_cd(p);
    return 1;
}

int lib_find(const char *name)
{
    for (int i = 0; i < lib_count; i++)
        if (!strcmp(lib_name(i), name)) return i;
    return -1;
}

int lib_load(int i, fm_patch *p)
{
    int r = md_cmd(CMD_LOAD, (uint16_t)i, 0, 0);
    if (r) return r;
    int size = md_p(C_P1);
    if (size > 512) size = 512;
    static uint8_t data[512];
    for (int off = 0; off < size; off += 6) {
        md_cmd(CMD_BUF_GET, (uint16_t)(BUF_DATA + off), 0, 0);
        uint16_t w[3] = { md_p(C_P1), md_p(C_P2), md_p(C_P3) };
        for (int k = 0; k < 6 && off + k < size; k++)
            data[off + k] = (uint8_t)(k & 1 ? w[k >> 1] : w[k >> 1] >> 8);
    }
    return patch_parse(p, patch_format_of(lib_name(i)), data, size) ? ERR_RANGE : 0;
}

static int put_bytes(int at, const uint8_t *b, int n)
{
    for (int off = 0; off < n; off += 4) {
        uint8_t q[4] = { 0, 0, 0, 0 };
        for (int k = 0; k < 4 && off + k < n; k++) q[k] = b[off + k];
        int r = md_cmd(CMD_BUF_PUT, (uint16_t)(at + off), (uint16_t)(q[0] << 8 | q[1]),
                       (uint16_t)(q[2] << 8 | q[3]));
        if (r) return r;
    }
    return 0;
}

static int put_name(const char *name)
{
    uint8_t n[BUF_DATA - BUF_NAME];
    memset(n, 0, sizeof n);
    int len = 0;
    for (; len < (int)sizeof n - 1 && name[len]; len++) n[len] = (uint8_t)name[len];
    return put_bytes(BUF_NAME, n, len + 1);         /* (with its zero) */
}

int lib_save(const char *name, const fm_patch *p)
{
    uint8_t data[PATCH_FILE_MAX];
    int len = patch_write(p, patch_format_of(name), base_name(name), data);
    if (!len) return ERR_RANGE;
    int r = put_name(name);
    if (!r) r = put_bytes(BUF_DATA, data, len);
    if (!r) r = md_cmd(CMD_SAVE, (uint16_t)len, 0, 0);
    return r;
}

int lib_delete(const char *name)
{
    int r = put_name(name);
    return r ? r : md_cmd(CMD_DELETE, 0, 0, 0);
}

void lib_new_name(char *out, const char *ext)
{
    for (int n = 1; n < 1000; n++) {
        char *o = out;
        const char *b = "newpatch";
        while (*b) *o++ = *b++;
        *o++ = (char)('0' + n / 100);
        *o++ = (char)('0' + n / 10 % 10);
        *o++ = (char)('0' + n % 10);
        *o++ = '.';
        while (*ext) *o++ = *ext++;
        *o = 0;
        ext -= 3;
        int taken = 0;
        for (int i = 0; i < lib_count && !taken; i++) {
            const char *a = lib_name(i);
            const char *c = out;
            while (*a && *c && (*a | 32) == (*c | 32)) { a++; c++; }
            taken = !*a && !*c;
        }
        if (!taken) return;
    }
}

/* ---- the sound chip ---- */

void ym_set(int ch, uint8_t reg, uint8_t val)
{
    int part = ch / 3;
    md_cmd(part ? CMD_YM1 : CMD_YM0, (uint16_t)((reg + ch % 3) << 8 | val), 0, 0);
    shadow_copy[part * 256 + reg + ch % 3] = val;
}

void ym_global(uint8_t reg, uint8_t val)
{
    md_cmd(CMD_YM0, (uint16_t)(reg << 8 | val), 0, 0);
    shadow_copy[reg] = val;
}

void voice_apply(int ch, const fm_patch *p)
{
    uint8_t regs[32][2];
    int n = patch_regs(p, ch, regs), part = ch / 3;
    for (int i = 0; i < n; i += 3) {
        uint16_t w[3] = { 0, 0, 0 };
        for (int k = 0; k < 3 && i + k < n; k++) {
            w[k] = (uint16_t)(regs[i + k][0] << 8 | regs[i + k][1]);
            shadow_copy[part * 256 + regs[i + k][0]] = regs[i + k][1];
        }
        md_cmd(part ? CMD_YM1 : CMD_YM0, w[0], w[1], w[2]);
    }
}

static void fetch(int from, int n)
{
    for (int i = from; i < from + n; i += 6) {
        md_cmd(CMD_SHADOW, (uint16_t)i, 0, 0);
        uint16_t w[3] = { md_p(C_P1), md_p(C_P2), md_p(C_P3) };
        for (int k = 0; k < 6 && i + k < (int)sizeof shadow_copy; k++)
            shadow_copy[i + k] = (uint8_t)(k & 1 ? w[k >> 1] : w[k >> 1] >> 8);
    }
}

void voice_read(int ch, fm_patch *p)
{
    int base = (ch / 3) * 256;
    fetch(base + 0x30, 0xB8 - 0x30);
    fetch(0x22, 6);
    patch_from_regs(p, ch, shadow_copy);
}

void shadow_refresh(void)
{
    fetch(0, sizeof shadow_copy);
}

uint8_t shadow_byte(int index)
{
    return shadow_copy[index];
}
