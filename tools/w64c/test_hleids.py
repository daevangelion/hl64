# SPDX-License-Identifier: MIT
"""hleids (the HLE1 id step) unit tests on synthetic maps only: a cooked
chunk gets classes.def / keys.def ids and typed key ids (models, submodels,
sounds, sentences, speaker groups, patterns, global states, titles,
classname references), the step is idempotent and touches nothing but the
id fields and the HLE_F_IDS flag, unknown names stop it with the record,
and keys.def is parsed strictly. Uses hlecook (HL64_HLECOOK or a build
into a temporary directory).

  python3 -m unittest tools.w64c.test_hleids
"""
from __future__ import annotations

import os
import shutil
import struct
import subprocess
import tempfile
import unittest

from tools.restab import rtb
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
        b'"targetname" "seq"\n}\n\0')


def table() -> rtb.Table:
    t = rtb.Table()
    t.class_names = ["worldspawn", "info_player_start", "func_door", "monster_scientist",
                     "ambient_generic", "speaker", "scripted_sentence", "light",
                     "env_global", "env_message", "monstermaker", "scripted_sequence",
                     "monster_barney"]
    t.res = [rtb.ResEntry(rtb.KIND_MODEL, 0, 0, 0, 1), rtb.ResEntry(rtb.KIND_SOUND, 0, 1, 0, 1),
             rtb.ResEntry(rtb.KIND_SOUND, 0, 2, 0, 1)]
    t.res_names = ["models/scientist.mdl", "ambience/hum.wav", "vox/hello.wav"]
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
        self.assertEqual(keys[(10, "monstertype")], 12)  # monster_barney
        self.assertEqual(keys[(11, "m_iszEntity")], 12)
        self.assertEqual(keys[(1, "origin")], hleids.NONE)

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
                            (lambda t: t.patterns.pop(), "pattern"),
                            (lambda t: t.group_names.__setitem__(0, "X"), "group"),
                            (lambda t: t.sentence_names.__setitem__(2, "X"), "sentence")):
            t = table()
            strip(t)
            with self.assertRaises(hleids.IdError) as cm:
                hleids.apply(self.blob, hleids.Ids.from_table(t, keys_def()), "m")
            self.assertIn("m:", str(cm.exception))
            self.assertIn(what, str(cm.exception))
        ids = hleids.Ids.from_table(table(), [k for k in keys_def() if k != "pattern"])
        with self.assertRaises(hleids.IdError):
            hleids.apply(self.blob, ids, "m")

    def test_missing_title_and_class_are_none(self) -> None:
        t = table()
        t.titles, t.title_names = [], []
        t.class_names = t.class_names[:-1]
        keys, cls, _kn, _f = self.parse(hleids.apply(self.blob, hleids.Ids.from_table(t, keys_def()), "m"))
        self.assertEqual(keys[(9, "message")], hleids.NONE)
        self.assertEqual(keys[(10, "monstertype")], hleids.NONE)

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


if __name__ == "__main__":
    unittest.main()
