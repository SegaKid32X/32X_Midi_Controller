/* md_drums.c - the drum kit: the .WAV files in the SD card's MIDI_DRUMS
 * folder, copied into cartridge memory (from 0x100000), where the 32X plays
 * them (src/32x/synth.c).
 *
 * Each file gets a MIDI note (General MIDI drum notes where the name says):
 *   - a name that starts with a number: that note ("36 Kick.wav", "038_snr.wav")
 *   - else words in the name: kick 36, rim 37, snare 38, clap 39, closed hat
 *     42, pedal hat 44, open hat 46, low tom 45, mid tom 47, high tom 50,
 *     crash 49, ride 51, tambourine 54, cowbell 56, shaker 70, ...
 *   - else the next free note from 60 up.
 */
#include "md.h"

#define CART_DRUMS  0x100000u
#define CART_END    0x400000u
#define POOL        3072

typedef struct { uint8_t note, vfs; uint16_t name_at; uint32_t addr, size; } drum_t;
static drum_t drums[MAX_DRUMS];
static int ndrums;
static char pool[POOL];
static int pool_used;
static char dpath[160] = "/MIDI_DRUMS";

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static int has(const char *n, const char *w)
{
    for (; *n; n++) {
        int k = 0;
        while (w[k] && lower(n[k]) == w[k]) k++;
        if (!w[k]) return 1;
    }
    return 0;
}

static int is_wav(const char *n)
{
    int len = 0;
    while (n[len]) len++;
    return len > 4 && n[len - 4] == '.' && lower(n[len - 3]) == 'w' && lower(n[len - 2]) == 'a'
           && lower(n[len - 1]) == 'v';
}

static int taken(int note)
{
    for (int i = 0; i < ndrums; i++) if (drums[i].note == note) return 1;
    return 0;
}

static int note_for(const char *n)
{
    if (n[0] >= '0' && n[0] <= '9') {
        int v = 0;
        for (int k = 0; k < 3 && n[k] >= '0' && n[k] <= '9'; k++) v = v * 10 + n[k] - '0';
        if (v < 128) return v;
    }
    static const struct { const char *w; uint8_t note; } words[] = {
        { "kick", 36 }, { "bass drum", 36 }, { "bd", 36 }, { "rim", 37 }, { "stick", 37 },
        { "snare", 38 }, { "sd", 38 }, { "clap", 39 },
        { "open", 46 }, { "pedal", 44 }, { "hat", 42 }, { "hh", 42 },
        { "low tom", 45 }, { "lo tom", 45 }, { "floor", 43 }, { "mid tom", 47 }, { "hi tom", 50 },
        { "high tom", 50 }, { "tom", 48 }, { "crash", 49 }, { "china", 52 }, { "splash", 55 },
        { "ride", 51 }, { "bell", 53 }, { "tamb", 54 }, { "cowbell", 56 }, { "cow", 56 },
        { "conga", 63 }, { "bongo", 60 }, { "clave", 75 }, { "shake", 70 }, { "maraca", 70 },
        { "cabasa", 69 }, { "triangle", 81 }, { "block", 76 }, { "guiro", 73 }, { "whistle", 71 },
    };
    for (unsigned k = 0; k < sizeof words / sizeof words[0]; k++)
        if (has(n, words[k].w) && !taken(words[k].note)) return words[k].note;
    for (int v = 60; v < 128; v++)
        if (!taken(v)) return v;
    return 127;
}

static int add_name(const char *n)
{
    int at = pool_used, k = 0;
    while (n[k] && pool_used < POOL - 1) pool[pool_used++] = n[k++];
    pool[pool_used++] = 0;
    return at;
}

/* The kit: the folder at BUF_NAME ("" = MIDI_DRUMS itself, else a folder in it) */
int drums_scan(void)
{
    ndrums = 0;
    pool_used = 0;
    file_buf[BUF_DATA - 1] = 0;
    const char *sub = (const char *)file_buf + BUF_NAME;
    int k = 0;
    for (const char *f = "/MIDI_DRUMS"; *f; f++) dpath[k++] = *f;
    if (*sub) {
        dpath[k++] = '/';
        while (*sub && k < 120) dpath[k++] = *sub++;
    }
    dpath[k] = 0;
    if (md_flags & FL_VFS && !(md_flags & FL_ED)) {
        /* test builds: "!drums/" files built into the ROM, played where they are */
        int n = vfs_scan();
        for (int i = 0; i < n && ndrums < MAX_DRUMS; i++) {
            const char *name;
            uint16_t len;
            const uint8_t *p = vfs_file(i, &name, &len);
            static const char tag[] = "!drums/";
            int t = 0;
            while (tag[t] && name[t] == tag[t]) t++;
            if (tag[t] || !is_wav(name + t)) continue;
            drum_t *d = &drums[ndrums];
            d->note = (uint8_t)note_for(name + t);
            d->addr = (uint32_t)(p - (const uint8_t *)0x880000);
            d->size = len;
            d->vfs = 1;
            d->name_at = (uint16_t)add_name(name + t);
            ndrums++;
        }
        return ndrums;
    }
    if (!(md_flags & FL_ED)) {
        set_flag(FL_ED, ed_detect());
        if (!(md_flags & FL_ED)) return 0;
    }
    int total = ed_dir_open(dpath);
    if (total < 0) return 0;
    uint32_t at = CART_DRUMS;
    for (int i = 0; i < total && ndrums < MAX_DRUMS; i++) {
        char name[128];
        int is_dir;
        rx_poll();
        if (ed_dir_entry(i, name, sizeof name, &is_dir)) break;
        if (is_dir || name[0] == '.' || !is_wav(name)) continue;
        int pl = 0;
        static char fpath[300];
        for (const char *f = dpath; *f; f++) fpath[pl++] = *f;
        fpath[pl++] = '/';
        for (const char *f = name; *f && pl < 298; f++) fpath[pl++] = *f;
        fpath[pl] = 0;
        uint32_t len;
        if (ed_load_to_cart(fpath, at, CART_END - at, &len)) continue;
        drum_t *d = &drums[ndrums];
        d->note = (uint8_t)note_for(name);
        d->addr = at;
        d->size = len;
        d->vfs = 0;
        d->name_at = (uint16_t)add_name(name);
        ndrums++;
        at = (at + len + 255) & ~255u;
        rx_poll();
    }
    return ndrums;
}

/* CMD_DRUM: P1 = i, P2 = 0xFFFF: P1 note << 8 | address bits 16-23, P2 the
 * address's low 16 bits, P3 the size in 64-byte units; else 6 name bytes
 * from offset P2 */
unsigned drum_info(int i, int off)
{
    if (i < 0 || i >= ndrums) return ERR_RANGE;
    drum_t *d = &drums[i];
    if (off == 0xFFFF) {
        COMM16(C_P1) = (uint16_t)(d->note << 8 | (d->addr >> 16 & 0xFF));
        COMM16(C_P2) = (uint16_t)d->addr;
        COMM16(C_P3) = (uint16_t)((d->size + 63) >> 6);
        return 0;
    }
    const char *n = pool + d->name_at;
    int len = 0;
    while (n[len]) len++;
    uint8_t b[6];
    for (int k = 0; k < 6; k++) b[k] = off + k < len ? (uint8_t)n[off + k] : 0;
    COMM16(C_P1) = (uint16_t)(b[0] << 8 | b[1]);
    COMM16(C_P2) = (uint16_t)(b[2] << 8 | b[3]);
    COMM16(C_P3) = (uint16_t)(b[4] << 8 | b[5]);
    return (unsigned)(len > 255 ? 255 : len);
}
