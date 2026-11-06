/* md_files.c - the patch files: listing the SD card's MIDI_PATCHES folder,
 * loading, saving.
 *
 * The list holds every .TFI, .DMP and .Y12 file in /MIDI_PATCHES, in
 * the EverDrive's sorted order. Names are packed into one pool (they can be
 * long) so the SH-2 can ask for them a few bytes at a time. Test builds
 * without an EverDrive use patches built into the ROM instead (md_vfs.c).
 */
#include "md.h"

#define POOL_BYTES  40960

uint8_t file_buf[BUF_SIZE];
static char pool[POOL_BYTES];
static uint16_t name_at[MAX_FILES];
static uint16_t vfs_index[MAX_FILES];   /* (built-in patches: which one) */
static uint8_t is_dir_of[MAX_FILES];
static int count, used;
static int from_vfs;
static char folder[] = "/MIDI_PATCHES";     /* (in RAM: md_ed.c reads it with RV = 1) */
static char sub[160];                   /* the folder shown, inside it ("" = the top) */
static char path[320];                  /* folder / sub / name */
int files_skipped;                      /* patch files left out (names too long, too many) */

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }

/* .tfi .dmp .y12 (any case) */
static int is_patch(const char *n)
{
    int len = slen(n);
    if (len < 5 || n[0] == '.') return 0;
    const char *e = n + len - 4;
    if (e[0] != '.') return 0;
    int a = lower(e[1]), b = lower(e[2]), c = lower(e[3]);
    return (a == 't' && b == 'f' && c == 'i') ||
           (a == 'd' && b == 'm' && c == 'p') ||
           (a == 'y' && b == '1' && c == '2');
}

/* the current folder: a path inside MIDI_PATCHES, from BUF_NAME */
void files_cd(void)
{
    file_buf[BUF_DATA - 1] = 0;
    const char *n = (const char *)file_buf + BUF_NAME;
    int k = 0;
    while (*n && k < (int)sizeof sub - 1) sub[k++] = *n++;
    sub[k] = 0;
}

static int add(const char *name, int len, int dir, int vfs_i)
{
    if (count >= MAX_FILES || used + len + 1 > POOL_BYTES) { files_skipped++; return 0; }
    for (int i = 0; i < count; i++)         /* (a folder seen already) */
        if (dir && is_dir_of[i]) {
            const char *a = pool + name_at[i];
            int k = 0;
            while (k < len && a[k] == name[k]) k++;
            if (k == len && !a[k]) return 1;
        }
    name_at[count] = (uint16_t)used;
    is_dir_of[count] = (uint8_t)dir;
    vfs_index[count] = (uint16_t)vfs_i;
    for (int k = 0; k < len; k++) pool[used++] = name[k];
    pool[used++] = 0;
    count++;
    return 1;
}

static int scan_vfs(void)
{
    int n = vfs_scan(), sl = slen(sub);
    for (int pass = 0; pass < 2; pass++)    /* folders first, then files */
        for (int i = 0; i < n; i++) {
            const char *name;
            uint16_t len;
            vfs_file(i, &name, &len);
            if (name[0] == '!') continue;   /* (the test drums) */
            if (sl) {                       /* inside sub/ ? */
                int k = 0;
                while (k < sl && name[k] == sub[k]) k++;
                if (k < sl || name[k] != '/') continue;
                name += sl + 1;
            }
            int slash = 0;
            while (name[slash] && name[slash] != '/') slash++;
            if (name[slash] == '/') {
                if (pass == 0) add(name, slash, 1, i);
            } else if (pass == 1 && is_patch(name)) {
                add(name, slen(name), 0, i);
            }
        }
    return count;
}

static void make_dir_path(void)
{
    int k = 0;
    for (const char *f = folder; *f; f++) path[k++] = *f;
    if (sub[0]) {
        path[k++] = '/';
        for (const char *f = sub; *f && k < (int)sizeof path - 1; f++) path[k++] = *f;
    }
    path[k] = 0;
}

int files_scan(void)
{
    count = used = 0;
    files_skipped = 0;
    from_vfs = 0;
    if (!(md_flags & FL_ED)) {
        set_flag(FL_ED, ed_detect());
        if (!(md_flags & FL_ED)) {
            if (vfs_scan() <= 0) return ERR_NO_ED;
            from_vfs = 1;
            return scan_vfs();
        }
    }
    make_dir_path();
    int total = ed_dir_open(path);
    if (total < 0)                      /* MIDI_PATCHES itself missing: no patches yet */
        return sub[0] ? ERR_SD : 0;     /* (saving makes it); a folder in it: an error */
    for (int pass = 0; pass < 2; pass++)    /* folders first, then files */
        for (int i = 0; i < total; i++) {
            char name[256];
            int is_dir;
            rx_poll();
            if (ed_dir_entry(i, name, sizeof name, &is_dir)) {
                files_skipped += total - i;     /* (keep what we have) */
                break;
            }
            if (name[0] == '.') continue;   /* (hidden, and macOS's "._" files) */
            int len = slen(name);
            int wanted = (is_dir && pass == 0) || (!is_dir && pass == 1 && is_patch(name));
            if (!wanted) continue;
            if (len > 127) { files_skipped++; continue; }
            add(name, len, is_dir, 0);
        }
    return count;
}

const char *file_name(int i)
{
    return i >= 0 && i < count ? pool + name_at[i] : "";
}

int file_name_len(int i)
{
    return slen(file_name(i));
}

int file_is_dir(int i)
{
    return i >= 0 && i < count && is_dir_of[i];
}

/* folder / rel (a path inside MIDI_PATCHES) */
static void make_path(const char *rel)
{
    int k = 0;
    for (const char *f = folder; *f; f++) path[k++] = *f;
    path[k++] = '/';
    while (*rel && k < (int)sizeof path - 1) path[k++] = *rel++;
    path[k] = 0;
}

/* File i (of the current folder) into file_buf + BUF_DATA */
int file_load(int i, uint16_t *size)
{
    if (i < 0 || i >= count || is_dir_of[i]) return ERR_RANGE;
    if (from_vfs) {
        const char *n;
        uint16_t len;
        const uint8_t *p = vfs_file(vfs_index[i], &n, &len);
        if (len > BUF_SIZE - BUF_DATA) return ERR_TOO_BIG;
        for (int k = 0; k < len; k++) file_buf[BUF_DATA + k] = p[k];
        *size = len;
        return 0;
    }
    make_dir_path();
    int k = slen(path);
    path[k++] = '/';
    for (const char *n = pool + name_at[i]; *n && k < (int)sizeof path - 1; n++) path[k++] = *n;
    path[k] = 0;
    return ed_read_file(path, file_buf + BUF_DATA, BUF_SIZE - BUF_DATA, size);
}

/* BUF_NAME: the path inside MIDI_PATCHES; BUF_DATA: the data */
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
