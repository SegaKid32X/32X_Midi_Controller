/* md_files.c - the patch files: listing the SD card's MIDI_PATCHES folder,
 * loading, saving.
 *
 * The list holds every .TFI, .DMP and .Y12 file in /MIDI_PATCHES, in
 * the EverDrive's sorted order. Names are packed into one pool (they can be
 * long) so the SH-2 can ask for them a few bytes at a time. Test builds
 * without an EverDrive use patches built into the ROM instead (md_vfs.c).
 */
#include "md.h"

#define POOL_BYTES  24576

uint8_t file_buf[BUF_SIZE];
static char pool[POOL_BYTES];
static uint16_t name_at[MAX_FILES];
static int count;
static int from_vfs;
static char folder[] = "/MIDI_PATCHES";     /* (in RAM: md_ed.c reads it with RV = 1) */
static char path[96];               /* the folder + "/" + a name */

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

/* .tfi .dmp .y12 (any case) */
static int is_patch(const char *n)
{
    int len = 0;
    while (n[len]) len++;
    if (len < 5 || n[0] == '.') return 0;
    const char *e = n + len - 4;
    if (e[0] != '.') return 0;
    int a = lower(e[1]), b = lower(e[2]), c = lower(e[3]);
    return (a == 't' && b == 'f' && c == 'i') ||
           (a == 'd' && b == 'm' && c == 'p') ||
           (a == 'y' && b == '1' && c == '2');
}

int files_scan(void)
{
    count = 0;
    from_vfs = 0;
    int used = 0;
    if (!(md_flags & FL_ED)) {
        set_flag(FL_ED, ed_detect());
        if (!(md_flags & FL_ED)) {
            int n = vfs_scan();
            if (n <= 0) return ERR_NO_ED;
            from_vfs = 1;
            count = n > MAX_FILES ? MAX_FILES : n;
            return count;
        }
    }
    int total = ed_dir_open(folder);
    if (total < 0) return 0;            /* no folder yet: no patches (saving makes it) */
    for (int i = 0; i < total && count < MAX_FILES; i++) {
        char name[256];
        int is_dir;
        rx_poll();
        if (ed_dir_entry(i, name, sizeof name, &is_dir)) return ERR_SD;
        if (is_dir || !is_patch(name)) continue;
        int len = 0;
        while (name[len]) len++;
        if (len > 74 || used + len + 1 > POOL_BYTES) continue;   /* (path[] has room for 74) */
        name_at[count++] = (uint16_t)used;
        for (int k = 0; k <= len; k++) pool[used++] = name[k];
    }
    return count;
}

const char *file_name(int i)
{
    if (i < 0 || i >= count) return "";
    if (from_vfs) {
        const char *n;
        uint16_t len;
        vfs_file(i, &n, &len);
        return n;
    }
    return pool + name_at[i];
}

int file_name_len(int i)
{
    const char *n = file_name(i);
    int len = 0;
    while (n[len]) len++;
    return len;
}

static void make_path(const char *name)
{
    int k = 0;
    for (const char *f = folder; *f; f++) path[k++] = *f;
    path[k++] = '/';
    while (*name && k < (int)sizeof path - 1) path[k++] = *name++;
    path[k] = 0;
}

/* File i into file_buf + BUF_DATA */
int file_load(int i, uint16_t *size)
{
    if (i < 0 || i >= count) return ERR_RANGE;
    if (from_vfs) {
        const char *n;
        uint16_t len;
        const uint8_t *p = vfs_file(i, &n, &len);
        if (len > BUF_SIZE - BUF_DATA) return ERR_TOO_BIG;
        for (int k = 0; k < len; k++) file_buf[BUF_DATA + k] = p[k];
        *size = len;
        return 0;
    }
    make_path(file_name(i));
    return ed_read_file(path, file_buf + BUF_DATA, BUF_SIZE - BUF_DATA, size);
}

/* The name at BUF_NAME, the data at BUF_DATA */
int file_save(uint16_t len)
{
    if (from_vfs || !(md_flags & FL_ED)) return ERR_NO_ED;
    if (len > BUF_SIZE - BUF_DATA) return ERR_TOO_BIG;
    file_buf[BUF_DATA - 1] = 0;
    make_path((const char *)file_buf + BUF_NAME);
    (void)ed_mkdir(folder);             /* (an error if it is there already) */
    return ed_write_file(path, file_buf + BUF_DATA, len);
}

int file_delete(void)
{
    if (from_vfs || !(md_flags & FL_ED)) return ERR_NO_ED;
    file_buf[BUF_DATA - 1] = 0;
    make_path((const char *)file_buf + BUF_NAME);
    return ed_delete(path);
}
