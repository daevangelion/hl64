# SPDX-License-Identifier: MIT
"""Unit tests of the content cook (tools/cook): synthetic inputs only, no
game data. Hostile-input cases check that every reader rejects malformed
files with its own error instead of a crash or an out-of-range read.

  python3 -m unittest tools.cook.test_cook
"""
from __future__ import annotations

import struct
import tempfile
import unittest
from pathlib import Path

from tools.cook import bake_hlhud, bake_ui, bake_world_tex, hl_classes
from tools.cook import hl_sound_classes, hl_sprite_cook
from tools.cook.data import Data, DataError


def pack(entries):
    """PACK archive of {name: bytes}."""
    body = b""
    table = b""
    ofs = 12
    for name, data in entries.items():
        table += struct.pack("<56sII", name.encode(), ofs, len(data))
        body += data
        ofs += len(data)
    return b"PACK" + struct.pack("<II", ofs, len(table)) + body + table


def bsp30(entities=b"", textures=b""):
    """BSP30 with an entity lump and a texture lump, other lumps empty."""
    lumps = [b""] * 15
    lumps[0] = entities
    lumps[2] = textures
    ofs = 4 + 8 * 15
    head = struct.pack("<i", 30)
    body = b""
    for lump in lumps:
        head += struct.pack("<ii", ofs + len(body), len(lump))
        body += lump
    return head + body


