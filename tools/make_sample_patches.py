#!/usr/bin/env python3
"""Write a set of sample .tfi patches (classic YM2612 recipes, with a few
variations of each) into a folder laid out as for the SD card:

    OUT/Bass/..., OUT/Keys/..., ... and OUT/Variations/ (every patch
    and its variations in one big folder: scrolling a long list);
    OUT/PSG/ and OUT/PSG Noise/: PSG instruments (.dmp, DefleMask standard)

usage: make_sample_patches.py OUT_DIR [--variations N]

TFI: algorithm, feedback, then the operators in the file's order OP1 OP3
OP2 OP4, ten bytes each: MUL, DT (3 = none), TL, RS, AR, DR, SR, RR, SL, SSG.
"""
import random
import sys
from pathlib import Path

# (name, alg, fb, [OP1, OP2, OP3, OP4]); an operator is
# (mul, dt, tl, rs, ar, d1r, d2r, rr, sl) with dt -3..3
R = [
    # ---- bass ----
    ("Bass", "Finger Bass", 0, 5, [(0, 0, 28, 1, 31, 12, 3, 8, 3), (1, 0, 38, 1, 31, 14, 3, 8, 4),
                                   (1, 1, 30, 1, 31, 10, 2, 8, 3), (1, 0, 0, 1, 31, 6, 4, 9, 2)]),
    ("Bass", "Slap Bass", 0, 6, [(1, 0, 20, 2, 31, 18, 6, 9, 5), (4, 0, 36, 2, 31, 20, 8, 9, 6),
                                 (1, 2, 24, 2, 31, 12, 4, 9, 3), (1, 0, 0, 1, 31, 8, 5, 10, 2)]),
    ("Bass", "Synth Bass", 4, 7, [(1, 0, 22, 1, 31, 8, 2, 9, 2), (1, 0, 4, 1, 31, 6, 3, 9, 2),
                                  (2, 3, 26, 1, 31, 9, 2, 9, 3), (1, -3, 6, 1, 31, 6, 3, 9, 2)]),
    ("Bass", "Fretless", 0, 3, [(0, 0, 34, 0, 26, 6, 2, 7, 3), (1, 0, 40, 0, 24, 6, 2, 7, 3),
                                (1, 0, 36, 0, 26, 5, 2, 7, 2), (1, 0, 0, 0, 28, 4, 3, 8, 1)]),
    ("Bass", "Acid Bass", 0, 7, [(1, 0, 18, 1, 31, 10, 6, 10, 5), (2, 0, 30, 1, 31, 12, 8, 10, 6),
                                 (1, 1, 26, 1, 31, 9, 5, 10, 4), (1, 0, 2, 1, 31, 8, 6, 10, 2)]),
    # ---- keys ----
    ("Keys", "E.Piano", 4, 0, [(1, 0, 30, 2, 31, 9, 2, 6, 4), (1, 0, 4, 1, 31, 7, 2, 6, 3),
                               (14, 0, 42, 2, 31, 14, 4, 7, 6), (1, 0, 4, 1, 31, 6, 2, 6, 3)]),
    ("Keys", "Tine Piano", 4, 2, [(1, 3, 28, 2, 31, 10, 3, 6, 5), (1, 0, 6, 1, 31, 7, 2, 6, 3),
                                  (7, 0, 38, 2, 31, 16, 5, 7, 7), (1, -3, 6, 1, 31, 7, 2, 6, 3)]),
    ("Keys", "Clavi", 0, 6, [(3, 0, 26, 2, 31, 14, 6, 9, 6), (1, 0, 30, 2, 31, 12, 6, 9, 5),
                             (5, 0, 36, 2, 31, 15, 6, 9, 6), (1, 0, 0, 2, 31, 10, 6, 9, 4)]),
    ("Keys", "Harpsichord", 0, 5, [(4, 0, 24, 2, 31, 12, 5, 8, 5), (2, 0, 30, 2, 31, 12, 5, 8, 5),
                                   (6, 1, 34, 2, 31, 14, 6, 8, 6), (1, 0, 0, 2, 31, 9, 5, 8, 4)]),
    ("Keys", "Toy Piano", 4, 4, [(5, 0, 26, 2, 31, 14, 4, 7, 6), (1, 0, 4, 2, 31, 10, 3, 7, 4),
                                 (9, 0, 30, 2, 31, 16, 6, 8, 7), (2, 0, 8, 2, 31, 10, 4, 7, 4)]),
    # ---- organ ----
    ("Organ", "Drawbar Organ", 7, 0, [(1, 0, 14, 0, 31, 0, 0, 10, 0), (2, 0, 16, 0, 31, 0, 0, 10, 0),
                                      (4, 0, 22, 0, 31, 0, 0, 10, 0), (0, 0, 14, 0, 31, 0, 0, 10, 0)]),
    ("Organ", "Rock Organ", 6, 4, [(1, 0, 22, 0, 31, 0, 0, 10, 0), (2, 0, 10, 0, 31, 0, 0, 10, 0),
                                   (4, 0, 14, 0, 31, 0, 0, 10, 0), (1, 0, 10, 0, 31, 0, 0, 10, 0)]),
    ("Organ", "Church Organ", 7, 2, [(1, 0, 16, 0, 22, 0, 0, 8, 0), (2, 0, 16, 0, 22, 0, 0, 8, 0),
                                     (3, 0, 20, 0, 22, 0, 0, 8, 0), (4, 1, 22, 0, 22, 0, 0, 8, 0)]),
    ("Organ", "Perc Organ", 6, 3, [(1, 0, 24, 0, 31, 0, 0, 10, 0), (1, 0, 10, 0, 31, 0, 0, 10, 0),
                                   (3, 0, 12, 0, 31, 12, 6, 10, 8), (2, 0, 12, 0, 31, 0, 0, 10, 0)]),
    # ---- brass ----
    ("Brass", "Brass Section", 4, 6, [(1, 0, 24, 1, 18, 4, 2, 7, 2), (1, 0, 4, 1, 22, 3, 1, 7, 1),
                                      (1, 1, 26, 1, 18, 4, 2, 7, 2), (1, -1, 6, 1, 22, 3, 1, 7, 1)]),
    ("Brass", "Trumpet", 0, 6, [(1, 0, 26, 1, 19, 4, 1, 7, 2), (1, 0, 30, 1, 20, 4, 1, 7, 2),
                                (2, 0, 34, 1, 20, 5, 1, 7, 3), (1, 0, 0, 1, 22, 3, 1, 8, 1)]),
    ("Brass", "Synth Brass", 4, 7, [(1, 0, 20, 1, 20, 6, 2, 8, 3), (1, 0, 6, 1, 24, 4, 1, 8, 1),
                                    (1, 3, 22, 1, 20, 6, 2, 8, 3), (1, -3, 8, 1, 24, 4, 1, 8, 1)]),
    ("Brass", "Horn", 0, 4, [(1, 0, 30, 0, 16, 3, 1, 6, 2), (1, 0, 34, 0, 16, 3, 1, 6, 2),
                             (1, 0, 36, 0, 17, 4, 1, 6, 2), (1, 0, 4, 0, 18, 3, 1, 7, 1)]),
    # ---- strings and pads ----
    ("Strings", "String Section", 4, 3, [(1, 1, 30, 0, 14, 2, 0, 6, 1), (1, 0, 8, 0, 14, 2, 0, 6, 1),
                                         (2, -1, 34, 0, 14, 2, 0, 6, 1), (1, -1, 8, 0, 14, 2, 0, 6, 1)]),
    ("Strings", "Warm Pad", 4, 2, [(1, 2, 34, 0, 10, 1, 0, 5, 1), (1, 0, 10, 0, 10, 1, 0, 5, 1),
                                   (1, -2, 36, 0, 10, 1, 0, 5, 1), (1, 1, 10, 0, 10, 1, 0, 5, 1)]),
    ("Strings", "Glass Pad", 4, 1, [(3, 0, 34, 0, 12, 2, 0, 5, 2), (1, 0, 10, 0, 12, 2, 0, 5, 1),
                                    (5, 1, 38, 0, 12, 2, 0, 5, 2), (2, -1, 12, 0, 12, 2, 0, 5, 1)]),
    ("Strings", "Choir", 5, 3, [(1, 0, 32, 0, 12, 2, 0, 6, 2), (1, 0, 10, 0, 12, 1, 0, 6, 1),
                                (2, 1, 14, 0, 12, 1, 0, 6, 1), (1, -1, 10, 0, 12, 1, 0, 6, 1)]),
    # ---- leads ----
    ("Lead", "Saw Lead", 0, 7, [(1, 0, 20, 1, 31, 2, 0, 9, 1), (1, 0, 24, 1, 31, 2, 0, 9, 1),
                                (1, 1, 26, 1, 31, 2, 0, 9, 1), (1, 0, 2, 1, 31, 2, 0, 9, 1)]),
    ("Lead", "Square Lead", 4, 6, [(2, 0, 22, 1, 31, 2, 0, 9, 1), (1, 0, 6, 1, 31, 2, 0, 9, 1),
                                   (2, 1, 24, 1, 31, 2, 0, 9, 1), (1, -1, 6, 1, 31, 2, 0, 9, 1)]),
    ("Lead", "Sync Lead", 0, 5, [(3, 0, 18, 1, 31, 6, 2, 9, 2), (5, 0, 26, 1, 31, 4, 1, 9, 2),
                                 (1, 0, 22, 1, 31, 2, 0, 9, 1), (1, 0, 0, 1, 31, 2, 0, 9, 1)]),
    ("Lead", "Flute", 4, 2, [(1, 0, 36, 0, 22, 2, 0, 7, 1), (1, 0, 4, 0, 22, 2, 0, 7, 1),
                             (2, 0, 44, 0, 26, 3, 0, 7, 2), (2, 0, 26, 0, 22, 2, 0, 7, 1)]),
    ("Lead", "Oboe", 0, 4, [(1, 0, 30, 1, 24, 3, 1, 8, 1), (3, 0, 34, 1, 24, 3, 1, 8, 1),
                            (2, 0, 30, 1, 24, 3, 1, 8, 1), (1, 0, 2, 1, 24, 2, 1, 8, 1)]),
    # ---- bells and mallets ----
    ("Bells", "Tubular Bell", 4, 2, [(7, 0, 30, 2, 31, 6, 2, 4, 5), (1, 0, 6, 1, 31, 5, 2, 4, 4),
                                     (11, 1, 34, 2, 31, 7, 2, 4, 5), (2, 0, 10, 1, 31, 5, 2, 4, 4)]),
    ("Bells", "Glockenspiel", 4, 0, [(8, 0, 30, 3, 31, 10, 4, 6, 6), (4, 0, 8, 2, 31, 9, 4, 6, 5),
                                     (12, 0, 36, 3, 31, 12, 4, 6, 6), (2, 0, 8, 2, 31, 9, 4, 6, 5)]),
    ("Bells", "Marimba", 4, 0, [(4, 0, 28, 2, 31, 16, 6, 7, 8), (1, 0, 4, 2, 31, 12, 6, 7, 6),
                                (10, 0, 38, 2, 31, 18, 7, 8, 8), (1, 0, 8, 2, 31, 12, 6, 7, 6)]),
    ("Bells", "Vibraphone", 4, 1, [(4, 0, 32, 2, 31, 8, 3, 5, 5), (1, 0, 6, 1, 31, 7, 3, 5, 4),
                                   (8, 0, 40, 2, 31, 10, 4, 5, 6), (1, 0, 8, 1, 31, 7, 3, 5, 4)]),
    ("Bells", "Music Box", 5, 4, [(9, 0, 30, 3, 31, 12, 5, 6, 7), (1, 0, 8, 2, 31, 11, 4, 6, 6),
                                  (3, 0, 10, 2, 31, 11, 4, 6, 6), (2, 0, 10, 2, 31, 11, 4, 6, 6)]),
    # ---- plucks ----
    ("Pluck", "Nylon Guitar", 0, 4, [(1, 0, 28, 2, 31, 12, 4, 8, 5), (3, 0, 36, 2, 31, 14, 5, 8, 6),
                                     (1, 0, 30, 2, 31, 10, 4, 8, 4), (1, 0, 2, 2, 31, 8, 4, 8, 3)]),
    ("Pluck", "Steel Guitar", 0, 6, [(2, 0, 24, 2, 31, 12, 4, 8, 5), (3, 1, 32, 2, 31, 14, 5, 8, 6),
                                     (1, 0, 28, 2, 31, 10, 4, 8, 4), (1, 0, 0, 2, 31, 9, 4, 8, 3)]),
    ("Pluck", "Harp", 4, 2, [(2, 0, 32, 2, 31, 10, 4, 6, 6), (1, 0, 4, 2, 31, 9, 3, 6, 4),
                             (4, 0, 40, 2, 31, 12, 5, 6, 6), (1, 0, 6, 2, 31, 9, 3, 6, 4)]),
    ("Pluck", "Koto", 0, 5, [(3, 0, 24, 2, 31, 14, 6, 8, 6), (5, 0, 34, 2, 31, 16, 6, 8, 7),
                             (2, 0, 28, 2, 31, 12, 5, 8, 5), (1, 0, 0, 2, 31, 10, 6, 8, 4)]),
    # ---- drums and effects ----
    ("Drums", "Kick", 0, 4, [(0, 0, 16, 3, 31, 18, 10, 12, 10), (1, 0, 30, 3, 31, 18, 10, 12, 10),
                             (0, 0, 26, 3, 31, 16, 10, 12, 10), (0, 0, 0, 3, 31, 14, 10, 12, 9)]),
    ("Drums", "Snare", 4, 7, [(15, 0, 6, 3, 31, 18, 12, 12, 12), (1, 0, 4, 3, 31, 16, 12, 12, 11),
                              (12, 3, 4, 3, 31, 18, 12, 12, 12), (1, 0, 8, 3, 31, 16, 12, 12, 11)]),
    ("Drums", "Tom", 0, 3, [(1, 0, 22, 3, 31, 14, 8, 11, 9), (1, 0, 34, 3, 31, 14, 8, 11, 9),
                            (2, 0, 30, 3, 31, 14, 8, 11, 9), (1, 0, 0, 3, 31, 12, 8, 11, 8)]),
    ("Drums", "Hi-Hat", 4, 7, [(15, 3, 0, 3, 31, 22, 14, 14, 14), (13, 0, 6, 3, 31, 22, 14, 14, 13),
                               (11, -3, 0, 3, 31, 22, 14, 14, 14), (14, 1, 6, 3, 31, 22, 14, 14, 13)]),
    ("FX", "Laser", 0, 7, [(5, 0, 10, 3, 31, 10, 6, 10, 6), (9, 0, 20, 3, 31, 9, 6, 10, 6),
                           (3, 0, 16, 3, 31, 8, 6, 10, 6), (1, 0, 0, 3, 31, 6, 6, 10, 5)]),
    ("FX", "Wind", 4, 7, [(15, 3, 20, 0, 8, 1, 0, 4, 1), (1, 0, 20, 0, 8, 1, 0, 4, 1),
                          (14, -3, 22, 0, 8, 1, 0, 4, 1), (2, 0, 20, 0, 8, 1, 0, 4, 1)]),
]

