#!/usr/bin/env python3
"""A few patch files of each format for the test ROM (made up here)."""
import struct
import sys
from pathlib import Path

out = Path(sys.argv[1])
out.mkdir(parents=True, exist_ok=True)


def ops(seed):
    # for S1 S3 S2 S4: mul, dt(0-6), tl, rs, ar, d1r, d2r, rr, sl, ssg
    r = []
    for k in range(4):
        s = seed * 7 + k * 3
        carrier = k == 3
        r.append([1 + s % 4, 3 + (s % 3) - 1, 0 if carrier else 20 + s % 30, s % 2,
                  31 - s % 6, 5 + s % 10, 2, 5 + s % 6, 2 + s % 8, 0])
    return r


def tfi(alg, fb, seed):
    b = bytes([alg, fb])
    for o in ops(seed):
        b += bytes(o)
    return b


def dmp(alg, fb, seed):
    b = bytes([0x0B, 0x02, 0x01, 0, fb, alg, 0])
    for mul, dt, tl, rs, ar, d1r, d2r, rr, sl, ssg in ops(seed):
        b += bytes([mul, tl, ar, d1r, sl, rr, 0, rs, dt, d2r, ssg])
    return b


def y12(alg, fb, seed, name):
    b = bytearray(128)
    for k, (mul, dt, tl, rs, ar, d1r, d2r, rr, sl, ssg) in enumerate(ops(seed)):
        reg_dt = [7, 6, 5, 0, 1, 2, 3][dt]
        b[k * 16:k * 16 + 7] = bytes([reg_dt << 4 | mul, tl, rs << 6 | ar, d1r, d2r, sl << 4 | rr, ssg])
    b[64], b[65] = alg, fb
    b[80:80 + len(name)] = name.encode()
    return bytes(b)


(out / "Brass Section.tfi").write_bytes(tfi(4, 5, 1))
(out / "E.Piano.tfi").write_bytes(tfi(5, 6, 2))
(out / "Bass").mkdir(exist_ok=True)
(out / "Bass" / "Synth").mkdir(exist_ok=True)
(out / "Bass" / "Slap Bass.tfi").write_bytes(tfi(0, 7, 3))
(out / "Bass" / "Fretless.dmp").write_bytes(dmp(1, 2, 7))
(out / "Bass" / "Synth" / "Acid.tfi").write_bytes(tfi(3, 5, 8))
# what a Mac leaves behind: none of it may show
(out / "._E.Piano.tfi").write_bytes(b"\0\x05\x16\x07" + bytes(38))
(out / "Bass" / "._Fretless.dmp").write_bytes(bytes(51))
(out / "__MACOSX").mkdir(exist_ok=True)
(out / "__MACOSX" / "._Lead.tfi").write_bytes(bytes(42))
(out / "Organ_01.dmp").write_bytes(dmp(7, 0, 4))
(out / "Strings [warm].dmp").write_bytes(dmp(2, 3, 5))
(out / "Lead Saw.y12").write_bytes(y12(6, 4, 6, "Lead Saw"))


# drum samples (made up here): WAVs in three formats
import math
import random
import wave

drums = out.parent / "testdrums"
drums.mkdir(parents=True, exist_ok=True)


def wav(path, rate, width, chans, samples):
    with wave.open(str(path), "wb") as w:
        w.setnchannels(chans)
        w.setsampwidth(width)
        w.setframerate(rate)
        frames = bytearray()
        for s in samples:
            for _ in range(chans):
                if width == 2:
                    frames += struct.pack("<h", int(max(-1, min(1, s)) * 30000))
                else:
                    frames.append(int(128 + max(-1, min(1, s)) * 120))
        w.writeframes(bytes(frames))


rnd = random.Random(1)
kick = [math.sin(2 * math.pi * (55 + 120 * math.exp(-i / 600)) * i / 22050) * math.exp(-i / 3000) for i in range(6000)]
snare = [(rnd.random() * 2 - 1) * math.exp(-i / 1500) for i in range(5000)]
hat = [(rnd.random() * 2 - 1) * math.exp(-i / 800) for i in range(4000)]
wav(drums / "36 Kick.wav", 22050, 2, 1, kick)
wav(drums / "Snare tight.wav", 22050, 1, 1, snare)
wav(drums / "closed hat.wav", 44100, 2, 2, hat)
