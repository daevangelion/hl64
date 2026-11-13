# SPDX-License-Identifier: MIT
"""hleids (the HLE1 id step) unit tests on synthetic maps: a cooked
chunk gets classes.def / keys.def ids and typed key ids (models, submodels,
sounds, sentences, speaker groups, patterns, global states, titles,
classname references), the step is idempotent and touches nothing but the
id fields and the HLE_F_IDS flag, unknown names stop it with the record,
and keys.def is parsed strictly; the HLE region proof (engine/ent/rec.c's
bytes, fails above HL64_HLE_BLOBS_KB), also on the cooked campaign when
HL64_CONTENT is set. Uses hlecook (HL64_HLECOOK or a build into a
temporary directory).

  [HL64_CONTENT=DIR] python3 -m unittest tools.w64c.test_hleids
"""
from __future__ import annotations

import contextlib
import os
import re
import shutil
import struct
import subprocess
import tempfile
import unittest

from tools.cook import decals
from tools.restab import restab, rtb
from tools.w64c import hleids
from tools.w64c.test_hlecook import CFLAGS, HERE, bsp

ROOT = os.path.dirname(os.path.dirname(HERE))

ENTS = (b'{\n"classname" "worldspawn"\n"chaptertitle" "T0A0TITLE"\n}\n'
        b'{\n"classname" "info_player_start"\n"origin" "0 0 40"\n}\n'
        b'{\n"classname" "func_door"\n"model" "*1"\n"targetname" "door"\n}\n'
        b'{\n"classname" "monster_scientist"\n"model" "models/scientist.mdl"\n'
        b'"origin" "8 0 0"\n}\n'
        b'{\n"classname" "ambient_generic"\n"message" "ambience/hum.wav"\n'
        b'"targetname" "hum"\n}\n'
        b'{\n"classname" "speaker"\n"message" "FAR_WAR"\n"targetname" "spk"\n}\n'
        b'{\n"classname" "scripted_sentence"\n"sentence" "!SC_HELLO"\n'
        b'"targetname" "ss"\n}\n'
        b'{\n"classname" "light"\n"pattern" "azza"\n"targetname" "lamp"\n}\n'
        b'{\n"classname" "env_global"\n"globalstate" "dish"\n"targetname" "g"\n}\n'
        b'{\n"classname" "env_message"\n"message" "DEMOTXT1"\n"targetname" "msg"\n}\n'
        b'{\n"classname" "monstermaker"\n"monstertype" "monster_barney"\n'
        b'"targetname" "mk"\n}\n'
        b'{\n"classname" "scripted_sequence"\n"m_iszEntity" "monster_barney"\n'
        b'"targetname" "seq"\n}\n'
        b'{\n"classname" "infodecal"\n"texture" "{BIGSHOT2"\n"origin" "1 2 3"\n}\n'
        b'{\n"classname" "infodecal"\n"texture" "{oil1"\n"targetname" "d"\n'
        b'"origin" "4 5 6"\n}\n'
        b'{\n"classname" "infodecal"\n"texture" "{capsa"\n"origin" "7 8 9"\n}\n'
        b'{\n"classname" "infodecal"\n"texture" "{nosuch"\n"origin" "0 0 1"\n}\n'
        b'{\n"classname" "env_glow"\n"model" "sprites/glow01.spr"\n"origin" "0 8 0"\n'
        b'"rendermode" "3"\n"renderamt" "128"\n"rendercolor" "255 200 100"\n'
        b'"renderfx" "14"\n"scale" "0.5"\n"framerate" "10"\n}\n\0')


