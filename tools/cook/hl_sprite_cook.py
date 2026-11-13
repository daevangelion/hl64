# SPDX-License-Identifier: MIT
"""hl_sprite_cook: GoldSrc SPR v2 sprite -> cart CI4 sprite (`sprites/<name>.sc4`).

Imported from the quakegeneric tree (tools/hl_sprite_cook.py,
provenance-audit.md entry CLEAN). The renderer draws sprites from this file
with one PI DMA into one heap block and no conversion on the cart:

  * every image (single frames and every member of a frame group, with the
    group's intervals) resampled nearest to power-of-two sides <= 64 (TMEM
    wrapping), 4-bit indices packed high nibble first;
  * one 16-colour TLUT (RGBA5551) per sprite, picked from the palette-index
    usage summed over every frame at full size (the most used colour, then
    the most used of each of 15 luminance bands, then popularity); every
    index maps to the nearest picked colour (green-weighted);
  * ALPHTEST sprites (blood): index 255 owns the first entry, alpha bit
    clear; no other index maps to it;
  * tlut_ia: the TLUT as IA16 (I = 255, A = entry brightness) and tint, the
    brightness-weighted mean colour, for the screen-blended additive / glow /
    index-alpha draws.

Layout v2 (big-endian; offsets absolute, 8-aligned data):
  0  u32 magic 'SC41'   4 u16 version (2)   6 u16 nframes (entity frames: a
     group counts once, GoldSrc ModelFrames)
  8  u8 type  9 u8 texformat  10 u8 tint[3]  13 u8 pad  14 u16 nimages
  16 u16 tlut[16]   48 u16 tlut_ia[16]
  80 images[nimages] (16 bytes): s16 left, s16 up, u16 src w, u16 src h,
     u8 w, u8 h (CI4 sides), u16 pad, u32 ci4 offset
  then frames[nframes] (4 bytes): u16 first image, u16 images (1, or the
     group's member count)
  then ends[nimages] (u16): a group member's cumulative interval end in ms
     (the SPR group's interval table; 0 for single frames)
  CI4 payloads (w * h / 2 bytes each), 8-aligned
File size is a multiple of 8. Malformed sprites raise ValueError.

  python3 -m tools.cook.hl_sprite_cook <in.spr> <out.sc4>
"""
from __future__ import annotations

import struct
import sys
from pathlib import Path
from typing import List

MAGIC = 0x53433431  # 'SC41'
VERSION = 2
HEADER = 80
FRAME = 16
EDGE = 64
SPR_ALPHTEST = 3


def _pow2_floor(v: int) -> int:
    p = 1
    while p * 2 <= v:
        p *= 2
    return p


def _frames(spr: bytes):
    try:
        return _parse(spr)
    except struct.error:
        raise ValueError("truncated sprite") from None


def _parse(spr: bytes):
    ident, version, typ, texformat = struct.unpack_from("<4siii", spr, 0)
    if ident != b"IDSP" or version != 2:
        raise ValueError("not a GoldSrc SPR v2")
    nframes = struct.unpack_from("<i", spr, 28)[0]
    if nframes < 1 or nframes > 1024:
        raise ValueError(f"bad frame count {nframes}")
    o = 40
    palcount = struct.unpack_from("<h", spr, o)[0]
    o += 2
    if palcount < 0 or palcount > 256:
        raise ValueError(f"bad palette count {palcount}")
    if o + palcount * 3 > len(spr):
        raise ValueError("truncated palette")
    pal = list(spr[o:o + palcount * 3]) + [0] * (768 - palcount * 3)
    o += palcount * 3
    # images: (left, up, w, h, px, end_ms); groups: (first image, count)
    images, groups = [], []
    for _ in range(nframes):
        ftype = struct.unpack_from("<i", spr, o)[0]
        o += 4
        count, ends = 1, [0]
        if ftype != 0:
            count = struct.unpack_from("<i", spr, o)[0]
            if count < 1 or count > 1024:
                raise ValueError(f"bad frame group size {count}")
            o += 4
            # SPR group intervals: cumulative end times (s), increasing
            iv = struct.unpack_from(f"<{count}f", spr, o)
            o += 4 * count
            ends, prev = [], 0
            for t in iv:
                ms = int(round(t * 1000))
                if not (ms > prev and ms <= 65535):
                    raise ValueError(f"bad group interval {t}")
                ends.append(ms)
                prev = ms
        groups.append((len(images), count))
        for k in range(count):
            left, up, w, h = struct.unpack_from("<4i", spr, o)
            o += 16
            if w < 1 or h < 1 or w > 1024 or h > 1024 or o + w * h > len(spr):
                raise ValueError("truncated frame")
            images.append((left, up, w, h, spr[o:o + w * h], ends[k]))
            o += w * h
    if len(images) > 4096:
        raise ValueError(f"{len(images)} images")
    return typ, texformat, pal, images, groups


