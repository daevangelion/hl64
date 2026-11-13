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
  tools.cook.bake_world_tex --ref-palette DIR/palette.lmp, hlecook when
  --hlecook is given (else the fixture .hle), tools.w64c.w64c; byte-compares
  .wci4 / .hle / .w64 with the fixtures. --w64c-only cooks only the .w64,
  from the fixture .wci4 and .hle. A .w64 mismatch names the first differing
  part: header, directory table, or chunk (fourcc, index). Exit 1 on any
  mismatch or failed tool.
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
                hle = td / f"{m}.hle"
                run([a.hlecook, bsp, hle], f"{m}: hlecook")
                same = hle.read_bytes() == (fx / f"{m}.hle").read_bytes()
                res.append("hle " + ("identical" if same else "DIFFERS"))
                bad += not same
        out = td / f"{m}.w64"
        run([sys.executable, "-m", "tools.w64c.w64c", bsp, wci4, out, "--hle", hle,
             "--materials", mats], f"{m}: w64c")
        new, old = out.read_bytes(), (fx / f"{m}.w64").read_bytes()
        if new == old:
            res.append(f"w64 identical ({len(new)} B)")
        else:
            res.append(f"w64 DIFFERS: {w64_first_diff(new, old)}")
            bad += 1
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
