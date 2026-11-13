# SPDX-License-Identifier: MIT
"""campaign.rtb: the cooked campaign resource table (docs/formats/rtb-format.md).

Format constants, the writer used by restab.py, and a hostile-input-safe
reader used by routec and the tests. The reader checks every rule of the
format doc (magic, version, sizes, section order, counts against bytes
present, offsets inside their pools, ids inside their tables, CRC) and
raises RtbError(offset, rule) on the first violation; it never raises
anything else for any input bytes.
"""
from __future__ import annotations

import struct
import zlib
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Sequence, Tuple

MAGIC = 0x52544231          # 'RTB1'
VERSION = 1
HEADER_SIZE = 32
DIRENT_SIZE = 16
ALIGN = 16

# res_t = kind << 13 | index
KIND_NONE, KIND_MODEL, KIND_SPRITE, KIND_SOUND, KIND_ANIM, \
    KIND_SENTENCE, KIND_GROUP, KIND_SUBMODEL = range(8)
KIND_NAMES = ("NONE", "MODEL", "SPRITE", "SOUND", "ANIM", "SENTENCE",
              "GROUP", "SUBMODEL")
FILE_KINDS = (KIND_MODEL, KIND_SPRITE, KIND_SOUND, KIND_ANIM)
INDEX_BITS = 13
INDEX_MAX = (1 << INDEX_BITS) - 1      # 8191 ids per kind

RES_F_EXTTEX = 0x01         # model: its texture file is file + 1
RES_F_SEQ_SHIFT = 4         # model: bits 4..7 = seqgroup files that follow

U16_MAX = 0xFFFF

# Sections in file order: (fourcc, element size; 1 = byte pool, 0 = count only).
SECTIONS: Tuple[Tuple[str, int], ...] = (
    # cart: loaded at boot
    ("FILE", 12), ("STRS", 1), ("MAPS", 4), ("KIND", 4), ("RES_", 12),
    ("SENT", 4), ("WORD", 8), ("SGRP", 4), ("PATN", 4), ("PPOL", 1),
    ("DEFS", 2), ("CLAS", 0), ("GLOB", 0),
    # test builds: route VM target resolution
    ("TARG", 8), ("TLMP", 2),
    # host / debug side tables (the cart never loads these)
    ("NMAP", 4), ("NRES", 4), ("NSEN", 4), ("NSGR", 4), ("NCLS", 4),
    ("NTGT", 4), ("NGLB", 4), ("NSTR", 1),
)
SECTION_INDEX = {name: i for i, (name, _) in enumerate(SECTIONS)}
CART_SECTIONS = 13          # FILE .. GLOB
TEST_SECTIONS = 15          # + TARG, TLMP


def res_t(kind: int, index: int) -> int:
    return kind << INDEX_BITS | index


def res_kind(r: int) -> int:
    return r >> INDEX_BITS


def res_index(r: int) -> int:
    return r & INDEX_MAX


def fourcc(s: str) -> int:
    return struct.unpack(">I", s.encode("ascii"))[0]


def align(n: int, a: int = ALIGN) -> int:
    return (n + a - 1) & ~(a - 1)


class RtbError(Exception):
    def __init__(self, offset: int, rule: str):
        super().__init__(f"rtb: offset {offset}: {rule}")
        self.offset = offset
        self.rule = rule


@dataclass
class ResEntry:
    kind: int
    flags: int
    file: int
    offset: int
    size: int


@dataclass
class FileEntry:
    path: str
    size: int
    crc: int


@dataclass
class Word:
    res: int
    pitch: int = 100
    volume: int = 100
    start: int = 0
    end: int = 100
    time: int = 0