DT_FILE = {-3: 0, -2: 1, -1: 2, 0: 3, 1: 4, 2: 5, 3: 6}

# PSG instruments (.dmp, DefleMask "standard", Genesis): (folder, name,
# volume steps, volume loop, arpeggio steps (semitones), arpeggio loop,
# noise steps (1 = white), noise loop); loop None = stay on the last step
P = [
    ("PSG", "Lead", [15, 14, 13, 12, 12, 11, 11, 10], None, [], None, [], None),
    ("PSG", "Pluck", [15, 12, 10, 8, 6, 5, 4, 3, 2, 1, 0], None, [], None, [], None),
    ("PSG", "Organ", [13], None, [], None, [], None),
    ("PSG", "Swell", [2, 4, 6, 8, 10, 12, 14, 15], None, [], None, [], None),
    ("PSG", "Tremolo", [12, 13, 14, 15, 14, 13, 12, 11, 10, 11], 0, [], None, [], None),
    ("PSG", "Echo", [15, 10, 6, 3, 0, 0, 9, 6, 3, 1, 0, 0, 5, 3, 1, 0], None, [], None, [], None),
    ("PSG", "Arp Major", [14, 13, 12, 12], None, [0, 0, 4, 4, 7, 7], 0, [], None),
    ("PSG", "Arp Minor", [14, 13, 12, 12], None, [0, 0, 3, 3, 7, 7], 0, [], None),
    ("PSG", "Octave", [14, 13, 12], None, [0, 0, 12, 12], 0, [], None),
    ("PSG", "Pitch Drop", [15, 14, 13, 12, 11, 10], None, [12, 9, 6, 3, 0], None, [], None),
    ("PSG", "Bleep", [15, 15, 13, 0], None, [12], None, [], None),
    ("PSG Noise", "Hi-Hat", [14, 8, 3, 0], None, [], None, [1], None),
    ("PSG Noise", "Open Hat", [14, 12, 10, 8, 6, 5, 4, 3, 2, 1, 0], None, [], None, [1], None),
    ("PSG Noise", "Snare", [15, 12, 9, 7, 5, 3, 2, 1, 0], None, [], None, [1], None),
    ("PSG Noise", "Kick", [15, 11, 7, 3, 0], None, [], None, [0], None),
    ("PSG Noise", "Crash", [15, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0], None, [], None, [1], None),
    ("PSG Noise", "Zap", [15, 12, 9, 6, 3, 0], None, [], None, [0, 1], 0),
]