def table() -> rtb.Table:
    t = rtb.Table()
    t.class_names = ["worldspawn", "info_player_start", "func_door", "monster_scientist",
                     "ambient_generic", "speaker", "scripted_sentence", "light",
                     "env_global", "env_message", "monstermaker", "scripted_sequence",
                     "infodecal", "env_glow", "monster_barney"]
    t.res = [rtb.ResEntry(rtb.KIND_MODEL, 0, 0, 0, 1), rtb.ResEntry(rtb.KIND_SPRITE, 0, 3, 0, 1),
             rtb.ResEntry(rtb.KIND_SOUND, 0, 1, 0, 1), rtb.ResEntry(rtb.KIND_SOUND, 0, 2, 0, 1)]
    t.res_names = ["models/scientist.mdl", "sprites/glow01.spr", "ambience/hum.wav",
                   "vox/hello.wav"]
    t.sentences = [(0, 1), (0, 1), (0, 1)]
    t.sentence_names = ["FAR_WAR0", "FAR_WAR1", "SC_HELLO"]
    t.groups = [(0, 2), (2, 1)]
    t.group_names = ["FAR_WAR", "SC_HELLO"]
    t.patterns = [bytes([12]), bytes([0, 25, 25, 0])]
    t.global_names = ["dish"]
    t.titles = [rtb.Title(text=b"x"), rtb.Title(text=b"y")]
    t.title_names = ["DEMOTXT1", "T0A0TITLE"]
    return t


def keys_def() -> list:
    return hleids.read_keys_def(os.path.join(ROOT, "game", "defs"))


def a16(n: int) -> int:
    return (n + 15) & ~15


