# SPDX-License-Identifier: MIT
"""studiocook: Half-Life studio models -> hl64 cooked studio files
(docs/formats/studio-format.md; architecture.md 9.4, D3(b)).

Per model `models/<x>.mdl` (+ `<x>t.mdl`, `<x>NN.mdl`): `models/<x>.s64`
(info block + draw mesh) and `models/<x>.a64` (per-sequence animation
chunks). The MDL is read by tools.hlsdk.studio_mdl (HLSDK layouts).

Draw mesh design after the old tree's STB2 v4 bake (tools/studio_bake,
provenance-audit.md entries tools/studio_bake/bake_studio.c and
source/studio_bake.h, CLEAN): bone-local vertices, per texture greedy
meshlets of <= 64 unique vertices sorted by bone (one load per bone run
under the bone's matrix), CI4 skins. hl64 keeps every body-part submodel
and skin family, welds identical corners and does not simplify.

  python3 -m tools.studio.studiocook --content DIR [--report]
        cook every staged models/*.mdl of DIR in place
  python3 -m tools.studio.studiocook --src DIR -o OUT name.mdl ...
        cook the named models of DIR into OUT/models/
"""
from __future__ import annotations

import argparse
import os
import struct
import sys
import zlib
from typing import Callable, Dict, List, Optional, Tuple

from tools.cook.bake_world_tex import build_ci4
from tools.hlsdk import studio_mdl as mdl
from tools.studio import s64
from tools.w64c.w64c import pack_normal

MESHLET_VERTS = 64
POS_SHIFT_MAX = 4


class CookError(Exception):
    pass


def pad16(b: bytearray) -> None:
    b.extend(b"\0" * (-len(b) % 16))


def pad4(b: bytearray) -> None:
    b.extend(b"\0" * (-len(b) % 4))


def s16(v: float) -> int:
    x = int(round(v))
    if x < -32768 or x > 32767:
        raise CookError(f"value {v} outside s16")
    return x


# ---- info block ---------------------------------------------------------------

class Strings:
    def __init__(self):
        self.pool = bytearray(b"\0")
        self.at: Dict[str, int] = {"": 0}

    def add(self, s: str) -> int:
        if s not in self.at:
            if len(self.pool) + len(s) + 1 > 0xFFFF:
                raise CookError("string pool over 64 KB")
            self.at[s] = len(self.pool)
            self.pool += s.encode("latin-1") + b"\0"
        return self.at[s]


