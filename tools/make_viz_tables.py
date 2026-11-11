#!/usr/bin/env python3
"""make_viz_tables.py - the visualizer's fixed pictures and sine (src/32x/viz_tab.h).

    python3 tools/make_viz_tables.py > src/32x/viz_tab.h

The scene is 320 x 224: a sky down to the horizon at HOR, a sun sitting on it,
and a floor grid below. Everything here goes in ROM: the Genesis has little RAM.
"""
import math, random

W, H, HOR, SUN_R, SUN_X = 320, 224, 148, 46, 160
out = ["/* viz_tab.h - made by tools/make_viz_tables.py: do not edit */",
       "#ifdef MD_ONLY", "#define VIZ_ROM", "#else",
       "#define VIZ_ROM __attribute__((section(\".romdata\")))   /* (32X: SDRAM is full) */", "#endif",
       "#define VIZ_HOR %d" % HOR, "#define VIZ_SUN_R %d" % SUN_R, "#define VIZ_SUN_X %d" % SUN_X, ""]

def arr(ctype, name, vals, per=16):
    out.append("static const %s %s[%d] VIZ_ROM = {" % (ctype, name, len(vals)))
    for i in range(0, len(vals), per):
        out.append("    " + ", ".join(str(v) for v in vals[i:i + per]) + ",")
    out.append("};")

# a sine, 256 steps a cycle, +-127
arr("int8_t", "viz_sin", [round(127 * math.sin(2 * math.pi * i / 256)) for i in range(256)])

out.append("#ifdef MD_ONLY  /* ---- the Genesis: the vaporwave scene ---- */")

# the sun: its half-width dy rows above the horizon (0: a stripe of sky)
stripes = set()
for top, n in ((3, 3), (9, 2), (15, 2), (21, 1), (27, 1)):
    stripes.update(range(top, top + n))
half = []
for dy in range(SUN_R + 1):
    w = int(math.sqrt(max(0, SUN_R * SUN_R - dy * dy)))
    half.append(0 if dy in stripes or dy == 0 else w)
arr("uint8_t", "viz_sun_half", half)

# stars: at most one a column (0: none), kept off the sun
random.seed(7)
star = [0] * W
for _ in range(70):
    x, y = random.randrange(W), random.randrange(14, HOR - 6)
    if (x - SUN_X) ** 2 + (y - HOR) ** 2 < (SUN_R + 8) ** 2:
        continue
    star[x] = y
arr("uint8_t", "viz_star", star)

# the floor: rows that are a horizontal line (1-3: its phase, for the palette
# to roll them toward you), and the lines running to the horizon as bits
rows = [0] * (H - HOR)
for k in range(2, 16):
    y = 2 + round((H - HOR - 3) * (k / 15) ** 2)
    if y < len(rows):
        rows[y] = 1 + k % 3
arr("uint8_t", "viz_floor_row", rows)
bits = []
for r in range(H - HOR):
    word = [0] * 10
    if r >= 2:
        spacing = 0.8 * (r + 18)         # (lines from a point 18 rows above the horizon)
        for k in range(-9, 10):
            x = round(SUN_X + k * spacing)
            if 0 <= x < W:
                word[x >> 5] |= 1 << (31 - (x & 31))
    bits.extend(word)
arr("uint32_t", "viz_floor_v", ["0x%08Xu" % b for b in bits], per=5)

# each column's top row of sun (VIZ_HOR: none), stripes or not
top = []
for x in range(W):
    t = HOR
    for dy in range(1, SUN_R + 1):
        w = int(math.sqrt(max(0, SUN_R * SUN_R - dy * dy)))
        if abs(x - SUN_X) < w:
            t = HOR - dy
    top.append(t)
arr("uint8_t", "viz_sun_top", top)
out.append("#else  /* ---- the 32X: the sphere ---- */")

# asin(i / 128) as a fraction of a quarter turn, 0-128
out.append("#undef VIZ_ROM\n#define VIZ_ROM     /* (read for every pixel: SDRAM) */")
arr("uint8_t", "viz_asin", [round(math.asin(i / 128) / (math.pi / 2) * 128) for i in range(129)])
# sqrt(1 - (i / 256)^2) * 256: a circle's half-width at height i / 256
arr("uint16_t", "viz_circle", [round(math.sqrt(max(0, 1 - (i / 256) ** 2)) * 256) for i in range(257)])
out.append("#endif")
print("\n".join(out))
