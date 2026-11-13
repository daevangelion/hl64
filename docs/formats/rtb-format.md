# `campaign.rtb` — campaign resource table, version 1

One file per cart, `campaign.rtb`, cooked by `tools/restab` (architecture.md
6.5, [precache-and-resources.md](../specs/precache-and-resources.md)). It
gives every model, sprite, sound, sentence and sentence group the campaign
or the game can use a stable 16-bit id (`res_t`), maps those ids to cart
files, and carries the lightstyle pattern table
([lightstyles.md](../specs/lightstyles.md) 2), the game's definition-line →
id table, the class and global-state counts, per-map entity names for the
route compiler and the route VM, and host-side name tables. At run time
there are no resource names, no precache slots and no slot reuse.

- Big-endian (cart native), read in place on the cart; the host swaps once.
- CRC-32 is the zlib polynomial.
- Every section starts 16-aligned; padding bytes are zero.
- Ids are indices; nothing in the file is a pointer.

## Header (32 B)

| off | field | rule |
|---:|---|---|
| 0 | `magic` u32 | `0x52544231` (`RTB1`) |
| 4 | `version` u16 | 1 |
| 6 | `flags` u16 | 0 |
| 8 | `build_id` u32 | CRC-32 of bytes `[32, file_size)`; compiled routes record it (route-bytecode.md) |
| 12 | `file_size` u32 | equals the file length |
| 16 | `cwg_crc` u32 | CRC-32 of the `campaign.cwg` the table was cooked against (map indices are CWG map indices) |
| 20 | `nsect` u16 | 26 |
| 22 | reserved u16 | 0 |
| 24 | `dir_ofs` u32 | 32 |
| 28 | reserved u32 | 0 |

## Section directory

`nsect` entries of 16 B: `fourcc` u32, `ofs` u32 (from file start), `size`
u32, `count` u32. The sections appear in exactly the order below, each once,
contiguous: the first at `align16(32 + 16·nsect)`, each next at
`align16(ofs + size)`, the file ending at `align16` of the last. `size` =
`count` × element size (byte pools: `size` = `count`; count-only sections:
`size` = 0).

Three groups, so a loader reads one prefix:

- **cart** (`FILE` … `TTXT`): loaded at boot; `STRS` is needed only while
  the boot resolves the file table (architecture.md 5.1) and may live in the
  boot arena (the engine keeps the files as (PI address, size) and drops the
  paths).
- **test** (`TARG`, `TLMP`): loaded by test builds for the route VM.
- **host** (`NMAP` … `NSTR`): names, for the cook tools, the route compiler
  and debug printing; never on the cart's steady path.

