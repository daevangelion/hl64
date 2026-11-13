# SPDX-License-Identifier: MIT
"""hl_campaign tests: the cook on tiny hand-built BSPs, and negative tests for
every kind of malformed input (truncated BSP or CWG3, lump outside the file,
bad magic or version, indices out of range, non-numeric entity keys, absurd
counts). Malformed input must give `hl_campaign: error: <file>: <what>` on
stderr, exit 1, no traceback. Run from the repository root:
python3 -m unittest tools.cook.test_hl_campaign
"""
import contextlib
import io
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from tools.cook import hl_campaign as hc


def make_bsp(ents, bounds=(-512, -512, -512, 512, 512, 512)):
    """BSP30 whose only content is the entity lump and the world model (lump 14)."""
    text = "".join("{" + "".join(f'"{k}" "{v}" ' for k, v in e.items()) + "}\n" for e in ents).encode("latin-1") + b"\0"
    model = struct.pack("<6f", *bounds).ljust(64, b"\0")
    table = [(124, len(text))] + [(0, 0)] * 13 + [(124 + len(text), 64)]
    return struct.pack("<i", 30) + b"".join(struct.pack("<ii", *t) for t in table) + text + model


def two_maps(root):
    maps = Path(root) / "maps"
    maps.mkdir()
    (maps / "a.bsp").write_bytes(make_bsp([
        {"classname": "worldspawn"},
        {"classname": "info_landmark", "targetname": "lm", "origin": "100 0 0"},
        {"classname": "trigger_changelevel", "map": "b", "landmark": "lm", "spawnflags": "0"}]))
    (maps / "b.bsp").write_bytes(make_bsp([
        {"classname": "worldspawn"},
        {"classname": "info_landmark", "targetname": "lm", "origin": "-50 0 0"}]))
    return maps


def cli(module_main, *argv):
    out, err = io.StringIO(), io.StringIO()
    with mock.patch.object(sys, "argv", ["prog", *map(str, argv)]), \
            contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        rc = module_main()
    return rc, out.getvalue(), err.getvalue()


