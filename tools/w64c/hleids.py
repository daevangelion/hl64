# SPDX-License-Identifier: MIT
"""hleids: the HLE1 id step (docs/formats/hle-format-v1.md "Ids").

Writes the cooked ids into an HLE1 entity chunk (the `.hle` hlecook wrote,
after touchrank, before w64c packs it), in place and without moving a byte
of the layout:

  class table    id = classes.def line order of the classname
                 (case-insensitive; 0xFFFF for classes the game lacks)
  keyname table  id = keys.def line order of the key name (exact; a key
                 name the cook keeps and keys.def lacks is an error)
  key values     HLE_KS: res_t of a resource value (a model / sprite /
                 sound name as restab tables it; 0xFFFF for a sound the
                 player's data lacks, `*N` -> RES_K_SUBMODEL
                 index N, `!NAME` in a sentence key -> RES_K_SENTENCE, a
                 speaker's `message` or a talk monster's UseSentence /
                 UnUseSentence group name -> RES_K_GROUP), the lightstyle
                 pattern id of `pattern`, the global-state id of
                 `globalstate`, the classes.def id of a monstermaker
                 `monstertype`, the title id (titles.txt message) of an
                 env_message `message` / worldspawn `chaptertitle` (0xFFFF
                 when titles.txt lacks it: the game shows the string);
                 HLE_KN: the classes.def id of a classname value; 0xFFFF
                 otherwise
  header         flags |= HLE_F_IDS

The ids come from the campaign resource table's name sections. The map
packages that table lists embed the chunk, so the stage computes the id
spaces first (restab.cook with the map list: ids depend on names only) and
`--check` proves afterwards that the final campaign.rtb gives every chunk
the same ids and that every res_t resolves inside its cart file
(precache-and-resources.md R2). It also proves the HLE region: the bytes
engine/ent/rec.c takes for the campaign's largest slots (architecture.md 4.1:
2 x (chunk + runtime) + 2 x runtime + rec_init's tables, the sizes from
engine/ent/rec_int.h) must fit HL64_HLE_BLOBS_KB of include/hl64/limits.h.

  python3 -m tools.w64c.hleids HLE --content DIR --defs DIR --maps M ...
  python3 -m tools.w64c.hleids --check CONTENT --defs DIR [--limits H] [--rec-int H]
"""
from __future__ import annotations

import argparse
import os
import re
import struct
import sys
from dataclasses import dataclass, field, replace
from pathlib import Path
from typing import Container, Dict, List, Optional, Sequence, Tuple

from tools.cook import decals
from tools.restab import restab, rtb

HLE_MAGIC = 0x31454C48
HLE_HDR = 96
HLE_REC = 24
HLE_F_BIGENDIAN = 1
HLE_F_IDS = 2
HLE_KF, HLE_KV, HLE_KS, HLE_KN = 1, 2, 3, 4
HLE_CF_LOGIC = 1
NONE = 0xFFFF
SUBMODEL_MAX = 1024          # RES_K_SUBMODEL index = slot << 10 | submodel

_KEY_RE = re.compile(r'^KEY\(\s*([A-Z_][A-Z0-9_]*)\s*,\s*"([^"\\\n]*)"\s*\)\s*$')


class IdError(Exception):
    pass


def read_keys_def(defs_dir: str) -> List[str]:
    """Key names of game/defs/keys.def in line order (key id = index)."""
    path = os.path.join(defs_dir, "keys.def")
    out: List[str] = []
    with open(path, "r", encoding="utf-8") as f:
        for ln, line in enumerate(f, 1):
            s = line.strip()
            if not s or s.startswith("//"):
                continue
            m = _KEY_RE.match(s)
            if not m:
                raise IdError(f"keys.def:{ln}: not a KEY(IDENT, \"name\") line")
            if m.group(2) in out:
                raise IdError(f"keys.def:{ln}: duplicate key {m.group(2)!r}")
            out.append(m.group(2))
    return out


