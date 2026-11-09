#!/usr/bin/env python3
"""After tests/scripts/1x_poly*.txt: every FM channel of the YM2612 (the
68000's register shadow, from the harness's dumpram) holds the patch that
was chosen (Bells/Glockenspiel.tfi of the sample patches), its carriers
brought up to full volume (the LOUDNESS setting).

usage: check_poly.py OUTDIR ELF PATCH.tfi
"""
import subprocess
import sys

out, elf, tfi = sys.argv[1], sys.argv[2], sys.argv[3]
nm = "/opt/toolchains/sega/m68k-elf/bin/m68k-elf-nm"
sym = {l.split()[2]: int(l.split()[0], 16) for l in subprocess.run([nm, elf], capture_output=True, text=True).stdout.splitlines() if len(l.split()) == 3}
ram = open(f"{out}/poly.ram", "rb").read()
shadow = sym["shadow"] & 0xFFFF
d = open(tfi, "rb").read()
dt = [7, 6, 5, 0, 1, 2, 3]
want = {0xB0: (d[1] & 7) << 3 | (d[0] & 7)}
for k, op_reg in enumerate((0x0, 0x4, 0x8, 0xC)):      # the file's OP1 OP3 OP2 OP4
    o = d[2 + 10 * k: 12 + 10 * k]
    mul, det, tl, rs, ar, dr, sr, rr, sl, ssg = o
    want[0x30 + op_reg] = dt[det] << 4 | mul
    want[0x40 + op_reg] = tl
    want[0x50 + op_reg] = rs << 6 | ar
    want[0x60 + op_reg] = dr
    want[0x70 + op_reg] = sr
    want[0x80 + op_reg] = sl << 4 | rr
# LOUDNESS (on by default): the carriers come up so the loudest is at TL 0
CARRIERS = {0: [0xC], 1: [0xC], 2: [0xC], 3: [0xC], 4: [0x8, 0xC], 5: [0x8, 0x4, 0xC],
            6: [0x8, 0x4, 0xC], 7: [0x0, 0x8, 0x4, 0xC]}[d[0] & 7]
lo = min(want[0x40 + r] for r in CARRIERS)
for r in CARRIERS:
    want[0x40 + r] -= lo
bad = []
for ch in range(6):
    base = shadow + (ch // 3) * 256 + ch % 3
    for reg, v in want.items():
        got = ram[base + reg]
        if reg == 0x60 + 0:     # (AM bit off in these patches)
            pass
        if got != v:
            bad.append(f"FM {ch + 1} reg {reg:02X}: {got:02X}, the patch has {v:02X}")
if bad:
    print("check_poly: channels without the chosen patch:")
    print("\n".join("  " + b for b in bad[:12]))
    sys.exit(1)
print("check_poly: all six FM channels have the chosen patch")