def cook_info(m: mdl.Model, anim_ranges: List[Tuple[int, int]], sub_first: List[int],
              nsub: int) -> bytes:
    if len(m.bones) > 128 or len(m.controllers) > 8 or len(m.hitboxes) > 255 \
            or len(m.attachments) > 255 or len(m.bodyparts) > 255 \
            or m.numtransitions > 255 or len(m.seqs) > 0xFFFF:
        raise CookError("model counts outside the info block's fields")
    strs = Strings()
    bone = bytearray()
    for i, b in enumerate(m.bones):
        if b.parent >= i or b.parent < -1:
            raise CookError(f"bone {i} parent {b.parent}")
        bone += struct.pack(">b6bB6f6f", b.parent, *b.bonecontroller, b.flags & 0xFF,
                            *b.value, *b.scale)
    ctl = bytearray()
    for c in m.controllers:
        ctl += struct.pack(">bBHffB3x", c.bone, c.index & 0xFF, c.type & 0xFFFF, c.start,
                           c.end, c.rest & 0xFF)
    hb = bytearray()
    for h in m.hitboxes:
        hb += struct.pack(">BBH3f3f", h.bone, h.group & 0xFF, 0, *h.bbmin, *h.bbmax)
    att = bytearray()
    for a in m.attachments:
        att += struct.pack(">B3x3f", a.bone, *a.org)
    seq = bytearray()
    ev = bytearray()
    nev = 0
    for i, s in enumerate(m.seqs):
        if len(s.events) > 0xFFFF or nev > 0xFFFF:
            raise CookError("too many events")
        seq += struct.pack(">HBBfIhHHHHBBI3f3f3f2H2f2fBxH II",
                           strs.add(s.label), max(1, s.numblends), s.motionbone & 0xFF,
                           s.fps, s.flags & 0xFFFFFFFF, s.activity, s.actweight & 0xFFFF,
                           s.numframes, len(s.events), nev, s.entrynode & 0xFF,
                           s.exitnode & 0xFF, s.motiontype & 0xFFFFFFFF, *s.linearmovement,
                           *s.bbmin, *s.bbmax, s.blendtype[0] & 0xFFFF,
                           s.blendtype[1] & 0xFFFF, *s.blendstart, *s.blendend,
                           s.nodeflags & 0xFF, s.nextseq & 0xFFFF, *anim_ranges[i])
        for e in s.events:
            ev += struct.pack(">iiHH", e.frame, e.event, strs.add(e.options), 0)
            nev += 1
    bp = bytearray()
    for p, part in enumerate(m.bodyparts):
        bp += struct.pack(">HHHH", len(part.models), part.base & 0xFFFF, sub_first[p], 0)
    assert len(seq) == 96 * len(m.seqs)

    out = bytearray(s64.INFO_HDR)
    ofs = {}
    for key, blob in (("bone", bone), ("ctl", ctl), ("hitbox", hb), ("attach", att),
                      ("seq", seq), ("event", ev), ("bodypart", bp),
                      ("trans", bytes(m.transitions))):
        pad4(out)
        ofs[key] = len(out)
        out += blob
    pad4(out)
    ofs["str"] = len(out)
    out += strs.pool
    str_size = len(strs.pool)
    pad16(out)
    struct.pack_into(">IBBBBHBBI3f3f3f3f3fHH9I3I", out, 0, s64.INFO_MAGIC, len(m.bones),
                     len(m.controllers), len(m.hitboxes), len(m.attachments), len(m.seqs),
                     len(m.bodyparts), m.numtransitions, m.flags & 0xFFFFFFFF,
                     *m.eyeposition, *m.min, *m.max, *m.bbmin, *m.bbmax, nev, nsub,
                     ofs["bone"], ofs["ctl"], ofs["hitbox"], ofs["attach"], ofs["seq"],
                     ofs["event"], ofs["bodypart"], ofs["trans"], ofs["str"], str_size, 0, 0)
    return bytes(out)


# ---- animation chunks -----------------------------------------------------------

def cook_anim(m: mdl.Model) -> Tuple[bytes, List[Tuple[int, int]], int]:
    out = bytearray(16)
    ranges = []
    biggest = 0
    nb = len(m.bones)
    for s in m.seqs:
        pad16(out)
        start = len(out)
        nblend = max(1, s.numblends)
        table = [0] * (nblend * nb * 6)
        words: List[int] = []
        base = len(table)
        for bl in range(nblend):
            for bi in range(nb):
                for ch in range(6):
                    vals = s.anim[bl][bi][ch]
                    if vals is None:
                        continue
                    table[(bl * nb + bi) * 6 + ch] = base + len(words)
                    words.extend(vals)
        if base + len(words) > 0xFFFF:
            raise CookError(f"sequence {s.label}: animation chunk over 65535 words")
        chunk = struct.pack(">%dH" % len(table), *table) + \
            struct.pack(">%dh" % len(words), *words)
        out += chunk
        size = len(chunk)
        size += -size % 16
        biggest = max(biggest, size)
        ranges.append((start, size))
    pad16(out)
    struct.pack_into(">IHHII", out, 0, s64.ANIM_MAGIC, len(m.seqs), nb, len(out), 0)
    return bytes(out), ranges, biggest


# ---- draw mesh -------------------------------------------------------------------

def tex_side(t: mdl.Texture) -> Tuple[int, int]:
    def p2(v):
        p = 1
        while p * 2 <= min(v, 64):
            p *= 2
        return p
    return p2(t.width), p2(t.height)


