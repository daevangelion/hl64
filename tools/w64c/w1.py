# SPDX-License-Identifier: MIT
"""W1 runner (docs/specs/world-surfaces.md 5): the rewritten world cook must
produce byte-identical packages to the old quakegeneric cook for the 10
Uplink / Hazard Course maps.

Fixtures (local, never committed; built by RUNNING the old tools, black-box):

  python3 -m tools.w64c.w1 --make-fixtures --old-tree ~/src/ports/quakegeneric \
      --data /scratch/hl1/valve --fixtures /scratch/hl64/fixtures/w1 \
      [--old-cook /scratch/hl64/tmp/CookImport/old]

  per map: <map>.wci4 (the old cook's texture pack, copied from
  OLD_COOK/stage/maps), <map>.hle (the old hlecook run on the BSP, or the old
  cook's copy when the binary is absent) and <map>.w64 (the old w64c.py run
  on those with the retail sound/materials.txt and without the ZONE / DRYS /
  STRM companions, so layouts compare 1:1); palette.lmp (the old cook's
  palette, OLD_COOK/fs/id1/gfx); manifest.json (sizes, CRCs, commands).

Compare (default):

  python3 -m tools.w64c.w1 --fixtures DIR --data /scratch/hl1/valve \
      [--hlecook PATH] [--w64c-only] [--maps m1,m2]

  cooks every map with the NEW tools into a temporary directory:
  tools.cook.bake_world_tex --ref-palette DIR/palette.lmp and
  tools.w64c.w64c (from the fixture .hle, so ENTS is the old blob and every
  other part is W1's), byte-comparing .wci4 / .w64 with the fixtures; with
  --hlecook PATH also the new hlecook, whose HLN1 node graph tail must equal
  the fixture's (its HLE1 part differs on purpose: the HLSDK entvars
  retarget, hle-format-v1.md "Cook rules"; reported, not counted).
  --w64c-only cooks only the .w64, from the fixture .wci4 and .hle. The .w64
  compares as version 2 (v2_view: the additive ramps removed); a map whose
  translucent func_wall / func_illusionary version 3 no longer folds is
  cooked again under the old fold rule (cook_v2_fold), which must give the
  fixture bytes, and the only submodel kind change allowed is FOLDED ->
  MOVR (listed). A .w64 mismatch names the first differing part: header,
  directory table, or chunk (fourcc, index). Exit 1 on any mismatch or
  failed tool.
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MAPS = ("hldemo1", "hldemo2", "hldemo3", "t0a0", "t0a0a", "t0a0b", "t0a0b1",
        "t0a0b2", "t0a0c", "t0a0d")
MATERIALS = Path("sound") / "materials.txt"
W64_HDR = 64
# w64_dir_t table offsets (w64-format.md 'Layer directory'): name, u32 field
# offset in the directory header
DIR_TABLES = (("chunk directory", 16), ("sectors", 20), ("page pool", 24),
              ("textures", 28), ("submodels", 32), ("leaf vis", 36),
              ("vis pool", 40), ("models", 56), ("leaf contents", 60),
              ("grid", 64), ("materials", 68))


def fail(msg: str) -> None:
    print(f"w1: error: {msg}", file=sys.stderr)
    sys.exit(1)


def run(cmd, what: str) -> str:
    """Run a tool; its stderr is returned (warnings), a failure is fatal
    for this map and reported by the caller."""
    p = subprocess.run([str(c) for c in cmd], cwd=ROOT, capture_output=True, text=True)
    if p.returncode != 0:
        raise RuntimeError(f"{what} failed ({p.returncode}): {p.stderr.strip()[-400:]}")
    return p.stderr


def w64_parts(b: bytes):
    """(header, directory header, [(table name, bytes)], [(fourcc, bytes)]);
    ValueError when the package does not parse."""
    if len(b) < W64_HDR or b[:4] != b"W64L":
        raise ValueError("not a W64L package")
    dofs, dsz, nch = struct.unpack_from(">III", b, 12)
    if dofs + dsz > len(b) or dsz < 96:
        raise ValueError("directory outside the file")
    d = b[dofs:dofs + dsz]
    offs = []
    for name, fo in DIR_TABLES:
        o = struct.unpack_from(">I", d, fo)[0]
        if o:
            offs.append((o, name))
    offs.sort()
    tables = []
    for i, (o, name) in enumerate(offs):
        end = offs[i + 1][0] if i + 1 < len(offs) else dsz
        tables.append((name, d[o:end]))
    cdo = struct.unpack_from(">I", d, 16)[0]
    if cdo + 16 * nch > dsz:
        raise ValueError("chunk directory outside the directory")
    chunks = []
    for i in range(nch):
        fc, o, n, _crc = struct.unpack_from(">4sIII", d, cdo + 16 * i)
        if o + n > len(b):
            raise ValueError(f"chunk {i} outside the file")
        chunks.append((fc.decode("latin1"), b[o:o + n]))
    return b[:W64_HDR], d[:96], tables, chunks


def w64_first_diff(new: bytes, old: bytes) -> str:
    """Where two packages first differ."""
    try:
        hn, dn, tn, cn = w64_parts(new)
        ho, do, to, co = w64_parts(old)
    except ValueError as e:
        return f"unparsable package ({e})"
    seen = {}
    for i, ((fa, a), (fb, b)) in enumerate(zip(cn, co)):
        k = seen.get(fb, 0)
        seen[fb] = k + 1
        if fa != fb or a != b:
            return (f"chunk {i} {fb} #{k}: new {fa} {len(a)} B, old {len(b)} B"
                    + (f", first byte {next(j for j in range(min(len(a), len(b))) if a[j] != b[j])}"
                       if fa == fb and len(a) == len(b) else ""))
    if len(cn) != len(co):
        return f"chunk count: new {len(cn)}, old {len(co)}"
    for (na, a), (nb, b) in zip(tn, to):
        if na != nb or a != b:
            return f"directory table {nb}: new {na} {len(a)} B, old {len(b)} B"
    if dn != do:
        return "directory header"
    if hn != ho:
        return "file header"
    return "trailing bytes"


TEXF_RAMP = 16
RAMP_SIZE = 40


def v2_view(b: bytes) -> bytes:
    """A version-3 package as the version-2 package the old cook wrote
    (w64-format.md: version 3 = version 2 + the TEXP additive ramps): every
    RAMP page loses its ramp (flag cleared, CI4 back at 48), the chunks
    are laid out again in order, 16-aligned, from the first chunk's offset,
    and the chunk directory, directory CRC, file size and version follow.
    Anything else (a version-2 or unparsable package) is returned as is."""
    if len(b) < W64_HDR or b[:4] != b"W64L" or struct.unpack_from(">H", b, 4)[0] != 3:
        return b
    dofs, dsz, nch = struct.unpack_from(">III", b, 12)
    d = bytearray(b[dofs:dofs + dsz])
    cdo = struct.unpack_from(">I", d, 16)[0]
    ents = [struct.unpack_from(">4sIII", d, cdo + 16 * i) for i in range(nch)]
    if not ents:
        return b
    out = bytearray(b[:ents[0][1]])
    for i, (fc, o, n, _crc) in enumerate(ents):
        data = b[o:o + n]
        if fc == b"TEXP" and data[4] & TEXF_RAMP:
            w, h = struct.unpack_from(">HH", data, 0)
            head = bytearray(data[:48])
            head[4] &= ~TEXF_RAMP & 0xFF
            struct.pack_into(">I", head, 8, 48)
            data = bytes(head) + data[48 + RAMP_SIZE:48 + RAMP_SIZE + w * h // 2]
            data += b"\0" * (-len(data) % 16)
        out += b"\0" * (-len(out) % 16)
        struct.pack_into(">4sIII", d, cdo + 16 * i, fc, len(out), len(data),
                         zlib.crc32(data) & 0xFFFFFFFF)
        out += data
    out[dofs:dofs + dsz] = d
    struct.pack_into(">H", out, 4, 2)
    struct.pack_into(">I", out, 8, len(out))
    struct.pack_into(">I", out, 24, zlib.crc32(bytes(d)) & 0xFFFFFFFF)
    return bytes(out)


SUB_NONE, SUB_FOLDED, SUB_MOVR = 0, 1, 2


def sub_kinds(b: bytes):
    """The submodel kinds of a package's directory (w64_sub_t.kind)."""
    dofs = struct.unpack_from(">I", b, 12)[0]
    nsub = struct.unpack_from(">H", b, dofs + 8)[0]
    so = dofs + struct.unpack_from(">I", b, dofs + 32)[0]
    return [struct.unpack_from(">H", b, so + 16 * i)[0] for i in range(nsub)]