| # | fourcc | element | count | content |
|---:|---|---|---|---|
| 0 | `FILE` | 12 B: `path` u32 (offset into `STRS`), `size` u32, `crc` u32 | files | the cart file table: every file the table refers to (map packages, resources and their companions) |
| 1 | `STRS` | byte | bytes | NUL-terminated paths, relative to the content root, `/`-separated |
| 2 | `MAPS` | 4 B: `file` u16, reserved u16 | CWG maps | the `.w64` package of each CWG map index |
| 3 | `KIND` | 4 B: `count` u16, `first` u16 | 8 | per `res_t` kind: ids in use; `first` = `RES_` index of the kind's first entry (file kinds), else 0 |
| 4 | `RES_` | 12 B: `kind` u8, `flags` u8, `file` u16, `offset` u32, `size` u32 | file-kind ids | resources of the file kinds, in kind order (precache-and-resources.md 1) |
| 5 | `SENT` | 4 B: `word_first` u16, `word_count` u8, reserved u8 | sentences | words of each sentence id |
| 6 | `WORD` | 8 B: `res` u16, `pitch` u8, `volume` u8, `start` u8, `end` u8, `time` u8, reserved u8 | words | one sentence word: a SOUND `res_t` and its VOX modifiers (sound.md 4) |
| 7 | `SGRP` | 4 B: `first` u16, `count` u16 | groups | sentence group id → its sentence ids |
| 8 | `PATN` | 4 B: `ofs` u16 (into `PPOL`), `len` u8, reserved u8 | patterns | lightstyle pattern id → its letters |
| 9 | `PPOL` | byte | bytes | pattern letters as indices 0–25 (`a`–`z`) |
| 10 | `DEFS` | u16 `res_t` | `res.def` lines | the id of each line of `game/defs/res.def`, in line order |
| 11 | `CLAS` | — | classes | count of `classes.def` lines (class id = line order) |
| 12 | `GLOB` | — | global states | count of global-state ids (sorted names, `NGLB`) |
| 13 | `TITL` | 40 B: `x`, `y`, `fadein`, `fadeout`, `holdtime`, `fxtime` f32, `text` u32 (into `TTXT`), `text_len` u16, `effect` u8, reserved u8, `color1` u8[4], `color2` u8[4] (r g b a) | titles | titles.txt messages (below) |
| 14 | `TTXT` | byte | bytes | title texts, each NUL-terminated |
| 15 | `TARG` | 8 B: `map` u16, `lump_first` u16 (into `TLMP`), `lump_count` u16, `spawners` u16 | names | entity name id → map and BSP entity lumps (below) |
| 16 | `TLMP` | u16 | lumps | BSP entity lump indices (worldspawn 0), HLE's `lump` |
| 17 | `NMAP` | u32 offset into `NSTR` | CWG maps | map names |
| 18 | `NRES` | u32 | `RES_` entries | resource names (below) |
| 19 | `NSEN` | u32 | sentences | sentence names (upper case) |
| 20 | `NSGR` | u32 | groups | group names (upper case) |
| 21 | `NCLS` | u32 | classes | classnames |
| 22 | `NTGT` | u32 | names | entity names |
| 23 | `NGLB` | u32 | global states | global-state names |
| 24 | `NTTL` | u32 | titles | title names (as in titles.txt) |
| 25 | `NSTR` | byte | bytes | NUL-terminated UTF-8 names |

## `res_t`

`res_t` = `kind << 13 | index` (u16; index < 8192 per kind). `res_t` 0 is
"none".

| kind | name | id space |
|---:|---|---|
| 0 | NONE | — (count 0) |
| 1 | MODEL | `RES_` entries; `models/<name>.mdl` |
| 2 | SPRITE | `RES_` entries; `sprites/<name>.spr` (cart file `.sc4`) |
| 3 | SOUND | `RES_` entries; `<path>.wav` relative to `sound/` (cart file `.wav64`) |
| 4 | ANIM | `RES_` entries; reserved for D3(b) studio animation chunk sets (count 0 until M3) |
| 5 | SENTENCE | `SENT` |
| 6 | GROUP | `SGRP` |
| 7 | SUBMODEL | not in the table: `index = map slot << 10 \| submodel` (architecture.md 6.5); count 0 |

Ids are dense per kind and ordered by name (byte order of the normalised
name: lower case, `/` separators), so the same inputs always give the same
ids (precache-and-resources.md R1).

`RES_` entry: `offset`/`size` are the resource's byte range in `file`
(whole files today: offset 0, size = file size). `flags`: bit 0 EXTTEX (a
model whose textures are in `<name>t.mdl`, cart file `file + 1`), bits 4–7
the number of sequence-group files `<name>01.mdl`, `<name>02.mdl`, … that
follow (after the texture file); other bits 0; only MODEL entries carry
flags.

## Sentences and groups

Sentence ids follow `sound/sentences.txt` order, restricted to the
sentences in use: every sentence named by a map key (`!NAME`) or a
`RES(SENTENCE, …)` line, and every member of a group named by a
`RES(GROUP, …)` line. A group is a run of consecutive sentences whose names
are equal after removing trailing digits (`HG_ALERT0` … `HG_ALERT6` →
`HG_ALERT`); `SGRP` entries tile `SENT` in order. A group name that occurs
in two separate runs gives two groups; name lookups return the first.

`WORD`: `res` is the word's SOUND id. Modifier defaults: pitch 100, volume
100, start 0, end 100, time 0; `start`, `end`, `time` are percent (≤ 100),
pitch 1–255. The runtime applies pitch, volume and end; start and time are
stored but not mixed (sound.md 4).