@dataclass
class Table:
    """In-memory form of a campaign.rtb (writer input, reader output)."""
    cwg_crc: int = 0
    files: List[FileEntry] = field(default_factory=list)
    map_names: List[str] = field(default_factory=list)
    map_files: List[int] = field(default_factory=list)
    res: List[ResEntry] = field(default_factory=list)       # kind order
    res_names: List[str] = field(default_factory=list)
    sentences: List[Tuple[int, int]] = field(default_factory=list)  # first, count
    sentence_names: List[str] = field(default_factory=list)
    words: List[Word] = field(default_factory=list)
    groups: List[Tuple[int, int]] = field(default_factory=list)     # first, count
    group_names: List[str] = field(default_factory=list)
    patterns: List[bytes] = field(default_factory=list)   # letter indices 0..25
    # (map, lumps, spawners): the last `spawners` lumps are monstermakers whose
    # children get the name (netname); the others carry it as targetname
    targets: List[Tuple[int, List[int], int]] = field(default_factory=list)
    target_names: List[str] = field(default_factory=list)
    defs: List[int] = field(default_factory=list)          # res_t per res.def line
    class_names: List[str] = field(default_factory=list)
    global_names: List[str] = field(default_factory=list)
    build_id: int = 0

    # ---- counts and lookups (host side) ----
    def kind_count(self, kind: int) -> int:
        if kind in FILE_KINDS:
            return sum(1 for r in self.res if r.kind == kind)
        if kind == KIND_SENTENCE:
            return len(self.sentences)
        if kind == KIND_GROUP:
            return len(self.groups)
        return 0

    def kind_first(self, kind: int) -> int:
        if kind not in FILE_KINDS:
            return 0
        return sum(1 for r in self.res if r.kind < kind)

    def res_of(self, kind: int, name: str) -> Optional[int]:
        """res_t of a named id of any kind, None when absent."""
        if kind in FILE_KINDS:
            first = self.kind_first(kind)
            for i in range(self.kind_count(kind)):
                if self.res_names[first + i] == name:
                    return res_t(kind, i)
        elif kind == KIND_SENTENCE:
            for i, n in enumerate(self.sentence_names):
                if n == name:
                    return res_t(kind, i)
        elif kind == KIND_GROUP:
            for i, n in enumerate(self.group_names):
                if n == name:
                    return res_t(kind, i)
        return None

    def map_id(self, name: str) -> Optional[int]:
        try:
            return self.map_names.index(name)
        except ValueError:
            return None

    def target_id(self, map_id: int, name: str) -> Optional[int]:
        for i, (m, _, _) in enumerate(self.targets):
            if m == map_id and self.target_names[i] == name:
                return i
        return None

    def class_id(self, name: str) -> Optional[int]:
        try:
            return self.class_names.index(name)
        except ValueError:
            return None

    def global_id(self, name: str) -> Optional[int]:
        try:
            return self.global_names.index(name)
        except ValueError:
            return None


# --------------------------------------------------------------------------
# writer

class _Pool:
    """NUL-terminated string pool with de-duplication."""

    def __init__(self) -> None:
        self.data = bytearray()
        self.at: Dict[str, int] = {}

    def add(self, s: str) -> int:
        if s not in self.at:
            raw = s.encode("utf-8")
            if b"\0" in raw:
                raise ValueError(f"string contains NUL: {s!r}")
            self.at[s] = len(self.data)
            self.data += raw + b"\0"
        return self.at[s]


