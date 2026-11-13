<!-- SPDX-License-Identifier: MIT -->
# provcheck: the hl64 provenance gate

`provcheck` enforces [PROVENANCE.md](../../docs/PROVENANCE.md) mechanically.
It is the M0 gate in [architecture.md §17](../../docs/architecture.md):
no forbidden file, no line-level match against Quake or Xash3D above a
threshold, correct licence headers, and no Quake engine identifiers in
`engine/`.

```
make provcheck                                   # = python3 tools/provcheck/provcheck.py check
python3 tools/provcheck/provcheck.py check       # exit 0 PASS, 1 findings, 2 broken DB/setup
python3 tools/provcheck/provcheck.py controls    # planted copies must fail, clean copies must pass
python3 tools/provcheck/provcheck.py scan [--idents] [--hlsdk] PATH...   # similarity report for any tree
python3 tools/provcheck/provcheck.py build-db    # regenerate db/ from the local corpora
```

Python 3 standard library only. `check` needs no reference sources: it
uses the committed hash database. `build-db` and `controls` need the
local corpora listed in `corpora.json`.

## What `check` scans

Every file `git ls-files -co --exclude-standard` reports: tracked files
and untracked files that are not ignored, so the gate also covers files
not yet committed. Symlinks are skipped.

| file class | extensions | checks |
|---|---|---|
| C family | `.c .h .cpp .cc .cxx .hpp .hh .inl .s .S .def .qc .ld` | licence, forbidden, token + comment similarity, identifiers (`engine/` only) |
| scripts | `.py .sh .mk .route .cmake`, `Makefile` | licence, forbidden, comment similarity |
| patches | `.patch` | forbidden only (a patch body cannot carry a header) |
| everything else | docs, data, `LICENSE*` | forbidden only |

**Zones.** `game/`, `tools/hlsdk/`, `test/oracle/probe/` and
`test/oracle/*.patch` are the HLSDK zone; everything else is the MIT zone.

### 1. Similarity to Quake / Xash3D (`SIMILAR-EXACT`, `SIMILAR-NORM`, `SIMILAR-COMMENT`)

C-family files are tokenised (comments and `#include` lines removed) into
two streams:

- **exact**: tokens as written; 12-token k-grams; a finding needs a region
  of at least 60 matched tokens;
- **normalised**: identifiers → `I`, numbers → `N`, strings → `S`,
  keywords and operators kept, so a copy with every name changed still
  matches; 24-token k-grams; a finding needs at least 100 matched tokens.

The reference side is winnowed (window 6 and 8; any shared run of 17 / 31
tokens is guaranteed to leave a fingerprint); the checked file is looked up
with every k-gram. Matched k-grams are merged into regions (gaps of up to 8
tokens), and a region is reported with its line range, its size, the
corpus label and the reference file and line of the most frequent match.

Comments (C comments; `#` comments and Python docstrings in scripts) are
reduced to lower-case words; every word 8-gram of the forbidden corpora is
stored. A run of matched words of at least 10 words is a finding (the size
of the copied Quake comments the audit found, `comment_overlap.md`). The
finding shows the matched words of the checked file.

What is not counted, because it is not expression:

- *declaration and data runs*: k-grams with no operator other than
  declaration punctuation (`; , [ ] * { } ( )`, unary signs) and no control
  keyword: struct field lists, prototypes, enum lists, number tables.
  These are interface layout (the audit's "format mimicry"; e.g. the
  GoldSrc `movevars` field order that `include/hl64/settings.h` follows
  per `pmove-host.md`). Quake type and function names in such runs are
  still caught by check 2 in `engine/`;
- *idioms*: normalised k-grams that repeat inside one reference file or
  inside the checked file (field-by-field byte swaps, `case`/`else if`
  ladders), or that occur in eight or more differently named reference
  files; any k-gram or comment n-gram that occurs in libdragon or tiny3d;
