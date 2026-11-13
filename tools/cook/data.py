# SPDX-License-Identifier: MIT
"""data: read-only view of the player's Half-Life game directory.

The cook reads every asset through this one class: loose files under the
directory (the retail Steam layout, e.g. `.../Half-Life/valve`) and the
`pak*.pak` PACK archives in it (older and demo releases such as Uplink).
Lookups are by `/`-separated path, case-insensitive; loose files win over
paks and a higher-numbered pak over a lower one (the game's search order).

Archives are untrusted: the PACK header, directory and every entry range
are checked against the file before use; a bad archive or two loose files
differing only in case stop the cook with a DataError naming the file.
"""
from __future__ import annotations

import os
import re
import struct
from pathlib import Path
from typing import Dict, Iterator, List, Optional, Tuple

PACK_ENTRY = 64
MAX_PACK_ENTRIES = 1 << 16


class DataError(Exception):
    pass


def _norm(rel: str) -> str:
    return rel.replace("\\", "/").lstrip("/").lower()


class Data:
    def __init__(self, root: Path):
        self.root = Path(root)
        if not self.root.is_dir():
            raise DataError(f"{self.root}: not a directory")
        self.loose: Dict[str, Path] = {}
        self.paks: List[Tuple[Path, Dict[str, Tuple[int, int]]]] = []
        for dirpath, dirnames, filenames in os.walk(self.root):
            dirnames.sort()
            for fn in sorted(filenames):
                p = Path(dirpath) / fn
                rel = _norm(p.relative_to(self.root).as_posix())
                if rel in self.loose:
                    raise DataError(f"{self.root}: {self.loose[rel]} and {p} differ only in case")
                self.loose[rel] = p
        pak_names = sorted((r for r in self.loose if re.fullmatch(r"pak\d+\.pak", r)),
                           key=lambda r: int(r[3:-4]), reverse=True)
        for r in pak_names:
            self.paks.append((self.loose[r], self._read_pack(self.loose[r])))

    @staticmethod
    def _read_pack(path: Path) -> Dict[str, Tuple[int, int]]:
        size = path.stat().st_size
        with open(path, "rb") as f:
            head = f.read(12)
            if len(head) < 12 or head[:4] != b"PACK":
                raise DataError(f"{path}: not a PACK archive")
            dofs, dlen = struct.unpack_from("<II", head, 4)
            if dlen % PACK_ENTRY or dofs > size or dlen > size - dofs \
                    or dlen // PACK_ENTRY > MAX_PACK_ENTRIES:
                raise DataError(f"{path}: directory {dofs}+{dlen} invalid for a {size}-byte file")
            f.seek(dofs)
            table = f.read(dlen)
        out: Dict[str, Tuple[int, int]] = {}
        for i in range(dlen // PACK_ENTRY):
            raw, pos, length = struct.unpack_from("<56sII", table, i * PACK_ENTRY)
            name = raw.split(b"\0", 1)[0].decode("latin-1")
            if pos > size or length > size - pos:
                raise DataError(f"{path}: entry {name!r} {pos}+{length} outside the file")
            out.setdefault(_norm(name), (pos, length))
        return out

    def _find(self, rel: str):
        r = _norm(rel)
        if r in self.loose:
            return ("loose", self.loose[r])
        for path, table in self.paks:
            if r in table:
                return ("pak", path, table[r])
        return None

    def has(self, rel: str) -> bool:
        return self._find(rel) is not None

    def read(self, rel: str) -> Optional[bytes]:
        hit = self._find(rel)
        if hit is None:
            return None
        if hit[0] == "loose":
            return hit[1].read_bytes()
        _, path, (pos, length) = hit
        with open(path, "rb") as f:
            f.seek(pos)
            data = f.read(length)
        if len(data) != length:
            raise DataError(f"{path}: short read of {rel}")
        return data

    def need(self, rel: str) -> bytes:
        data = self.read(rel)
        if data is None:
            raise DataError(f"{self.root}: no {rel}")
        return data

    def where(self, rel: str) -> str:
        hit = self._find(rel)
        if hit is None:
            return "-"
        return str(hit[1]) if hit[0] == "loose" else f"{hit[1]}:{_norm(rel)}"

    def names(self, prefix: str = "") -> Iterator[str]:
        """Every lower-case path under prefix, sorted, each once."""
        p = _norm(prefix)
        seen = set(r for r in self.loose if r.startswith(p))
        for _, table in self.paks:
            seen.update(r for r in table if r.startswith(p))
        return iter(sorted(seen))
