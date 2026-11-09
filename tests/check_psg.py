#!/usr/bin/env python3
"""After tests/scripts/1x_psg*.txt: PSG 1 played the chosen instrument
(PSG/Arp Major.dmp: an arpeggio 0 0 4 4 7 7): over a few frames its tone
register took more than one value, and it sounded (volume not off).

usage: check_psg.py OUTDIR ELF
"""
import subprocess
import sys

out, elf = sys.argv[1], sys.argv[2]
nm = "/opt/toolchains/sega/m68k-elf/bin/m68k-elf-nm"
sym = {l.split()[2]: int(l.split()[0], 16) for l in subprocess.run([nm, elf], capture_output=True, text=True).stdout.splitlines() if len(l.split()) == 3}
psg = (sym["shadow"] & 0xFFFF) + 512            # SH_PSG: tone 0 at 0-1, volumes at 8-11
tones, vols = [], []
for n in ("p1", "p2", "p3", "p4"):
    ram = open(f"{out}/{n}.ram", "rb").read()
    tones.append(ram[psg] << 8 | ram[psg + 1])
    vols.append(ram[psg + 8])
bad = []
if len(set(tones)) < 2:
    bad.append(f"the tone stayed {tones[0]:#x}: no arpeggio")
if any(v >= 15 for v in vols):
    bad.append(f"silent (attenuation {vols})")
if bad:
    print("check_psg: " + "; ".join(bad))
    sys.exit(1)
print(f"check_psg: tones {[hex(t) for t in tones]}, attenuation {vols}")
