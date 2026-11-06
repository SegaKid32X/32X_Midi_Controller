/* md_ed.c - the Mega EverDrive PRO's SD card, from the 68000 (from SegaTube).
 *
 * The protocol is the EverDrive's documented I/O API (krikzz/mega-ed-pub,
 * edio/everdrive.c, MIT licence): commands go byte by byte through a FIFO at
 * 0xA130D1 as '+', ~'+', cmd, ~cmd and their arguments, most significant
 * byte first; answers come back through the same FIFO.
 *
 * With the 32X attached the EverDrive's I/O is only there with RV = 1 (the
 * 32X hands the cartridge bus to the 68000): with RV = 0 it reads as open bus
 * and writes are lost (console tests, 2026-11-04; ZERO SECTOR's EverDrive
 * save code sets RV = 1 the same way). With RV = 1 the 68000 cannot run code
 * from the cartridge, so this whole driver lives in work RAM (section
 * .ramtext, copied there with .data at start-up) and turns RV on only for
 * each operation. It must not call anything outside work RAM meanwhile (no
 * library calls: tools/check_ramtext.py checks the build). The SH-2s keep off
 * the cartridge while a command runs (they wait for the answer in SDRAM).
 *
 * Patch files are small: they go through the FIFO to and from work RAM.
 *
 * Every wait has a time limit, so a console without an EverDrive (or an
 * emulator) gets "no EverDrive" instead of hanging.
 */
#include "md.h"

#define RAM __attribute__((section(".ramtext"), noinline))

#define ED_FIFO     (*(volatile uint8_t *)0xA130D1)
#define ED_FSTAT    (*(volatile uint16_t *)0xA130D2)
#define ED_SYSSTAT  (*(volatile uint16_t *)0xA130D4)

#define FIFO_RXF_MSK 0x07FF

#define CMD_STATUS   0x10
#define CMD_DISK_INIT 0xC0
#define CMD_F_DIR_LD 0xC5
#define CMD_F_DIR_SIZE 0xC6
#define CMD_F_DIR_GET 0xC8
#define CMD_F_FOPN   0xC9
#define CMD_F_FRD    0xCA
#define CMD_F_FWR    0xCC
#define CMD_F_FCLOSE 0xCE
#define CMD_F_FINFO  0xD0
#define CMD_F_DEL    0xD3

#define STATUS_CMD_OK 0x02
#define STATUS_STROBE 0x08
#define FA_READ      0x01
#define FA_WRITE     0x02
#define FA_CREATE_ALWAYS 0x08
#define DIR_OPT_SORTED 0x01
#define AT_DIR       0x10

#define ERR_TIMEOUT  0x50
#define ERR_STATUS   0x40

static int ed_ok;
static long spin_limit = 600000;    /* FIFO waits (about 1.5 s) */
/* (strings in work RAM too: string constants would be in the cartridge) */
static char path_root[] = "/";

/* ---- RV ---- */

/* With RV = 1 the 68000's vectors come from the cartridge's Mega Drive
 * table, not the 32X's: no interrupts meanwhile (the GenMDM's strobe would
 * jump into the boot code). */
static void RAM rv_on(void)
{
    __asm__ volatile ("move.w #0x2700,%%sr\n\tbset #0,0xA15107" ::: "memory", "cc");
}

static void RAM rv_off(void)
{
    __asm__ volatile ("bclr #0,0xA15107\n\tmove.w #0x2100,%%sr" ::: "memory", "cc");
}

/* ---- the FIFO ---- */

static void RAM fifo_wr8(unsigned v) { ED_FIFO = (uint8_t)v; }
static void RAM fifo_wr16(unsigned v) { ED_FIFO = (uint8_t)(v >> 8); ED_FIFO = (uint8_t)v; }
static void RAM fifo_wr32(uint32_t v)
{
    ED_FIFO = (uint8_t)(v >> 24);
    ED_FIFO = (uint8_t)(v >> 16);
    ED_FIFO = (uint8_t)(v >> 8);
    ED_FIFO = (uint8_t)v;
}

