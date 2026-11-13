# SPDX-License-Identifier: MIT
"""tools/cook/decals.py unit tests on a synthetic decals.wad: the DI4 sheet
(entry order, shared duplicates, the I4 alpha, widths resampled to a
multiple of 16 with the lump's world size kept), the sheet reader's rules
(engine/render/decal.c load_sheet), and the decal region proof with its
positive control (fails above HL64_DECAL_TEXELS_KB).

  python3 -m unittest tools.cook.test_decals
"""
from __future__ import annotations

import os
import re
import struct
import tempfile
import unittest

from tools.cook import decals
from tools.restab import restab

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
LIMITS = os.path.join(ROOT, "include", "hl64", "limits.h")


def miptex(name: str, w: int, h: int, alpha) -> bytes:
    """A '{' decal lump: palette index = alpha(x, y), palette[255] = tint."""
    px = bytes(alpha(x, y) for y in range(h) for x in range(w))
    o0 = 40
    o1 = o0 + w * h
    o2 = o1 + (w // 2) * (h // 2)
    o3 = o2 + (w // 4) * (h // 4)
    end = o3 + (w // 8) * (h // 8)
    pal = bytes(765) + bytes((10, 20, 30))
    return (name.encode().ljust(16, b"\0") + struct.pack("<II4I", w, h, o0, o1, o2, o3)
            + px + bytes(end - o1) + struct.pack("<H", 256) + pal + bytes(2))


def wad(lumps) -> bytes:
    body, dirents = b"", b""
    for name, data in lumps:
        dirents += struct.pack("<iiibbh16s", 12 + len(body), len(data), len(data), 0x43, 0, 0,
                               name.encode().ljust(16, b"\0"))
        body += data
    return b"WAD3" + struct.pack("<ii", len(lumps), 12 + len(body)) + body + dirents


class Decals(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.tmp = tempfile.mkdtemp(prefix="test_decals.")
        cls.wad = wad([("{A", miptex("{A", 32, 16, lambda x, y: 255 if x < 16 else 0)),
                       ("{WIDE", miptex("{WIDE", 40, 24, lambda x, y: 255)),
                       ("{BIG", miptex("{BIG", 128, 64, lambda x, y: 136))])

    def test_sheet(self) -> None:
        b = decals.sheet(self.wad, ["{a", "{wide", "{big", "{a"], "w")
        self.assertEqual(decals.sheet_names(b, "s"), ["{a", "{wide", "{big", "{a"])
        magic, n, esz = struct.unpack_from(">IHH", b)
        self.assertEqual((magic, n, esz), (decals.SHEET_MAGIC, 4, decals.SHEET_ENTRY))
        e = [b[8 + 28 * i:8 + 28 * (i + 1)] for i in range(4)]
        self.assertEqual(e[3], e[0])                       # a name listed twice shares
        self.assertEqual(tuple(e[0][16:20]), (32, 16, 32, 16))
        self.assertEqual(tuple(e[1][16:20]), (48, 24, 40, 24))   # 40 -> 48 texels, 40 u
        self.assertEqual(tuple(e[2][16:20]), (64, 32, 128, 64))  # halved, 128 u
        self.assertEqual(tuple(e[0][20:23]), (10, 20, 30))       # tint = palette[255]
        ofs = [struct.unpack_from(">I", x, 24)[0] for x in e]
        self.assertTrue(all(o % 8 == 0 for o in ofs))
        row = b[ofs[0]:ofs[0] + 16]                        # 32 texels: 16 opaque, 16 clear
        self.assertEqual(row, b"\xff" * 8 + b"\x00" * 8)
        wide = b[ofs[1]:ofs[1] + 48 * 24 // 2]
        self.assertEqual(wide, b"\xff" * (48 * 24 // 2))     # constant alpha stays constant
        big = b[ofs[2]:ofs[2] + 64 * 32 // 2]
        self.assertEqual(set(big), {0x88})                 # 136 -> 8 / 15

    def test_sheet_rejects(self) -> None:
        with self.assertRaisesRegex(decals.DecalError, "missing"):
            decals.sheet(self.wad, ["{a", "{nope"], "w")
        with self.assertRaisesRegex(decals.DecalError, "decals in one sheet"):
            decals.sheet(self.wad, ["{a"] * (decals.SHEET_MAX + 1), "w")
        with self.assertRaises(decals.DecalError):
            decals.sheet(self.wad, [], "w")
        with self.assertRaises(decals.DecalError):
            decals.wad_lumps(b"WAD2" + self.wad[4:], "w")

    def test_sheet_names_rules(self) -> None:
        b = bytearray(decals.sheet(self.wad, ["{a", "{wide"], "w"))
        for bad in (b"", bytes(b[:6]), b"XXXX" + bytes(b[4:]),
                    bytes(b[:8 + 16]) + bytes((24,)) + bytes(b[8 + 17:]),     # width 24
                    bytes(b[:8 + 24]) + struct.pack(">I", len(b)) + bytes(b[8 + 28:]),
                    bytes(b[:4]) + struct.pack(">H", 0) + bytes(b[6:])):
            with self.assertRaises(decals.DecalError):
                decals.sheet_names(bad, "s")

    def limits(self, kb: int) -> str:
        with open(LIMITS) as f:
            text = f.read()
        text, n = re.subn(r"^(#define\s+HL64_DECAL_TEXELS_KB\s+)\d+",
                          lambda m: m.group(1) + str(kb), text, flags=re.M)
        self.assertEqual(n, 1)
        path = os.path.join(self.tmp, f"limits{kb}.h")
        with open(path, "w") as f:
            f.write(text)
        return path

    def test_region(self) -> None:
        """global sheet + 2 x the largest map sheet (16-aligned) + piece
        vertices; positive control: the largest whole-KB carve below the
        need fails, the need rounded up passes"""
        verts = (restab.read_limit(LIMITS, "HL64_FX_DECALS")
                 * restab.read_limit(LIMITS, "HL64_FX_DECAL_VERTS") * decals.DVERT_BYTES)
        r = decals.region_need(30000, {"m1": 4000, "m2": 17001, "m3": 9000}, LIMITS)
        self.assertEqual((r.map_name, r.map_bytes, r.verts), ("m2", 17001, verts))
        self.assertEqual(r.total, 30000 + 2 * 17008 + verts)
        self.assertEqual(decals.region_need(30000, {}, LIMITS).total, 30000 + verts)
        with self.assertRaisesRegex(decals.DecalError,
                                    rf"decal region need {r.total} B .* exceeds HL64_DECAL_TEXELS_KB"):
            decals.check_region(r, self.limits((r.total - 1) // 1024))
        kb = (r.total + 1023) // 1024
        self.assertEqual(decals.check_region(r, self.limits(kb)), kb * 1024)


if __name__ == "__main__":
    unittest.main()