def _pick16(usage: List[int], pal: List[int]) -> List[int]:
    """The 16 palette indices of the sprite's TLUT."""
    lum = [(pal[3 * c] * 3 + pal[3 * c + 1] * 6 + pal[3 * c + 2]) // 10
           for c in range(256)]
    best, bestuse = -1, 0
    for c in range(256):
        if usage[c] > bestuse:
            bestuse, best = usage[c], c
    top = [max(best, 0)]
    for band in range(15):
        if len(top) >= 16:
            break
        lo, hi = band * 256 // 15, (band + 1) * 256 // 15
        bbest, buse = -1, 0
        for c in range(256):
            if usage[c] <= 0 or not lo <= lum[c] < hi or c in top:
                continue
            if usage[c] > buse:
                buse, bbest = usage[c], c
        if bbest >= 0:
            top.append(bbest)
    while len(top) < 16:
        best, bestuse = -1, 0
        for c in range(256):
            if c not in top and usage[c] > bestuse:
                bestuse, best = usage[c], c
        top.append(max(best, 0))
    return top


def cook(spr: bytes) -> bytes:
    typ, texformat, pal, frames, groups = _frames(spr)
    usage = [0] * 256
    for _l, _u, _w, _h, px, _e in frames:
        for p in px:
            usage[p] += 1
    alpha = texformat == SPR_ALPHTEST and usage[255] > 0
    if alpha:
        usage[255] = 0x3fffffff  # the first pick: an entry of its own
    top = _pick16(usage, pal)
    rgb = [(pal[3 * c], pal[3 * c + 1], pal[3 * c + 2]) for c in top]
    tlut = [((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | 1
            for r, g, b in rgb]
    if alpha:
        tlut[0] &= ~1
    remap = []
    for i in range(256):
        if alpha and i == 255:
            remap.append(0)
            continue
        cr, cg, cb = pal[3 * i], pal[3 * i + 1], pal[3 * i + 2]
        bestdist, bestslot = None, 0
        for s, (tr, tg, tb) in enumerate(rgb):
            if alpha and s == 0:
                continue
            d = (cr - tr) ** 2 + 2 * (cg - tg) ** 2 + (cb - tb) ** 2
            if bestdist is None or d < bestdist:
                bestdist, bestslot = d, s
        remap.append(bestslot)
    # tlut_ia + brightness-weighted tint (screen-blended draws)
    ia, s = [], [0, 0, 0]
    for t in tlut:
        r, g, b = (t >> 11) & 31, (t >> 6) & 31, (t >> 1) & 31
        lv = max(r, g, b)
        ia.append(0xFF00 | (lv * 255 // 31))
        s[0] += r * lv
        s[1] += g * lv
        s[2] += b * lv
    m = max(s)
    tint = [s[i] * 255 // m if m > 0 else 255 for i in range(3)]

    table, blobs = [], []
    ofs = HEADER + FRAME * len(frames) + 4 * len(groups) + 2 * len(frames)
    ofs = (ofs + 7) & ~7
    for left, up, w, h, px, _e in frames:
        pw, ph = _pow2_floor(min(w, EDGE)), _pow2_floor(min(h, EDGE))
        if pw < 2:
            raise ValueError(f"frame {w}x{h}: under 2 texels wide")
        src = [px[(y * h // ph) * w + (x * w // pw)]
               for y in range(ph) for x in range(pw)]
        data = bytes((remap[src[k]] << 4) | remap[src[k + 1]]
                     for k in range(0, len(src), 2))
        if not -32768 <= left < 32768 or not -32768 <= up < 32768:
            raise ValueError("frame origin out of range")
        table.append(struct.pack(">hhHHBBHI", left, up, w, h, pw, ph, 0, ofs))
        blob = data.ljust((len(data) + 7) & ~7, b"\0")
        blobs.append(blob)
        ofs += len(blob)
    head = struct.pack(">IHHBB3BxH", MAGIC, VERSION, len(groups), typ & 255,
                       texformat & 255, *tint, len(frames))
    head += struct.pack(">16H", *tlut) + struct.pack(">16H", *ia)
    assert len(head) == HEADER
    out = head + b"".join(table)
    out += b"".join(struct.pack(">HH", first, n) for first, n in groups)
    out += b"".join(struct.pack(">H", f[5]) for f in frames)
    out = out.ljust((len(out) + 7) & ~7, b"\0") + b"".join(blobs)
    return out


def main(argv: List[str]) -> int:
    if len(argv) != 3:
        print("usage: python3 -m tools.cook.hl_sprite_cook <in.spr> <out.sc4>", file=sys.stderr)
        return 2
    try:
        out = cook(Path(argv[1]).read_bytes())
    except (ValueError, OSError) as e:
        print(f"hl_sprite_cook: error: {argv[1]}: {e}", file=sys.stderr)
        return 1
    Path(argv[2]).write_bytes(out)
    print(f"{argv[2]}: {len(out)} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