/* Read len bytes; 0, or -1 if the EverDrive stopped answering. */
static int RAM fifo_rd(uint8_t *p, int len)
{
    long spins = 0;
    while (len) {
        int n = ED_FSTAT & FIFO_RXF_MSK;
        if (!n) {
            if (++spins > spin_limit)
                return -1;
            continue;
        }
        spins = 0;
        if (n > len) n = len;
        len -= n;
        while (n--)
            *p++ = ED_FIFO;
    }
    return 0;
}

static void RAM fifo_flush(void)
{
    for (int i = 0; i < 4096 && (ED_FSTAT & FIFO_RXF_MSK); i++)
        (void)ED_FIFO;
}

static void RAM cmd_tx(unsigned cmd)
{
    fifo_wr8('+');
    fifo_wr8('+' ^ 0xFF);
    fifo_wr8(cmd);
    fifo_wr8(cmd ^ 0xFF);
}

static void RAM tx_string(const char *s)
{
    int n = 0;
    while (s[n]) n++;
    fifo_wr16((unsigned)n);
    for (int i = 0; i < n; i++)
        fifo_wr8((uint8_t)s[i]);
}

/* The status command: 0 if the last command went well, else its error. */
static int RAM check_status(void)
{
    uint8_t b[2];
    cmd_tx(CMD_STATUS);
    if (fifo_rd(b, 2))
        return ERR_TIMEOUT;
    if (b[0] != 0xA5)
        return ERR_STATUS;
    return b[1];
}

/* ---- the commands (each one RV = 1 only while it runs) ---- */

static int RAM detect_rv(void)
{
    fifo_flush();
    spin_limit = 100000;                    /* no EverDrive: give up quickly */
    uint8_t b[2];
    cmd_tx(CMD_STATUS);
    int r = fifo_rd(b, 2);
    spin_limit = 600000;
    /* (the second byte is the previous command's result, which can be an
     * error left over from the EverDrive's menu: only the key matters) */
    if (r || b[0] != 0xA5)
        return 0;
    /* the SD card: initialise it if the root does not list */
    cmd_tx(CMD_F_DIR_LD);
    fifo_wr8(0);
    tx_string(path_root);
    if (check_status()) {
        cmd_tx(CMD_DISK_INIT);
        if (check_status())
            return 0;
    }
    return 1;
}

int RAM ed_detect(void)
{
    rv_on();
    ed_ok = detect_rv();
    rv_off();
    return ed_ok;
}

static int RAM rx_name(char *out, int max)
{
    uint8_t b[2];
    if (fifo_rd(b, 2)) return -1;
    int n = b[0] << 8 | b[1];
    for (int i = 0; i < n; i++) {
        uint8_t c;
        if (fifo_rd(&c, 1)) return -1;
        if (i < max - 1) out[i] = (char)c;
    }
    out[n < max - 1 ? n : max - 1] = 0;
    return 0;
}

/* Load a folder's listing (sorted) into the EverDrive: the entry count, or -1 */
int RAM ed_dir_open(const char *path)
{
    if (!ed_ok) return -1;
    COMM16(C_FLAGS) |= FL_BUSY;
    rv_on();
    int r = -1;
    cmd_tx(CMD_F_DIR_LD);
    fifo_wr8(DIR_OPT_SORTED);
    tx_string(path);
    if (!check_status()) {
        uint8_t b[2];
        cmd_tx(CMD_F_DIR_SIZE);
        if (!fifo_rd(b, 2))
            r = b[0] << 8 | b[1];
    }
    rv_off();
    COMM16(C_FLAGS) &= (uint16_t)~FL_BUSY;
    return r;
}

/* Entry i of the loaded listing: its name (cut to max - 1) and whether it is
 * a folder. 0, or -1 */