def write(t: Table) -> bytes:
    """Serialise a Table; raises ValueError when a value cannot be encoded."""
    def u16(v: int, what: str) -> int:
        if not 0 <= v <= U16_MAX:
            raise ValueError(f"{what} {v} does not fit u16")
        return v

    strs = _Pool()
    nstr = _Pool()
    body: Dict[str, Tuple[bytes, int]] = {}

    body["FILE"] = (b"".join(struct.pack(">III", strs.add(f.path), f.size, f.crc)
                             for f in t.files), len(t.files))
    body["STRS"] = (bytes(strs.data), len(strs.data))
    body["MAPS"] = (b"".join(struct.pack(">HH", u16(f, "map file"), 0)
                             for f in t.map_files), len(t.map_files))
    kinds = b""
    for k in range(8):
        kinds += struct.pack(">HH", u16(t.kind_count(k), "kind count"),
                             u16(t.kind_first(k), "kind first"))
    body["KIND"] = (kinds, 8)
    body["RES_"] = (b"".join(struct.pack(">BBHII", r.kind, r.flags, u16(r.file, "file"),
                                         r.offset, r.size) for r in t.res), len(t.res))
    body["SENT"] = (b"".join(struct.pack(">HBB", u16(f, "word first"), c, 0)
                             for f, c in t.sentences), len(t.sentences))
    body["WORD"] = (b"".join(struct.pack(">HBBBBBB", w.res, w.pitch, w.volume, w.start,
                                         w.end, w.time, 0) for w in t.words), len(t.words))
    body["SGRP"] = (b"".join(struct.pack(">HH", f, c) for f, c in t.groups), len(t.groups))
    ppol = bytearray()
    patn = b""
    for p in t.patterns:
        patn += struct.pack(">HBB", u16(len(ppol), "pattern pool"), len(p), 0)
        ppol += p
    u16(len(ppol), "pattern pool size")
    body["PATN"] = (patn, len(t.patterns))
    body["PPOL"] = (bytes(ppol), len(ppol))
    tlmp: List[int] = []
    targ = b""
    for m, lumps, spawners in t.targets:
        targ += struct.pack(">HHHH", u16(m, "target map"), u16(len(tlmp), "lump pool"),
                            u16(len(lumps), "lump count"), u16(spawners, "spawners"))
        tlmp += lumps
    u16(len(tlmp), "lump pool size")
    body["TARG"] = (targ, len(t.targets))
    body["TLMP"] = (b"".join(struct.pack(">H", u16(x, "lump")) for x in tlmp), len(tlmp))
    body["DEFS"] = (b"".join(struct.pack(">H", d) for d in t.defs), len(t.defs))
    body["CLAS"] = (b"", len(t.class_names))
    body["GLOB"] = (b"", len(t.global_names))
    for sec, names in (("NMAP", t.map_names), ("NRES", t.res_names),
                       ("NSEN", t.sentence_names), ("NSGR", t.group_names),
                       ("NCLS", t.class_names), ("NTGT", t.target_names),
                       ("NGLB", t.global_names)):
        body[sec] = (b"".join(struct.pack(">I", nstr.add(n)) for n in names), len(names))
    body["NSTR"] = (bytes(nstr.data), len(nstr.data))

    dir_size = DIRENT_SIZE * len(SECTIONS)
    pos = align(HEADER_SIZE + dir_size)
    dirents = b""
    blob = bytearray()
    for name, _ in SECTIONS:
        data, count = body[name]
        dirents += struct.pack(">IIII", fourcc(name), pos, len(data), count)
        blob += data + bytes(align(len(data)) - len(data))
        pos += align(len(data))
    pre = dirents + bytes(align(HEADER_SIZE + dir_size) - HEADER_SIZE - dir_size)
    rest = pre + bytes(blob)
    file_size = HEADER_SIZE + len(rest)
    build_id = zlib.crc32(rest) & 0xFFFFFFFF
    hdr = struct.pack(">IHHIIIHHII", MAGIC, VERSION, 0, build_id, file_size,
                      t.cwg_crc, len(SECTIONS), 0, HEADER_SIZE, 0)
    return hdr + rest


# --------------------------------------------------------------------------
# reader

def _cstr(pool: bytes, pool_ofs: int, off: int, what: str) -> str:
    if not 0 <= off < len(pool):
        raise RtbError(pool_ofs, f"{what}: string offset {off} outside pool")
    end = pool.find(b"\0", off)
    if end < 0:
        raise RtbError(pool_ofs + off, f"{what}: string not NUL-terminated")
    try:
        return pool[off:end].decode("utf-8")
    except UnicodeDecodeError:
        raise RtbError(pool_ofs + off, f"{what}: string not UTF-8") from None