class CampaignTests(unittest.TestCase):
    def rejected(self, *argv, where):
        rc, out, err = cli(hc.main, *argv)
        self.assertEqual(rc, 1, (out, err))
        self.assertTrue(err.startswith("hl_campaign: error: "), err)
        self.assertIn(where, err)
        self.assertNotIn("Traceback", err)
        return err

    def test_two_maps_make_one_component(self):
        with tempfile.TemporaryDirectory() as t:
            maps = two_maps(t)
            out = Path(t) / "campaign.cwg"
            rc, stdout, err = cli(hc.main, "--maps-dir", maps, "--maps", "a", "b", "-o", out)
            self.assertEqual((rc, err), (0, ""), stdout)
            g = hc.read_cwg(out)
            self.assertEqual([m["name"] for m in g["maps"]], ["a", "b"])
            self.assertEqual(g["start"], "a")
            self.assertEqual(g["maps"][0]["links"][0]["to"], "b")
            self.assertEqual(g["maps"][1]["landmarks"], {"lm": [-50.0, 0.0, 0.0]})
            rc, listing, _ = cli(hc.main, "--list", out)
            self.assertEqual((rc, listing.strip()), (0, "a b"))

    def test_bad_bsp_inputs(self):
        with tempfile.TemporaryDirectory() as t:
            maps = two_maps(t)
            out = Path(t) / "o.cwg"
            good = (maps / "a.bsp").read_bytes()
            for data, msg in ((b"", "shorter than a BSP header"), (good[:10], "shorter than a BSP header"),
                              (struct.pack("<i", 28) + good[4:], "BSP version"),
                              (good[:4] + struct.pack("<ii", 100000, 50) + good[12:], "outside"),
                              (good[:4 + 14 * 8] + struct.pack("<ii", 124, 0) + good[4 + 15 * 8:], "invalid BSP model lump")):
                (maps / "a.bsp").write_bytes(data)
                self.rejected("--maps-dir", maps, "--maps", "a", "b", "-o", out, where="a.bsp")
            # world bounds that are inverted or not finite
            for bounds in ((5, 0, 0, 1, 1, 1), (0, 0, 0, float("inf"), 1, 1), (float("nan"), 0, 0, 1, 1, 1)):
                (maps / "a.bsp").write_bytes(make_bsp([{"classname": "worldspawn"}], bounds))
                self.rejected("--maps-dir", maps, "--maps", "a", "b", "-o", out, where="a.bsp")
            self.assertFalse(out.exists())

    def test_bad_entity_keys(self):
        with tempfile.TemporaryDirectory() as t:
            maps = two_maps(t)
            out = Path(t) / "o.cwg"
            for ent, msg in (({"classname": "info_landmark", "targetname": "x", "origin": "1 2 q"}, "origin"),
                             ({"classname": "info_landmark", "targetname": "x", "origin": "1 2 nan"}, "not finite"),
                             ({"classname": "info_landmark", "targetname": "x", "origin": "inf 0 0"}, "not finite"),
                             ({"classname": "trigger_changelevel", "map": "b", "spawnflags": "many"}, "invalid literal")):
                (maps / "a.bsp").write_bytes(make_bsp([{"classname": "worldspawn"}, ent]))
                err = self.rejected("--maps-dir", maps, "--maps", "a", "b", "-o", out, where="a.bsp")
                self.assertIn(msg, err)

    def test_no_maps_and_bad_names_and_unwritable_output(self):
        with tempfile.TemporaryDirectory() as t:
            maps = two_maps(t)
            out = Path(t) / "o.cwg"
            self.rejected("--maps-dir", Path(t) / "gone", "-o", out, where="no maps")
            self.rejected("--maps-dir", maps, "--maps", "zzz", "-o", out, where="no maps")
            (maps / "a b.bsp").write_bytes((maps / "a.bsp").read_bytes())
            self.rejected("--maps-dir", maps, "--maps", "a b", "-o", out, where="a b.bsp")
            (maps / "a b.bsp").unlink()
            self.rejected("--maps-dir", maps, "--maps", "a", "b", "-o", Path(t) / "no" / "dir" / "o.cwg",
                          where="o.cwg")

    def test_malformed_cwg(self):
        with tempfile.TemporaryDirectory() as t:
            maps = two_maps(t)
            out = Path(t) / "o.cwg"
            self.assertEqual(cli(hc.main, "--maps-dir", maps, "-o", out)[0], 0)
            good = out.read_bytes()
            for n in (0, 5, 31, 32, 100, len(good) - 1):
                out.write_bytes(good[:n])
                self.rejected("--list", out, where="o.cwg")
            cases = []
            for off, fmt, val in ((0, ">I", 0x43574732), (4, ">H", 4), (6, ">H", 1), (8, ">H", 60000),
                                  (8, ">H", 3), (10, ">H", 9), (12, ">H", 9), (14, ">H", 9), (14, ">H", 0xFFFE),
                                  (18, ">H", 0xFFFF), (26, ">H", 1), (28, ">I", 0), (28, ">I", 10**9)):
                b = bytearray(good)
                struct.pack_into(fmt, b, off, val)
                cases.append(b)
            m0 = 32
            for off, fmt, val in ((m0 + 0, ">H", 0xFF00), (m0 + 2, ">H", 5), (m0 + 4, ">H", 9),
                                  (m0 + 12, ">f", float("nan")), (m0 + 12, ">f", 17.0),
                                  (m0 + 24, ">f", 10**6), (m0 + 48 + 0, ">H", 0)):
                b = bytearray(good)
                struct.pack_into(fmt, b, off, val)
                cases.append(b)
            lm0 = 32 + 2 * hc.MAP_BYTES
            for off, fmt, val in ((lm0 + 2, ">H", 1), (lm0 + 4, ">f", float("nan")), (lm0 + 4, ">f", float("inf"))):
                b = bytearray(good)
                struct.pack_into(fmt, b, off, val)
                cases.append(b)
            link0 = lm0 + 2 * 16
            for off, fmt, val in ((link0, ">H", 7), (link0 + 6, ">H", 3), (link0 + 6, ">H", 0xFFFE)):
                b = bytearray(good)
                struct.pack_into(fmt, b, off, val)
                cases.append(b)
            b = bytearray(good)
            b[-1] = 1
            cases.append(b)
            for b in cases:
                out.write_bytes(bytes(b))
                self.rejected("--list", out, where="o.cwg")

    def test_duplicate_and_non_file_names_in_cwg(self):
        with tempfile.TemporaryDirectory() as t:
            maps = two_maps(t)
            out = Path(t) / "o.cwg"
            cli(hc.main, "--maps-dir", maps, "-o", out)
            good = out.read_bytes()
            b = bytearray(good)
            i = b.index(b"a\0")
            b[i] = ord("b")                                         # both maps now called "b"
            out.write_bytes(bytes(b))
            self.assertIn("duplicate map name", self.rejected("--list", out, where="o.cwg"))
            b = bytearray(good)
            b[b.index(b"a\0")] = ord("/")
            out.write_bytes(bytes(b))
            self.assertIn("plain file name", self.rejected("--list", out, where="o.cwg"))

    def test_missing_cwg(self):
        with tempfile.TemporaryDirectory() as t:
            self.rejected("--list", Path(t) / "none.cwg", where="none.cwg")


if __name__ == "__main__":
    unittest.main()
