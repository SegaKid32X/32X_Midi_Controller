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


# a song for the tracker (src/common/song.h): MIDI_SONGS/demo.sng, its parts
# naming the patches above (FM 1 brass, FM 2 bass, the 32X's CH 11 piano)
def song():
    lanes = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 10, 11, 15, 15, 15, 15, 15, 15]
    pats = {0: [(0, 0, 60, 100), (4, 0, 64, 100), (8, 0, 67, 100), (14, 0, 0xFF, 0),
                (0, 1, 36, 110), (8, 1, 36, 90), (0, 10, 72, 90), (0, 14, 76, 90), (12, 10, 0xFF, 0),
                (0, 18, 36, 127), (4, 18, 38, 120), (8, 18, 36, 127), (12, 18, 38, 120)],
            1: [(0, 0, 65, 100), (4, 0, 69, 100), (0, 1, 41, 110), (8, 1, 41, 90),
                (0, 18, 36, 127), (8, 18, 36, 127)]}
    h = bytearray(1408)
    h[0:4] = b"MSNG"
    h[4], h[5], h[6:8] = 1, 4, (140).to_bytes(2, "big")
    h[8], h[9], h[10] = 3, 0, 64
    h[12:16] = b"demo"
    h[36:36 + len(lanes)] = bytes(lanes)
    h[60:63] = bytes([0, 1, 0])
    for p in range(64):
        h[188 + p] = 16
        h[252 + p * 2:254 + p * 2] = len(pats.get(p, [])).to_bytes(2, "big")
    paths = [""] * 16
    paths[0], paths[1], paths[10] = "Brass Section.tfi", "Bass/Slap Bass.tfi", "E.Piano.tfi"
    packed = b"".join(x.encode() + b"\0" for x in paths)
    h[384:384 + len(packed)] = packed
    ev = b"".join(bytes(e) for p in sorted(pats) for e in pats[p])
    return bytes(h) + ev


(out / "MIDI_SONGS").mkdir(exist_ok=True)
(out / "MIDI_SONGS" / "demo.sng").write_bytes(song())


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