def read(data: bytes) -> Table:
    """Validate and decode a campaign.rtb. Raises RtbError only."""
    n = len(data)
    if n < HEADER_SIZE:
        raise RtbError(0, "file shorter than the header")
    (magic, version, flags, build_id, file_size, cwg_crc, nsect, res0,
     dir_ofs, res1) = struct.unpack_from(">IHHIIIHHII", data, 0)
    if magic != MAGIC:
        raise RtbError(0, "bad magic")
    if version != VERSION:
        raise RtbError(4, f"unsupported version {version}")
    if flags != 0 or res0 != 0 or res1 != 0:
        raise RtbError(6, "reserved header field not zero")
    if file_size != n:
        raise RtbError(12, f"file_size {file_size} != bytes present {n}")
    if nsect != len(SECTIONS):
        raise RtbError(20, f"section count {nsect} != {len(SECTIONS)}")
    if dir_ofs != HEADER_SIZE:
        raise RtbError(24, "directory not right after the header")
    dir_end = HEADER_SIZE + DIRENT_SIZE * nsect
    if dir_end > n:
        raise RtbError(24, "directory runs past the end of the file")
    if zlib.crc32(data[HEADER_SIZE:]) & 0xFFFFFFFF != build_id:
        raise RtbError(8, "build_id (CRC-32 of the body) mismatch")

    secs: Dict[str, Tuple[int, int, int]] = {}
    expect = align(dir_end)
    for i, (name, elsize) in enumerate(SECTIONS):
        at = HEADER_SIZE + DIRENT_SIZE * i
        fcc, ofs, size, count = struct.unpack_from(">IIII", data, at)
        if fcc != fourcc(name):
            raise RtbError(at, f"section {i} is not {name}")
        if ofs != expect:
            raise RtbError(at + 4, f"{name}: offset {ofs} != expected {expect}")
        if ofs + size > n:
            raise RtbError(at + 8, f"{name}: runs past the end of the file")
        want = count * elsize if elsize else 0
        if size != want:
            raise RtbError(at + 8, f"{name}: size {size} != count {count} x {elsize}")
        if any(data[ofs + size:align(ofs + size)]):
            raise RtbError(ofs + size, f"{name}: padding not zero")
        secs[name] = (ofs, size, count)
        expect = align(ofs + size)
    if expect != n:
        raise RtbError(expect, "bytes after the last section")

    def raw(name: str) -> bytes:
        ofs, size, _ = secs[name]
        return data[ofs:ofs + size]

    def recs(name: str, fmt: str) -> List[tuple]:
        ofs, _, count = secs[name]
        sz = struct.calcsize(fmt)
        return [struct.unpack_from(fmt, data, ofs + i * sz) for i in range(count)]

    def where(name: str, i: int) -> int:
        ofs = secs[name][0]
        return ofs + i * dict(SECTIONS)[name]

    t = Table(cwg_crc=cwg_crc, build_id=build_id)
    strs = raw("STRS")
    if strs and strs[-1] != 0:
        raise RtbError(secs["STRS"][0], "STRS does not end with NUL")
    for i, (p, size, crc) in enumerate(recs("FILE", ">III")):
        path = _cstr(strs, secs["STRS"][0], p, f"FILE[{i}]")
        if not path or path.startswith("/") or ".." in path.split("/"):
            raise RtbError(where("FILE", i), f"FILE[{i}]: bad path {path!r}")
        t.files.append(FileEntry(path, size, crc))
    nfiles = len(t.files)

    for i, (f, pad) in enumerate(recs("MAPS", ">HH")):
        if f >= nfiles or pad:
            raise RtbError(where("MAPS", i), f"MAPS[{i}]: file {f} out of range")
        t.map_files.append(f)

    kinds = recs("KIND", ">HH")
    if len(kinds) != 8:
        raise RtbError(secs["KIND"][0], "KIND must have 8 entries")
    res = recs("RES_", ">BBHII")
    first_expect = 0
    for k, (count, first) in enumerate(kinds):
        if count > INDEX_MAX + 1:
            raise RtbError(where("KIND", k), f"kind {k}: count {count} > 8192")
        if k in FILE_KINDS:
            if first != first_expect:
                raise RtbError(where("KIND", k), f"kind {k}: first {first} != {first_expect}")
            first_expect += count
        elif first != 0 or (k in (KIND_NONE, KIND_SUBMODEL) and count != 0):
            raise RtbError(where("KIND", k), f"kind {k}: must be count-only")
    if first_expect != len(res):
        raise RtbError(secs["RES_"][0], "RES_ count != sum of file-kind counts")
    for i, (kind, flags, f, off, size) in enumerate(res):
        k_ok = kinds[kind][1] <= i < kinds[kind][1] + kinds[kind][0] \
            if kind in FILE_KINDS else False
        if not k_ok:
            raise RtbError(where("RES_", i), f"RES_[{i}]: kind {kind} outside its range")
        extra = (1 if flags & RES_F_EXTTEX else 0) + (flags >> RES_F_SEQ_SHIFT)
        if flags & 0x0E or (kind != KIND_MODEL and extra):
            raise RtbError(where("RES_", i), f"RES_[{i}]: bad flags {flags:#x}")
        if f + extra >= nfiles:
            raise RtbError(where("RES_", i), f"RES_[{i}]: file {f} (+{extra}) out of range")
        if off + size > t.files[f].size:
            raise RtbError(where("RES_", i), f"RES_[{i}]: range outside file {f}")
        t.res.append(ResEntry(kind, flags, f, off, size))

    words = recs("WORD", ">HBBBBBB")
    nsound = kinds[KIND_SOUND][0]
    for i, (r, pitch, vol, start, end, tm, pad) in enumerate(words):
        if res_kind(r) != KIND_SOUND or res_index(r) >= nsound:
            raise RtbError(where("WORD", i), f"WORD[{i}]: res {r:#x} is not a sound")
        if pad or pitch == 0 or start > 100 or end > 100 or tm > 100:
            raise RtbError(where("WORD", i), f"WORD[{i}]: modifier out of range")
        t.words.append(Word(r, pitch, vol, start, end, tm))
    for i, (wf, wc, fl) in enumerate(recs("SENT", ">HBB")):
        if fl or wc == 0 or wf + wc > len(words):
            raise RtbError(where("SENT", i), f"SENT[{i}]: bad word range")
        t.sentences.append((wf, wc))
    if kinds[KIND_SENTENCE][0] != len(t.sentences):
        raise RtbError(where("KIND", KIND_SENTENCE), "sentence count != SENT")
    nxt = 0
    for i, (gf, gc) in enumerate(recs("SGRP", ">HH")):
        if gf != nxt or gc == 0 or gf + gc > len(t.sentences):
            raise RtbError(where("SGRP", i), f"SGRP[{i}]: groups must tile SENT")
        t.groups.append((gf, gc))
        nxt = gf + gc
    if nxt != len(t.sentences):
        raise RtbError(secs["SGRP"][0], "SGRP does not cover every sentence")
    if kinds[KIND_GROUP][0] != len(t.groups):
        raise RtbError(where("KIND", KIND_GROUP), "group count != SGRP")

    ppol = raw("PPOL")
    if any(b > 25 for b in ppol):
        raise RtbError(secs["PPOL"][0], "PPOL holds a letter index > 25")
    for i, (po, ln, pad) in enumerate(recs("PATN", ">HBB")):
        if pad or ln == 0 or po + ln > len(ppol):
            raise RtbError(where("PATN", i), f"PATN[{i}]: bad pool range")
        t.patterns.append(bytes(ppol[po:po + ln]))

    tlmp = [x[0] for x in recs("TLMP", ">H")]
    prev_map = 0
    for i, (m, lf, lc, ns) in enumerate(recs("TARG", ">HHHH")):
        if m >= len(t.map_files) or m < prev_map or lc == 0 or lf + lc > len(tlmp) or ns > lc:
            raise RtbError(where("TARG", i), f"TARG[{i}]: bad map, lump or spawner range")
        prev_map = m
        t.targets.append((m, tlmp[lf:lf + lc], ns))

    for i, (d,) in enumerate(recs("DEFS", ">H")):
        k = res_kind(d)
        if k in (KIND_NONE, KIND_ANIM, KIND_SUBMODEL) or res_index(d) >= kinds[k][0]:
            raise RtbError(where("DEFS", i), f"DEFS[{i}]: res {d:#x} not in the table")
        t.defs.append(d)

    nstr = raw("NSTR")
    if nstr and nstr[-1] != 0:
        raise RtbError(secs["NSTR"][0], "NSTR does not end with NUL")
    expect_counts = {"NMAP": len(t.map_files), "NRES": len(t.res),
                     "NSEN": len(t.sentences), "NSGR": len(t.groups),
                     "NCLS": secs["CLAS"][2], "NTGT": len(t.targets),
                     "NGLB": secs["GLOB"][2]}
    names: Dict[str, List[str]] = {}
    for sec, cnt in expect_counts.items():
        if secs[sec][2] != cnt:
            raise RtbError(secs[sec][0], f"{sec}: count {secs[sec][2]} != {cnt}")
        names[sec] = [_cstr(nstr, secs["NSTR"][0], o, f"{sec}[{i}]")
                      for i, (o,) in enumerate(recs(sec, ">I"))]
    t.map_names = names["NMAP"]
    t.res_names = names["NRES"]
    t.sentence_names = names["NSEN"]
    t.group_names = names["NSGR"]
    t.class_names = names["NCLS"]
    t.target_names = names["NTGT"]
    t.global_names = names["NGLB"]
    if len(set(t.map_names)) != len(t.map_names):
        raise RtbError(secs["NMAP"][0], "duplicate map name")
    if len(set(t.class_names)) != len(t.class_names):
        raise RtbError(secs["NCLS"][0], "duplicate class name")
    seen = set()
    for i, (m, _, _) in enumerate(t.targets):
        key = (m, t.target_names[i])
        if key in seen:
            raise RtbError(where("NTGT", i), f"duplicate target {key}")
        seen.add(key)
    return t
