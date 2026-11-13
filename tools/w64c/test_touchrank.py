# SPDX-License-Identifier: MIT
"""touchrank (trigger touch ranks, area-links-and-touch.md 3.1). Run from the
repository root: python3 -m unittest tools.w64c.test_touchrank"""
import contextlib
import io
import json
import struct
import tempfile
import unittest
from pathlib import Path

from tools.w64c import touchrank as tr

WORLD = (-1024.0, -512.0, -256.0, 1024.0, 512.0, 256.0)


def make_bsp(ents, models=(WORLD,), version=30):
    """BSP whose only content is the entity lump and the model lump."""
    text = "".join("{" + "".join(f'"{k}" "{v}" ' for k, v in e.items()) + "}\n"
                   for e in ents).encode("latin-1") + b"\0"
    mlump = b"".join(struct.pack("<6f", *m).ljust(64, b"\0") for m in models)
    hdr = 4 + 8 * 15
    table = [(hdr, len(text))] + [(0, 0)] * 13 + [(hdr + len(text), len(mlump))]
    return (struct.pack("<i", version) + b"".join(struct.pack("<ii", *t) for t in table)
            + text + mlump), text


def make_hle(recs, srchash, big=False):
    """Minimal HLE1 chunk: header, records (lump, class), classes, strings."""
    bo = ">" if big else "<"
    classes = sorted({c for _l, c in recs})
    strings = b"".join(c.encode() + b"\0" for c in classes)
    soff = {}
    o = 0
    for c in classes:
        soff[c] = o
        o += len(c) + 1
    ofs_recs = 96
    ofs_classes = ofs_recs + 24 * len(recs)
    ofs_strings = ofs_classes + 8 * len(classes)
    size = ofs_strings + len(strings)
    numlumps = max([l for l, _c in recs] + [0]) + 1
    h = struct.pack(bo + "IHHIIHHHHHH", tr.HLE_MAGIC, 1, 1 if big else 0, size, srchash,
                    numlumps, len(recs), len(classes), 0, 0, 1)
    h += struct.pack(bo + "4I", 0, 0, 0, 0)                      # counts
    h += struct.pack(bo + "II", len(strings), 0)                 # strsize, namestr_end
    h += struct.pack(bo + "11I", ofs_recs, 0, ofs_classes, *([0] * 7), ofs_strings)
    assert len(h) == 96
    body = b"".join(struct.pack(bo + "12H", l, classes.index(c), 0xFFFF, 0, 0, 0, 0, 0,
                                0xFFFF, 0xFFFF, 0, 0) for l, c in recs)
    body += b"".join(struct.pack(bo + "IHH", soff[c], 0, 0) for c in classes)
    return h + body + strings


ENTS = [
    {"classname": "worldspawn"},
    {"classname": "ammo_9mmclip", "origin": "512 0 0"},                 # 1
    {"classname": "weapon_shotgun", "origin": "600 0 0"},               # 2
    {"classname": "item_battery", "origin": "520 0 0"},                 # 3
    {"classname": "info_target", "origin": "0 0 0"},                    # 4
    {"classname": "trigger_once", "model": "*1"},                       # 5
    {"classname": "trigger_hurt", "model": "*2", "origin": "-600 0 0"},  # 6
    {"classname": "path_track", "origin": "-9 0 0"},                    # 7
]
MODELS = (WORLD, (300.0, -40.0, -8.0, 700.0, 40.0, 8.0), (-10.0, -10.0, 0.0, 10.0, 10.0, 8.0))


class CellTests(unittest.TestCase):
    def test_root_straddle_and_plane_contact_stay_above(self):
        self.assertEqual(tr.cell(WORLD, [-10, 0, 0], [10, 1, 1]), 0)
        # face on the split plane x = 0 stays in the root
        self.assertEqual(tr.cell(WORLD, [0, 0, 0], [10, 1, 1]), 0)
        self.assertEqual(tr.cell(WORLD, [-10, 0, 0], [0, 1, 1]), 0)

    def test_preorder_upper_child_first(self):
        # x > 0 half: cell 1; x < 0 half: cell 16 (after the 15 upper cells)
        self.assertEqual(tr.cell(WORLD, [1, 0, 0], [2, 1, 0]), 1)
        self.assertEqual(tr.cell(WORLD, [-2, 0, 0], [-1, 1, 0]), 16)
        # then [0,1024]x[-512,512] is square: split y (y > 0 first)
        self.assertEqual(tr.cell(WORLD, [500, 1, 0], [600, 2, 0]), 2)
        self.assertEqual(tr.cell(WORLD, [500, -2, 0], [600, -1, 0]), 9)
        self.assertEqual(tr.cell(WORLD, [1, 1, 0], [2, 2, 0]), 8)
        # deepest: x 512..1024, y 256..512 -> 4 levels, upper every time
        self.assertEqual(tr.cell(WORLD, [900, 400, 0], [1000, 500, 0]), 4)
        self.assertEqual(tr.cell(WORLD, [-1000, -500, 0], [-900, -400, 0]), 30)

    def test_non_finite_box_never_raises(self):
        nan = float("nan")
        self.assertEqual(tr.cell(WORLD, [nan] * 3, [nan] * 3), 0)