@dataclass
class Ids:
    """Name -> id maps of one table (restab prepass or the final rtb)."""
    classes: Dict[str, int] = field(default_factory=dict)     # lower-case
    keys: Dict[str, int] = field(default_factory=dict)
    res: Dict[tuple, int] = field(default_factory=dict)       # (kind, name) -> res_t
    sentences: Dict[str, int] = field(default_factory=dict)   # upper-case -> res_t
    groups: Dict[str, int] = field(default_factory=dict)      # upper-case -> res_t
    patterns: Dict[bytes, int] = field(default_factory=dict)  # letters a-z -> id
    globals_: Dict[str, int] = field(default_factory=dict)
    titles: Dict[str, int] = field(default_factory=dict)      # upper-case
    decals: Dict[str, int] = field(default_factory=dict)      # lower-case -> decal id (one map)

    @classmethod
    def from_table(cls, t: rtb.Table, keys: Sequence[str]) -> "Ids":
        ids = cls()
        for i, n in enumerate(t.class_names):
            ids.classes.setdefault(n.lower(), i)
        ids.keys = {k: i for i, k in enumerate(keys)}
        for kind in rtb.FILE_KINDS:
            first = t.kind_first(kind)
            for i in range(t.kind_count(kind)):
                ids.res[(kind, t.res_names[first + i])] = rtb.res_t(kind, i)
        for i, n in enumerate(t.sentence_names):
            ids.sentences.setdefault(n, rtb.res_t(rtb.KIND_SENTENCE, i))
        for i, n in enumerate(t.group_names):
            ids.groups.setdefault(n, rtb.res_t(rtb.KIND_GROUP, i))
        for i, p in enumerate(t.patterns):
            ids.patterns.setdefault(bytes(c + 97 for c in p), i)
        ids.globals_ = {n: i for i, n in enumerate(t.global_names)}
        for i, n in enumerate(t.title_names):
            ids.titles.setdefault(n.upper(), i)
        ids.decals = {n: i for i, n in reversed(list(enumerate(decals.DECAL_NAMES)))}
        return ids

    def for_map(self, local: Sequence[str]) -> "Ids":
        """These ids with one map's decal ids: the global sheet's names
        (decals.DECAL_NAMES, from_table) and the map's DECL sheet names
        `local` (id = len(DECAL_NAMES) + index)."""
        d = dict(self.decals)
        for i, n in enumerate(local):
            d.setdefault(n, decals.GLOBAL_COUNT + i)
        return replace(self, decals=d)


def decal_textures(blob: bytes, where: str = "") -> List[str]:
    """The `texture` values (lower case) of the chunk's infodecal records,
    in record order."""
    if len(blob) < HLE_HDR or struct.unpack_from("<I", blob)[0] != HLE_MAGIC:
        raise IdError(f"{where}: not a little-endian HLE1 chunk")
    (_m, _v, _f, size, _s, _nl, numrecs, numclasses, numkeynames, _nn, _hs, numkeys,
     _nll, _nfire, _nfl, strsize, _nse) = struct.unpack_from("<IHHIIHHHHHHIIIIII", blob)
    (o_recs, _l2r, o_cls, o_kn, _o_names, _o_hash, o_keys, _o_ll, _o_fire,
     _o_fl, o_str) = struct.unpack_from("<11I", blob, 52)
    if size > len(blob):
        raise IdError(f"{where}: HLE1 size outside the chunk")
    for ofs, n, elt, what in ((o_recs, numrecs, HLE_REC, "records"), (o_cls, numclasses, 8, "classes"),
                              (o_kn, numkeynames, 8, "keynames"), (o_keys, numkeys, 8, "keys"),
                              (o_str, strsize, 1, "strings")):
        _section(size, ofs, n, elt, what)

    def s_at(ofs: int, what: str) -> str:
        return _cstr(blob, o_str, strsize, ofs, f"{where}: {what}")
    cls = {i for i in range(numclasses)
           if s_at(struct.unpack_from("<I", blob, o_cls + 8 * i)[0], f"class {i}").lower() == "infodecal"}
    tex = {i for i in range(numkeynames)
           if s_at(struct.unpack_from("<I", blob, o_kn + 8 * i)[0], f"keyname {i}") == "texture"}
    out = []
    for r in range(numrecs):
        _lump, c, _tn, _fl, kf, kc = struct.unpack_from("<6H", blob, o_recs + HLE_REC * r)
        if c not in cls or kf + kc > numkeys:
            continue
        for k in range(kf, kf + kc):
            kn, _id, v = struct.unpack_from("<HHI", blob, o_keys + 8 * k)
            if kn in tex:
                out.append(s_at(v, f"record {r} texture").lower())
    return out