## Lightstyle patterns

Pattern ids: the `lightstyles.def` lines first, id = line order (the game's
generated constants), then every distinct map `pattern` key value not
already present, sorted. Letters are stored as 0–25; length 1–255. The
value of letter index `k` is `k × 22` (lightstyles.md 1).

## Titles

Title ids follow the staged `titles.txt` order (the stage keeps the titles
the maps name: env_message `message`, worldspawn `chaptertitle`); a name
repeated (case-insensitive, as GoldSrc looks titles up) keeps its first
message. Each `TITL` entry is HLSDK's `client_textmessage_t` without the
name: the parameters the `$` directives in effect before the message set
(`$position x y`, `$effect 0-2`, `$color r g b [a]`, `$color2 r g b [a]`,
`$fadein`, `$fadeout`, `$holdtime`, `$fxtime`; a missing directive leaves
x = y = -1 (centred), colours 255 255 255 0, effect and times 0) and the
text lines between `{` and `}` joined by `\n` (≤ 1023 bytes). The HLE1 id
step types the two title keys (hle-format-v1.md "Ids"); the runtime gives
the game `res_title(id)` / `res_title_text(id)` (include/hl64/rec.h).

## Entity names (`TARG`)

One entry per distinct (map, name), ordered by map index then name: names
are `targetname` values, plus the `netname` of `monstermaker` entities (the
name its children receive). `TLMP[lump_first .. +lump_count)` lists the
entities carrying the name as targetname, ascending, then the
`spawners` monstermakers giving it to their children. The route VM
resolves a name to live entities through these records (route-bytecode.md);
entities spawned at run time with the name are found through their
spawner.

## Validation (reader)

A reader rejects the file (`sys_error(file, offset, rule)` on the cart,
`RtbError` in `tools/restab/rtb.py`) unless all of:

- header fields as above; `file_size` = bytes present; `build_id` matches
  (host always, cart debug builds);
- the directory holds exactly the 26 sections in order, contiguous,
  16-aligned, inside the file, sizes = count × element size, zero padding,
  nothing after the last section;
- `STRS` / `NSTR` end with NUL; every string offset lies inside its pool
  and its string is NUL-terminated (UTF-8 for names); file paths are
  relative and contain no `..`;
- `MAPS.file` < files; `KIND` has 8 entries, file-kind `first` values are
  the running sum of their counts, other kinds have `first` 0, kinds 0 and
  7 count 0, counts ≤ 8192, the file-kind counts sum to the `RES_` count;
- each `RES_` entry lies in its kind's range, `file` + companion count <
  files, `offset + size` ≤ that file's size, flags as above;
- `WORD.res` is a SOUND id < the SOUND count, modifiers in range;
  `SENT` word ranges inside `WORD`, `word_count` ≥ 1; `SGRP` tiles `SENT`
  exactly; the SENTENCE / GROUP counts equal `SENT` / `SGRP`;
- `PPOL` bytes ≤ 25; `PATN` ranges inside `PPOL`, length ≥ 1;
- `DEFS` ids name existing entries (kinds 1–3, 5, 6);
- `TTXT` ends with NUL; each `TITL` text range lies inside `TTXT`, is
  NUL-terminated there, holds no NUL and is ≤ 1023 bytes; effect ≤ 2,
  reserved 0; every float finite with magnitude < 2^20;
- `TARG.map` < maps, non-decreasing; lump ranges inside `TLMP`,
  `lump_count` ≥ 1, `spawners` ≤ `lump_count`;
- each name section's count equals its id space; map names, class names
  and (map, entity name) pairs are unique.

## Inputs (`tools/restab`)

```
python3 -m tools.restab.restab --content DIR --defs DIR -o campaign.rtb
```

