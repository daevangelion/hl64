# SPDX-License-Identifier: MIT
"""restab / rtb unit tests: cook, determinism, round trip, bad input
rejection (cook inputs and rtb bytes). Run from the repository root:

  python3 -m unittest tools.restab.test_restab
"""
from __future__ import annotations

import os
import random
import struct
import tempfile
import unittest
import zlib

from . import restab, rtb, testdata


class Base(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.root = self.tmp.name

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def cook(self, files=None, defs=None, maps=None) -> rtb.Table:
        content, defs_dir = testdata.build(tempfile.mkdtemp(dir=self.root), files, defs, maps)
        return restab.cook(content, defs_dir)

    def files_with(self, **changes) -> dict:
        f = dict(testdata.FILES)
        for k, v in changes.items():
            rel = k.replace("__", "/").replace("_DOT_", ".")
            if v is None:
                f.pop(rel)
            else:
                f[rel] = v
        return f


class CookTest(Base):
    def test_tables(self) -> None:
        t = self.cook()
        self.assertEqual(t.map_names, ["mapa", "mapb"])
        self.assertEqual([t.files[i].path for i in t.map_files], ["maps/mapa.w64", "maps/mapb.w64"])
        models = [n for r, n in zip(t.res, t.res_names) if r.kind == rtb.KIND_MODEL]
        self.assertEqual(models, ["models/bar.mdl", "models/gun.mdl"])
        bar = t.res[0]
        self.assertEqual(bar.flags, rtb.RES_F_EXTTEX | 1 << rtb.RES_F_SEQ_SHIFT)
        self.assertEqual([t.files[bar.file + k].path for k in range(3)],
                         ["models/bar.mdl", "models/bart.mdl", "models/bar01.mdl"])
        sprite = t.res[t.kind_first(rtb.KIND_SPRITE)]
        self.assertEqual(t.files[sprite.file].path, "sprites/glow.sc4")
        sounds = [n for r, n in zip(t.res, t.res_names) if r.kind == rtb.KIND_SOUND]
        self.assertEqual(sounds, sorted(sounds))
        self.assertEqual(sounds, ["doors/doormove1.wav", "hgrunt/alert!.wav", "hgrunt/clik.wav",
                                  "vox/_comma.wav", "vox/_period.wav", "vox/hello.wav",
                                  "weapons/click.wav"])
        for r in t.res:
            self.assertEqual(r.size, t.files[r.file].size)
        # sentences: referenced + group members, file order; UNUSED0 dropped
        self.assertEqual(t.sentence_names, ["SC_HELLO0", "SC_HELLO1", "HG_ALERT0"])
        self.assertEqual(t.group_names, ["SC_HELLO", "HG_ALERT"])
        self.assertEqual(t.groups, [(0, 2), (2, 1)])
        f0, c0 = t.sentences[0]
        w = t.words[f0:f0 + c0]
        snd = lambda name: t.res_of(rtb.KIND_SOUND, name)
        self.assertEqual([x.res for x in w], [snd("vox/hello.wav"), snd("vox/_comma.wav"),
                                              snd("vox/hello.wav"), snd("vox/_period.wav")])
        self.assertEqual([(x.pitch, x.volume, x.end) for x in w],
                         [(110, 100, 100), (110, 100, 100), (110, 80, 50), (110, 100, 100)])
        f2, _ = t.sentences[2]
        self.assertEqual((t.words[f2].res, t.words[f2].time), (snd("hgrunt/alert!.wav"), 20))
        # patterns: definition lines first, then new map patterns sorted
        self.assertEqual([bytes(97 + c for c in p).decode() for p in t.patterns],
                         ["m", "mmnmmommommnonmmonqnmmo", "a", "aazz", "mmnn"])
        # targets per map, sorted by name, lumps ascending; a monstermaker's
        # netname names its children (spawner count), other netnames do not
        self.assertEqual(list(zip(t.target_names, t.targets)),
                         [("door1", (0, [1, 3], 0)), ("lamp", (0, [5], 0)), ("sci", (0, [2], 0)),
                          ("door1", (1, [1], 0)), ("garg", (1, [4], 1)), ("maker", (1, [4], 0))])
        self.assertEqual(t.target_id(1, "door1"), 3)
        self.assertIsNone(t.target_id(1, "sci"))
        self.assertIsNone(t.target_id(1, "notachild"))
        self.assertEqual(t.class_names, ["worldspawn", "monster_scientist", "weapon_gun"])
        self.assertEqual(t.global_names, ["dish_on"])
        self.assertEqual(t.defs, [rtb.res_t(rtb.KIND_MODEL, 1), snd("weapons/click.wav"),
                                  rtb.res_t(rtb.KIND_GROUP, 1), rtb.res_t(rtb.KIND_SENTENCE, 1)])

    def test_round_trip_and_determinism(self) -> None:
        t = self.cook()
        data = rtb.write(t)
        back = rtb.read(data)
        for fld in ("cwg_crc", "files", "map_names", "map_files", "res", "res_names",
                    "sentences", "sentence_names", "words", "groups", "group_names",
                    "patterns", "targets", "target_names", "defs", "class_names",
                    "global_names"):
            self.assertEqual(getattr(back, fld), getattr(t, fld), fld)
        self.assertEqual(back.build_id, zlib.crc32(data[rtb.HEADER_SIZE:]) & 0xFFFFFFFF)
        # same inputs written elsewhere give the same bytes
        self.assertEqual(rtb.write(self.cook()), data)

    def test_cli(self) -> None:
        content, defs = testdata.build(os.path.join(self.root, "c"))
        out = os.path.join(self.root, "campaign.rtb")
        self.assertEqual(restab.main(["--content", content, "--defs", defs, "-o", out]), 0)
        with open(out, "rb") as f:
            rtb.read(f.read())
        os.remove(os.path.join(content, "models/gun.mdl"))
        self.assertEqual(restab.main(["--content", content, "--defs", defs, "-o", out]), 1)

    def assertCookFails(self, pattern: str, **kw) -> None:
        with self.assertRaisesRegex(restab.CookError, pattern):
            self.cook(**kw)

    def test_unresolved_names(self) -> None:
        self.assertCookFails(r"mapa:2 model: no file models/bar\.mdl",
                             files=self.files_with(models__bar_DOT_mdl=None))
        self.assertCookFails(r"mapa:3 model: no file sprites/glow\.sc4",
                             files=self.files_with(sprites__glow_DOT_sc4=None))
        self.assertCookFails(r"res\.def:2: no file sound/weapons/click\.wav64",
                             files=self.files_with(sound__weapons__click_DOT_wav64=None))
        # a sentence word without its sample is dropped with a warning
        # (test_missing_word_sounds), not an error
        self.assertCookFails(r"no file maps/mapb\.w64",
                             files=self.files_with(maps__mapb_DOT_w64=None))
        self.assertCookFails(r"no file maps/mapb\.ent",
                             files=self.files_with(maps__mapb_DOT_ent=None))
        self.assertCookFails(r"mapa:4 message: sentence SC_NOPE",
                             files=self.files_with(maps__mapa_DOT_ent=testdata.ENTS_A.replace(
                                 b"SC_HELLO0", b"SC_NOPE")))
        defs = dict(testdata.DEFS)
        defs["res.def"] += b'RES(GROUP, NOPE, "NOPE")\n'
        self.assertCookFails(r"res\.def:5: sentence group NOPE", defs=defs)

    def test_bad_entity_text(self) -> None:
        for text, rule in ((b'{\n"classname" "x\n}\n', "unterminated string"),
                           (b'{\n{\n}\n', "nested"),
                           (b'}\n', "unexpected '}'"),
                           (b'{\n"key"\n}\n', "unexpected '}'"),
                           (b'{\n"a" "b"\n', "missing final"),
                           (b'"a" "b"\n', "outside an entity"),
                           (b'{\nkey "b"\n}\n', "unexpected character"),
                           (b'{\n"pattern" "aBc"\n}\n', "not 1-255 letters")):
            with self.subTest(rule=rule):
                self.assertCookFails(rule, files=self.files_with(maps__mapb_DOT_ent=text),
                                     maps=["mapa", "mapb"])

    def test_bad_defs(self) -> None:
        for name, text, rule in (
                ("classes.def", b"CLASS(X, x)\n", r"classes\.def:1: not a definition"),
                ("classes.def", b'CLASS(X, "x")\nCLASS(X, "y")\n', "duplicate X"),
                ("classes.def", b'CLASS(X, "x")\nCLASS(Y, "x")\n', "duplicate classname"),
                ("res.def", b'RES(MODEL, A, "sprites/glow.spr")\n', "is not a MODEL"),
                ("res.def", b'RES(TEXTURE, A, "x")\n', "not a definition"),
                ("lightstyles.def", b'LIGHTSTYLE(64, "a")\n', "out of range"),
                ("lightstyles.def", b'LIGHTSTYLE(1, "a")\nLIGHTSTYLE(1, "b")\n', "repeated"),
                ("lightstyles.def", b'LIGHTSTYLE(1, "A")\n', "1-255 letters")):
            with self.subTest(rule=rule):
                defs = dict(testdata.DEFS)
                defs[name] = text
                self.assertCookFails(rule, defs=defs)

    def test_bad_sentences(self) -> None:
        for line, rule in ((b"X0 vox/hello(q5)\n", "bad modifier"),
                           (b"X0 vox/hello(p5\n", "unterminated"),
                           (b"X0\n", "bad sentence line"),
                           (b"X0 vox/(p5)\n", "has no words"),
                           (b"X0 bad$dir/hello\n", "bad directory"),
                           (b"X0 vox/hel@lo\n", "bad word")):
            with self.subTest(rule=rule):
                self.assertCookFails(rule, files=self.files_with(
                    sound__sentences_DOT_txt=testdata.SENTENCES + line))
        self.assertCookFails("modifier out of range", files=self.files_with(
            sound__sentences_DOT_txt=testdata.SENTENCES.replace(b"(t20)", b"(s150)")))

    def test_bad_cwg(self) -> None:
        good = testdata.make_cwg(["mapa", "mapb"])
        for data, rule in ((good[:-1], "size"), (b"CWG2" + good[4:], "not CWG3"),
                           (good[:20], "shorter"),
                           (good[:32] + struct.pack(">H", 900) + good[34:], "outside the string"),
                           (testdata.make_cwg(["mapa", "mapa"]), "duplicate map"),
                           (testdata.make_cwg(["ma p"]), "bad name")):
            with self.subTest(rule=rule):
                with self.assertRaisesRegex(restab.CookError, rule):
                    restab.read_cwg_maps(data)

    def test_case_ambiguity(self) -> None:
        self.assertCookFails("differing in case", files=self.files_with(
            models__Bar_DOT_mdl=b"IDST"))

    def test_companion_conflict(self) -> None:
        ents = testdata.ENTS_A + b'{\n"classname" "x"\n"model" "models/bart.mdl"\n}\n'
        self.assertCookFails("both a model", files=self.files_with(maps__mapa_DOT_ent=ents))

    TITLES = (b"// titles\n$position -1 0.7\n$effect 2\n$color 100 100 100\n"
              b"$color2 240 110 0 200\n$fadein 0.02\n$fadeout 1.5\n$holdtime 3.5\n"
              b"$fxtime 0.25\nDEMOTXT1\n{\nSUBJECT:\nGORDON\n}\n$effect 0\n"
              b"T0A0TITLE\n{\nHAZARD COURSE\n}\ndemotxt1\n{\nduplicate\n}\n")

    def test_titles(self) -> None:
        t = self.cook(files=self.files_with(titles_DOT_txt=self.TITLES))
        self.assertEqual(t.title_names, ["DEMOTXT1", "T0A0TITLE"])
        a, b = t.titles
        self.assertEqual((a.effect, a.color1, a.color2, a.x, a.y, a.text),
                         (2, (100, 100, 100, 0), (240, 110, 0, 200), -1.0, 0.7, b"SUBJECT:\nGORDON"))
        self.assertEqual((b.effect, b.fadeout, b.holdtime, b.text), (0, 1.5, 3.5, b"HAZARD COURSE"))
        r = rtb.read(rtb.write(t))
        self.assertEqual(r.title_names, t.title_names)
        self.assertEqual([x.text for x in r.titles], [a.text, b.text])
        self.assertAlmostEqual(r.titles[0].y, 0.7, places=6)
        self.assertEqual(r.title_id("t0a0title"), 1)
        for bad, rule in ((b"$effect 7\nX\n{\nx\n}\n", "effect"),
                          (b"$color 300 0 0\nX\n{\nx\n}\n", "colour"),
                          (b"$bogus 1\n", "directive"), (b"X\n{\nno end\n", "closing"),
                          (b"{\nx\n}\n", "without a message name"),
                          (b"$fadein nan\nX\n{\nx\n}\n", "range")):
            with self.subTest(rule=rule):
                self.assertCookFails(rule, files=self.files_with(titles_DOT_txt=bad))

    def test_speaker_group(self) -> None:
        ents = testdata.ENTS_A + b'{\n"classname" "speaker"\n"message" "HG_ALERT"\n}\n'
        defs = dict(testdata.DEFS)
        defs["res.def"] = b"RES(MODEL, GUN, \"models/gun.mdl\")\n"
        t = self.cook(files=self.files_with(maps__mapa_DOT_ent=ents), defs=defs)
        self.assertIn("HG_ALERT", t.group_names)
        ents = testdata.ENTS_A + b'{\n"classname" "speaker"\n"message" "NOPE"\n}\n'
        self.assertCookFails("NOPE", files=self.files_with(maps__mapa_DOT_ent=ents))
        ents = testdata.ENTS_A + (b'{\n"classname" "speaker"\n"message" "NOPE"\n'
                                  b'"preset" "2"\n}\n')
        self.cook(files=self.files_with(maps__mapa_DOT_ent=ents))  # preset: no group

    def test_missing_word_sounds(self) -> None:
        """GoldSrc skips a word whose sample is missing: the word goes, a
        sentence without words goes, its group skips it"""
        import contextlib
        import io
        f = self.files_with(sound__vox__hello_DOT_wav64=None)
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            t = self.cook(files=f, defs=dict(testdata.DEFS, **{
                "res.def": b"RES(GROUP, HG_ALERT, \"HG_ALERT\")\n"}))
        self.assertIn("sentences.txt:2 SC_HELLO0: word vox/hello", err.getvalue())
        self.assertEqual(t.sentence_names, ["SC_HELLO0", "HG_ALERT0"])
        f0, c0 = t.sentences[0]
        self.assertEqual([t.res_names[t.kind_first(rtb.KIND_SOUND) + rtb.res_index(w.res)]
                          for w in t.words[f0:f0 + c0]], ["vox/_comma.wav", "vox/_period.wav"])
        self.assertEqual(t.group_names, ["SC_HELLO", "HG_ALERT"])
        # a res.def sentence that lost every word cannot get an id
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            self.assertCookFails("SENTENCE SC_HELLO1: none of its words", files=f)
        self.assertIn("sentences.txt:3 SC_HELLO1: no word left", err.getvalue())

    def test_prepass_ids_equal_final(self) -> None:
        """the HLE1 id step's prepass: no campaign graph, no packages, same ids"""
        content, defs_dir = testdata.build(tempfile.mkdtemp(dir=self.root))
        final = restab.cook(content, defs_dir)
        os.remove(os.path.join(content, "maps", "campaign.cwg"))
        for m in ("mapa", "mapb"):
            os.remove(os.path.join(content, "maps", m + ".w64"))
        pre = restab.cook(content, defs_dir, ["mapb", "mapa"])
        self.assertEqual(pre.map_files, [])
        for f in ("res_names", "sentence_names", "group_names", "patterns", "global_names",
                  "class_names", "defs", "title_names"):
            self.assertEqual(getattr(pre, f), getattr(final, f), f)
        self.assertEqual([(r.kind, r.flags) for r in pre.res], [(r.kind, r.flags) for r in final.res])


class ReaderTest(Base):
    def setUp(self) -> None:
        super().setUp()
        self.data = rtb.write(self.cook())

    @staticmethod
    def refresh_crc(b: bytearray) -> bytes:
        struct.pack_into(">I", b, 8, zlib.crc32(bytes(b[rtb.HEADER_SIZE:])) & 0xFFFFFFFF)
        return bytes(b)

    def assertRejects(self, data: bytes, rule: str = "") -> None:
        with self.assertRaisesRegex(rtb.RtbError, rule):
            rtb.read(data)

    def test_header_rules(self) -> None:
        d = self.data
        self.assertRejects(d[:10], "shorter")
        self.assertRejects(b"XXXX" + d[4:], "magic")
        self.assertRejects(d[:4] + b"\0\2" + d[6:], "version")
        self.assertRejects(d + b"\0", "file_size")
        b = bytearray(d)
        b[-1] ^= 1
        self.assertRejects(bytes(b), "build_id")

    def sec(self, name: str) -> tuple:
        i = rtb.SECTION_INDEX[name]
        return struct.unpack_from(">IIII", self.data, rtb.HEADER_SIZE + 16 * i)[1:]

    def test_semantic_rules(self) -> None:
        def patch(name: str, index: int, fmt: str, field_ofs: int, value) -> bytes:
            ofs = self.sec(name)[0]
            el = dict(rtb.SECTIONS)[name]
            b = bytearray(self.data)
            struct.pack_into(fmt, b, ofs + index * el + field_ofs, value)
            return self.refresh_crc(b)
        self.assertRejects(patch("MAPS", 0, ">H", 0, 999), "MAPS")
        self.assertRejects(patch("RES_", 0, ">B", 0, rtb.KIND_SOUND), "outside its range")
        self.assertRejects(patch("RES_", 0, ">H", 2, 999), "out of range")
        self.assertRejects(patch("RES_", 0, ">I", 8, 1 << 20), "range outside file")
        self.assertRejects(patch("WORD", 0, ">H", 0, rtb.res_t(rtb.KIND_MODEL, 0)), "not a sound")
        self.assertRejects(patch("WORD", 0, ">B", 2, 0), "modifier")
        self.assertRejects(patch("SENT", 0, ">B", 2, 200), "word range")
        self.assertRejects(patch("SGRP", 1, ">H", 0, 1), "tile")
        self.assertRejects(patch("PPOL", 0, ">B", 0, 26), "letter")
        self.assertRejects(patch("PATN", 0, ">B", 2, 0), "pool range")
        self.assertRejects(patch("TARG", 0, ">H", 0, 7), "TARG")
        self.assertRejects(patch("TARG", 3, ">H", 0, 0), "duplicate target")
        self.assertRejects(patch("TARG", 4, ">H", 6, 2), "spawner range")
        self.assertRejects(patch("DEFS", 0, ">H", 0, rtb.res_t(rtb.KIND_MODEL, 99)), "DEFS")
        self.assertRejects(patch("FILE", 0, ">I", 0, 1 << 30), "outside pool")
        self.assertRejects(patch("NMAP", 1, ">I", 0, 0), "duplicate map")
        self.assertRejects(patch("KIND", rtb.KIND_SUBMODEL, ">H", 0, 1), "count-only")

    def test_layout_rules(self) -> None:
        b = bytearray(self.data)
        struct.pack_into(">I", b, rtb.HEADER_SIZE + 4, self.sec("FILE")[0] + 16)
        self.assertRejects(self.refresh_crc(b), "offset")
        b = bytearray(self.data)
        struct.pack_into(">I", b, rtb.HEADER_SIZE + 12, self.sec("FILE")[2] + 1)
        self.assertRejects(self.refresh_crc(b), "size")
        b = bytearray(self.data)
        b[rtb.HEADER_SIZE:rtb.HEADER_SIZE + 4] = b"ELIF"
        self.assertRejects(self.refresh_crc(b), "is not FILE")

    def test_fuzz(self) -> None:
        """Random corruptions (with and without a fixed-up CRC) and every
        truncation are rejected with RtbError and nothing else."""
        rng = random.Random(1234)
        for n in range(len(self.data)):
            self.assertRejects(self.data[:n])
        for _ in range(3000):
            b = bytearray(self.data)
            for _ in range(rng.randint(1, 4)):
                b[rng.randrange(len(b))] = rng.randrange(256)
            if rng.random() < 0.8:
                self.refresh_crc(b)
            try:
                rtb.read(bytes(b))
            except rtb.RtbError:
                pass


if __name__ == "__main__":
    unittest.main()