def map_decals(textures: Sequence[str], in_wad: Container[str]) -> Tuple[List[str], List[str]]:
    """(the map's DECL sheet names: the textures outside the global sheet
    that decals.wad has, sorted; the textures decals.wad lacks, sorted)."""
    rest = {t for t in textures if decals.global_id(t) < 0}
    return sorted(t for t in rest if t in in_wad), sorted(t for t in rest if t not in in_wad)


def _cstr(blob: bytes, base: int, size: int, ofs: int, what: str) -> str:
    if ofs >= size:
        raise IdError(f"{what}: string offset {ofs} outside the string table")
    end = blob.find(b"\0", base + ofs, base + size)
    if end < 0:
        raise IdError(f"{what}: unterminated string")
    return blob[base + ofs:end].decode("latin-1")


def _section(size: int, ofs: int, n: int, elt: int, what: str) -> None:
    if ofs > size or n * elt > size - ofs:
        raise IdError(f"HLE1 {what} outside the chunk")


def apply(blob: bytes, ids: Ids, where: str = "") -> bytes:
    """The chunk with every id written (idempotent); IdError on malformed
    chunks or names the table does not hold."""
    return apply_typed(blob, ids, where)[0]


def apply_typed(blob: bytes, ids: Ids, where: str = "") -> Tuple[bytes, List[Tuple[int, str, int]]]:
    """apply() and the typed ids written: (key index, type, id)."""
    if len(blob) < HLE_HDR or struct.unpack_from("<I", blob)[0] != HLE_MAGIC:
        raise IdError(f"{where}: not a little-endian HLE1 chunk")
    (_m, version, flags, size, _src, numlumps, numrecs, numclasses, numkeynames,
     numnames, _hs, numkeys, _nll, _nfire, _nfl, strsize, _nse) = \
        struct.unpack_from("<IHHIIHHHHHHIIIIII", blob)
    (o_recs, _o_l2r, o_cls, o_kn, o_names, _o_hash, o_keys, _o_ll, _o_fire,
     _o_fl, o_str) = struct.unpack_from("<11I", blob, 52)
    if version != 1 or flags & HLE_F_BIGENDIAN or size > len(blob):
        raise IdError(f"{where}: HLE1 header (version / byte order / size)")
    for ofs, n, elt, what in ((o_recs, numrecs, HLE_REC, "records"),
                              (o_cls, numclasses, 8, "classes"),
                              (o_kn, numkeynames, 8, "keynames"),
                              (o_names, numnames, 12, "names"),
                              (o_keys, numkeys, 8, "keys"), (o_str, strsize, 1, "strings")):
        _section(size, ofs, n, elt, what)
    out = bytearray(blob)
    written: List[Tuple[int, str, int]] = []

    def s_at(ofs: int, what: str) -> str:
        return _cstr(blob, o_str, strsize, ofs, f"{where}: {what}")

    cnames = []
    for i in range(numclasses):
        cn = s_at(struct.unpack_from("<I", blob, o_cls + 8 * i)[0], f"class {i}")
        cnames.append(cn)
        struct.pack_into("<H", out, o_cls + 8 * i + 6, ids.classes.get(cn.lower(), NONE))
    knames, kkinds = [], []
    for i in range(numkeynames):
        s, kind = struct.unpack_from("<IH", blob, o_kn + 8 * i)
        kn = s_at(s, f"keyname {i}")
        if kn not in ids.keys:
            raise IdError(f"{where}: key {kn!r} is not in game/defs/keys.def")
        knames.append(kn)
        kkinds.append(kind)
        struct.pack_into("<H", out, o_kn + 8 * i + 6, ids.keys[kn])
    nstr = []
    for i in range(numnames):
        nstr.append(s_at(struct.unpack_from("<I", blob, o_names + 12 * i)[0], f"name {i}"))

    for r in range(numrecs):
        lump, cls, _tn, _fl, kf, kc = struct.unpack_from("<6H", blob, o_recs + HLE_REC * r)
        if cls >= numclasses or kf + kc > numkeys:
            raise IdError(f"{where}: record {r} out of range")
        cname = cnames[cls].lower()
        rk = [struct.unpack_from("<HHI", blob, o_keys + 8 * k) for k in range(kf, kf + kc)]
        for kn, _id, _v in rk:
            if kn >= numkeynames:
                raise IdError(f"{where}: record {r}: keyname {kn} out of range")
        preset = any(knames[kn] == "preset" and kkinds[kn] == HLE_KF
                     and struct.unpack("<f", struct.pack("<I", v))[0] != 0 for kn, _i, v in rk)
        for j, (kn, _id, val) in enumerate(rk):
            key, kind = knames[kn], kkinds[kn]
            at = f"{where}:{lump} {key}"
            tid, ty = NONE, ""
            if kind == HLE_KN:
                if val >= numnames:
                    raise IdError(f"{at}: name {val} out of range")
                ty, tid = "cls", ids.classes.get(nstr[val].lower(), NONE)
            elif kind == HLE_KS:
                v = s_at(val, at)
                ty, tid = typed(key, v, cname, preset, ids, at)
            if tid != NONE:
                written.append((kf + j, ty, tid))
            struct.pack_into("<H", out, o_keys + 8 * (kf + j) + 2, tid)
    struct.pack_into("<H", out, 6, flags | HLE_F_IDS)
    return bytes(out), written