int RAM ed_dir_entry(int i, char *name, int max, int *is_dir)
{
    if (!ed_ok) return -1;
    COMM16(C_FLAGS) |= FL_BUSY;
    rv_on();
    int r = -1;
    cmd_tx(CMD_F_DIR_GET);
    fifo_wr16((unsigned)i);
    fifo_wr16(1);
    fifo_wr16(255);
    uint8_t resp, info[9];
    if (!fifo_rd(&resp, 1) && !resp && !fifo_rd(info, 9) && !rx_name(name, max)) {
        *is_dir = (info[8] & AT_DIR) != 0;
        r = 0;
    }
    rv_off();
    COMM16(C_FLAGS) &= (uint16_t)~FL_BUSY;
    return r;
}

/* A whole (small) file into dst: 0 or an error; *len = its size */
int RAM ed_read_file(const char *path, uint8_t *dst, int max, uint16_t *len)
{
    if (!ed_ok) return ERR_STATUS;
    COMM16(C_FLAGS) |= FL_BUSY;
    rv_on();
    int e;
    uint32_t size = 0;
    cmd_tx(CMD_F_FINFO);
    tx_string(path);
    uint8_t resp, info[9];
    char skip[2];
    if (fifo_rd(&resp, 1)) { e = ERR_TIMEOUT; goto out; }
    if (resp) { e = resp; goto out; }
    if (fifo_rd(info, 9) || rx_name(skip, 1)) { e = ERR_TIMEOUT; goto out; }
    size = (uint32_t)info[0] << 24 | (uint32_t)info[1] << 16 | (uint32_t)info[2] << 8 | info[3];
    if (size > (uint32_t)max) { e = ERR_TOO_BIG; goto out; }
    cmd_tx(CMD_F_FOPN);
    fifo_wr8(FA_READ);
    tx_string(path);
    e = check_status();
    if (e) goto out;
    for (uint32_t done = 0; done < size && !e; ) {
        uint32_t block = size - done > 512 ? 512 : size - done;
        cmd_tx(CMD_F_FRD);
        fifo_wr32(block);
        if (fifo_rd(&resp, 1)) { e = ERR_TIMEOUT; break; }
        if (resp) { e = resp; break; }
        if (fifo_rd(dst + done, (int)block)) { e = ERR_TIMEOUT; break; }
        done += block;
    }
    cmd_tx(CMD_F_FCLOSE);
    if (!e) e = check_status();
    else (void)check_status();
out:
    rv_off();
    COMM16(C_FLAGS) &= (uint16_t)~FL_BUSY;
    *len = (uint16_t)size;
    return e;
}

/* Create or replace a file with len bytes from src: 0 or an error */
int RAM ed_write_file(const char *path, const uint8_t *src, int len)
{
    if (!ed_ok) return ERR_STATUS;
    COMM16(C_FLAGS) |= FL_BUSY;
    rv_on();
    cmd_tx(CMD_F_FOPN);
    fifo_wr8(FA_WRITE | FA_CREATE_ALWAYS);
    tx_string(path);
    int e = check_status();
    if (!e) {
        cmd_tx(CMD_F_FWR);
        fifo_wr32((uint32_t)len);
        while (len > 0 && !e) {             /* the EverDrive asks for each 1 KiB */
            int block = len > 1024 ? 1024 : len;
            uint8_t resp;
            if (fifo_rd(&resp, 1)) { e = ERR_TIMEOUT; break; }
            if (resp) { e = resp; break; }
            for (int i = 0; i < block; i++)
                fifo_wr8(src[i]);
            src += block;
            len -= block;
        }
        if (!e) e = check_status();
        cmd_tx(CMD_F_FCLOSE);
        int c = check_status();
        if (!e) e = c;
    }
    rv_off();
    COMM16(C_FLAGS) &= (uint16_t)~FL_BUSY;
    return e;
}

int RAM ed_delete(const char *path)
{
    if (!ed_ok) return ERR_STATUS;
    COMM16(C_FLAGS) |= FL_BUSY;
    rv_on();
    cmd_tx(CMD_F_DEL);
    tx_string(path);
    int e = check_status();
    rv_off();
    COMM16(C_FLAGS) &= (uint16_t)~FL_BUSY;
    return e;
}