def cook_v2_fold(argv) -> int:
    """tools.w64c.w64c with the old cook's fold rule: rendermode ignored, so
    translucent func_wall / func_illusionary fold as version 2 did (the one
    by-design change of version 3's sectors, w64-format.md 'FOLDED')."""
    from tools.w64c import w64c as W
    kinds = W.submodel_kinds
    W.submodel_kinds = lambda ents, n: kinds(
        [{k: v for k, v in e.items() if k != "rendermode"} for e in ents], n)
    return W.main(argv)


def hle_split(b: bytes):
    """(HLE1 part, HLN1 tail) of an hlecook blob (hle-format-v1.md: the node
    graph starts at align16 of the HLE1 header's little-endian size)."""
    if len(b) < 12 or b[:4] != b"HLE1":
        raise RuntimeError("not an HLE1 blob")
    size = struct.unpack_from("<I", b, 8)[0]
    if size > len(b):
        raise RuntimeError("HLE1 size past the blob")
    return b[:size], b[(size + 15) & ~15:]


def make_fixtures(a) -> int:
    old = Path(a.old_tree).expanduser()
    oc = Path(a.old_cook)
    w64c_old = old / "tools" / "w64c" / "w64c.py"
    hlecook_old = old / "tools" / "w64c" / "hlecook"
    if not w64c_old.is_file():
        fail(f"{w64c_old}: missing")
    fx = Path(a.fixtures)
    fx.mkdir(parents=True, exist_ok=True)
    mats = Path(a.data) / MATERIALS
    pal = oc / "fs" / "id1" / "gfx" / "palette.lmp"
    if not pal.is_file():
        fail(f"{pal}: missing (run the old cook first)")
    shutil.copyfile(pal, fx / "palette.lmp")
    manifest = {"old_tree": str(old), "old_cook": str(oc), "materials": str(mats),
                "maps": {}}
    for m in a.maps:
        bsp = Path(a.data) / "maps" / f"{m}.bsp"
        src_wci4 = oc / "stage" / "maps" / f"{m}.wci4"
        if not bsp.is_file() or not src_wci4.is_file():
            fail(f"{m}: {bsp} or {src_wci4} missing")
        shutil.copyfile(src_wci4, fx / f"{m}.wci4")
        if os.access(hlecook_old, os.X_OK):
            hle_cmd = [hlecook_old, bsp, fx / f"{m}.hle"]
            run(hle_cmd, f"{m}: old hlecook")
        else:
            hle_cmd = ["copy", oc / "stage" / "maps" / f"{m}.hle"]
            shutil.copyfile(hle_cmd[1], fx / f"{m}.hle")
        w64_cmd = [sys.executable, w64c_old, bsp, fx / f"{m}.wci4", fx / f"{m}.w64",
                   "--hle", fx / f"{m}.hle", "--materials", mats, "--no-budget"]
        run(w64_cmd, f"{m}: old w64c")
        ent = {}
        for ext in ("wci4", "hle", "w64"):
            b = (fx / f"{m}.{ext}").read_bytes()
            ent[ext] = {"bytes": len(b), "crc32": "%08x" % (zlib.crc32(b) & 0xFFFFFFFF)}
        ent["hle_cmd"] = [str(c) for c in hle_cmd]
        ent["w64_cmd"] = [str(c) for c in w64_cmd]
        manifest["maps"][m] = ent
        print(f"w1: fixture {m}: wci4 {ent['wci4']['bytes']} B, hle {ent['hle']['bytes']} B, "
              f"w64 {ent['w64']['bytes']} B")
    (fx / "manifest.json").write_text(json.dumps(manifest, indent=1))
    return 0