def dmp_psg(vol, vloop, arp, aloop, noise, nloop):
    out = bytearray([0x0B, 0x02, 0x00])     # version 11, Genesis, standard

    def seq(vals, loop, offset):
        b = bytearray([len(vals)])
        for v in vals:
            b += ((v + offset) & 0xFFFFFFFF).to_bytes(4, "little")
        if vals:
            b.append(0xFF if loop is None else loop)
        return b
    out += seq(vol, vloop, 0)
    out += seq(arp, aloop, 12)              # (arpeggio values are stored + 12)
    out.append(0)                           # arpeggio: relative
    out += seq(noise, nloop, 0)
    out.append(0)                           # no wavetable
    return bytes(out)


def tfi(alg, fb, ops):
    out = bytearray([alg, fb])
    for i in (0, 2, 1, 3):          # the file's order: OP1 OP3 OP2 OP4
        mul, dt, tl, rs, ar, d1r, d2r, rr, sl = ops[i]
        out += bytes([mul, DT_FILE[dt], tl, rs, ar, d1r, d2r, rr, sl, 0])
    return bytes(out)


def carriers(alg):
    return {0: [3], 1: [3], 2: [3], 3: [3], 4: [1, 3], 5: [1, 2, 3], 6: [1, 2, 3], 7: [0, 1, 2, 3]}[alg]