class HleIds(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.tmp = tempfile.mkdtemp(prefix="test_hleids.")
        exe = os.environ.get("HL64_HLECOOK")
        if not exe:
            exe = os.path.join(cls.tmp, "hlecook")
            cc = os.environ.get("CC") or shutil.which("cc") or "gcc"
            subprocess.run([cc, *CFLAGS, "-o", exe, os.path.join(HERE, "hlecook.c"), "-lm"],
                           check=True)
        models = struct.pack("<9f4i3i", -256, -256, -256, 256, 256, 256, 0, 0, 0, 0, 0, 1, 2, 0, 0, 0)
        models += struct.pack("<9f4i3i", -8, -8, 0, 8, 8, 64, 0, 0, 0, 0, 0, 1, 2, 0, 0, 0)
        src = os.path.join(cls.tmp, "m.bsp")
        dst = os.path.join(cls.tmp, "m.hle")
        with open(src, "wb") as f:
            f.write(bsp(ents=ENTS, models=models))
        subprocess.run([exe, src, dst], check=True, capture_output=True)
        with open(dst, "rb") as f:
            cls.blob = f.read()
        cls.ids = hleids.Ids.from_table(table(), keys_def())

    @classmethod
    def tearDownClass(cls) -> None:
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def parse(self, blob: bytes):
        """{(lump, key): (id, kind)}, [class id], [keyname id], flags."""
        (numrecs, numclasses, numkeynames) = struct.unpack_from("<HHH", blob, 18)
        o_recs, _l, o_cls, o_kn = struct.unpack_from("<4I", blob, 52)
        o_keys, o_str = struct.unpack_from("<I", blob, 76)[0], struct.unpack_from("<I", blob, 92)[0]

        def s(o):
            return blob[o_str + o:blob.index(b"\0", o_str + o)].decode()
        kn = [(s(struct.unpack_from("<I", blob, o_kn + 8 * i)[0]),
               struct.unpack_from("<HH", blob, o_kn + 8 * i + 4)) for i in range(numkeynames)]
        out = {}
        for r in range(numrecs):
            lump, _c, _t, _f, kf, kc = struct.unpack_from("<6H", blob, o_recs + 24 * r)
            for k in range(kf, kf + kc):
                n, tid, _v = struct.unpack_from("<HHI", blob, o_keys + 8 * k)
                out[(lump, kn[n][0])] = tid
        cls = [struct.unpack_from("<H", blob, o_cls + 8 * i + 6)[0] for i in range(numclasses)]
        return out, cls, kn, struct.unpack_from("<H", blob, 6)[0]

    def test_ids(self) -> None:
        out = hleids.apply(self.blob, self.ids, "m")
        keys, cls, kn, flags = self.parse(out)
        self.assertTrue(flags & hleids.HLE_F_IDS)
        kd = keys_def()
        for name, (_kind, kid) in kn:
            self.assertEqual(kd[kid], name)
        self.assertEqual(sorted(cls), list(range(len(cls))))  # every class is in the table
        self.assertEqual(keys[(2, "model")], rtb.res_t(rtb.KIND_SUBMODEL, 1))
        self.assertEqual(keys[(3, "model")], rtb.res_t(rtb.KIND_MODEL, 0))
        self.assertEqual(keys[(4, "message")], rtb.res_t(rtb.KIND_SOUND, 0))
        self.assertEqual(keys[(5, "message")], rtb.res_t(rtb.KIND_GROUP, 0))
        self.assertEqual(keys[(6, "sentence")], rtb.res_t(rtb.KIND_SENTENCE, 2))
        self.assertEqual(keys[(7, "pattern")], 1)
        self.assertEqual(keys[(8, "globalstate")], 0)
        self.assertEqual(keys[(9, "message")], 0)        # title DEMOTXT1
        self.assertEqual(keys[(0, "chaptertitle")], 1)   # title T0A0TITLE
        self.assertEqual(keys[(10, "monstertype")], 14)  # monster_barney
        self.assertEqual(keys[(11, "m_iszEntity")], 14)
        self.assertEqual(keys[(1, "origin")], hleids.NONE)
        # the env_glow keeps HLSDK CGlow's keys; its sprite gets its res_t
        self.assertEqual(keys[(16, "model")], rtb.res_t(rtb.KIND_SPRITE, 0))
        for k in ("origin", "rendermode", "renderamt", "rendercolor", "renderfx", "scale",
                  "framerate"):
            self.assertEqual(keys[(16, k)], hleids.NONE)
        # infodecal textures without the map's DECL names: global ones only
        self.assertEqual(keys[(12, "texture")], decals.DECAL_NAMES.index("{bigshot2"))
        self.assertEqual(keys[(13, "texture")], hleids.NONE)
        self.assertEqual(keys[(12, "origin")], hleids.NONE)

    def test_decal_ids(self) -> None:
        """infodecal `texture`: the global sheet's entry, else 42 + the
        entry of the map's DECL sheet (sorted textures decals.wad has);
        a texture decals.wad lacks gets no id (HLSDK CDecal removes it)"""
        tex = hleids.decal_textures(self.blob, "m")
        self.assertEqual(tex, ["{bigshot2", "{oil1", "{capsa", "{nosuch"])
        local, absent = hleids.map_decals(tex, {"{bigshot2", "{oil1", "{capsa", "{shot1"})
        self.assertEqual((local, absent), (["{capsa", "{oil1"], ["{nosuch"]))
        keys, _c, _kn, _f = self.parse(hleids.apply(self.blob, self.ids.for_map(local), "m"))
        self.assertEqual(keys[(12, "texture")], decals.DECAL_NAMES.index("{bigshot2"))
        self.assertEqual(keys[(13, "texture")], decals.GLOBAL_COUNT + 1)
        self.assertEqual(keys[(14, "texture")], decals.GLOBAL_COUNT + 0)
        self.assertEqual(keys[(15, "texture")], hleids.NONE)
        # infodecal with and without targetname are both kept (HLSDK CDecal)
        self.assertIn((13, "texture"), keys)
        # the engine's constants (engine/ent/rec_int.h) match the cook's
        rec_int = hleids.DEFAULT_REC_INT
        self.assertEqual(restab.read_limit(rec_int, "REC_DECALS_GLOBAL"), decals.GLOBAL_COUNT)
        self.assertEqual(restab.read_limit(rec_int, "REC_DECALS_MAP_MAX"), decals.SHEET_MAX)
        with open(rec_int) as f:
            src = f.read()
        self.assertIn(f"REC_DECL_FOURCC\t\t0x{int.from_bytes(decals.DECL, 'big'):08X}u", src)
        self.assertIn(f"REC_DECL_MAGIC\t\t0x{decals.SHEET_MAGIC:08X}u", src)

    def test_only_id_fields_change_and_idempotent(self) -> None:
        out = hleids.apply(self.blob, self.ids, "m")
        self.assertEqual(hleids.apply(out, self.ids, "m"), out)
        self.assertEqual(len(out), len(self.blob))
        (numclasses, numkeynames) = struct.unpack_from("<HH", self.blob, 20)
        numkeys = struct.unpack_from("<I", self.blob, 28)[0]
        o_cls, o_kn = struct.unpack_from("<2I", self.blob, 60)
        o_keys = struct.unpack_from("<I", self.blob, 76)[0]
        allowed = {6, 7}
        allowed |= {o_cls + 8 * i + j for i in range(numclasses) for j in (6, 7)}
        allowed |= {o_kn + 8 * i + j for i in range(numkeynames) for j in (6, 7)}
        allowed |= {o_keys + 8 * i + j for i in range(numkeys) for j in (2, 3)}
        diff = {i for i in range(len(out)) if out[i] != self.blob[i]}
        self.assertLessEqual(diff, allowed)

    def test_unknown_names_stop(self) -> None:
        for strip, what in ((lambda t: t.res_names.__setitem__(0, "models/x.mdl"), "MODEL"),
                            (lambda t: t.global_names.clear(), "global state"),
                            (lambda t: t.patterns.pop(), "pattern")):
            t = table()
            strip(t)
            with self.assertRaises(hleids.IdError) as cm:
                hleids.apply(self.blob, hleids.Ids.from_table(t, keys_def()), "m")
            self.assertIn("m:", str(cm.exception))
            self.assertIn(what, str(cm.exception))
        ids = hleids.Ids.from_table(table(), [k for k in keys_def() if k != "pattern"])
        with self.assertRaises(hleids.IdError):
            hleids.apply(self.blob, ids, "m")

    def test_missing_title_class_sentence_sound_are_none(self) -> None:
        """titles.txt lacks it / the game lacks the class / restab dropped
        a sentence or group whose sounds are all missing / the player's data
        lacks a keyed sound (restab tables it absent): no id"""
        t = table()
        t.group_names[0] = "X"
        t.sentence_names[2] = "X"
        t.titles, t.title_names = [], []
        t.class_names = t.class_names[:-1]
        t.res_names[2] = "ambience/x.wav"
        keys, cls, _kn, _f = self.parse(hleids.apply(self.blob, hleids.Ids.from_table(t, keys_def()), "m"))
        self.assertEqual(keys[(9, "message")], hleids.NONE)
        self.assertEqual(keys[(10, "monstertype")], hleids.NONE)
        self.assertEqual(keys[(5, "message")], hleids.NONE)
        self.assertEqual(keys[(6, "sentence")], hleids.NONE)
        self.assertEqual(keys[(4, "message")], hleids.NONE)

    def test_malformed_chunks(self) -> None:
        for bad in (b"", self.blob[:50], b"HLX1" + self.blob[4:],
                    self.blob[:8] + struct.pack("<I", len(self.blob) + 1) + self.blob[12:]):
            with self.assertRaises(hleids.IdError):
                hleids.apply(bad, self.ids, "m")

    def test_keys_def_strict(self) -> None:
        d = os.path.join(self.tmp, "defs")
        os.makedirs(d, exist_ok=True)
        for text in ('KEY(A, "a")\nKEY(B, "a")\n', 'KEY(a b)\n', 'KEY(A, "a") junk\n'):
            with open(os.path.join(d, "keys.def"), "w") as f:
                f.write(text)
            with self.assertRaises(hleids.IdError):
                hleids.read_keys_def(d)

    def test_keys_def_covers_the_cook(self) -> None:
        """every key the cook keeps has a keys.def id, in cook table order"""
        import re
        with open(os.path.join(HERE, "hl_entcook.h")) as f:
            src = f.read()
        body = src[src.index("hle_fields[] = {"):src.index("hle_mm_fields")]
        names = re.findall(r'\{"([^"]+)",HLE_K[FVSN]\}', body)
        self.assertEqual(names, keys_def()[:len(names)])
        self.assertEqual(len(names), len(keys_def()))

    def limits(self, name: str, kb: int) -> str:
        """include/hl64/limits.h with one KB define changed"""
        with open(os.path.join(ROOT, "include", "hl64", "limits.h")) as f:
            text = f.read()
        text, n = re.subn(r"^(#define\s+" + name + r"\s+)\d+", lambda m: m.group(1) + str(kb),
                          text, flags=re.M)
        self.assertEqual(n, 1)
        path = os.path.join(self.tmp, f"limits-{name}-{kb}.h")
        with open(path, "w") as f:
            f.write(text)
        return path

    def test_hle_region_accounting(self) -> None:
        """open_chunk's bytes for the synthetic chunk: need bounds logic and
        pending space by the records, the open block keeps logic blocks of
        the logic records only"""
        rt, logic, tables = hleids.accounting(hleids.DEFAULT_REC_INT)
        self.assertEqual((rt, logic), (24, 16))
        blob = self.blob + bytes(-len(self.blob) % 16)
        n = struct.unpack_from("<H", blob, 18)[0]
        o = struct.unpack_from("<I", blob, 52)[0]
        nlog = [struct.unpack_from("<H", blob, o + 24 * r + 6)[0] & 1 for r in range(n)].count(1)
        self.assertGreater(nlog, 0)
        s = hleids.slot_bytes("m", blob, rt, logic)
        self.assertEqual(s.need, len(blob) + 42 * n + 16)
        self.assertEqual(s.block, ((len(blob) + 24 * n + 16 * nlog + 3) & ~3) + 2 * n)
        self.assertEqual(s.runtime, s.block - len(blob))
        empty = blob[:18] + bytes(2) + blob[20:96] + bytes(16)                # no records
        small = hleids.slot_bytes("s", empty, rt, logic)
        self.assertEqual((small.need, small.block), (112 + 16, 112))
        r = hleids.hle_region([small, s], tables)
        self.assertIs(r.opening, s)
        self.assertEqual(r.total, tables + a16(s.need) + a16(s.block) + 2 * a16(s.runtime))
        for bad in (b"", blob[:50], b"HLX1" + blob[4:], blob[:52] + struct.pack("<I", len(blob)) + blob[56:]):
            with self.assertRaises(hleids.IdError):
                hleids.slot_bytes("m", bad, rt, logic)

    def test_hle_region_carve(self) -> None:
        """positive control: the largest whole-KB carve below the need fails
        the cook, the need rounded up to KB passes, and so does limits.h's"""
        rt, logic, tables = hleids.accounting(hleids.DEFAULT_REC_INT)
        r = hleids.hle_region([hleids.slot_bytes("m", self.blob, rt, logic)], tables)
        with self.assertRaisesRegex(hleids.IdError, r"HLE region need \d+ B .* exceeds HL64_HLE_BLOBS_KB"):
            hleids.check_region(r, self.limits("HL64_HLE_BLOBS_KB", (r.total - 1) // 1024))
        self.assertEqual(hleids.check_region(r, self.limits("HL64_HLE_BLOBS_KB",
                                                            (r.total + 1023) // 1024)),
                         (r.total + 1023) // 1024 * 1024)
        hleids.check_region(r, os.path.join(ROOT, "include", "hl64", "limits.h"))

    def test_hle_region_real_cook(self) -> None:
        """the cooked campaign (HL64_CONTENT) fits limits.h's HLE and decal
        carves, and the largest whole-KB carve below either need fails
        --check"""
        content = os.environ.get("HL64_CONTENT")
        if not content:
            self.skipTest("HL64_CONTENT not set")
        defs = os.path.join(ROOT, "game", "defs")
        fails, r, d = hleids.check(content, defs)
        self.assertEqual(fails, [])
        assert r is not None and d is not None
        for name, need, what in (("HL64_HLE_BLOBS_KB", r.total, "HLE region"),
                                 ("HL64_DECAL_TEXELS_KB", d.total, "decal region")):
            tight = self.limits(name, (need - 1) // 1024)
            fails, _r, _d = hleids.check(content, defs, tight)
            self.assertEqual(len(fails), 1)
            self.assertIn(f"{what} need {need} B", fails[0])
            self.assertIn(f"exceeds {name}", fails[0])
            with open(os.devnull, "w") as null, contextlib.redirect_stderr(null), \
                    contextlib.redirect_stdout(null):
                self.assertEqual(hleids.main(["--check", content, "--defs", defs,
                                              "--limits", tight]), 1)


if __name__ == "__main__":
    unittest.main()