def compare(a) -> int:
    fx = Path(a.fixtures)
    if not (fx / "palette.lmp").is_file():
        fail(f"{fx}: no fixtures (run --make-fixtures)")
    if not (Path(a.data) / MATERIALS).is_file():
        fail(f"{Path(a.data) / MATERIALS}: missing")
    with tempfile.TemporaryDirectory(prefix="w1-") as td:
        with ThreadPoolExecutor(max_workers=max(1, a.jobs)) as ex:
            results = list(ex.map(lambda m: compare_map(a, fx, Path(td), m), a.maps))
    bad = 0
    for m, (res, nbad) in zip(a.maps, results):
        print(f"w1: {m}: " + "; ".join(res))
        bad += nbad
    print(f"w1: {'PASS' if not bad else 'FAIL'} ({len(a.maps)} maps, {bad} mismatches)")
    return 1 if bad else 0


def compare_map(a, fx: Path, td: Path, m: str):
    """Cook one map with the new tools; (result phrases, mismatch count)."""
    bsp = Path(a.data) / "maps" / f"{m}.bsp"
    mats = Path(a.data) / MATERIALS
    res, bad = [], 0
    try:
        wci4, hle = fx / f"{m}.wci4", fx / f"{m}.hle"
        if not a.w64c_only:
            wci4 = td / f"{m}.wci4"
            run([sys.executable, "-m", "tools.cook.bake_world_tex", bsp, "-o", wci4,
                 "--ref-palette", fx / "palette.lmp"], f"{m}: bake_world_tex")
            same = wci4.read_bytes() == (fx / f"{m}.wci4").read_bytes()
            res.append("wci4 " + ("identical" if same else "DIFFERS"))
            bad += not same
            if a.hlecook:
                new_hle = td / f"{m}.hle"
                run([a.hlecook, bsp, new_hle], f"{m}: hlecook")
                (n1, n2), (o1, o2) = hle_split(new_hle.read_bytes()), hle_split(hle.read_bytes())
                res.append("hln " + ("identical" if n2 == o2 else "DIFFERS"))
                bad += n2 != o2
                res.append("hle1 " + ("identical" if n1 == o1 else "differs (entvars retarget)"))
        out = td / f"{m}.w64"
        opts = ["--hle", hle, "--materials", mats]
        run([sys.executable, "-m", "tools.w64c.w64c", bsp, wci4, out] + opts, f"{m}: w64c")
        # the old cook wrote version 2: compare the package without the
        # version-3 additive ramps (v2_view)
        cooked = out.read_bytes()
        new, old = v2_view(cooked), (fx / f"{m}.w64").read_bytes()
        if new == old:
            res.append(f"w64 identical ({len(new)} B, v2 view)")
            return res, bad
        # version 3 keeps non-Normal brushes out of the sectors: the cook
        # under the version-2 fold rule must give the old bytes, and the
        # submodels it folds that the real cook keeps are those (FOLDED ->
        # MOVR, or NONE when no face cooks: nothing drawn either way)
        old_rule = td / f"{m}.v2fold.w64"
        run([sys.executable, "-c", "import sys; from tools.w64c import w1; "
             "sys.exit(w1.cook_v2_fold(sys.argv[1:]))", bsp, wci4, old_rule] + opts,
            f"{m}: w64c (v2 fold rule)")
        ob = old_rule.read_bytes()
        kn, ko = sub_kinds(cooked), sub_kinds(ob)
        kept = [i for i, (a, b) in enumerate(zip(kn, ko)) if a != b]
        stray = [i for i in kept if ko[i] != SUB_FOLDED or kn[i] not in (SUB_MOVR, SUB_NONE)]
        if v2_view(ob) != old:
            res.append(f"w64 DIFFERS under the v2 fold rule: {w64_first_diff(v2_view(ob), old)}")
            bad += 1
        elif stray or len(kn) != len(ko) or not kept:
            res.append(f"w64 DIFFERS: submodel kinds {stray or 'unchanged'}")
            bad += 1
        else:
            empty = [i for i in kept if kn[i] == SUB_NONE]
            movr = [i for i in kept if kn[i] == SUB_MOVR]
            res.append(f"w64 identical under the v2 fold rule ({len(ob)} B, v2 view); "
                       f"by design {len(movr)} non-Normal submodels now MOVR"
                       + (" (" + " ".join(f"*{i}" for i in movr) + ")" if len(movr) <= 8 else "")
                       + (f", {len(empty)} without faces now NONE ("
                          + " ".join(f"*{i}" for i in empty) + ")" if empty else ""))
    except (RuntimeError, OSError) as e:
        res.append(f"FAILED: {e}")
        bad += 1
    return res, bad


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="w1", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--fixtures", required=True, help="fixture directory")
    ap.add_argument("--data", required=True, help="retail Half-Life valve directory")
    ap.add_argument("--maps", default=",".join(MAPS), help="comma-separated map names")
    ap.add_argument("--make-fixtures", action="store_true",
                    help="build the fixtures by running the old tools")
    ap.add_argument("--old-tree", default="~/src/ports/quakegeneric",
                    help="quakegeneric tree whose tools are run (--make-fixtures)")
    ap.add_argument("--old-cook", default="/scratch/hl64/tmp/CookImport/old",
                    help="old cook output root: stage/maps/*.wci4, fs/id1/gfx/palette.lmp")
    ap.add_argument("--hlecook", default=None, help="new hlecook binary to compare")
    ap.add_argument("--w64c-only", action="store_true",
                    help="cook only the .w64, from the fixture .wci4 and .hle")
    ap.add_argument("-j", "--jobs", type=int, default=min(4, os.cpu_count() or 1),
                    help="maps cooked in parallel (default: min(4, CPUs))")
    a = ap.parse_args(argv)
    a.maps = [m for m in a.maps.split(",") if m]
    bad = [m for m in a.maps if m not in MAPS]
    if bad:
        fail(f"unknown map(s) {', '.join(bad)} (W1 covers {', '.join(MAPS)})")
    return make_fixtures(a) if a.make_fixtures else compare(a)


if __name__ == "__main__":
    sys.exit(main())