class BoxTests(unittest.TestCase):
    def test_trigger_boxes(self):
        self.assertEqual(tr.trigger_box(MODELS, ENTS[5]),
                         (tr.G_SPAWN, [298.0, -42.0, -10.0], [702.0, 42.0, 10.0]))
        self.assertEqual(tr.trigger_box(MODELS, ENTS[6])[1:], ([-612.0, -12.0, -2.0], [-588.0, 12.0, 10.0]))
        self.assertEqual(tr.trigger_box(MODELS, ENTS[7]), (tr.G_SPAWN, [-18.0, -9.0, -9.0], [0.0, 9.0, 9.0]))
        self.assertEqual(tr.trigger_box(MODELS, ENTS[1]), (tr.G_AMMO, [495.0, -17.0, -1.0], [529.0, 17.0, 17.0]))
        self.assertEqual(tr.trigger_box(MODELS, ENTS[3])[0], tr.G_SPAWN)
        self.assertEqual(tr.trigger_box(MODELS, ENTS[2]), (tr.G_WEAPON, [576.0, -24.0, 0.0], [624.0, 24.0, 16.0]))
        self.assertIsNone(tr.trigger_box(MODELS, ENTS[4]))
        self.assertIsNone(tr.trigger_box(MODELS, ENTS[0]))

    def test_hostile_entities_get_no_rank(self):
        for e in ({"classname": "trigger_once"}, {"classname": "trigger_once", "model": "*9"},
                  {"classname": "trigger_once", "model": "*0"}, {"classname": "trigger_once", "model": "*x"},
                  {"classname": "trigger_once", "model": "*-1"},
                  {"classname": "item_battery", "origin": "nan 0 0"},
                  {"classname": "item_battery", "origin": "1e999 0 0"}):
            self.assertIsNone(tr.trigger_box(MODELS, e), e)
        self.assertIsNone(tr.trigger_box(MODELS[:1] + ((float("inf"),) * 6,), {"classname": "trigger_once", "model": "*1"}))
        # short / junk vectors read like GoldSrc: missing components are 0
        self.assertEqual(tr.finite_vec("5"), [5.0, 0.0, 0.0])
        self.assertEqual(tr.finite_vec("1 x 3"), [1.0, 0.0, 0.0])

    def test_rank_order_inside_a_cell(self):
        rk = tr.ranks(MODELS, ENTS)
        # cell 1 (x > 0): spawn group by lump (battery 3, trigger_once 5),
        # then ammo (1), then weapon (2); path_track 7 touches x = 0 (root)
        c = 1 << tr.CELL_SHIFT
        self.assertEqual(rk, {3: c | 0, 5: c | 1, 1: c | 2, 2: c | 3, 7: 0, 6: (16 << tr.CELL_SHIFT) | 0})
        self.assertEqual(sorted(rk, key=rk.get), [7, 3, 5, 1, 2, 6])


def map_files(t, ents=ENTS, models=MODELS, recs=None, big=False, srchash=None):
    bsp, text = make_bsp(ents, models)
    p = Path(t) / "m.bsp"
    p.write_bytes(bsp)
    recs = recs if recs is not None else [(i, e["classname"]) for i, e in enumerate(ents)
                                          if e["classname"] != "info_target"]
    h = Path(t) / "m.hle"
    h.write_bytes(make_hle(recs, tr.fnv1a(text) if srchash is None else srchash, big))
    return p, h