def cook_mesh(m: mdl.Model) -> Tuple[bytes, List[int], int, Dict[str, int]]:
    if not m.textures:
        raise CookError("no textures")
    nskinref = len(m.skinref[0]) if m.skinref else 0
    # textures in use (every family's), dense
    used_tex: Dict[int, int] = {}
    for fam in m.skinref:
        for t in fam:
            if not (0 <= t < len(m.textures)):
                raise CookError(f"skin table names texture {t}")
            used_tex.setdefault(t, len(used_tex))
    chrome_model = any(m.textures[t].flags & mdl.STUDIO_NF_CHROME for t in used_tex)

    # corners and triangles per (submodel, skinref)
    maxc = 0.0
    for part in m.bodyparts:
        for sm in part.models:
            for v in sm.verts:
                maxc = max(maxc, *(abs(x) for x in v))
    shift = POS_SHIFT_MAX
    while shift > 0 and maxc * (1 << shift) > 32000:
        shift -= 1
    if maxc * (1 << shift) > 32000:
        raise CookError(f"vertex extent {maxc} too large")
    scale = float(1 << shift)

    corners: List[tuple] = []        # (bone, pos3, nrm, st2, nbone)
    corner_at: Dict[tuple, int] = {}
    subs: List[List[Tuple[int, List[Tuple[int, int, int]]]]] = []
    sub_first: List[int] = []
    bones_used = set()
    for part in m.bodyparts:
        sub_first.append(len(subs))
        for sm in part.models:
            runs: Dict[int, List[Tuple[int, int, int]]] = {}
            for mesh in sm.meshes:
                sref = mesh.skinref
                if not (0 <= sref < nskinref):
                    raise CookError(f"mesh skinref {sref} outside {nskinref}")
                tex = m.textures[m.skinref[0][sref]]
                pw, ph = tex_side(tex)
                chrome = tex.flags & mdl.STUDIO_NF_CHROME
                tl = runs.setdefault(sref, [])
                for strip, pts in mesh.tricmds:
                    ids = []
                    for vi, ni, ss, tt in pts:
                        if not (0 <= vi < len(sm.verts)):
                            raise CookError(f"vertex index {vi} outside {len(sm.verts)}")
                        bone = sm.vertbone[vi]
                        if bone >= len(m.bones):
                            raise CookError(f"vertex bone {bone}")
                        if 0 <= ni < len(sm.norms):
                            n = sm.norms[ni]
                            nb = sm.normbone[ni] if sm.normbone[ni] < len(m.bones) else bone
                        else:
                            n, nb = (0.0, 0.0, 1.0), bone
                        pos = tuple(s16(x * scale) for x in sm.verts[vi])
                        if chrome:
                            st = (pw * 16, ph * 16)
                        else:
                            st = (s16(ss * pw / tex.width * 32), s16(tt * ph / tex.height * 32))
                        key = (bone, pos, pack_normal(*n), st, nb if chrome_model else 0)
                        c = corner_at.get(key)
                        if c is None:
                            c = corner_at[key] = len(corners)
                            corners.append(key)
                        bones_used.add(bone)
                        ids.append(c)
                    for k in range(len(ids) - 2):
                        if strip:
                            tri = (ids[k + 1], ids[k], ids[k + 2]) if k & 1 else \
                                (ids[k], ids[k + 1], ids[k + 2])
                        else:
                            tri = (ids[0], ids[k + 1], ids[k + 2])
                        if len(set(tri)) == 3:
                            tl.append(tri)
            subs.append(sorted(runs.items()))
    bslots = sorted(bones_used)
    slot_of = {b: i for i, b in enumerate(bslots)}

    run_rec, meshlet_rec, load_rec = bytearray(), bytearray(), bytearray()
    sub_rec = bytearray()
    verts = bytearray()
    tris = bytearray()
    nbone = bytearray()
    nslots = nruns = nmesh = nloads = ntris = 0

    def put_pair(a, b):
        nonlocal verts
        ca, cb = corners[a], corners[b]
        verts += struct.pack(">3hH3hHII2h2h", *ca[1], ca[2], *cb[1], cb[2],
                             0xFFFFFFFF, 0xFFFFFFFF, *ca[3], *cb[3])

    for runs in subs:
        sub_rec += struct.pack(">HH", nruns, len([r for r in runs if r[1]]))
        for sref, tl in runs:
            if not tl:
                continue
            first_ml = nmesh
            left = list(tl)
            while left:
                cur: List[int] = []
                cset = set()
                take, rest = [], []
                for t in left:
                    new = [c for c in t if c not in cset]
                    if len(cur) + len(set(new)) <= MESHLET_VERTS:
                        for c in new:
                            if c not in cset:
                                cset.add(c)
                                cur.append(c)
                        take.append(t)
                    else:
                        rest.append(t)
                left = rest
                order = sorted(range(len(cur)), key=lambda i: (slot_of[corners[cur[i]][0]], i))
                cur = [cur[i] for i in order]
                cache = {c: i for i, c in enumerate(cur)}
                load_first = nloads
                i = 0
                while i < len(cur):
                    j = i
                    b = corners[cur[i]][0]
                    while j < len(cur) and corners[cur[j]][0] == b:
                        j += 1
                    cnt = j - i
                    load_rec += struct.pack(">BBBx", slot_of[b], i, cnt)
                    nloads += 1
                    seg = cur[i:j] + ([cur[j - 1]] if cnt & 1 else [])
                    for k in range(0, len(seg), 2):
                        put_pair(seg[k], seg[k + 1])
                        if chrome_model:
                            nbone += bytes((corners[seg[k]][4], corners[seg[k + 1]][4]))
                    nslots += len(seg)
                    i = j
                for t in take:
                    tris += bytes(cache[c] for c in t)
                meshlet_rec += struct.pack(">HBBHH", load_first, nloads - load_first, len(cur),
                                           ntris, len(take))
                ntris += len(take)
                nmesh += 1
            run_rec += struct.pack(">HHHH", sref, first_ml, nmesh - first_ml, 0)
            nruns += 1
    if nslots > 0xFFFF or ntris > 0xFFFF or nmesh > 0xFFFF or nloads > 0xFFFF:
        raise CookError("mesh tables over 65535 entries")

    # skins
    tex_list = sorted(used_tex, key=lambda t: used_tex[t])
    skin_rec = bytearray()
    for fam in m.skinref:
        skin_rec += struct.pack(">%dH" % nskinref, *(used_tex[t] for t in fam))
    tex_blobs = []
    for t in tex_list:
        tx = m.textures[t]
        fence = bool(tx.flags & mdl.STUDIO_NF_MASKED)
        ci4, pw, ph, _, _, tlut = build_ci4(tx.pixels, tx.width, tx.height, tx.palette, fence)
        blob = bytearray(struct.pack(">HHB3xII", pw, ph, 1 if fence else 0, 48,
                                     tx.flags & 0xFFFFFFFF))
        blob += struct.pack(">16H", *tlut)
        blob += ci4
        pad16(blob)
        tex_blobs.append(bytes(blob))

    out = bytearray(s64.MESH_HDR)
    ofs = {}
    for key, blob, align in (("bslot", bytes(bslots), 4), ("sub", sub_rec, 4),
                             ("run", run_rec, 4), ("meshlet", meshlet_rec, 4),
                             ("load", load_rec, 4), ("vert", verts, 16), ("tri", tris, 4),
                             ("skin", skin_rec, 4)):
        out.extend(b"\0" * (-len(out) % align))
        ofs[key] = len(out)
        out += blob
    pad16(out)
    ofs["tex"] = len(out)
    table = bytearray(4 * len(tex_blobs))
    out += table
    pad16(out)
    for i, blob in enumerate(tex_blobs):
        struct.pack_into(">I", out, ofs["tex"] + 4 * i, len(out))
        out += blob
    ofs["nbone"] = 0
    if chrome_model:
        ofs["nbone"] = len(out)
        out += nbone
    pad16(out)
    struct.pack_into(">IHHHHHHBBBBHH10I", out, 0, s64.MESH_MAGIC, len(subs), nruns, nmesh,
                     nloads, nslots, ntris, len(bslots), shift, nskinref, len(m.skinref),
                     len(tex_blobs), 0, ofs["bslot"], ofs["sub"], ofs["run"], ofs["meshlet"],
                     ofs["load"], ofs["vert"], ofs["tri"], ofs["skin"], ofs["tex"], ofs["nbone"])
    stats = {"corners": len(corners), "slots": nslots, "tris": ntris, "meshlets": nmesh,
             "loads": nloads, "bslots": len(bslots), "tex": len(tex_blobs)}
    return bytes(out), sub_first, len(subs), stats