def typed(key: str, v: str, cname: str, preset: bool, ids: Ids, at: str) -> Tuple[str, int]:
    """(type, id) of one string value (restab's naming rules); type "res",
    "pattern", "global", "title" or "decal"."""
    if key == "texture" and cname == "infodecal":
        # global sheet entry or the map's DECL entry; none: decals.wad lacks
        # it (HLSDK CDecal::Spawn removes the entity)
        return "decal", ids.decals.get(v.lower(), NONE)
    if key == "pattern":
        if v.encode("latin-1") not in ids.patterns:
            raise IdError(f"{at}: pattern {v!r} not in the pattern table")
        return "pattern", ids.patterns[v.encode("latin-1")]
    if key == "globalstate":
        if v not in ids.globals_:
            raise IdError(f"{at}: global state {v!r} not in the table")
        return "global", ids.globals_[v]
    if (key, cname) in (("message", "env_message"), ("chaptertitle", "worldspawn")):
        return "title", ids.titles.get(v.upper(), NONE)
    if key in restab.SENTENCE_KEYS and v.startswith("!") and len(v) > 1:
        # one missing here is absent from the player's sentences.txt or
        # lost every word to missing sounds (GoldSrc plays nothing)
        return "res", ids.sentences.get(v[1:].upper(), NONE)
    if key == "monstertype":
        return "cls", ids.classes.get(v.lower(), NONE)
    if key == "message" and cname == "speaker" and not preset or key in restab.GROUP_KEYS:
        return "res", ids.groups.get(v.upper(), NONE)
    if key == "model" and re.fullmatch(r"\*[0-9]+", v):
        n = int(v[1:])
        if not 0 < n < SUBMODEL_MAX:
            raise IdError(f"{at}: submodel {v} out of range")
        return "res", rtb.res_t(rtb.KIND_SUBMODEL, n)
    rk = restab.res_kind_of_value(v)
    if rk is not None:
        if rk not in ids.res:
            # restab tables every keyed sound the content holds: one it lacks
            # is missing from the player's data (absent, no id)
            if rk[0] == rtb.KIND_SOUND:
                return "res", NONE
            raise IdError(f"{at}: {rtb.KIND_NAMES[rk[0]]} {rk[1]!r} not in the table")
        return "res", ids.res[rk]
    return "res", NONE


# ---- check (after restab) -----------------------------------------------------

def w64_chunk(b: bytes, fourcc: bytes, where: str) -> Optional[bytes]:
    """The first chunk of that kind in a .w64 (DECL: found by fourcc), or None."""
    if len(b) < 16 or b[:4] != b"W64L":
        raise IdError(f"{where}: not a W64L package")
    dofs = struct.unpack_from(">I", b, 12)[0]
    if dofs > len(b) - 56:
        raise IdError(f"{where}: directory outside the file")
    nchunks = struct.unpack_from(">H", b, dofs + 10)[0]
    cd = dofs + struct.unpack_from(">I", b, dofs + 16)[0]
    if cd > len(b) - 16 * nchunks:
        raise IdError(f"{where}: chunk directory outside the file")
    for i in range(nchunks):
        fcc, o, sz, _crc = struct.unpack_from(">4sIII", b, cd + 16 * i)
        if fcc == fourcc:
            if o > len(b) or sz > len(b) - o:
                raise IdError(f"{where}: {fourcc.decode()} chunk outside the file")
            return b[o:o + sz]
    return None


