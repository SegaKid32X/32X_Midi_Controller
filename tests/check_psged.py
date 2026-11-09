#!/usr/bin/env python3
"""After tests/scripts/1x_psged*.txt: the 68000's instrument for PSG 1 (psgi[0])
has the editor's changes to Arp Major (volume 14 13 12 12, arpeggio
0 0 4 4 7 7 loop 1): volume step 2 down 3 to 10; arpeggio step 4 up 5 to 9
a seventh step (9 again), the loop at step 7.

usage: check_psged.py OUTDIR ELF
"""
import subprocess
import sys

out, elf = sys.argv[1], sys.argv[2]
nm = "/opt/toolchains/sega/m68k-elf/bin/m68k-elf-nm"
sym = {l.split()[2]: int(l.split()[0], 16) for l in subprocess.run([nm, elf], capture_output=True, text=True).stdout.splitlines() if len(l.split()) == 3}
ram = open(f"{out}/pe.ram", "rb").read()
a = sym["psgi"] & 0xFFFF
STEPS = 48
def seq(k):
    b = a + k * (2 + STEPS)
    n, loop = ram[b], ram[b + 1]
    return n, loop, [x - 256 if x > 127 else x for x in ram[b + 2: b + 2 + n]]
vol, arp = seq(0), seq(1)
want_vol = (4, 0xFF, [14, 10, 12, 12])
want_arp = (7, 6, [0, 0, 4, 9, 7, 7, 7])
bad = []
if vol != want_vol: bad.append(f"volume {vol}, wanted {want_vol}")
if arp != want_arp: bad.append(f"arpeggio {arp}, wanted {want_arp}")
if bad:
    print("check_psged: " + "; ".join(bad))
    sys.exit(1)
print(f"check_psged: volume {vol[2]}, arpeggio {arp[2]} loop {arp[1]}")