def miptex(name, w, h, pix, pal):
    """One embedded miptex: header, 4 mips, u16 colours, palette."""
    sizes = [w * h, (w // 2) * (h // 2), (w // 4) * (h // 4), (w // 8) * (h // 8)]
    ofs, o = [], 40
    for s in sizes:
        ofs.append(o)
        o += s
    mips = pix + bytes(sum(sizes[1:]))
    return (name.encode().ljust(16, b"\0") + struct.pack("<6I", w, h, *ofs)
            + mips + struct.pack("<H", 256) + pal)


def texlump(*tex):
    head = struct.pack("<i", len(tex))
    ofs = 4 + 4 * len(tex)
    body = b""
    for t in tex:
        head += struct.pack("<i", ofs + len(body))
        body += t
    return head + body


def spr(frames, palcount=256, texformat=0):
    head = b"IDSP" + struct.pack("<iiifiiiii", 2, 0, texformat, 1.0, 8, 8,
                                 len(frames), 0, 0)
    out = head + struct.pack("<h", palcount) + bytes(3 * palcount)
    for w, h, px in frames:
        out += struct.pack("<i4i", 0, 0, 0, w, h) + px
    return out


class TestData(unittest.TestCase):
    def test_loose_beats_pak_case_insensitive(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            (root / "pak0.pak").write_bytes(pack({"sound/A.wav": b"pak", "gfx/x.lmp": b"x"}))
            (root / "sound").mkdir()
            (root / "sound" / "a.wav").write_bytes(b"loose")
            data = Data(root)
            self.assertEqual(data.read("SOUND/a.WAV"), b"loose")
            self.assertEqual(data.read("gfx/X.lmp"), b"x")
            self.assertIsNone(data.read("nothing"))

    def test_bad_pack(self):
        cases = {
            "magic": b"KCAP" + bytes(8),
            "dir past end": b"PACK" + struct.pack("<II", 12, 640),
            "entry past end": b"PACK" + struct.pack("<II", 12, 64)
                               + struct.pack("<56sII", b"a", 0, 9999),
        }
        for what, blob in cases.items():
            with self.subTest(what), tempfile.TemporaryDirectory() as d:
                (Path(d) / "pak0.pak").write_bytes(blob)
                with self.assertRaises(DataError):
                    Data(Path(d))


class TestEntities(unittest.TestCase):
    def test_entities(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "m.bsp"
            p.write_bytes(bsp30(b'{\n"classname" "worldspawn"\n}\n{\n"a" "b"\n}\n\0'))
            self.assertEqual(hl_classes.map_entities(p),
                             [{"classname": "worldspawn"}, {"a": "b"}])

    def test_hostile_bsp(self):
        good = bsp30(b"{}")
        cases = {
            "short": good[:8],
            "version": struct.pack("<i", 31) + good[4:],
            "lump outside": good[:4] + struct.pack("<ii", 100, 1 << 20) + good[12:],
        }
        for what, blob in cases.items():
            with self.subTest(what), tempfile.TemporaryDirectory() as d:
                p = Path(d) / "m.bsp"
                p.write_bytes(blob)
                with self.assertRaises(hl_classes.BspError):
                    hl_classes.map_entities(p)


class TestWorldTex(unittest.TestCase):
    PAL = bytes(range(256)) * 3

    def bake(self, blob, ref=None):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "m.bsp"
            p.write_bytes(blob)
            return bake_world_tex.bake(p, ref)

    def test_one_texture(self):
        pix = bytes((x * 7 + y) & 255 for y in range(16) for x in range(16))
        out = self.bake(bsp30(textures=texlump(miptex("wall", 16, 16, pix, self.PAL))))
        magic, ver, n = struct.unpack_from(">IHH", out, 0)
        self.assertEqual((magic, ver, n), (bake_world_tex.MAGIC, 1, 1))
        name, pw, ph, w, h = struct.unpack_from(">16s4H", out, 76)
        self.assertEqual((name.rstrip(b"\0"), pw, ph, w, h), (b"wall", 16, 16, 16, 16))
        self.assertEqual(len(out) % 4, 0)

    def test_fence_slot0_transparent(self):
        pix = bytes([255] * 128 + [3] * 128)
        out = self.bake(bsp30(textures=texlump(miptex("{grate", 16, 16, pix, self.PAL))))
        tlut_ofs = struct.unpack_from(">I", out, 76 + 40)[0]
        self.assertEqual(struct.unpack_from(">H", out, tlut_ofs)[0] & 1, 0)
        ci4_ofs = struct.unpack_from(">I", out, 76 + 32)[0]
        self.assertEqual(out[ci4_ofs], 0)          # index 255 -> slot 0

    def test_wad_texture_skipped(self):
        wadtex = b"ext".ljust(16, b"\0") + struct.pack("<6I", 16, 16, 0, 0, 0, 0)
        out = self.bake(bsp30(textures=texlump(wadtex)))
        self.assertEqual(struct.unpack_from(">H", out, 6)[0], 0)

    def test_wad_texture_found(self):
        pix = bytes(range(256))
        wadtex = b"ext".ljust(16, b"\0") + struct.pack("<6I", 16, 16, 0, 0, 0, 0)
        lump = miptex("EXT", 16, 16, pix, self.PAL)
        wad = (b"WAD3" + struct.pack("<ii", 1, 12 + len(lump)) + lump
               + struct.pack("<iiiBBH16s", 12, len(lump), len(lump), 0x43, 0, 0, b"EXT"))
        ents = b'{\n"classname" "worldspawn"\n"wad" "\\half-life\\valve\\none.wad;\\half-life\\valve\\Tex.wad"\n}\n'
        with tempfile.TemporaryDirectory() as d:
            (Path(d) / "TEX.WAD").write_bytes(wad)
            p = Path(d) / "m.bsp"
            p.write_bytes(bsp30(ents, texlump(wadtex)))
            out = bake_world_tex.bake(p, None, Path(d))
            self.assertEqual(struct.unpack_from(">H", out, 6)[0], 1)
            (Path(d) / "TEX.WAD").write_bytes(b"WAD2" + bytes(8))
            with self.assertRaises(bake_world_tex.TexError):
                bake_world_tex.bake(p, None, Path(d))

    def test_hostile(self):
        tex = miptex("wall", 16, 16, bytes(256), self.PAL)
        cases = {
            "version": struct.pack("<i", 29) + bsp30()[4:],
            "count": bsp30(textures=struct.pack("<i", 100000)),
            "miptex outside": bsp30(textures=struct.pack("<ii", 1, 4000)),
            "texels outside": bsp30(textures=texlump(tex[:200])),
            "size": bsp30(textures=texlump(tex[:16] + struct.pack("<6I", 0, 16, 40, 0, 0, 0))),
        }
        for what, blob in cases.items():
            with self.subTest(what):
                with self.assertRaises(bake_world_tex.TexError):
                    self.bake(blob)


class TestSprite(unittest.TestCase):
    def test_cook(self):
        out = hl_sprite_cook.cook(spr([(8, 8, bytes(range(64)))]))
        self.assertEqual(struct.unpack_from(">IHH", out, 0), (hl_sprite_cook.MAGIC, 1, 1))
        self.assertEqual(len(out) % 8, 0)

    def test_hostile(self):
        good = spr([(8, 8, bytes(64))])
        cases = {
            "truncated": good[:50],
            "frame past end": good[:-10],
            "palette count": good[:40] + struct.pack("<h", 300) + good[42:],
            "magic": b"IDSQ" + good[4:],
        }
        for what, blob in cases.items():
            with self.subTest(what):
                with self.assertRaises(ValueError):
                    hl_sprite_cook.cook(blob)


class TestHud(unittest.TestCase):
    def test_hostile_sprite(self):
        with self.assertRaises(bake_hlhud.HudError):
            bake_hlhud.parse_hl_spr(spr([(8, 8, bytes(64))])[:60], "x.spr")

    def test_rects(self):
        rects = bake_hlhud.parse_hud_txt("2\nd_9mm 320 320hud1 0 0 32 16\nd_9mm 640 640hud1 0 0 1 1\n")
        self.assertEqual(rects, [("d_9mm", 0, 0, 0, 32, 16)])


class TestUi(unittest.TestCase):
    def wad(self, lumps):
        body, table = b"", b""
        ofs = 12
        for name, typ, data in lumps:
            table += struct.pack("<iiiBBH16s", ofs + len(body), len(data), len(data),
                                 typ, 0, 0, name.encode())
            body += data
        return b"WAD3" + struct.pack("<ii", len(lumps), ofs + len(body)) + body + table

    def test_pic(self):
        pal = bytearray(768)
        pal[3:6] = b"\xff\xff\xff"
        pal[6:9] = b"\x00\x00\xff"
        pic = struct.pack("<ii", 2, 2) + bytes([1, 2, 255, 1]) + struct.pack("<H", 256) + pal
        gpal = bytearray(768)
        gpal[30:33] = b"\xff\xff\xfe"
        out = bake_ui.bake(self.wad([("lambda", 0x42, pic)]), bytes(gpal))["lambda.ui8"]
        self.assertEqual(out[68:], bytes([10, 255, 255, 10]))

    def test_hostile(self):
        cases = {
            "magic": b"WAD2" + bytes(8),
            "dir": b"WAD3" + struct.pack("<ii", 5, 1 << 20),
            "pic": self.wad([("x", 0x42, struct.pack("<ii", 64, 64) + bytes(10))]),
        }
        for what, blob in cases.items():
            with self.subTest(what):
                with self.assertRaises(bake_ui.UiError):
                    bake_ui.bake(blob, bytes(768))


class TestSound(unittest.TestCase):
    def wav(self, rate, cue=None):
        fmt = struct.pack("<HHIIHH", 1, 1, rate, rate, 1, 8)
        out = b"fmt " + struct.pack("<I", len(fmt)) + fmt
        out += b"data" + struct.pack("<I", 4) + bytes(4)
        if cue is not None:
            c = struct.pack("<I", 1) + struct.pack("<II4sIII", 0, 0, b"data", 0, 0, cue)
            out += b"cue " + struct.pack("<I", len(c)) + c
        return b"RIFF" + struct.pack("<I", 4 + len(out)) + b"WAVE" + out

    def test_plan(self):
        rules = hl_sound_classes.parse_rules(hl_sound_classes.RULES)
        self.assertEqual(hl_sound_classes.plan(rules, "scientist/x.wav", self.wav(44100))[:3],
                         ("voice", "vadpcm,huffman=true", 22050))
        self.assertEqual(hl_sound_classes.plan(rules, "misc/hum.wav", self.wav(11025, 77)),
                         ("ambience", "vadpcm,huffman=false", 0, 77))
        self.assertEqual(hl_sound_classes.plan(rules, "weapons/x.wav", self.wav(11025))[0], "fx")

    def test_hostile(self):
        for blob in (b"RIFX" + bytes(8), b"RIFF\0\0\0\0WAVE", self.wav(22050)[:12]):
            with self.subTest(blob[:4]):
                with self.assertRaises(hl_sound_classes.SoundError):
                    hl_sound_classes.wav_info(blob, "x.wav")


if __name__ == "__main__":
    unittest.main()