def w64_ents(b: bytes, where: str) -> bytes:
    if len(b) < 16 or b[:4] != b"W64L":
        raise IdError(f"{where}: not a W64L package")
    dofs = struct.unpack_from(">I", b, 12)[0]
    if dofs > len(b) - 56:
        raise IdError(f"{where}: directory outside the file")
    nchunks = struct.unpack_from(">H", b, dofs + 10)[0]
    cd = dofs + struct.unpack_from(">I", b, dofs + 16)[0]
    ents = struct.unpack_from(">H", b, dofs + 52)[0]
    if ents >= nchunks or cd > len(b) - 16 * nchunks:
        raise IdError(f"{where}: no ENTS chunk")
    _fcc, o, sz, _crc = struct.unpack_from(">4sIII", b, cd + 16 * ents)
    if o > len(b) or sz > len(b) - o:
        raise IdError(f"{where}: ENTS chunk outside the file")
    return b[o:o + sz]


# ---- HLE region proof (architecture.md 4.1 "HLE slots for M4") ---------------

DEFAULT_REC_INT = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))), "engine", "ent", "rec_int.h")


def _a16(n: int) -> int:
    return (n + 15) & ~15


@dataclass
class Slot:
    """One map's bytes in the HLE region, as engine/ent/rec.c open_chunk
    takes them: `need` while it opens (logic and pending space bounded by
    the records), `block` once open (logic blocks of the logic records
    only), `runtime` = block without the chunk."""
    name: str
    chunk: int
    need: int
    block: int
    runtime: int


@dataclass
class Region:
    """2 x (chunk + runtime) + 2 x runtime + tables, with each slot block
    16-aligned as heap_find places it: one open slot at its open-time
    need (the next map opening), one resident open slot, two runtime-only
    slots (frozen maps), rec_init's tables."""
    tables: int
    opening: Slot
    resident: Slot
    frozen: Slot
    total: int


def slot_bytes(name: str, chunk: bytes, rt_bytes: int, logic_bytes: int) -> Slot:
    """open_chunk's byte accounting for one packaged ENTS chunk."""
    if len(chunk) < HLE_HDR or struct.unpack_from("<I", chunk)[0] != HLE_MAGIC:
        raise IdError(f"{name}: not a little-endian HLE1 chunk")
    n = struct.unpack_from("<H", chunk, 18)[0]
    o_recs = struct.unpack_from("<I", chunk, 52)[0]
    if o_recs > len(chunk) or HLE_REC * n > len(chunk) - o_recs:
        raise IdError(f"{name}: HLE1 records outside the chunk")
    nlog = sum(1 for r in range(n)
               if struct.unpack_from("<H", chunk, o_recs + HLE_REC * r + 6)[0] & HLE_CF_LOGIC)
    a = _a16(len(chunk))
    need = a + n * (rt_bytes + logic_bytes) + 2 * n + 16
    block = ((a + n * rt_bytes + nlog * logic_bytes + 3) & ~3) + 2 * n
    return Slot(name, len(chunk), need, block, block - a)


def hle_region(slots: Sequence[Slot], tables: int) -> Region:
    if not slots:
        raise IdError("no map chunks")
    opening = max(slots, key=lambda s: s.need)
    resident = max(slots, key=lambda s: s.block)
    frozen = max(slots, key=lambda s: s.runtime)
    total = (tables + _a16(opening.need) + _a16(resident.block) + 2 * _a16(frozen.runtime))
    return Region(tables, opening, resident, frozen, total)


def check_region(region: Region, limits: str) -> int:
    """Fail when the region need exceeds HL64_HLE_BLOBS_KB; returns the
    carve in bytes."""
    kb = restab.read_limit(limits, "HL64_HLE_BLOBS_KB")
    if region.total > kb * 1024:
        raise IdError(f"HLE region need {region.total} B ({describe(region)}) exceeds "
                      f"HL64_HLE_BLOBS_KB {kb} ({kb * 1024} B): raise it in {limits}")
    return kb * 1024


