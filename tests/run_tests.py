#!/usr/bin/env python3
"""Emulator tests: boot the test ROMs in PicoDrive and drive them.

Each tests/scripts/*.txt is run by tests/harness (a small libretro host). A
line "# rom: plain" picks rom/midi32x.32x ("md": rom/midimd-test.bin, the
Genesis-only build), else rom/midi32x-test.32x;
"# pad6" plugs in a 6-button pad.
Every screenshot must be lit and colourful (the black-screen guard; "text_"
shots need only be lit, "allow_dark" ones neither), the
screenshots of a script must not all be the same, and the harness must exit 0
(its expect/wait commands fail it otherwise).

    python3 tests/run_tests.py            every script
    python3 tests/run_tests.py 02         scripts whose name contains "02"
"""
from __future__ import annotations

import os
import subprocess
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
CORE = os.path.join(HERE, "emu", "picodrive_libretro.so")
HARNESS = os.path.join(HERE, "harness")
SCRIPTS = os.path.join(HERE, "scripts")
OUT = os.path.join(HERE, "out")
ROMS = {"test": "rom/midi32x-test.32x", "plain": "rom/midi32x.32x",
        "md": "rom/midimd-test.bin", "mdplain": "rom/midimd.bin",
        "samples": "rom/midi32x-samples.32x", "mdsamples": "rom/midimd-samples.bin"}

MIN_LIT = 0.08          # at least 8% of pixels not black
MIN_COLOURS = 6


def read_ppm(path):
    data = open(path, "rb").read()
    parts = data.split(b"\n", 3)
    w, h = map(int, parts[1].split())
    return w, h, parts[3]


def check_shot(path):
    w, h, px = read_ppm(path)
    n = w * h
    lit = sum(1 for i in range(0, len(px), 3) if px[i] > 8 or px[i + 1] > 8 or px[i + 2] > 8)
    colours = len({px[i:i + 3] for i in range(0, len(px), 3 * 7)})
    problems = []
    if lit < n * MIN_LIT:
        problems.append(f"dark ({lit * 100 // n}% lit)")
    if colours < MIN_COLOURS:
        problems.append(f"{colours} colours")
    return problems, zlib.crc32(px)


ELFS = {"rom/midi32x-test.32x": "obj/md.elf", "rom/midi32x.32x": "obj/md.elf",
        "rom/midimd-test.bin": "obj/midimd.elf", "rom/midimd.bin": "obj/midimd.elf",
        "rom/midi32x-samples.32x": "obj/md.elf", "rom/midimd-samples.bin": "obj/midimd.elf"}


def rx_symbols(rom):
    """rx_ring,rx_head of the ROM's 68000 program (the harness's midi command)"""
    nm = "/opt/toolchains/sega/m68k-elf/bin/m68k-elf-nm"
    try:
        out = subprocess.run([nm, os.path.join(ROOT, ELFS[rom])], capture_output=True, text=True).stdout
    except OSError:
        return None
    sym = {l.split()[2]: l.split()[0] for l in out.splitlines() if len(l.split()) == 3}
    if "rx_ring" in sym and "rx_head" in sym:
        return sym["rx_ring"] + "," + sym["rx_head"]
    return None


def run(script):
    name = os.path.splitext(os.path.basename(script))[0]
    rom = ROMS["test"]
    env = dict(os.environ)
    for line in open(script):
        if line.startswith("# rom:"):
            rom = ROMS[line.split(":", 1)[1].strip()]
        if line.startswith("# pad6"):           # a 6-button pad (X Y Z MODE)
            env["HARNESS_PAD6"] = "1"
    rx = rx_symbols(rom)
    if rx:
        env["HARNESS_RX"] = rx
    out = os.path.join(OUT, name)
    os.makedirs(out, exist_ok=True)
    for f in os.listdir(out):
        if f.endswith(".ppm"):
            os.remove(os.path.join(out, f))
    p = subprocess.run([HARNESS, CORE, os.path.join(ROOT, rom), script, out],
                       capture_output=True, text=True, env=env)
    failures = []
    if p.returncode:
        failures.append(f"harness exit {p.returncode}: {p.stderr.strip()[-300:]}")
    shots = sorted(f for f in os.listdir(out) if f.endswith(".ppm"))
    crcs = set()
    for s in shots:
        problems, crc = check_shot(os.path.join(out, s))
        crcs.add(crc)
        if s.startswith("text_"):           # a text panel: lit, few colours
            problems = [p for p in problems if not p.endswith("colours")]
        if problems and not s.startswith("allow_dark"):
            failures.append(f"{s}: {', '.join(problems)}")
    if len(shots) > 1 and len(crcs) == 1:
        failures.append("every screenshot is the same")
    for line in open(script):                   # "# check: psg": tests/check_psg.py
        if line.strip() == "# check: psg":
            c = subprocess.run([sys.executable, os.path.join(HERE, "check_psg.py"), out,
                                os.path.join(ROOT, ELFS[rom])], capture_output=True, text=True)
            if c.returncode:
                failures.append(c.stdout.strip())
    for line in open(script):                   # "# check: psged": tests/check_psged.py
        if line.strip() == "# check: psged":
            c = subprocess.run([sys.executable, os.path.join(HERE, "check_psged.py"), out,
                                os.path.join(ROOT, ELFS[rom])], capture_output=True, text=True)
            if c.returncode:
                failures.append(c.stdout.strip())
    for line in open(script):                   # "# check: poly": tests/check_poly.py
        if line.strip() == "# check: poly":
            c = subprocess.run([sys.executable, os.path.join(HERE, "check_poly.py"), out,
                                os.path.join(ROOT, ELFS[rom]),
                                os.path.join(ROOT, "obj/samples/Bells/Glockenspiel.tfi")],
                               capture_output=True, text=True)
            if c.returncode:
                failures.append(c.stdout.strip())
    print(f"{'FAIL' if failures else 'ok  '} {name} ({len(shots)} shots)")
    for f in failures:
        print("     " + f)
    return not failures


def main():
    if not os.path.exists(CORE):
        print("SKIP: no tests/emu/picodrive_libretro.so")
        return 0
    want = sys.argv[1] if len(sys.argv) > 1 else ""
    scripts = sorted(os.path.join(SCRIPTS, f) for f in os.listdir(SCRIPTS)
                     if f.endswith(".txt") and want in f)
    ok = all([run(s) for s in scripts])
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
