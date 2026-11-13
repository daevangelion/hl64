# Provenance rules

hl64 is a new Nintendo 64 engine for Half-Life content. It is not derived from
Quake, quakegeneric, Xash3D or GoldSrc engine code.

## Licences

| Tree | Licence | Notes |
|---|---|---|
| everything except `game/` | MIT (`LICENSE`) | engine, renderer, streaming, tools |
| `game/` | Half Life 1 SDK LICENSE (`game/LICENSE-HLSDK`) | Half-Life game logic; free distribution only; carries Valve's copyright notice and disclaimer |
| game data | not distributed | players supply their own Half-Life / Uplink data; the cook tools convert it locally |

## Forbidden sources (never open, copy, paraphrase or translate)

- id Software Quake / QuakeWorld / Quake 2 source, including every file of
  `~/src/ports/quakegeneric` present in its first commit (`git ls-tree -r
  9fd3580`) and its `source/g_*.c` QuakeC translations.
- Xash3D / Xash3D FWGS engine source (GPL).
- GoldSrc engine binaries (no disassembly).
- Any file of the quakegeneric tree not marked CLEAN in
  `docs/provenance-audit.md`.

## Allowed sources

- Half Life 1 SDK (`ValveSoftware/halflife`): game/ code and the engine
  interface headers it defines; copied code stays under `game/`.
- libdragon, tiny3d and their documentation.
- Files marked CLEAN in `docs/provenance-audit.md` (our own code from the
  quakegeneric N64 work), copied with their history noted in the commit.
- Format specifications we wrote (`docs/formats/`).
- Public documentation of GoldSrc behaviour, and black-box observation of the
  quakegeneric build or retail Half-Life (run it, measure, compare output).

## Process

- Engine behaviour that the old tree provided (traces, physics, entity
  lifetime) is specified in prose in `docs/` by an agent that may read the old
  tree; implementing agents work only from those specs and the allowed sources.
- Every commit that imports a file from quakegeneric names the audit entry.