`--content` is the staged content directory (the cook's output root; the
same layout is packed into the ROM's DFS):

| path | use |
|---|---|
| `maps/campaign.cwg` | CWG3 ([cwg-format.md](cwg-format.md)): map names and order; its CRC is `cwg_crc` |
| `maps/<map>.w64` | per CWG map: `MAPS` / `FILE` |
| `maps/<map>.ent` | per CWG map: the BSP entity lump text verbatim (cook intermediate, not packed) |
| `models/*.mdl` (+ `<name>t.mdl`, `<name>NN.mdl`) | MODEL files and companions |
| `sprites/*.sc4` | SPRITE cart files |
| `sound/**/*.wav64` | SOUND cart files |
| `sound/sentences.txt` | the player's sentence file (optional; without it no sentence may be referenced) |
| `titles.txt` | the staged titles (optional; without it there are no titles) |

File lookups ignore case; two staged files differing only in case are an
error when used.

Entity key values (every key of every entity):

- `targetname` → entity name; `monstermaker`'s `netname` → spawned name;
- `globalstate` → global-state id;
- `pattern` → lightstyle pattern (must be 1–255 letters `a`–`z`);
- `!NAME` in `message`, `sentence`, `noise`, `noise1`–`noise3`,
  `UseSentence`, `UnUseSentence` → sentence; a speaker's `message` (unless
  `preset` is set) and a plain `UseSentence` / `UnUseSentence` value →
  sentence group;
- a value ending in `.mdl` (not `*N`), `.spr` or `.wav` (leading `*`
  dropped) → MODEL, SPRITE or SOUND by name.

The HLE1 id step (`tools/w64c/hleids.py`, hle-format-v1.md "Ids") writes
these ids into each map's entity chunk; class-dependent keys (titles,
monstermaker `monstertype`, `func_breakable` gib models chosen by material
through `res.def`) are typed there or by the game.

`restab.cook(content, defs, map_names)` (the id step's prepass) builds the
same id spaces without `maps/campaign.cwg` and the map packages.

`--defs` is `game/defs/`: plain X-macro lines (C includes them with the
macro defined), `//` comments, blank lines; any other line is an error:

```
classes.def      CLASS(IDENT, "classname")            class id = line order
res.def          RES(MODEL|SPRITE|SOUND|SENTENCE|GROUP, IDENT, "name")
lightstyles.def  LIGHTSTYLE(style 0-63, "pattern")
```

A missing definition file counts as empty; so does a missing `--defs`
directory (`game/` arrives in M3; restab prints a note). Routes naming
classes need `classes.def`.

`sentences.txt` lines: `NAME words…`; `//` comments. Words are separated
by spaces; `dir/` before a word sets the directory for it and the following
words (default `vox`); a `(…)` group of `p`/`v`/`s`/`e`/`t` + number
standing alone sets defaults for the following words, one attached to a
word applies to that word; `,` and `.` are the pause words `_comma` and
`_period` of the current directory; a stray `)` is ignored. Word `w` in
directory `d` is the SOUND `d/w.wav`.

Every name must resolve to a staged file: the cook fails with the
referencing map and lump (`hldemo1:573 shootmodel: no file …`), definition
line or sentence line.

## Sizes

Uplink + Hazard (10 maps) with the player's entity lumps and sentences, no
`res.def` yet (M0, **measured** on the scratch staging): 180 files, 170
resources, 59 sentences, 54 groups, 1082 entity names; cart part
10,944 B, test part (`TARG`/`TLMP`) 11,664 B, host names 25,216 B;
47,824 B total. The cart part grows with `res.def` (≈ 12 B per resource
and 12 B per file).

M3 (**measured**, same 10 maps with titles and speaker / talk-monster
groups, no `res.def`): 195 files, 185 resources, 93 sentences, 60 groups,
33 titles; file 54,544 B, cart part 16,352 B. The engine keeps 9,516 B of
native tables (files as PI address + size, no paths; `mem res`), so the
RESTAB region is 28 KB for the game's `res.def` (≈ 20 B per resource).

## Open points

- Multi-cart campaigns (architecture.md 19, 2026-11-13 update): global-state
  ids are per table; the Controller Pak carry needs ids that agree between
  carts, so every cart's table must be cooked with the whole campaign's
  global-state list (or the carry stores names). Decided with the cart
  split.
