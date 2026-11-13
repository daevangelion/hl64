# SPDX-License-Identifier: MIT
"""stage: cook the player's Half-Life data into the hl64 content directory
(`make cook`).

Imported from the quakegeneric tree (tools/hl_stage_content.py,
provenance-audit.md entry CLEAN) and retargeted: it reads the retail game
directory (loose files; pak archives also work, tools/cook/data.py), writes
a content directory that holds only cooked hl64 files (no Quake base tree
underneath, no backups) and runs the hl64 tools. The map set is every
maps/*.bsp of the data directory or the --maps globs; models, sprites and
sounds are derived from those maps' entity lumps, the class table
(tools/cook/hl_classes.py), studio model events and the game's definitions
(game/defs/res.def when it exists):

  maps/<m>.ent          the BSP entity lump text (restab input)
  models/<x>.mdl        studio models the maps and the game name, with their
                        <x>t.mdl texture and <x>NN.mdl sequence-group files
                        (raw MDL until the M3 studio bake, see STUDIO below)
  sprites/<x>.sc4       cart CI4 sprites (tools/cook/hl_sprite_cook.py) of
                        every sprite the maps and the game effects name
  gsfx/decals.i4        the decals the game shoots (cart I4)
  gsfx/palette.lmp      the player's gfx/palette.lmp (palette-index colours)
  gfx/palette.lmp       the palette the ui8 sheets index (the same file,
                        or --ref-palette)
  gfx/*.ui8             HUD sheets (tools/cook/bake_hlhud.py) and the
                        gfx.wad pictures (tools/cook/bake_ui.py)
  sound/**/*.wav64      wav64 of every referenced sample
                        (tools/cook/hl_sound_refs.py, hl_sound_classes.py)
  sound/sentences.txt   pruned to the referenced sentences
  titles.txt            pruned to the env_message titles
  maps/<m>.w64          layer packages: tools/cook/bake_world_tex.py ->
                        <staging>/maps/<m>.wci4 (embedded and WAD
                        textures), hlecook -> <m>.hle, tools/w64c/touchrank.py
                        (trigger touch ranks into the .hle),
                        tools/w64c/hleids.py (class / key / typed value ids
                        into the .hle, from the id spaces restab gives the
                        staged names), tools/w64c/w64c.py
                        (docs/formats/w64-format.md)
  maps/campaign.cwg     tools/cook/hl_campaign.py graph + tools/w64c/w64seam.py
                        seams (docs/formats/cwg-format.md)

`make cook` then runs tools/restab over the directory (campaign.rtb) and
`tools/w64c/hleids --check` (the packages' ids equal the final table's).
Intermediates (.wci4, .hle, the unproved graph, hl_hud_rects.h generated
from the player's hud.txt, converted sounds) stay in the staging directory.
Rerunning rewrites the same bytes and touches nothing unchanged; files a
previous run wrote that this run does not are removed (staging/content.lst).

--ref-palette FILE quantises world textures and ui8 sheets through FILE
instead of the player's palettes: with the old tree's palette the world
textures equal the old cook's (W1); never used for a shipped image.

  python3 -m tools.cook.stage --data DIR --content DIR [--maps GLOB ...]
        [--staging DIR] [--hlecook PATH] [--defs DIR] [--ref-palette FILE]
        [--jobs N]
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Set, Tuple

from tools.cook import bake_hlhud, bake_ui, hl_classes, hl_sound_classes, hl_sound_refs
from tools.cook import hl_sprite_cook
from tools.cook.data import Data, DataError
from tools.cook.hl_classes import BspError
from tools.restab import restab, rtb
from tools.w64c import hleids

ROOT = Path(__file__).resolve().parents[2]
PY = sys.executable

# Effect assets the game names in its temporary entities and the client
# draws (sprites: cart .sc4; client-only studio models: gibs, shells).
FX_SPRITES = ("blood", "bloodspray", "richo1", "zerogxplode", "wxplo1", "steam1")
FX_MODELS = ("shell", "shotgunshell", "hgibs", "agibs", "glassgibs",
             "woodgibs", "metalplategibs", "fleshgibs", "cindergibs",
             "ceilinggibs", "computergibs", "rockgibs")

# HUD sheets (tools/cook/bake_hlhud.py SHEETS) + hud.txt, under sprites/.
HUD_SHEETS = bake_hlhud.SHEETS

# gfx.wad pictures baked to ui8 (menu / pause / loading art and the font).
UI_LUMPS = ("lambda", "lambda64", "lambda32", "paused", "backtile", "conchars",
            "creditsfont")

# GoldSrc decals the game shoots (HLSDK weapons, TraceBleed, explosions,
# breakables, infodecal).
DECAL_NAMES = (
    [f"{{shot{i}" for i in range(1, 6)] + [f"{{bigshot{i}" for i in range(1, 6)]
    + [f"{{break{i}" for i in range(1, 4)] + [f"{{blood{i}" for i in range(1, 9)]
    + [f"{{yblood{i}" for i in range(1, 7)] + [f"{{scorch{i}" for i in range(1, 4)]
    + [f"{{smscorch{i}" for i in range(1, 4)] + ["{bproof1"])
DECAL_I4_MAX = 64  # texels per side (TMEM); larger lumps are halved

MAX_SEQ_GROUPS = 15

# STUDIO: the M3 studio bake (D3 b: per-sequence animation chunks,
# architecture.md 16.3 tools/studio_bake) runs after the model step and
# writes models/<x>.stb* next to the raw files; until then the content
# carries the raw MDL files restab tables.


class CookError(Exception):
    pass


def log(msg: str) -> None:
    print(msg, flush=True)


def run(cmd: List, **kw) -> None:
    log("+ " + " ".join(str(c) for c in cmd))
    r = subprocess.run([str(c) for c in cmd], **kw)
    if r.returncode != 0:
        raise CookError(f"{Path(str(cmd[0])).name} {' '.join(str(c) for c in cmd[1:3])}: "
                        f"exit {r.returncode}")


def lower_rel(rel: str) -> str:
    return rel.replace("\\", "/").lstrip("/").lower()


class Content:
    """Writes into the content directory; only changed files are written."""

    def __init__(self, root: Path, staging: Path):
        self.root = root
        self.manifest = staging / "content.lst"
        self.owned: Set[str] = set()
        self.written = 0

    def put(self, rel: str, data: bytes) -> None:
        rel = lower_rel(rel)
        if rel in self.owned:
            raise CookError(f"content: {rel} produced twice")
        self.owned.add(rel)
        dest = self.root / rel
        if dest.is_file() and dest.stat().st_size == len(data) and dest.read_bytes() == data:
            return
        dest.parent.mkdir(parents=True, exist_ok=True)
        tmp = dest.with_name(dest.name + ".part")
        tmp.write_bytes(data)
        os.replace(tmp, dest)
        self.written += 1

    def put_file(self, rel: str, src: Path) -> None:
        self.put(rel, src.read_bytes())

    def claim(self, rel: str) -> None:
        """A file a tool wrote straight into the content directory."""
        rel = lower_rel(rel)
        if not (self.root / rel).is_file():
            raise CookError(f"content: {rel} was not produced")
        self.owned.add(rel)

    def finish(self) -> int:
        """Remove what the previous run wrote and this run did not."""
        stale = 0
        if self.manifest.is_file():
            for rel in self.manifest.read_text().split("\n"):
                if rel and rel not in self.owned and (self.root / rel).is_file():
                    (self.root / rel).unlink()
                    stale += 1
        self.manifest.write_text("".join(f"{r}\n" for r in sorted(self.owned)))
        return stale


# ---------------------------------------------------------------------------
# references
# ---------------------------------------------------------------------------

def key_refs(ents: List[Dict[str, str]], kind: int) -> Set[str]:
    """Lower-case resource names of `kind` that entity key values name, by
    restab's rule (tools/restab/restab.py res_kind_of_value), so the content
    holds every file restab will table."""
    out: Set[str] = set()
    for e in ents:
        for v in e.values():
            rk = restab.res_kind_of_value(v)
            if rk and rk[0] == kind:
                out.add(rk[1])
    return out


def read_res_def(defs: Optional[Path]) -> Dict[str, Set[str]]:
    """{MODEL|SPRITE|SOUND|SENTENCE|GROUP: names} of game/defs/res.def."""
    out: Dict[str, Set[str]] = {k: set() for k in ("MODEL", "SPRITE", "SOUND", "SENTENCE", "GROUP")}
    if defs is None or not (defs / "res.def").is_file():
        return out
    try:
        lines = restab.read_defs(str(defs), "res.def")
    except restab.CookError as e:
        raise CookError(str(e)) from None
    for _ln, (kw, _ident, value) in lines:
        out[kw].add(value.strip().replace("\\", "/").lower() if kw in ("MODEL", "SPRITE", "SOUND")
                    else value.strip().upper())
    return out


# ---------------------------------------------------------------------------
# steps
# ---------------------------------------------------------------------------

def stage_entities(data: Data, maps: List[str], ct: Content) -> Dict[str, List[Dict[str, str]]]:
    ents = {}
    for m in maps:
        bsp = data.root / "maps" / f"{m}.bsp"
        ct.put(f"maps/{m}.ent", hl_classes.entity_text(bsp).encode("latin-1"))
        ents[m] = hl_classes.map_entities(bsp)
    return ents


def stage_models(data: Data, maps: List[str], ents, res: Dict[str, Set[str]],
                 gib_models: Set[str], ct: Content) -> Dict[str, bytes]:
    """Every referenced model + companions; returns {rel: bytes}."""
    bases: Set[str] = set()
    for per in hl_classes.models_for_maps(data.root / "maps", maps).values():
        bases.update(per)
    bases.update(FX_MODELS)
    names = {f"models/{b}.mdl" for b in bases}
    for m in maps:
        names |= key_refs(ents[m], rtb.KIND_MODEL)
    names |= gib_models | res["MODEL"]
    # one name per file: map keys spell models with any case, separator or
    # a leading "/" (c1a* gibmodel "/models/mbarrel.mdl"), as restab names them
    names = {restab._norm(n) for n in names}
    missing = sorted(n for n in names if not data.has(n))
    if missing:
        raise CookError("models missing from the game data: " + ", ".join(missing))
    out: Dict[str, bytes] = {}
    for n in sorted(names):
        out[n] = data.need(n)
        base = n[:-4]
        if data.has(base + "t.mdl") and base + "t.mdl" not in names:
            out[base + "t.mdl"] = data.need(base + "t.mdl")
        for k in range(1, MAX_SEQ_GROUPS + 1):
            seq = f"{base}{k:02d}.mdl"
            if not data.has(seq) or seq in names:
                break
            out[seq] = data.need(seq)
    for n, b in out.items():
        ct.put(n, b)
    log(f"    {len(names)} models, {len(out) - len(names)} texture / sequence-group files")
    return out


def stage_sprites(data: Data, maps: List[str], ents, res: Dict[str, Set[str]],
                  ct: Content) -> int:
    names = {f"sprites/{s}.spr" for s in FX_SPRITES} | res["SPRITE"]
    for m in maps:
        names |= key_refs(ents[m], rtb.KIND_SPRITE)
    missing = sorted(n for n in names if not data.has(n))
    if missing:
        raise CookError("sprites missing from the game data: " + ", ".join(missing))
    total = 0
    for n in sorted(names):
        try:
            out = hl_sprite_cook.cook(data.need(n))
        except ValueError as e:
            raise CookError(f"{data.where(n)}: {e}") from None
        ct.put(n[:-4] + ".sc4", out)
        total += len(out)
    log(f"    {len(names)} sprites, {total} bytes of .sc4")
    return len(names)


def stage_decals(data: Data, ct: Content) -> None:
    """gsfx/decals.i4 (big-endian): u32 magic 'DI4'1, u16 count, u16 28, then
    per decal name[16] (lower case), u8 texel w, h, world w, h (the lump's
    size), u8 tint r, g, b (palette[255]), pad, u32 data offset; then I4
    texels (high nibble first, 8-aligned): the '{' decal's palette index
    is its alpha, quantised to 4 bits; sides over DECAL_I4_MAX averaged down
    by 2 until they fit (scorch 96 -> 48 texels, still 96 units)."""
    wad = data.need("decals.wad")
    what = data.where("decals.wad")
    if len(wad) < 12 or wad[:4] != b"WAD3":
        raise CookError(f"{what}: not a WAD3")
    count, dirofs = struct.unpack_from("<ii", wad, 4)
    if count < 0 or dirofs < 0 or dirofs + 32 * count > len(wad):
        raise CookError(f"{what}: directory outside the file")
    lumps: Dict[str, Tuple[int, int]] = {}
    for i in range(count):
        pos, _dsize, size, typ, _cmp, _pad, raw = struct.unpack_from(
            "<iiibbh16s", wad, dirofs + 32 * i)
        if typ == 0x43:
            lumps[raw.split(b"\0")[0].decode("latin-1").lower()] = (pos, size)
    missing = [n for n in DECAL_NAMES if n not in lumps]
    if missing:
        raise CookError(f"{what}: decals missing: " + ", ".join(missing))
    entries, blobs = [], []
    ofs = 8 + 28 * len(DECAL_NAMES)
    ofs = (ofs + 7) & ~7
    for name in DECAL_NAMES:
        pos, size = lumps[name]
        if pos < 0 or size < 40 or pos + size > len(wad):
            raise CookError(f"{what}: decal {name} outside the file")
        w, h, o0, _o1, _o2, o3 = struct.unpack_from("<II4I", wad, pos + 16)
        if w < 1 or h < 1 or w > 255 or h > 255:
            raise CookError(f"{what}: decal {name} is {w}x{h}")
        pal = pos + o3 + (w // 8) * (h // 8) + 2
        if pos + o0 + w * h > pos + size or pal + 256 * 3 > pos + size:
            raise CookError(f"{what}: decal {name} texels or palette outside its lump")
        tint = wad[pal + 255 * 3:pal + 256 * 3]
        px = list(wad[pos + o0:pos + o0 + w * h])
        tw, th = w, h
        while tw > DECAL_I4_MAX or th > DECAL_I4_MAX:
            px = [(px[(2 * y) * tw + 2 * x] + px[(2 * y) * tw + 2 * x + 1]
                   + px[(2 * y + 1) * tw + 2 * x] + px[(2 * y + 1) * tw + 2 * x + 1] + 2) // 4
                  for y in range(th // 2) for x in range(tw // 2)]
            tw, th = tw // 2, th // 2
        if tw % 16 or th < 1:
            raise CookError(f"{what}: decal {name}: {tw}x{th} texels not storable")
        q = [(v * 15 + 127) // 255 for v in px]
        blob = bytes((q[k] << 4) | q[k + 1] for k in range(0, len(q), 2))
        entries.append(name.encode("latin-1").ljust(16, b"\0")
                       + bytes((tw, th, w, h)) + tint + b"\0"
                       + struct.pack(">I", ofs))
        blobs.append(blob.ljust((len(blob) + 7) & ~7, b"\0"))
        ofs += len(blobs[-1])
    head = struct.pack(">IHH", 0x44493401, len(DECAL_NAMES), 28) + b"".join(entries)
    out = head.ljust((len(head) + 7) & ~7, b"\0") + b"".join(blobs)
    ct.put("gsfx/decals.i4", out)
    log(f"    gsfx/decals.i4: {len(DECAL_NAMES)} decals, {len(out)} bytes")


def stage_2d(data: Data, ct: Content, staging: Path, ref_palette: Optional[bytes]) -> None:
    hl_pal = data.need("gfx/palette.lmp")
    if len(hl_pal) < 768:
        raise CookError(f"{data.where('gfx/palette.lmp')}: shorter than 768 bytes")
    hl_pal = hl_pal[:768]
    pal2d = ref_palette if ref_palette is not None else hl_pal
    ct.put("gsfx/palette.lmp", hl_pal)
    ct.put("gfx/palette.lmp", pal2d)
    sprites = {n: data.need(f"sprites/{n}.spr") for n in HUD_SHEETS}
    try:
        files, header = bake_hlhud.bake(sprites, data.need("sprites/hud.txt").decode("latin-1"),
                                        pal2d)
        files.update(bake_ui.bake(data.need("gfx.wad"), pal2d, list(UI_LUMPS),
                                  data.where("gfx.wad")))
    except ValueError as e:
        raise CookError(str(e)) from None
    for n, b in sorted(files.items()):
        ct.put(f"gfx/{n}", b)
    (staging / "hl_hud_rects.h").write_text(header)
    log(f"    {len(files)} ui8 sheets; HUD rects -> {staging / 'hl_hud_rects.h'}")


def stage_sounds(data: Data, maps: List[str], ents, models: Dict[str, bytes],
                 res: Dict[str, Set[str]], ct: Content, staging: Path, jobs: int) -> None:
    try:
        refs, table, titles = hl_sound_refs.collect(
            data, maps, models,
            samples=(s[6:] if s.startswith("sound/") else s for s in res["SOUND"]),
            sentences=res["SENTENCE"], groups_in=res["GROUP"])
        words = hl_sound_refs.sentence_samples(refs, table)
    except (DataError, BspError, ValueError) as e:
        raise CookError(str(e)) from None
    keyed: Set[str] = set()
    for m in maps:
        keyed |= key_refs(ents[m], rtb.KIND_SOUND)
    samples = {lower_rel(s) for s in refs.samples | words}
    samples |= keyed
    missing = sorted(s for s in samples if not data.has("sound/" + s))
    hard = sorted(s for s in missing if s in keyed)
    if hard:
        raise CookError("sounds the maps name are missing from the game data: " + ", ".join(hard))
    if missing:
        log("    note: not in the game data (skipped): " + ", ".join(missing))
    have = sorted(set(samples) - set(missing))
    ct.put("sound/sentences.txt", hl_sound_refs.sentences_txt(refs, table).encode("latin-1"))
    ct.put("titles.txt", hl_sound_refs.titles_txt(refs, titles).encode("latin-1"))

    rules = hl_sound_classes.parse_rules(hl_sound_classes.RULES)
    sdir = staging / "sound"
    todo = []
    for s in have:
        wav = data.need("sound/" + s)
        try:
            cls, spec, resample, cue = hl_sound_classes.plan(rules, s, wav)
        except hl_sound_classes.SoundError as e:
            raise CookError(f"{data.where('sound/' + s)}: {e}") from None
        key = f"{hashlib.sha256(wav).hexdigest()} {spec} {resample} {cue}\n"
        dest = sdir / s
        stamp = dest.with_suffix(".plan")
        if stamp.is_file() and stamp.read_text() == key and dest.with_suffix(".wav64").is_file():
            continue
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(wav)
        stamp.unlink(missing_ok=True)
        todo.append((dest, spec, resample, cue, stamp, key))
    if todo:
        log(f"    converting {len(todo)} of {len(have)} samples (audioconv64)")
        try:
            done = hl_sound_classes.convert_all([t[:4] for t in todo], jobs)
        except hl_sound_classes.SoundError as e:
            raise CookError(str(e)) from None
        for (dest, spec, _rs, _cue, stamp, key), (_out, used) in zip(todo, done):
            dest.unlink()
            stamp.write_text(key)
            if used != spec:
                log(f"    {dest.relative_to(sdir)}: encoded with --wav-compress {used}")
    for s in have:
        ct.put_file("sound/" + s[:-4] + ".wav64", (sdir / s).with_suffix(".wav64"))
    log(f"    {len(have)} sounds, {len(hl_sound_refs.selected_sentences(refs, table))} sentences, "
        f"{len(refs.titles)} titles")


def cook_world(data: Data, maps: List[str], ct: Content, staging: Path, hlecook: Path,
               ref_palette_path: Optional[Path], jobs: int, ids: hleids.Ids) -> None:
    cook = staging / "maps"
    cook.mkdir(parents=True, exist_ok=True)
    out_dir = ct.root / "maps"
    out_dir.mkdir(parents=True, exist_ok=True)
    materials = data.root / "sound" / "materials.txt"
    if not materials.is_file():
        raise CookError(f"{materials}: missing (surface materials)")
    env = {**os.environ, "PYTHONPATH": str(ROOT)}

    def one(m: str) -> None:
        bsp = data.root / "maps" / f"{m}.bsp"
        tex = [PY, "-m", "tools.cook.bake_world_tex", bsp, "-o", cook / f"{m}.wci4",
               "--wad-dir", data.root]
        if ref_palette_path is not None:
            tex += ["--ref-palette", ref_palette_path]
        run(tex, env=env, cwd=ROOT)
        run([hlecook, bsp, cook / f"{m}.hle"])
        run([PY, "-m", "tools.w64c.touchrank", bsp, cook / f"{m}.hle"], env=env, cwd=ROOT)
        hle = cook / f"{m}.hle"
        log(f"+ hleids {hle}")
        hle.write_bytes(hleids.apply(hle.read_bytes(), ids, m))
        run([PY, "-m", "tools.w64c.w64c", bsp, cook / f"{m}.wci4", out_dir / f"{m}.w64",
             "--hle", cook / f"{m}.hle", "--materials", materials,
             "--json", cook / f"{m}.json"], env=env, cwd=ROOT,
            stdout=subprocess.DEVNULL)

    with ThreadPoolExecutor(max_workers=max(1, jobs)) as ex:
        for f in [ex.submit(one, m) for m in maps]:
            f.result()
    for m in maps:
        ct.claim(f"maps/{m}.w64")


def cook_campaign(data: Data, maps: List[str], ct: Content, staging: Path) -> None:
    env = {**os.environ, "PYTHONPATH": str(ROOT)}
    graph = staging / "campaign.cwg"
    run([PY, "-m", "tools.cook.hl_campaign", "--maps-dir", data.root / "maps",
         "--maps", *maps, "-o", graph, "--json", staging / "campaign.json"], env=env, cwd=ROOT)
    run([PY, "-m", "tools.w64c.w64seam", graph, "--packages", ct.root / "maps",
         "--maps-dir", data.root / "maps", "--report", staging / "seams.json"], env=env, cwd=ROOT)
    ct.put_file("maps/campaign.cwg", graph)


def w64_ents(path: Path) -> bytes:
    """The ENTS chunk of a .w64 (docs/formats/w64-format.md)."""
    b = path.read_bytes()
    magic, _ver, _fl, _fsz, dofs = struct.unpack_from(">4sHHII", b, 0)
    if magic != b"W64L":
        return b""
    nchunks = struct.unpack_from(">H", b, dofs + 10)[0]
    cd = dofs + struct.unpack_from(">I", b, dofs + 16)[0]
    ents = struct.unpack_from(">H", b, dofs + 52)[0]
    if ents >= nchunks:
        return b""
    _fcc, o, sz, _crc = struct.unpack_from(">4sIII", b, cd + 16 * ents)
    return b[o:o + sz]


def verify(data: Data, maps: List[str], ents, ct: Content) -> List[str]:
    """Cross-checks of the cooked outputs against the maps."""
    fails: List[str] = []
    for m in maps:
        raw = w64_ents(ct.root / "maps" / f"{m}.w64")
        if not raw.startswith(b"HLE1"):
            fails.append(f"{m}.w64 has no HLE1 entity chunk")
        elif not struct.unpack_from("<H", raw, 6)[0] & hleids.HLE_F_IDS:
            fails.append(f"{m}.w64 entity chunk has no ids")
        src = {e.get("classname", "") for e in ents[m]}
        for cls in ("multi_manager", "trigger_changelevel", "info_landmark",
                    "info_teleport_destination"):
            if cls in src and cls.encode() not in raw:
                fails.append(f"{m}.w64 entity chunk lost {cls}")
    from tools.cook.hl_campaign import NONE, read_cwg
    try:
        graph = read_cwg(ct.root / "maps" / "campaign.cwg")
        for gm in graph["maps"]:
            triggers = [e for e in ents.get(gm["name"], [])
                        if e.get("classname") == "trigger_changelevel" and e.get("map") in maps]
            if len(triggers) != len(gm["links"]):
                fails.append(f"{gm['name']}: campaign/BSP link count mismatch")
                continue
            for link, ent in zip(gm["links"], triggers):
                unreachable = bool(link["spawnflags"] & 2) and not ent.get("targetname")
                self_link = link["to"] == gm["name"]
                if link["seam"] == NONE and not (unreachable or self_link):
                    fails.append(f"{gm['name']}->{link['to']} via {link['landmark']}: missing seam")
                if link["seam"] != NONE and (unreachable or self_link):
                    fails.append(f"{gm['name']}: self/unreachable link has a seam")
        for seam in graph["seams"]:
            package = (ct.root / "maps" / f"{seam['to']}.w64").read_bytes()
            if struct.unpack_from(">I", package, 24)[0] != seam["b_dir_crc"]:
                fails.append(f"{seam['from']}->{seam['to']}: seam/package directory CRC mismatch")
    except (ValueError, OSError, struct.error) as error:
        fails.append(f"campaign.cwg: {error}")
    return fails


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--data", type=Path, required=True,
                    help="the player's Half-Life game directory (e.g. .../Half-Life/valve)")
    ap.add_argument("--content", type=Path, required=True, help="content directory to write")
    ap.add_argument("--maps", nargs="*", default=None,
                    help="map names / globs (default: every maps/*.bsp)")
    ap.add_argument("--staging", type=Path, default=None,
                    help="intermediates (default: <content>.stage)")
    ap.add_argument("--hlecook", type=Path, required=True, help="built tools/w64c/hlecook")
    ap.add_argument("--defs", type=Path, default=ROOT / "game" / "defs")
    ap.add_argument("--ref-palette", type=Path, default=None)
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    a = ap.parse_args(argv)

    content = a.content.resolve()
    staging = (a.staging or content.with_name(content.name + ".stage")).resolve()
    try:
        data = Data(a.data.expanduser().resolve())
        maps = hl_classes.discover_maps(data.root / "maps", a.maps)
        if not maps:
            raise CookError(f"no maps under {data.root / 'maps'} match {a.maps}")
        if not os.access(a.hlecook, os.X_OK):
            raise CookError(f"{a.hlecook}: not an executable (build it with make cook)")
        ref = None
        if a.ref_palette is not None:
            ref = a.ref_palette.read_bytes()[:768]
            if len(ref) < 768:
                raise CookError(f"{a.ref_palette}: shorter than 768 bytes")
        content.mkdir(parents=True, exist_ok=True)
        staging.mkdir(parents=True, exist_ok=True)
        ct = Content(content, staging)
        res = read_res_def(a.defs)

        log("== hl64 cook ==")
        log(f"  data    : {data.root}")
        log(f"  maps    : {' '.join(maps)}")
        log(f"  content : {content}")
        log(f"  staging : {staging}")
        log("== entities -> maps/*.ent ==")
        ents = stage_entities(data, maps, ct)
        # the gib shooters name models; model events name sounds
        gibs = {e["shootmodel"].lower() for m in maps for e in ents[m]
                if e.get("classname") == "env_shooter" and e.get("shootmodel")}
        gibs |= {"models/hgibs.mdl" for m in maps for e in ents[m]
                 if e.get("classname") == "gibshooter"}
        log("== models -> models/ ==")
        models = stage_models(data, maps, ents, res, gibs, ct)
        log("== sounds, sentences, titles -> sound/, titles.txt ==")
        stage_sounds(data, maps, ents, models, res, ct, staging, a.jobs)
        log("== sprites -> sprites/*.sc4 ==")
        stage_sprites(data, maps, ents, res, ct)
        log("== decals -> gsfx/decals.i4 ==")
        stage_decals(data, ct)
        log("== palettes, HUD and UI -> gfx/ ==")
        stage_2d(data, ct, staging, ref)
        log("== HLE1 id spaces (restab names) ==")
        ids = hleids.Ids.from_table(restab.cook(str(content), str(a.defs), maps),
                                    hleids.read_keys_def(str(a.defs)))
        log("== world packages -> maps/*.w64 ==")
        cook_world(data, maps, ct, staging, a.hlecook.resolve(), a.ref_palette, a.jobs, ids)
        log("== campaign graph + seams -> maps/campaign.cwg ==")
        cook_campaign(data, maps, ct, staging)
        log("== verification ==")
        fails = verify(data, maps, ents, ct)
        stale = ct.finish()
    except (CookError, DataError, BspError, OSError, hleids.IdError, restab.CookError) as e:
        print(f"stage: error: {e}", file=sys.stderr)
        return 1
    for f in fails:
        log(f"  FAIL: {f}")
    log(f"== done: {len(ct.owned)} files, {ct.written} written, {stale} stale removed ==")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
