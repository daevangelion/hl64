#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""add_shot.py -- wrapped-pixel check of an additive glow shot (render-fx.md
2a), run by test/m3/add_ares.sh on the ares harness dumps of
test/routes/addblend.route.

  python3 test/m3/add_shot.py SHOT.ppm [SHOT.ppm ...]

Each shot looks straight at an env_glow (glow sprite, Glow mode) from its
height, so the glow's centre is the screen centre. An additive glow can
only brighten what is behind it; the RDP adder's wrap turns a sum past 255
into a dark or false-colour core. A shot fails when any pixel within R_IN
of the centre is darker than the median of the ring R_RING..R_RING+4
around it by more than DROP (luma, 0-255), or when a core pixel shows a
false colour: for an untinted (grey) glow, a channel more than DROP below
the pixel's luma while another is above it; for a tinted glow (label
suffix `_red`: rendercolor 238 30 46), a strongly coloured pixel whose
dominant channel is not red (the wrapped red turns cyan). Prints one line
per shot and `add-shot PASS|FAIL`; exit 0 when all pass.
"""
import sys

R_IN = 10
R_RING = 22
DROP = 48


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    parts, pos = [], 0
    while len(parts) < 4:
        while data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b"#":
            pos = data.index(b"\n", pos) + 1
            continue
        end = pos
        while not data[end:end + 1].isspace():
            end += 1
        parts.append(data[pos:end])
        pos = end
    if parts[0] != b"P6" or int(parts[3]) != 255:
        raise ValueError(f"{path}: not an 8-bit P6 ppm")
    w, h = int(parts[1]), int(parts[2])
    px = data[pos + 1:pos + 1 + 3 * w * h]
    if len(px) != 3 * w * h:
        raise ValueError(f"{path}: truncated")
    return w, h, px


def check(path):
    w, h, px = read_ppm(path)
    cx, cy = w // 2, h // 2
    red = path.rsplit(".", 1)[0].endswith("_red")

    def rgb(x, y):
        o = 3 * (y * w + x)
        return px[o], px[o + 1], px[o + 2]

    def luma(c):
        return (c[0] * 3 + c[1] * 6 + c[2]) // 10

    def false_colour(c, lum):
        if red:
            return max(c) - min(c) > DROP and max(c) != c[0]
        return min(c) < lum - DROP and max(c) > lum

    ring = sorted(luma(rgb(cx + dx, cy + dy))
                  for dy in range(-R_RING - 4, R_RING + 5)
                  for dx in range(-R_RING - 4, R_RING + 5)
                  if R_RING ** 2 <= dx * dx + dy * dy <= (R_RING + 4) ** 2)
    base = ring[len(ring) // 2]
    dark = false = core = n = 0
    for dy in range(-R_IN, R_IN + 1):
        for dx in range(-R_IN, R_IN + 1):
            if dx * dx + dy * dy > R_IN * R_IN:
                continue
            c = rgb(cx + dx, cy + dy)
            lum = luma(c)
            core += lum
            n += 1
            dark += lum < base - DROP
            false += false_colour(c, lum)
    ok = dark == 0 and false == 0
    print(f"add-shot {path}: {'red' if red else 'grey'} ring median {base} core mean {core // n} "
          f"dark {dark} false-colour {false} -> {'ok' if ok else 'WRAPPED'}")
    return ok


def main(argv):
    if len(argv) < 2:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    ok = all([check(p) for p in argv[1:]])
    print("add-shot", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