- *boilerplate comments*: comment lines naming a licence, copyright or a
  generated file (no n-gram spans such a line), comment 8-grams found in
  twelve or more differently named reference files (licence headers),
  8-grams that occur in a licence text (`/usr/share/common-licenses`, the
  libdragon/tiny3d licences, hl64's `LICENSE`), and 8-grams with fewer
  than five words of three or more letters (numbered lists).

In the HLSDK zone, fingerprints that also occur anywhere in the Half-Life
SDK (ValveSoftware/halflife, FWGS hlsdk-portable) are allowed: HLSDK code is
an allowed source there, including the Quake heritage inside it (e.g.
`pm_shared`). In the MIT zone every Quake/Xash fingerprint counts.

### 2. Engine identifiers (`IDENT`, `engine/` only)

An identifier in an `engine/` C file is reported when it

- is in the curated list (`edict_t`, `entvars_t`, `qboolean`, `vec3_t`,
  `trace_t`, `cvar_t`, `Z_Malloc`, `R_RecursiveWorldNode`, `VectorCopy`,
  `DotProduct`, … see `CURATED_IDENTS`), or
- has a Quake/GoldSrc subsystem prefix: `SV_ CL_ Host_ Cvar_ Cmd_ Cbuf_
  Con_ Hunk_ Cache_ Z_ Mod_ R_ Draw_ SCR_ COM_ MSG_ ED_ PF_ PR_ SZ_ S_ V_
  M_ D_ VID_ IN_ Key_ Sys_ Sbar_ NET_ Netchan_ GL_ Q_ Info_ Mem_ FS_ Image_
  Delta_ CDAudio_ Chase_ SNDDMA_ Studio_ Pmove_ PM_` followed by a
  lower-case-led word (`Sys_Error` but not `M_PI` or `S_IFREG`), or
  `pfn[A-Z]…`, or
- is in the hashed name set: every type, tag, function, file-scope
  variable, macro and enum constant defined by the Quake and Xash3D
  corpora, restricted to Quake-style names (`*_t`/`*_s` types,
  mixed-case names, snake_case names under a Quake subsystem prefix such
  as `host_`, `sv_`, `cl_`, `r_`, `pr_`) and minus ALL-CAPS constants,
  third-party API names (`SDL_`, `gl…`) and every identifier that appears
  in libdragon, tiny3d or the newlib headers.

Matching is case-sensitive. Names under the hl64 module prefixes
(`io_ wld_ col_ phy_ ent_ res_ snd_ ren_ in_ dbg_ ui_ sav_ rt_ prof_ sys_
mem_ hl64_`, architecture.md 18.2) are never looked up in the hashed set,
so `sys_error` and `snd_initialized` pass.

### 3. Licence headers (`LICENCE`)

- MIT zone: `SPDX-License-Identifier: MIT` within the first 10 lines (e.g.
  `/* SPDX-License-Identifier: MIT */` or `# SPDX-License-Identifier: MIT`
  after a shebang); no Valve or id Software notice.
- HLSDK zone: Valve's notice (`Copyright (c) 1996-2002, Valve LLC`) within
  the first 20 lines and no MIT SPDX line.
- Everywhere: no GPL, "quakegeneric contributors" or Xash3D notice in the
  licence/copyright lines of the first 20 lines.

### 4. Forbidden files (`FORBIDDEN-FILE`)

- game data, ROMs, cooked content and build outputs: `.bsp .mdl .spr .wad
  .pak .pk3 .dem .sav .dat .z64 .n64 .v64 .dfs .elf .o .a .so .dll .exe
  .w64 .q64 .wav64 .t3dm .rtb .rbc` (route and resource tables are
  generated by tests into `TMPDIR`, never committed);
- QuakeC: `.qc`, `progs.src`, `progdefs.h`, `progs.h`, anything under
  `qc/`;
- the file names of the quakegeneric QuakeC translations (`g_*.c`,
  `game_weapons.c`, …) except names that are also HLSDK file stems
  (`game.c` ← `game.cpp` stays allowed; its content is still checked);
- a C file at the same path as a file of quakegeneric `9fd3580` or
  `1305210`;
- a whole-file copy (whitespace-insensitive hash) of any forbidden corpus
  file.

## The database

`db/provcheck.db.gz` holds, per check, sorted 48-bit fingerprints (BLAKE2b
token hashes combined by a polynomial rolling hash; BLAKE2b for names,
paths and whole files), a flag byte (QUAKE, XASH, HLSDK-shared) and a
reference file index and line used only to report where a match comes
from. No source text is stored; identifiers, paths and file contents are
stored as hashes. `db/manifest.json` records the corpus list with pinned
revisions, file counts, build parameters, per-section counts and the
SHA-256 of the DB. `check` refuses a DB whose SHA-256 differs from the
manifest or that has an empty section (exit 2), so a missing or corrupt
DB cannot pass.

The build is deterministic: the same corpora give a byte-identical DB.

### Corpora (`corpora.json`)

| corpus | label | what |
|---|---|---|
| `quakegeneric-9fd3580` | QUAKE | Quake/quakegeneric import commit (git object store) |
| `quakegeneric-1305210` | QUAKE | last upstream quakegeneric commit (GPL) |
| `quakegeneric-qctrans` | QUAKE | `source/g_*.c`, `game*.c` QuakeC translations |
| `quakegeneric-audit` | QUAKE/XASH | the CONTAINS-QUAKE / CONTAINS-XASH line ranges and FORBIDDEN files of [provenance-audit.md](../../docs/provenance-audit.md), taken from the quakegeneric working tree only where its line count still equals the audit snapshot (others are listed under `audit_rows_skipped` in the manifest) |
| `id-quake` | QUAKE | id-Software/Quake (WinQuake, QW, qw-qc) |
| `id-quake2` | QUAKE | id-Software/Quake-2 |
| `xash3d-fwgs` | XASH | FWGS xash3d-fwgs: engine, ref, common, pm_shared, public, filesystem, utils |
| `hlsdk`, `hlsdk-portable` | HLSDK | allowed in the HLSDK zone |
| `libdragon`, `tiny3d` | GENERIC | common code, subtracted |
| `common-licenses`, `sdk-licences`, `hl64-licence` | GENERIC-TEXT | licence texts, subtracted from the comment set |
| `newlib-headers` | GENERIC-NAMES | C library names, subtracted from the name set |

Our own lines inside the quakegeneric copies of Quake files (audit §4)
are not a corpus: they are our expression and some of them also live in
importable files; the Quake text around them is covered by `9fd3580`.

The audit corpus is what makes D9 verifiable: an import of a
QUAKE-INTERFACE / split file that keeps a flagged range fails even when
the range is a translation that no longer matches Quake token for token.

Rebuild after any change to `corpora.json`, to the audit, or to the
quakegeneric working tree's flagged files:

```
git clone https://github.com/id-Software/Quake.git /scratch/hl64/corpora/quake   # etc., at the pinned revs
python3 tools/provcheck/provcheck.py build-db       # ~1.5 min, ~1 GB RAM
python3 tools/provcheck/provcheck.py controls
```

Each corpus path can be overridden with the environment variable named in
its `env` field. `build-db` stops if a `dir` corpus is not at its pinned
revision.

## Controls

`controls` extracts `HEAD` (`--worktree`: the working tree, including
untracked files) into `$TMPDIR/provcheck-controls`, plants one item per
run and runs the same check `make provcheck` runs. Every planted item must
produce its finding (exit 1) and the allowed items must pass (exit 0):

| control | expected |
|---|---|
| clean copy of `HEAD` | PASS |
| Quake `SV_RecursiveHullCheck` in `engine/` | SIMILAR-EXACT |
| the same with every identifier renamed, in `plat/` | SIMILAR-NORM |
| the same in `game/` with a Valve header | SIMILAR-EXACT (Quake is not allowed in the HLSDK zone) |
| Xash `CL_FxBlend` in `engine/`, in `tools/` | SIMILAR-EXACT |
| a Quake comment block in `include/`, and as `#` comments in a Python tool | SIMILAR-COMMENT |
| audit-flagged QUAKE range `w64_world.c` 880–996 kept in an import | SIMILAR |
| CLEAN `q64_pool.c` imported with the MIT header | PASS |
| `edict_t`, `Sys_Error` in `engine/` | IDENT |
| `sys_error`, `snd_initialized` in `engine/` | PASS |
| missing SPDX line, GPL notice, MIT file in `game/` | LICENCE |
| `.bsp` file, `game/hl/g_combat.c`, Quake `world.c` copied whole | FORBIDDEN-FILE |
| HLSDK `pm_shared.c` in `game/pm/` with its Valve header | PASS |
| `check` with a corrupted DB, with a missing DB | exit 2 |

Planted text is read from the local corpora at run time and lives only in
the scratch directory.

## Findings

A finding is fixed by rewriting the code from a spec (`docs/specs`,
`docs/formats`, allowed sources) or moving HLSDK-derived code into the
HLSDK zone; there is no allow-list. If a finding is an idiom the filters
above should have excluded, fix the filter, rebuild, and re-run `controls`
and `check`.

## Limitations

- Token similarity reads C-family files only; Python and shell tools get
  the comment check. A Python translation of a Quake function is not
  detected by tokens.
- Reordered statements, changed control flow or merged functions defeat
  token matching; the structural review of PROVENANCE.md still applies.
- Matches shorter than the thresholds (≈ 60 exact / 100 normalised tokens,
  10 comment words) are not reported; short Quake helpers in `engine/`
  are usually caught by their names (check 2).
- Only the listed corpora are covered (no QuakeWorld forks, Quake 3,
  DarkPlaces, Source SDK).
- Quake heritage that is also in the HLSDK passes in the HLSDK zone by
  design.
- The audit corpus reads the quakegeneric working tree, which still
  changes; a rebuild drops any flagged file whose line count has moved
  away from the audit snapshot (`audit_rows_skipped`) rather than guess
  shifted ranges.
