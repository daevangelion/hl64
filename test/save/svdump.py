#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Saved-game images (engine/save/save.c, sv.c): list a FlashRAM image's
slots, unpack a slot's state stream, or compare two slots byte for byte
and name the section of the first difference.

  svdump.py IMAGE                      slot headers
  svdump.py IMAGE SLOT -o RAW          the slot's uncompressed stream
  svdump.py IMAGE:SLOT IMAGE:SLOT      compare two slots' streams

IMAGE is the host's --save file or an ares FlashRAM dump (the cart is
big-endian: --be). Exit 0 when the compared streams are equal, 1 when they
differ, 2 on a bad image.
"""
import argparse
import struct
import sys

SECTOR = 16 * 1024
SLOT_SECTORS = 7
HDR = 128
SLOT_MAGIC = 0x484C5356
TAGS = ["HL64", "ENG1", "CLK1", "RNG1", "SET1", "REC1", "ENT1", "CBR1", "PLR1", "INP1",
        "CONV", "TRN1", "SEAM", "STY1", "SND1", "GAME", "RTE1", "END!"]


def slot_off(slot):
    return SECTOR * (1 + slot * SLOT_SECTORS)


def header(img, slot, e):
    b = img[slot_off(slot):slot_off(slot) + HDR]
    if len(b) < HDR:
        return None
    f = struct.unpack(e + "IIHHIIIIIfBB2x", b[:40])
    keys = ("magic", "hdr_crc", "format", "hdr_bytes", "layout", "seq", "raw", "img",
            "img_crc", "time", "skill", "nlayers")
    h = dict(zip(keys, f))
    h["map"] = b[52:68].split(b"\0")[0].decode(errors="replace")
    return h


def unpack(data):
    out = bytearray()
    i = 0
    n = len(data)
    while i < n:
        flags = data[i]
        i += 1
        for bit in range(8):
            if i >= n:
                break
            if not flags & (0x80 >> bit):
                out.append(data[i])
                i += 1
                continue
            t = data[i] << 8 | data[i + 1]
            i += 2
            ln = (t >> 13) + 3
            if t >> 13 == 7:
                ln = 10 + data[i]
                i += 1
            off = (t & 0x1FFF) + 1
            for _ in range(ln):
                out.append(out[-off])
    return bytes(out)


def stream(path, slot, e):
    img = open(path, "rb").read()
    h = header(img, slot, e)
    if not h or h["magic"] != SLOT_MAGIC:
        sys.exit(f"{path}: slot {slot}: no save")
    a = slot_off(slot) + HDR
    raw = unpack(img[a:a + h["img"]])
    return h, raw[:h["raw"]]


def section_at(raw, pos, e):
    best = ("?", 0)
    for t in TAGS:
        tb = struct.pack(e + "I", int.from_bytes(t.encode(), "big"))
        k = raw.rfind(tb, 0, pos + 1)
        if k >= 0 and k > best[1]:
            best = (t, k)
    return best


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("a")
    ap.add_argument("b", nargs="?")
    ap.add_argument("-o")
    ap.add_argument("--be", action="store_true", help="cart (big-endian) image")
    ap.add_argument("--slots", type=int, default=2)
    o = ap.parse_args()
    e = ">" if o.be else "<"
    if o.b is None:
        img = open(o.a, "rb").read()
        for s in range(o.slots):
            h = header(img, s, e)
            if not h or h["magic"] != SLOT_MAGIC:
                print(f"slot {s}: empty / not a save")
                continue
            print(f"slot {s}: seq {h['seq']} {h['map']} t={h['time']:.3f} skill {h['skill']} "
                  f"layers {h['nlayers']} state {h['raw']} B image {h['img']} B "
                  f"crc {h['img_crc']:08x} layout {h['layout']:08x}")
        return 0
    if ":" not in o.a:
        h, raw = stream(o.a, int(o.b), e)
        if o.o:
            open(o.o, "wb").write(raw)
        print(f"slot {o.b}: {len(raw)} B")
        return 0
    pa, sa = o.a.rsplit(":", 1)
    pb, sb = o.b.rsplit(":", 1)
    ha, ra = stream(pa, int(sa), e)
    hb, rb = stream(pb, int(sb), e)
    if ra == rb:
        print(f"equal: {len(ra)} B state, image crc {ha['img_crc']:08x} / {hb['img_crc']:08x}")
        return 0
    n = min(len(ra), len(rb))
    d = next((i for i in range(n) if ra[i] != rb[i]), n)
    tag, at = section_at(ra, d, e)
    ndiff = sum(1 for i in range(n) if ra[i] != rb[i]) + abs(len(ra) - len(rb))
    print(f"DIFFER: {len(ra)} / {len(rb)} B, first difference at {d} "
          f"(section {tag} + {d - at}), {ndiff} bytes differ")
    print("  a:", ra[d:d + 32].hex())
    print("  b:", rb[d:d + 32].hex())
    return 1


if __name__ == "__main__":
    sys.exit(main())