class HleTests(unittest.TestCase):
    def test_apply_both_byte_orders(self):
        for big in (False, True):
            with tempfile.TemporaryDirectory() as t:
                p, h = map_files(t, big=big)
                new, n = tr.apply(h.read_bytes(), p)
                self.assertEqual(n, 6)
                got = tr.read_tranks(new)
                want = tr.ranks(MODELS, ENTS)
                self.assertEqual(got[0], tr.HLE_NONE)
                self.assertEqual({k: v for k, v in got.items() if v != tr.HLE_NONE}, want)
                # nothing but the trank bytes changed
                old = h.read_bytes()
                diff = [i for i in range(len(old)) if old[i] != new[i]]
                self.assertTrue(all((i - 96) % 24 in (18, 19) for i in diff), diff)

    def test_mismatched_source_rejected(self):
        with tempfile.TemporaryDirectory() as t:
            p, h = map_files(t, srchash=1234)
            with self.assertRaisesRegex(tr.RankError, "not the one"):
                tr.apply(h.read_bytes(), p)
        with tempfile.TemporaryDirectory() as t:
            p, h = map_files(t, recs=[(0, "worldspawn"), (2, "trigger_once")])
            with self.assertRaisesRegex(tr.RankError, "is not BSP entity 2"):
                tr.apply(h.read_bytes(), p)

    def test_hostile_hle(self):
        with tempfile.TemporaryDirectory() as t:
            _p, h = map_files(t)
            good = h.read_bytes()
        cases = {
            "shorter than its header": good[:95],
            "not an HLE1": b"XXXX" + good[4:],
            "outside the": good[:8] + struct.pack("<I", len(good) + 1) + good[12:],
            "section outside": good[:18] + struct.pack("<H", 0xFFFF) + good[20:],
        }
        bad_str = bytearray(good)
        struct.pack_into("<I", bad_str, struct.unpack_from("<I", good, 60)[0], 1 << 20)
        cases["string outside"] = bytes(bad_str)
        recs_lump = bytearray(good)
        struct.pack_into("<H", recs_lump, 96, 0xFFF0)
        cases["out of range"] = bytes(recs_lump)
        for what, blob in cases.items():
            with self.assertRaisesRegex(tr.RankError, what):
                tr.read_tranks(blob)

    def test_hostile_bsp(self):
        with tempfile.TemporaryDirectory() as t:
            p = Path(t) / "m.bsp"
            for what, data in (("not a BSP", b"\0" * 20),
                               ("not a BSP", struct.pack("<i", 31) + b"\0" * 200),
                               ("model lump", make_bsp(ENTS)[0][:-1]),
                               ("world bounds", make_bsp(ENTS, ((0.0,) * 6,))[0]),
                               ("world bounds", make_bsp(ENTS, ((float("nan"),) * 6,))[0])):
                p.write_bytes(data)
                with self.assertRaisesRegex(tr.RankError, what):
                    tr.read_models(p)

    def test_too_many_triggers_in_one_cell(self):
        ents = [{"classname": "worldspawn"}] + [{"classname": "path_track", "origin": "0 0 0"}] * 2049
        with self.assertRaisesRegex(tr.RankError, "one cell"):
            tr.ranks(MODELS, ents)


def run_main(*argv):
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        rc = tr.main([str(a) for a in argv])
    return rc, out.getvalue(), err.getvalue()


class CliTests(unittest.TestCase):
    def test_in_place_and_out(self):
        with tempfile.TemporaryDirectory() as t:
            p, h = map_files(t)
            old = h.read_bytes()
            rc, out, err = run_main(p, h, "-o", Path(t) / "o.hle")
            self.assertEqual((rc, err), (0, ""))
            self.assertEqual(h.read_bytes(), old)
            rc, out, _ = run_main(p, h)
            self.assertEqual(rc, 0)
            self.assertIn("6 triggers ranked", out)
            self.assertEqual(h.read_bytes(), (Path(t) / "o.hle").read_bytes())

    def test_errors_are_one_line(self):
        with tempfile.TemporaryDirectory() as t:
            p, h = map_files(t)
            h.write_bytes(b"HLE1")
            rc, _out, err = run_main(p, h)
            self.assertEqual(rc, 1)
            self.assertTrue(err.startswith("touchrank: error: "), err)
            self.assertNotIn("Traceback", err)
            rc, _out, err = run_main(p, Path(t) / "missing.hle")
            self.assertEqual(rc, 1)
            self.assertIn("touchrank: error: ", err)

    def test_check_counts_and_fails(self):
        with tempfile.TemporaryDirectory() as t:
            maps = Path(t) / "maps"
            maps.mkdir()
            (maps / "m.bsp").write_bytes(make_bsp(ENTS, MODELS)[0])
            fx = Path(t) / "fx" / "area-links-and-touch" / "A-order"
            fx.mkdir(parents=True)

            def trig(i, lump, cls, o, model="-"):
                return {"trigger": {"i": i, "lump": lump, "class": cls, "o": o, "model": model}}
            recs = [trig(10, 5, "trigger_once", [0, 0, 0], "*1"), trig(11, 1, "ammo_9mmclip", [512, 0, -5]),
                    trig(12, 2, "weapon_shotgun", [600, 0, 0]), trig(13, -1, "item_battery", [0, 0, 0]),
                    {"place": 0, "phase": 1, "pair": [10, 11], "order": [10, 11]},
                    {"place": 1, "phase": 1, "pair": [11, 10], "order": [10, 11]},
                    {"place": 2, "phase": 1, "pair": [13, 10], "order": [10, 13]},
                    {"place": 3, "phase": 4, "pair": [12, 11], "order": [12, 11]}]
            f = fx / "m.jsonl"
            f.write_text("".join(json.dumps(r) + "\n" for r in recs))
            rc, out, _ = run_main("--check", Path(t) / "fx", "--maps-dir", maps)
            self.assertEqual(rc, 0, out)
            self.assertIn("m: 1/2 pairs in rank order, 1 with a game-created trigger", out)
            recs.append({"place": 4, "phase": 1, "pair": [11, 12], "order": [12, 11]})
            f.write_text("".join(json.dumps(r) + "\n" for r in recs))
            rc, out, _ = run_main("--check", Path(t) / "fx", "--maps-dir", maps)
            self.assertEqual(rc, 1)
            self.assertIn("FAIL m 12<11", out)


if __name__ == "__main__":
    unittest.main()
