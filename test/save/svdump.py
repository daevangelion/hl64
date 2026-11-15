#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Saved-game images (engine/save/save.c, sv.c): list a FlashRAM image's
saves, unpack a logical slot's state stream, or compare two slots byte for
byte and name the section of the first difference.

  svdump.py IMAGE                      every save in the pool
  svdump.py IMAGE SLOT -o RAW          the slot's uncompressed stream
  svdump.py IMAGE:SLOT IMAGE:SLOT      compare two slots' streams

SLOT is the logical slot (0 the autosave, 1.. the manual ones): its newest
copy whose header and image check out, as save.c's directory picks it.
IMAGE is the host's --save file or an ares FlashRAM dump (the cart is
big-endian: --be). Exit 0 when the compared streams are equal, 1 when they
differ, 2 on a bad image.
"""
import argparse
import struct
import sys
import zlib

from svcodec import unpack

SECTOR = 16 * 1024
POOL_SECTORS = 8
HDR = 128
FLASH_HDR = 512        # header + the 384 B settings block before a flash copy's image
FORMAT = 4
SLOT_MAGIC = 0x484C5356
SET_SLOT = 0xFE
TAGS = ["HL64", "ENG1", "CLK1", "RNG1", "SET1", "REC2", "ENT2", "CBR1", "PLR1", "INP1",
        "CONV", "TRN1", "DCL1", "SEAM", "STY1", "SND1", "GAME", "RTE1", "END!"]


def header(img, sect, e):
    """The copy whose header is at pool sector sect; its image gathered
    from its sectors (sect[0] after the header and settings block, then
    sect[1])."""
    off = SECTOR * sect
    b = img[off:off + HDR]
    if len(b) < HDR:
        return None
    f = struct.unpack(e + "IIHHIIIIIfBBBB", b[:40])
    keys = ("magic", "hdr_crc", "format", "hdr_bytes", "layout", "seq", "raw", "img",
            "img_crc", "time", "skill", "nlayers", "slot", "nsect")
    h = dict(zip(keys, f))
    h["map"] = b[52:68].split(b"\0")[0].decode(errors="replace")
    h["sects"] = list(b[92:92 + max(1, min(h["nsect"], 2))])
    h["sector"] = sect
    data = img[off + FLASH_HDR:off + SECTOR]
    if h["nsect"] == 2 and h["sects"][1] < POOL_SECTORS:
        data += img[SECTOR * h["sects"][1]:SECTOR * (h["sects"][1] + 1)]
    h["data"] = data[:h["img"]]
    h["ok"] = (h["magic"] == SLOT_MAGIC and h["format"] == FORMAT and h["hdr_bytes"] == FLASH_HDR
               and h["hdr_crc"] == zlib.crc32(b[8:HDR]) and h["sects"][0] == sect
               and (h["slot"] == SET_SLOT
                    or (0 < h["img"] == len(h["data"])
                        and zlib.crc32(h["data"]) == h["img_crc"])))
    return h


def saves(img, e):
    out = []
    for s in range(POOL_SECTORS):
        h = header(img, s, e)
        if h and h["magic"] == SLOT_MAGIC:
            out.append(h)
    return out


def current(img, slot, e):
    best = None
    for h in saves(img, e):
        if h["ok"] and h["slot"] == slot and (best is None or h["seq"] > best["seq"]):
            best = h
    return best


def stream(path, slot, e):
    img = open(path, "rb").read()
    h = current(img, slot, e)
    if not h:
        print(f"{path}: slot {slot}: no good save", file=sys.stderr)
        sys.exit(2)
    try:
        raw = unpack(h["data"])
    except (ValueError, IndexError) as x:
        print(f"{path}: slot {slot}: image does not decode ({x})", file=sys.stderr)
        sys.exit(2)
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
    o = ap.parse_args()
    e = ">" if o.be else "<"
    if o.b is None:
        img = open(o.a, "rb").read()
        for h in saves(img, e):
            where = "sectors " + ",".join(str(s) for s in h["sects"])
            if h["slot"] == SET_SLOT:
                print(f"{where}: settings seq {h['seq']} {'ok' if h['ok'] else 'BAD'}")
                continue
            print(f"{where}: slot {h['slot']} seq {h['seq']} "
                  f"{'ok' if h['ok'] else 'BAD'} {h['map']} t={h['time']:.3f} skill {h['skill']} "
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