def variation(alg, fb, ops, k, rnd):
    """k = 1, 2 ...: brighter / softer / slower takes on the recipe"""
    ops = [list(o) for o in ops]
    mods = [i for i in range(4) if i not in carriers(alg)]
    kind = k % 4
    for i in range(4):
        o = ops[i]
        if kind == 1 and i in mods:      # brighter
            o[2] = max(0, o[2] - 6 - rnd.randrange(4))
        elif kind == 2 and i in mods:    # softer
            o[2] = min(127, o[2] + 6 + rnd.randrange(4))
        elif kind == 3:                  # slower attack, longer release
            o[4] = max(8, o[4] - 8)
            o[7] = max(2, o[7] - 3)
        elif kind == 0:                  # detuned
            o[1] = max(-3, min(3, o[1] + rnd.choice((-1, 1))))
    return alg, fb, [tuple(o) for o in ops]


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    out = Path(sys.argv[1])
    nvar = int(sys.argv[3]) if len(sys.argv) > 3 and sys.argv[2] == "--variations" else 3
    rnd = random.Random(7)
    big = out / "Variations"
    big.mkdir(parents=True, exist_ok=True)
    n = 0
    for cat, name, alg, fb, ops in R:
        d = out / cat
        d.mkdir(parents=True, exist_ok=True)
        (d / f"{name}.tfi").write_bytes(tfi(alg, fb, ops))
        (big / f"{name}.tfi").write_bytes(tfi(alg, fb, ops))
        n += 1
        for k in range(1, nvar + 1):
            a, f, o = variation(alg, fb, ops, k, rnd)
            (big / f"{name} {k + 1}.tfi").write_bytes(tfi(a, f, o))
            n += 1
    for cat, name, vol, vl, arp, al, nz, nl in P:
        d = out / cat
        d.mkdir(parents=True, exist_ok=True)
        (d / f"{name}.dmp").write_bytes(dmp_psg(vol, vl, arp, al, nz, nl))
        n += 1
    print(f"make_sample_patches: {len(R)} FM patches, {len(P)} PSG instruments, {n} files in {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
