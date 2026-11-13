#***
#*
#*	Copyright (c) 1996-2002, Valve LLC. All rights reserved.
#*
#*	This product contains software technology licensed from Id
#*	Software, Inc. ("Id Technology").  Id Technology (c) 1996 Id Software, Inc.
#*	All Rights Reserved.
#*
#*   Use, distribution, and modification of this source code and/or resulting
#*   object code is restricted to non-commercial enhancements to products from
#*   Valve LLC.  All other use, distribution, or modification is prohibited
#*   without written permission from Valve LLC.
#*
#****
# Half Life 1 SDK LICENSE: tools/hlsdk/LICENSE-HLSDK.
"""Surface material types from sound/materials.txt, read exactly as the
Half Life 1 SDK reads them (pm_shared/pm_shared.c PM_InitTextureTypes,
PM_SortTextures, PM_FindTextureType; the texture-name prefix strip of
dlls/sound.cpp TEXTURETYPE_PlaySound).

Used by tools.w64c.w64c to cook the per-node surface materials of a layer
package (docs/formats/w64-format.md 'Surface materials'). Material codes
index MAT_CHARS, the CHAR_TEX_* letter set (pm_shared/pm_materials.h);
0 = concrete, letters outside the set read as concrete.
"""
from __future__ import annotations

from pathlib import Path

CBTEXTURENAMEMAX = 13          # pm_materials.h: 12 name chars count
CTEXTURESMAX = 512
MAT_CHARS = "CMDVGTSWPYFN"     # CHAR_TEX_* letters, code = index
MAT_MIXED = 15                 # w64-format.md W64_MAT_MIXED (not a letter)
LINE_MAX = 510                 # memfgets(..., 511): <= 510 chars incl. '\n'
_SPACE = b" \t\n\v\f\r"


class MaterialsError(ValueError):
    """Unreadable materials file; the message starts with the file name."""


def _lower(b: bytes) -> bytes:
    return bytes(c + 32 if 65 <= c <= 90 else c for c in b)


def _strnicmp(a: bytes, b: bytes, n: int) -> int:
    for i in range(n):
        x = a[i] if i < len(a) else 0
        y = b[i] if i < len(b) else 0
        x = x + 32 if 65 <= x <= 90 else x
        y = y + 32 if 65 <= y <= 90 else y
        if x != y:
            return -1 if x < y else 1
        if not x:
            return 0
    return 0


class Materials:
    """sound/materials.txt as PM_InitTextureTypes / PM_FindTextureType
    read it."""

    def __init__(self, data: bytes):
        names, types = [], []
        pos, n = 0, len(data)
        while pos < n and len(names) < CTEXTURESMAX:
            end = min(n, pos + LINE_MAX)
            i = pos
            while i < end:
                i += 1
                if data[i - 1] == 10:
                    break
            line, pos = data[pos:i], i
            line = line.split(b"\0")[0]      # the C string ends at a NUL
            k = 0
            while k < len(line) and line[k] in _SPACE:
                k += 1
            if k >= len(line) or line[k] == ord("/") or not chr(line[k]).isascii() \
                    or not chr(line[k]).isalpha():
                continue
            t = chr(line[k]).upper()
            k += 1
            while k < len(line) and line[k] in _SPACE:
                k += 1
            if k >= len(line):
                continue
            j = k
            while j < len(line) and line[j] not in _SPACE:
                j += 1
            if j >= len(line):           # unterminated last name: dropped
                continue
            names.append(line[k:min(j, k + CBTEXTURENAMEMAX - 1)])
            types.append(t)
        for a in range(len(names)):      # PM_SortTextures' exchange sort
            for b in range(a + 1, len(names)):
                if _lower(names[a]) > _lower(names[b]):
                    names[a], names[b] = names[b], names[a]
                    types[a], types[b] = types[b], types[a]
        self.names, self.types = names, types

    @classmethod
    def load(cls, path: Path) -> "Materials":
        """A loose sound/materials.txt."""
        try:
            return cls(Path(path).read_bytes())
        except OSError as e:
            raise MaterialsError(f"{path}: {e.strerror}") from None

    def code(self, texname: str) -> int:
        """TEXTURETYPE_PlaySound prefix strip + PM_FindTextureType -> code."""
        s = texname.encode("latin1")
        if s[:1] in (b"-", b"+"):
            s = s[2:]
        if s[:1] in (b"{", b"!", b"~", b" "):
            s = s[1:]
        s = s[:CBTEXTURENAMEMAX - 1]
        lo, hi = 0, len(self.names) - 1
        while lo <= hi:
            p = (lo + hi) // 2
            v = _strnicmp(s, self.names[p], CBTEXTURENAMEMAX - 1)
            if v == 0:
                t = self.types[p]
                return MAT_CHARS.index(t) if t in MAT_CHARS else 0
            if v > 0:
                lo = p + 1
            else:
                hi = p - 1
        return 0
