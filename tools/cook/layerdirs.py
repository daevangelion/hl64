# SPDX-License-Identifier: MIT
"""layerdirs: cook proof of the layer-directory region (include/hl64/limits.h
HL64_LAYER_DIRS_KB, docs/architecture.md 4.1 / 5.3).

While a seam is approached both maps of a CWG link are open layers. Each
open layer holds, in the layer region (engine/world/world.c
wld_layer_bytes), its .w64 directory block and one tables block, every
part 16-aligned:

    directory          dir_size
    chunk records      nchunks * sizeof(wld_chunk_t) (20 B)
    style epochs       WLD_STYLE_SLOTS (1024) * 4
    per-page GPU state npages * WLD_PAGE_STATE (96)
    movers             nsub * sizeof(wld_mover_t) (16 B)

world.c keeps one layer's blocks at the region's low end and the other's at
the high end, so a pair opens whenever its sum fits (one free hole between
them). For every link A -> B of maps/campaign.cwg (self links excluded) the
proof sums both layers' bytes, reports the worst pair and fails above
HL64_LAYER_DIRS_KB. The region block descriptors are static (world.c
desc_layers), not in the region.

  python3 -m tools.cook.layerdirs CONTENT [--limits include/hl64/limits.h]
"""
import argparse
import re
import struct
import sys
from pathlib import Path

from tools.cook.hl_campaign import read_cwg

ROOT = Path(__file__).resolve().parents[2]
CHUNK_REC = 20          # sizeof(wld_chunk_t) (world.c _Static_assert)
MOVER = 16              # sizeof(wld_mover_t) (world.c _Static_assert)
STYLE_SLOTS = 1024      # WLD_STYLE_SLOTS
PAGE_STATE = 96         # WLD_PAGE_STATE
W64_MAGIC = 0x5736344C


def a16(n: int) -> int:
    return (n + 15) & ~15


def layer_bytes(path: Path) -> dict:
    """The layer-region bytes of one .w64 (header + directory counts)."""
    with path.open("rb") as f:
        h = f.read(64)
        if len(h) < 64:
            raise ValueError(f"{path}: truncated header")
        magic, _ver, _fl, _fsz, dofs, dsz, nchunks = struct.unpack_from(">IHHIIII", h, 0)
        if magic != W64_MAGIC:
            raise ValueError(f"{path}: not a .w64")
        f.seek(dofs)
        d = f.read(16)
        nsect, nleaf, ntex, npages, nsub, dn, texp_first, nvis = struct.unpack(">8H", d)
    if dn != nchunks:
        raise ValueError(f"{path}: directory chunk count {dn} != header {nchunks}")
    parts = {"dir": a16(dsz), "chunks": a16(nchunks * CHUNK_REC), "styles": STYLE_SLOTS * 4,
             "pages": a16(npages * PAGE_STATE), "movers": a16(nsub * MOVER)}
    return {"bytes": sum(parts.values()), "parts": parts}


def limit_kb(limits: Path) -> int:
    m = re.search(r"#define\s+HL64_LAYER_DIRS_KB\s+(\d+)", limits.read_text())
    if not m:
        raise ValueError(f"{limits}: no HL64_LAYER_DIRS_KB")
    return int(m.group(1))


def prove(content: Path, limits: Path) -> int:
    graph = read_cwg(content / "maps" / "campaign.cwg")
    size = {m["name"]: layer_bytes(content / "maps" / f"{m['name']}.w64")["bytes"]
            for m in graph["maps"]}
    pairs = []
    for m in graph["maps"]:
        for link in m["links"]:
            if link["to"] != m["name"]:
                pairs.append((size[m["name"]] + size[link["to"]], m["name"], link["to"]))
    if not pairs:
        print("layerdirs: no links: nothing to prove")
        return 0
    worst = max(pairs)
    kb = limit_kb(limits)
    for b, a, t in sorted(set(pairs), reverse=True)[:5]:
        print(f"layerdirs: {a} + {t}: {size[a]} + {size[t]} = {b} B")
    print(f"layerdirs: worst pair {worst[1]} + {worst[2]}: {worst[0]} B "
          f"({(worst[0] + 1023) // 1024} KB); x1.25 = {(worst[0] * 5 // 4 + 1023) // 1024} KB; "
          f"HL64_LAYER_DIRS_KB {kb} ({len(pairs)} links)")
    if worst[0] > kb * 1024:
        print(f"layerdirs: FAIL: {worst[1]} + {worst[2]} needs {worst[0]} B > "
              f"HL64_LAYER_DIRS_KB {kb} KB", file=sys.stderr)
        return 1
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("content", type=Path)
    ap.add_argument("--limits", type=Path, default=ROOT / "include" / "hl64" / "limits.h")
    a = ap.parse_args(argv)
    try:
        return prove(a.content, a.limits)
    except (OSError, ValueError, struct.error) as e:
        print(f"layerdirs: error: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
