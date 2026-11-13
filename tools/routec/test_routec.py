# SPDX-License-Identifier: MIT
"""routec / rbc unit tests: compile every verb, round trip through the
reader, name resolution failures, bad source rejection, bad bytecode
rejection, and the committed test/routes/*.route sources. Run from the
repository root:

  python3 -m unittest tools.routec.test_routec
"""
from __future__ import annotations

import glob
import os
import random
import re
import struct
import tempfile
import unittest
import zlib

from ..restab import restab, rtb, testdata
from . import rbc, routec

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

ALL_VERBS = """\
# every verb of the route language
+duck
waitmap mapa 600
waitnear -2048 744 36 48 60
waitnear 1 2 3 1 0
shot start
stats mapa-1
setpos -2048 -496 70 0 270
setpos 10 20 30
wait 0.3
drop weapon_gun 1 2 3
waitmoved door1 15
waitgone sci 5
waitclass weapon_gun -2000 -620 112 32 5
waitglobal dish_on 30
waitglobal dish_on 30 off
look 20 90
lookat lamp
lookat monster_scientist
+use
-use
+attack
+moveright
-moveright
impulse 100
god 1
god 0
waitmap mapb 30
waitmoved door1 20
waitgone garg 120
waitend 60
-duck
pass
"""


class Base(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.tmp = tempfile.TemporaryDirectory()
        content, defs = testdata.build(cls.tmp.name)
        cls.table = rtb.read(rtb.write(restab.cook(content, defs)))

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def compile(self, text: str) -> bytes:
        ops = routec.compile_route(text, "t.route", self.table)
        return rbc.write(self.table.build_id, "t.route", ops)


class CompileTest(Base):
    def test_every_verb(self) -> None:
        data = self.compile(ALL_VERBS)
        prog = rbc.read(data, self.table)
        names = [rbc.OPS[o.code][0] for o in prog.ops]
        self.assertEqual(set(names), set(rbc.OPCODE))      # every op is produced
        lines = [ln for ln, l in enumerate(ALL_VERBS.splitlines(), 1)
                 if l.strip() and not l.startswith("#")]
        self.assertEqual([o.line for o in prog.ops], lines)
        t = self.table
        o = {i: op for i, op in enumerate(prog.ops)}
        self.assertEqual((o[0].b, o[0].words), (rbc.BUTTONS["duck"], (0,)))
        self.assertEqual((o[1].b, o[1].words), (0, (600_000_000,)))
        self.assertEqual(o[2].words, (-2048.0, 744.0, 36.0, 48.0, 60_000_000))
        self.assertEqual(o[3].words[4], 0)
        self.assertEqual((o[4].words, o[5].words), (("start",), ("mapa-1",)))
        self.assertEqual((o[6].a, o[6].words), (1, (-2048.0, -496.0, 70.0, 0.0, 270.0)))
        self.assertEqual(o[7].a, 0)
        self.assertEqual(o[8].words, (300_000,))
        self.assertEqual(o[9].words[0], t.class_id("weapon_gun"))
        self.assertEqual(o[10].b, t.target_id(0, "door1"))
        self.assertEqual(o[11].b, t.target_id(0, "sci"))
        self.assertEqual((o[13].a, o[14].a), (rbc.GLOBAL_ON, rbc.GLOBAL_OFF))
        self.assertEqual((o[16].a, o[16].b, o[16].words), (rbc.LOOKAT_TARGET, t.target_id(0, "lamp"), (0,)))
        self.assertEqual((o[17].a, o[17].b), (rbc.LOOKAT_CLASS, t.class_id("monster_scientist")))
        self.assertEqual((o[19].b, o[19].words), (0, (rbc.BUTTONS["use"],)))
        self.assertEqual(o[23].a, 100)
        self.assertEqual(o[26].b, 1)                         # mapb
        self.assertEqual(o[27].b, t.target_id(1, "door1"))   # names resolve in the current map
        self.assertEqual(prog.source, "t.route")
        self.assertIn("WAIT_MOVED mapb:door1 20000000us", rbc.disasm(prog, t))

    def test_deterministic(self) -> None:
        self.assertEqual(self.compile(ALL_VERBS), self.compile(ALL_VERBS))

    def test_errors(self) -> None:
        pre = "waitmap mapa 10\n"
        cases = (
            ("frobnicate 1\npass\n", r"t\.route:1: unknown verb"),
            ("+fly\npass\n", "unknown button"),
            ("waitmap nomap 10\npass\n", "not in the campaign"),
            ("setpos 1 2 3\npass\n", "before the first waitmap"),
            (pre + "waitgone nobody 5\npass\n", r"t\.route:2: no entity named 'nobody' in map mapa"),
            ("waitmap mapb 1\nwaitgone sci 5\npass\n", "no entity named 'sci' in map mapb"),
            (pre + "waitclass monster_none 1 2 3 4 5\npass\n", "not in classes.def"),
            (pre + "drop weapon_none 1 2 3\npass\n", "not in classes.def"),
            (pre + "lookat nothing\npass\n", "neither an entity name"),
            ("waitglobal nope 1\npass\n", "not set by any map"),
            ("waitglobal dish_on 1 maybe\npass\n", "on, off or dead"),
            ("wait\npass\n", "takes 1 arguments"),
            ("wait 1 2\npass\n", "takes 1 arguments"),
            ("wait nan\npass\n", "not a number"),
            ("wait 1_0\npass\n", "not a number"),
            ("wait -1\npass\n", "outside"),
            ("wait 5000\npass\n", "outside"),
            (pre + "setpos 1 2 3 4\npass\n", "takes 3 or 5"),
            (pre + "setpos 1e99 2 3\npass\n", "out of range"),
            (pre + "waitnear 1 2 3 -1 5\npass\n", "negative radius"),
            ("god\npass\n", "takes 1 arguments"),
            ("god 2\npass\n", "god takes 0 or 1"),
            ("impulse 256\npass\n", "0..255"),
            ("shot bad/label\npass\n", "label"),
            ("shot " + "x" * 48 + "\npass\n", "label"),
            ("wait 1\n", "must end with pass"),
            ("pass\npass\n", "once"),
            ("pass\nwait 1\n", "must end with pass"),
            ("wait 1 # ok\nlook 0 0\nwait 1 é\npass\n", r"t\.route:3: non-ASCII"),
        )
        for text, rule in cases:
            with self.subTest(text=text):
                with self.assertRaisesRegex(routec.CompileError, rule):
                    routec.compile_route(text, "t.route", self.table)

    def test_cli(self) -> None:
        d = self.tmp.name
        r = os.path.join(d, "campaign.rtb")
        with open(r, "wb") as f:
            f.write(rtb.write(self.table))
        src = os.path.join(d, "all.route")
        with open(src, "w") as f:
            f.write("# ünïcode comment\n" + ALL_VERBS)
        out = os.path.join(d, "all.rbc")
        self.assertEqual(routec.main(["--rtb", r, src, "-o", out]), 0)
        self.assertEqual(routec.main(["--rtb", r, "--dump", out]), 0)
        with open(src, "a") as f:
            f.write("wait 1\n")
        self.assertEqual(routec.main(["--rtb", r, src, "-o", out]), 1)


class ReaderTest(Base):
    def setUp(self) -> None:
        self.data = self.compile(ALL_VERBS)

    @staticmethod
    def fix(b: bytearray) -> bytes:
        struct.pack_into(">I", b, 16, zlib.crc32(bytes(b[rbc.HEADER_SIZE:])) & 0xFFFFFFFF)
        return bytes(b)

    def assertRejects(self, data: bytes, rule: str = "", table=True) -> None:
        with self.assertRaisesRegex(rbc.RbcError, rule):
            rbc.read(data, self.table if table else None)

    def op_at(self, index: int) -> int:
        p = rbc.HEADER_SIZE
        for _ in range(index):
            p += 4 + 4 * len(rbc.OPS[self.data[p]][1])
        return p

    def patch(self, index: int, fmt: str, rel: int, value) -> bytes:
        b = bytearray(self.data)
        struct.pack_into(fmt, b, self.op_at(index) + rel, value)
        return self.fix(b)

    def test_rules(self) -> None:
        d = self.data
        self.assertRejects(d[:20], "shorter")
        self.assertRejects(b"RBC2" + d[4:], "magic")
        self.assertRejects(d + b"\0", "file_size")
        b = bytearray(d)
        b[-2] ^= 0x40
        self.assertRejects(bytes(b), "CRC")
        b = bytearray(d)
        struct.pack_into(">I", b, 8, self.table.build_id ^ 1)
        self.assertRejects(bytes(b), "compiled against rtb")
        rbc.read(bytes(b), None)                  # structure alone is fine
        self.assertRejects(self.patch(0, ">B", 0, 0x7F), "unknown opcode")
        self.assertRejects(self.patch(1, ">H", 2, len(self.table.map_names)), "map")
        self.assertRejects(self.patch(10, ">H", 2, len(self.table.targets)), "target")
        self.assertRejects(self.patch(9, ">I", 4, len(self.table.class_names)), "class")
        self.assertRejects(self.patch(13, ">B", 1, 3), "bad a operand")
        self.assertRejects(self.patch(2, ">f", 16, -1.0), "bad float")
        self.assertRejects(self.patch(2, ">I", 4, 0x7FC00000), "bad float")      # NaN
        self.assertRejects(self.patch(16, ">I", 4, 1), "not in map")
        self.assertRejects(self.patch(0, ">I", 4, rbc.BUTTONS["duck"]), "BUTTONS")
        self.assertRejects(self.patch(4, ">H", 2, 3), "string start")
        self.assertRejects(self.patch(8, ">H", 2, 1), "b must be 0")
        self.assertRejects(self.patch(len(rbc.read(d).ops) - 1, ">B", 0, rbc.OPCODE["GOD"]),
                           "last op is not PASS")
        self.assertRejects(self.patch(3, ">B", 0, rbc.OPCODE["PASS"]), "PASS before")

    def test_fuzz(self) -> None:
        rng = random.Random(4321)
        for n in range(len(self.data)):
            self.assertRejects(self.data[:n])
        for _ in range(3000):
            b = bytearray(self.data)
            for _ in range(rng.randint(1, 4)):
                b[rng.randrange(len(b))] = rng.randrange(256)
            if rng.random() < 0.8:
                self.fix(b)
            for table in (self.table, None):
                try:
                    rbc.read(bytes(b), table)
                except rbc.RbcError:
                    pass


class CommittedRoutesTest(unittest.TestCase):
    """Every committed route compiles: verbs, arguments and the map-context
    rules are checked against a table holding the names the route uses
    (real names are checked by `make routes` against the cooked rtb)."""

    def test_routes(self) -> None:
        paths = sorted(glob.glob(os.path.join(ROOT, "test", "routes", "*.route")))
        self.assertTrue(paths)
        for path in paths:
            with self.subTest(route=os.path.basename(path)):
                with open(path, encoding="utf-8") as f:
                    text = f.read()
                table = self.table_for(text)
                ops = routec.compile_route(text, os.path.basename(path), table)
                prog = rbc.read(rbc.write(table.build_id, os.path.basename(path), ops), table)
                self.assertEqual(rbc.OPS[prog.ops[-1].code][0], "PASS")

    @staticmethod
    def table_for(text: str) -> rtb.Table:
        t = rtb.Table()
        cur = None
        targets = {}
        classes = []
        for raw in text.splitlines():
            tok = raw.split("#", 1)[0].split()
            if not tok:
                continue
            v, a = tok[0], tok[1:]
            if v == "waitmap" and a[0] not in t.map_names:
                t.map_names.append(a[0])
            if v == "waitmap":
                cur = t.map_names.index(a[0])
            elif v in ("waitmoved", "waitgone") or (v == "lookat" and not re.match(
                    r"(monster|weapon|item|ammo)_", a[0])):
                targets.setdefault((cur, a[0]), [len(targets) + 1])
            elif v in ("waitclass", "drop", "lookat") and a[0] not in classes:
                classes.append(a[0])
            elif v == "waitglobal" and a[0] not in t.global_names:
                t.global_names.append(a[0])
        t.files = [rtb.FileEntry(f"maps/{m}.w64", 0, 0) for m in t.map_names]
        t.map_files = list(range(len(t.map_names)))
        for (m, name), lumps in sorted(targets.items()):
            t.targets.append((m, lumps, 0))
            t.target_names.append(name)
        t.class_names = classes
        return rtb.read(rtb.write(t))


if __name__ == "__main__":
    unittest.main()