# ---- model ---------------------------------------------------------------------------

def cook_model(path_of: Callable[[str], Optional[bytes]], name: str):
    base = os.path.basename(name)[:-4]
    try:
        m = mdl.read(path_of, name)
        mesh, sub_first, nsub, st = cook_mesh(m)
        anim, ranges, biggest = cook_anim(m)
        info = cook_info(m, ranges, sub_first, nsub)
    except (mdl.MdlError, CookError, struct.error) as e:
        raise CookError(f"{name}: {e}") from None
    flags = 0
    if any(t.flags & mdl.STUDIO_NF_CHROME for t in m.textures):
        flags |= s64.F_CHROME
    if base.lower().startswith("v_"):
        flags |= s64.F_VIEW
    hdr = bytearray(64)
    struct.pack_into(">IHHIIIIII32s", hdr, 0, s64.MODEL_MAGIC, s64.VERSION, flags, 64,
                     len(info), 64 + len(info), len(mesh), len(anim),
                     zlib.crc32(anim) & 0xFFFFFFFF, base.encode("latin-1")[:31])
    model = bytes(hdr) + info + mesh
    s64.read_model(model)
    s64.read_anim(anim, s64.read_model(model))
    st.update(info=len(info), mesh=len(mesh), anim=len(anim), chunk_max=biggest,
              seqs=len(m.seqs), bones=len(m.bones))
    return model, anim, st


