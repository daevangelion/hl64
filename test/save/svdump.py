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

SECTOR = 16 * 1024
POOL_FIRST = 1
POOL_SECTORS = 7
HDR = 128
FORMAT = 3
SLOT_MAGIC = 0x484C5356
TAGS = ["HL64", "ENG1", "CLK1", "RNG1", "SET1", "REC2", "ENT2", "CBR1", "PLR1", "INP1",
        "CONV", "TRN1", "SEAM", "STY1", "SND1", "GAME", "RTE1", "END!"]


def header(img, off, e):
    b = img[off:off + HDR]
    if len(b) < HDR:
        return None
    f = struct.unpack(e + "IIHHIIIIIfBBBB", b[:40])
    keys = ("magic", "hdr_crc", "format", "hdr_bytes", "layout", "seq", "raw", "img",
            "img_crc", "time", "skill", "nlayers", "slot", "nsect")
    h = dict(zip(keys, f))
    h["map"] = b[52:68].split(b"\0")[0].decode(errors="replace")
    h["off"] = off
    h["ok"] = (h["magic"] == SLOT_MAGIC and h["format"] == FORMAT and h["hdr_bytes"] == HDR
               and h["hdr_crc"] == zlib.crc32(b[8:HDR])
               and 0 < h["img"] <= len(img) - off - HDR
               and zlib.crc32(img[off + HDR:off + HDR + h["img"]]) == h["img_crc"])
    return h


def saves(img, e):
    out = []
    for s in range(POOL_SECTORS):
        h = header(img, SECTOR * (POOL_FIRST + s), e)
        if h and h["magic"] == SLOT_MAGIC:
            h["sector"] = POOL_FIRST + s
            out.append(h)
    return out


def current(img, slot, e):
    best = None
    for h in saves(img, e):
        if h["ok"] and h["slot"] == slot and (best is None or h["seq"] > best["seq"]):
            best = h
    return best


SYM_LL, SYM_D, SYM_END, MAXBITS = 286, 26, 256, 15
LBASE = [3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115,
         131, 163, 195, 227, 258]
LEXT = [0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0]
DBASE = [1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025,
         1537, 2049, 3073, 4097, 6145]
DEXT = [0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11]


def unpack(data):
    """sv.c's image: code lengths (4 bits each), then canonical Huffman
    tokens (MSB-first bits) up to the end code."""
    pos = [0]

    def bits(n):
        v = 0
        for _ in range(n):
            p = pos[0]
            if p >> 3 >= len(data):
                raise ValueError("image ends early")
            v = v << 1 | (data[p >> 3] >> (7 - (p & 7))) & 1
            pos[0] = p + 1
        return v

    lens = [bits(4) for _ in range(SYM_LL + SYM_D)]

    def table(ls):
        codes, code, nxt = {}, 0, {}
        cnt = [0] * (MAXBITS + 1)
        for n in ls:
            cnt[n] += 1
        cnt[0] = 0
        for b in range(1, MAXBITS + 1):
            code = (code + cnt[b - 1]) << 1
            nxt[b] = code
        for s, n in enumerate(ls):
            if n:
                codes[(n, nxt[n])] = s
                nxt[n] += 1
        return codes

    tll, td = table(lens[:SYM_LL]), table(lens[SYM_LL:])

    def sym(t):
        c = 0
        for n in range(1, MAXBITS + 1):
            c = c << 1 | bits(1)
            if (n, c) in t:
                return t[(n, c)]
        raise ValueError("bad code")

    out = bytearray()
    while True:
        s = sym(tll)
        if s < 256:
            out.append(s)
            continue
        if s == SYM_END:
            return bytes(out)
        s -= 257
        ln = LBASE[s] + bits(LEXT[s])
        d = sym(td)
        off = DBASE[d] + bits(DEXT[d])
        for _ in range(ln):
            out.append(out[-off])


def stream(path, slot, e):
    img = open(path, "rb").read()
    h = current(img, slot, e)
    if not h:
        print(f"{path}: slot {slot}: no good save", file=sys.stderr)
        sys.exit(2)
    a = h["off"] + HDR
    try:
        raw = unpack(img[a:a + h["img"]])
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
            print(f"sector {h['sector']}+{h['nsect']}: slot {h['slot']} seq {h['seq']} "
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