def describe(r: Region) -> str:
    return (f"tables {r.tables} + opening {r.opening.name} {r.opening.need} + open "
            f"{r.resident.name} {r.resident.block} + 2 x frozen {r.frozen.name} runtime "
            f"{r.frozen.runtime}, each 16-aligned")


def accounting(rec_int: str) -> Tuple[int, int, int]:
    """(REC_RT_BYTES, REC_LOGIC_BYTES, REC_TABLE_BYTES) of engine/ent/rec_int.h"""
    return tuple(restab.read_limit(rec_int, n)        # type: ignore[return-value]
                 for n in ("REC_RT_BYTES", "REC_LOGIC_BYTES", "REC_TABLE_BYTES"))


def check(content: str, defs: str, limits: str = restab.DEFAULT_LIMITS,
          rec_int: str = DEFAULT_REC_INT
          ) -> Tuple[List[str], Optional[Region], Optional[decals.Region]]:
    """Every map chunk of content/campaign.rtb carries exactly the ids the
    final table gives (the prepass agreed), and every res_t resolves to an
    entry whose byte range lies inside its cart file (R2); the DEFS ids
    too; every infodecal texture id names its texture in the global sheet
    or the map's DECL sheet, which holds only those textures; the HLE region
    need of the campaign's chunks fits HL64_HLE_BLOBS_KB and the decal
    region need HL64_DECAL_TEXELS_KB. Returns failures and both needs."""
    with open(os.path.join(content, "campaign.rtb"), "rb") as f:
        t = rtb.read(f.read())
    ids = Ids.from_table(t, read_keys_def(defs))
    rt_bytes, logic_bytes, tables = accounting(rec_int)
    fails: List[str] = []
    slots: List[Slot] = []
    decl_sizes: Dict[str, int] = {}
    gpath = os.path.join(content, "gsfx", "decals.i4")
    try:
        with open(gpath, "rb") as f:
            gsheet = f.read()
        if decals.sheet_names(gsheet, gpath) != list(decals.DECAL_NAMES):
            fails.append(f"{gpath}: entries are not the HLSDK decals.h order")
    except (OSError, decals.DecalError) as e:
        fails.append(str(e))
        gsheet = b""
    counts = {k: t.kind_count(k) for k in range(8)}

    def res_ok(r: int, at: str) -> None:
        k, i = rtb.res_kind(r), rtb.res_index(r)
        if k == rtb.KIND_SUBMODEL:
            if not 0 < i < SUBMODEL_MAX:
                fails.append(f"{at}: submodel {i}")
            return
        if k == 0 or i >= counts[k]:
            fails.append(f"{at}: res_t {r:#06x} outside its kind")
            return
        if k in rtb.FILE_KINDS:
            e = t.res[t.kind_first(k) + i]
            if e.file >= len(t.files) or e.offset + e.size > t.files[e.file].size:
                fails.append(f"{at}: res_t {r:#06x} outside its file")

    for ln, r in enumerate(t.defs, 1):
        res_ok(r, f"res.def line-id {ln}")
    for mi, name in enumerate(t.map_names):
        fe = t.files[t.map_files[mi]]
        path = os.path.join(content, fe.path)
        try:
            with open(path, "rb") as f:
                pkg = f.read()
            blob = w64_ents(pkg, path)
            decl = w64_chunk(pkg, decals.DECL, path)
            local = decals.sheet_names(decl, f"{name} DECL") if decl is not None else []
            want = sorted({x for x in decal_textures(blob, name) if decals.global_id(x) < 0})
            if local != sorted(set(local)) or not set(local) <= set(want):
                fails.append(f"{name}: DECL entries {local} are not the map's own "
                             f"textures outside the global sheet {want}, sorted")
            if decl is not None:
                decl_sizes[name] = len(decl)
            again, written = apply_typed(blob, ids.for_map(local), name)
            slots.append(slot_bytes(name, blob, rt_bytes, logic_bytes))
        except (IdError, decals.DecalError, OSError, struct.error) as e:
            fails.append(f"{name}: {e}")
            continue
        size = struct.unpack_from("<I", blob, 8)[0]
        if not struct.unpack_from("<H", blob, 6)[0] & HLE_F_IDS:
            fails.append(f"{name}: chunk has no ids (HLE_F_IDS)")
        elif again[:size] != blob[:size]:
            fails.append(f"{name}: chunk ids differ from campaign.rtb's")
        for k, ty, tid in written:
            at = f"{name}: key {k}"
            if ty == "pattern" and tid >= len(t.patterns):
                fails.append(f"{at}: pattern id {tid}")
            elif ty == "global" and tid >= len(t.global_names):
                fails.append(f"{at}: global id {tid}")
            elif ty == "title" and tid >= len(t.titles):
                fails.append(f"{at}: title id {tid}")
            elif ty == "cls" and tid >= len(t.class_names):
                fails.append(f"{at}: class id {tid}")
            elif ty == "res":
                res_ok(tid, at)
            elif ty == "decal" and tid >= decals.GLOBAL_COUNT + len(local):
                fails.append(f"{at}: decal id {tid}")
    region = hle_region(slots, tables) if slots else None
    if region:
        try:
            check_region(region, limits)
        except IdError as e:
            fails.append(str(e))
    dregion = None
    if gsheet:
        dregion = decals.region_need(len(gsheet), decl_sizes, limits)
        try:
            decals.check_region(dregion, limits)
        except decals.DecalError as e:
            fails.append(str(e))
    return fails, region, dregion


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("hle", nargs="*", type=Path, help=".hle chunks to rewrite in place")
    ap.add_argument("--content", help="staged content directory")
    ap.add_argument("--defs", required=True, help="game/defs directory")
    ap.add_argument("--maps", nargs="*", default=None, help="campaign map names (prepass)")
    ap.add_argument("--check", metavar="CONTENT", help="verify the packages against campaign.rtb")
    ap.add_argument("--limits", default=restab.DEFAULT_LIMITS,
                    help="include/hl64/limits.h (HL64_HLE_BLOBS_KB)")
    ap.add_argument("--rec-int", default=DEFAULT_REC_INT,
                    help="engine/ent/rec_int.h (REC_RT_BYTES, REC_LOGIC_BYTES, REC_TABLE_BYTES)")
    ap.add_argument("--decals-wad", type=Path, default=None,
                    help="decals.wad: map-sheet textures it lacks get no decal id "
                         "(default: every texture counts as present)")
    a = ap.parse_args(argv)
    try:
        if a.check:
            fails, region, dregion = check(a.check, a.defs, a.limits, a.rec_int)
            for f in fails:
                print(f"hleids: FAIL {f}", file=sys.stderr)
            if region and not fails:
                kb = restab.read_limit(a.limits, "HL64_HLE_BLOBS_KB")
                print(f"hleids: HLE region need {region.total} B of HL64_HLE_BLOBS_KB {kb} "
                      f"({kb * 1024} B): {describe(region)}")
            if dregion and not fails:
                kb = restab.read_limit(a.limits, "HL64_DECAL_TEXELS_KB")
                print(f"hleids: decal region need {dregion.total} B of HL64_DECAL_TEXELS_KB {kb} "
                      f"({kb * 1024} B): {decals.describe(dregion)}")
            print(f"hleids: check {a.check}: {'FAIL' if fails else 'ok'}")
            return 1 if fails else 0
        if not a.content or a.maps is None:
            ap.error("rewriting needs --content and --maps")
        ids = Ids.from_table(restab.cook(a.content, a.defs, a.maps), read_keys_def(a.defs))
        in_wad = None
        if a.decals_wad:
            in_wad = decals.wad_lumps(a.decals_wad.read_bytes(), str(a.decals_wad))
        for p in a.hle:
            blob = p.read_bytes()
            tex = decal_textures(blob, p.stem)
            local, _absent = map_decals(tex, in_wad if in_wad is not None else set(tex))
            p.write_bytes(apply(blob, ids.for_map(local), p.stem))
            print(f"hleids: {p}: ids written")
    except (IdError, decals.DecalError, restab.CookError, rtb.RtbError, OSError, ValueError) as e:
        print(f"hleids: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