def is_companion(data: bytes) -> bool:
    """IDSQ sequence-group files and bone-less IDST texture files."""
    if len(data) < 8:
        return True
    ident = struct.unpack_from("<i", data, 0)[0]
    if ident == mdl.IDSEQGRPHEADER:
        return True
    if ident != mdl.IDSTUDIOHEADER or len(data) < 0xA0:
        return True
    return struct.unpack_from("<i", data, 0x8C)[0] == 0     # numbones


def lookup(src: str) -> Callable[[str], Optional[bytes]]:
    """Case-insensitive file lookup under src (relative paths)."""
    index: Dict[str, str] = {}
    for root, _, files in os.walk(src):
        for f in files:
            p = os.path.join(root, f)
            index.setdefault(os.path.relpath(p, src).replace(os.sep, "/").lower(), p)

    def get(rel: str) -> Optional[bytes]:
        p = index.get(rel.lower())
        if p is None:
            return None
        with open(p, "rb") as f:
            return f.read()
    get.index = index      # type: ignore[attr-defined]
    return get


def write(path: str, data: bytes) -> None:
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, path)


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--content", help="staged content directory: cook models/*.mdl in place")
    ap.add_argument("--src", help="directory holding models/ (with -o and names)")
    ap.add_argument("-o", "--out", help="output root for --src")
    ap.add_argument("--report", action="store_true", help="one size line per model")
    ap.add_argument("names", nargs="*", help="models/<x>.mdl names (--src)")
    a = ap.parse_args(argv)
    if bool(a.content) == bool(a.src):
        ap.error("give --content DIR, or --src DIR -o OUT names")
    src = a.content or a.src
    out_root = a.content or a.out
    if not out_root:
        ap.error("--src needs -o")
    get = lookup(src)
    if a.content:
        names = []
        for rel, p in sorted(get.index.items()):        # type: ignore[attr-defined]
            if rel.startswith("models/") and rel.endswith(".mdl") and rel.count("/") == 1:
                with open(p, "rb") as f:
                    if not is_companion(f.read(0xA0)):
                        names.append(os.path.relpath(p, src).replace(os.sep, "/"))
    else:
        names = a.names
    os.makedirs(os.path.join(out_root, "models"), exist_ok=True)
    total = 0
    try:
        for n in names:
            model, anim, st = cook_model(get, n)
            base = os.path.join(out_root, "models", os.path.basename(n)[:-4].lower())
            write(base + ".s64", model)
            write(base + ".a64", anim)
            total += len(model) + len(anim)
            if a.report:
                print("studiocook: %s info=%d mesh=%d anim=%d chunk_max=%d seqs=%d bones=%d "
                      "bslots=%d corners=%d slots=%d tris=%d meshlets=%d loads=%d tex=%d" % (
                          n, st["info"], st["mesh"], st["anim"], st["chunk_max"], st["seqs"],
                          st["bones"], st["bslots"], st["corners"], st["slots"], st["tris"],
                          st["meshlets"], st["loads"], st["tex"]))
    except CookError as e:
        print(f"studiocook: {e}", file=sys.stderr)
        return 1
    print(f"studiocook: {len(names)} models, {total} B")
    return 0


if __name__ == "__main__":
    sys.exit(main())
