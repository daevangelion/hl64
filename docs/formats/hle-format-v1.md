# HLE1 — GoldSrc binary entity chunk (v1)

Imported from the quakegeneric tree (`n64/docs/hle-format-v1.md`, audit
entry CLEAN, docs/provenance-audit.md); old-engine references restated.

One self-contained, position-independent chunk per GoldSrc map that replaces
the BSP entity text. Produced by the cook's `hle_build` (`tools/w64c/hlecook`,
imported per architecture.md 16.3) and stored as the `.w64` package's `ENTS`
chunk ([w64-format.md](w64-format.md)); the engine's record module
(`engine/ent`, architecture.md 6.4) uses it in place after one
endian/validation pass (`hle_fixup`).

hl64's cook writes cooked ids into the chunk (section "Ids" below:
`tools/w64c/hleids.py`, the HLE1 id step): classes, key names and the
string-valued keys arrive as `cls_t`, key ids, `res_t`, pattern, global-state
and title ids from [rtb-format.md](rtb-format.md); names stay name indices.
The engine accepts only chunks that carry them.

## Layout

All offsets are bytes from the chunk start; every section starts 4-aligned
(8-aligned except the hash). Indices, not pointers, everywhere.

| Section | Element | Count field | Notes |
|---|---|---|---|
| header | `hle_header_t` (96 B) | — | magic, version, flags, sizes, offsets |
| records | `hle_rec_t` (24 B) | `numrecs` | kept entities, BSP lump order |
| lump2rec | u16 | `numlumps` | BSP lump → record, 0xFFFF = not kept |
| classes | `{u32 str; u16 flags; u16 id}` | `numclasses` | classname string + `HLE_CF_*`; `id` = classes.def id |
| keynames | `{u32 str; u16 kind; u16 id}` | `numkeynames` | interned key names + value kind; `id` = keys.def id |
| names | `{u32 str; u16 first, count; u16 flags, pad}` | `numnames` | targetname-like strings → lumplist slice |
| hash | u16 | `hashsize` (pow2) | open-addressed FNV-1a of name strings → name index |
| keys | `{u16 kn; u16 id; u32 val}` | `numkeys` | typed key values; `id` = the value's cooked id |
| lumplist | u16 | `numlumplist` | name lists, then multisource input lists (BSP lumps) |
| fire | `{u16 name; u16 pad; f32 delay}` | `numfire` | multi_manager fire lists, sorted by delay |
| floats | f32 | `numfloats` | vec3 values |
| strings | bytes | `strsize` | names region `[0, namestr_end)`, then other strings |

`hle_rec_t`: `lump` (BSP entity index), `cls`, `tname` (name index of the
targetname, 0xFFFF none), `flags`, `key_first`/`key_num`,
`aux_first`/`aux_count` (fire list when `HLE_RF_AUXFIRE`, else lumplist —
the multisource inputs), and three streaming fields: `sector` (0xFFFF =
global), `trank` (cooked trigger touch rank, specs/area-links-and-touch.md
3.1: written by the `touchrank` cook step (`tools/w64c/touchrank.py`) after
`hlecook` from trigger geometry: `cell << 11 | place`, `cell` the
trigger's cell (0–30, pre-order) in the measured cell model and `place` its
position in that cell's link order; ascending `trank` is touch order. The
GoldSrc probe's A-order captures are the step's regression test
(`touchrank --check`, A9). 0xFFFF for records that are not triggers. The
field was `twin`, reserved and always
0xFFFF; per-neighbour twins live in the [CWG3 seam table](cwg-format.md)),
`res_first`/`res_count` (precache refs, 0 count today).

Key kinds: `HLE_KF` float (val = IEEE bits), `HLE_KV` vec3 (val = float
index), `HLE_KS` string (val = string offset), `HLE_KN` name (val = name
index). Classname and targetname are not keys: they are `cls` / `tname`.

Names: every name string in the names region is preceded by its u16 name
index (2-aligned), so a string pointer into the chunk resolves to its name
in O(1); any other string resolves through the hash.

Flags: class `HLE_CF_LOGIC` (no entity: logic table record),
`HLE_CF_POINT` (no entity: origin/angles queried by name or class),
record `HLE_RF_PIN` (scripted_sequence `m_iszEntity` target or a
monstermaker: keep resident), `HLE_RF_PERSIST` (landmark / changelevel),
`HLE_RF_AUXFIRE`.

## Byte order

`magic` is the u32 0x31454C48 (`"HLE1"` on a little-endian file) in the
file's byte order; `flags & HLE_F_BIGENDIAN` must agree. `hle_fixup` swaps
every multi-byte field once when the chunk's order differs from the host
(none when it matches), rewrites the flag, and validates every offset,
count and index (hostile-input safe; idempotent). The cook writes little
endian; the cart fixes the order once when the chunk is opened.

## Ids

Written by the id step after `touchrank`, before `w64c` packs the chunk
(`tools/cook/stage.py`); header flag `HLE_F_IDS` (2) says it ran. The step
only writes the three `id` fields and the flag; every offset and string
stays, so `rec_key_str` keeps working at spawn time.

- `class.id`: line order of the classname in `game/defs/classes.def`
  (case-insensitive), 0xFFFF for a class the game lacks.
- `keyname.id`: line order of the key name in `game/defs/keys.def`
  (exact); a key the cook keeps that keys.def lacks stops the cook.
- `key.id` by value kind, 0xFFFF when none:
  - `HLE_KS`: `pattern` → lightstyle pattern id; `globalstate` →
    global-state id; monstermaker `monstertype` → classes.def id;
    env_message `message`, worldspawn `chaptertitle` → title id (0xFFFF
    when titles.txt lacks it: the game shows the string); a resource value
    → `res_t`: model / sprite / sound names as restab names them, `*N` →
    `RES_K_SUBMODEL` index N (the runtime ORs the slot: `RES_SUBMODEL(slot,
    N)`), `!NAME` in `message`, `sentence`, `noise*`, `UseSentence`,
    `UnUseSentence` → `RES_K_SENTENCE`, a speaker's `message` (no `preset`)
    and a talk monster's plain `UseSentence` / `UnUseSentence` →
    `RES_K_GROUP`; infodecal `texture` → decal id: below 42 the entry of
    the global sheet `gsfx/decals.i4` (HLSDK decals.h order), 42 + i entry
    i of the map's own placed-decal sheet (the package's `DECL` chunk,
    w64-format.md), 0xFFFF when the player's `decals.wad` lacks the texture
    (HLSDK `CDecal::Spawn` removes the entity; the cook warns).
  - `HLE_KN`: the classes.def id when the name is a classname
    (`m_iszEntity "monster_barney"`).
  - `HLE_KF`, `HLE_KV`: 0xFFFF.

The ids come from the campaign resource table's name sections. The table
lists the map packages (whose CRCs depend on the chunk), so the stage asks
restab for the id spaces first (`restab.cook` with the map list and no
packages: ids depend on names only) and `make cook` checks afterwards
(`hleids --check`) that the final `campaign.rtb` gives every packaged chunk
identical ids and that every `res_t` lies in the table and inside its cart
file (precache-and-resources.md R2). The same check proves the HLE region
(architecture.md 4.1): with engine/ent/rec.c's bytes (`REC_RT_BYTES`,
`REC_LOGIC_BYTES`, `REC_TABLE_BYTES` in `engine/ent/rec_int.h`, asserted
against the structs) it takes rec_init's tables + the largest open-time
need (16-aligned chunk + records × (runtime + logic + 2) + 16) + the
largest open block (chunk + runtime, logic blocks for logic records only) +
2 × the largest frozen runtime part (a closed map keeps 2-bit record
states, padded to 4 B, + its logic blocks; `REC_FROZEN_MAPS` 2), each
16-aligned, prints it, and fails above
`HL64_HLE_BLOBS_KB`. The engine (`engine/ent/rec_hle.c`)
rejects a chunk without `HLE_F_IDS`, an id outside its space (classes,
keys.def, patterns, global states, titles, each `res_t` kind's count,
submodels of the layer), a lump map that disagrees with its record, and
name / multisource lists naming dropped lumps. The same check proves the
decal region (tools/cook/decals.py): align16(global sheet) + 2 ×
align16(largest `DECL`) + `HL64_FX_DECALS` × `HL64_FX_DECAL_VERTS` × 10 B
(piece vertices) must fit `HL64_DECAL_TEXELS_KB`; it prints the need.

## Cook rules

- Classes: only `hle_classes[]` (what the native game spawns); others are
  dropped (`info_node`, `env_beam`, `env_bubbles`, `env_sound`,
  `trigger_autosave`, `func_tank*`, …). `infodecal`
  and `env_glow` are kept: GoldSrc shows placed decals and glows.
  - `func_tracktrain` (HLSDK `CFuncTrackTrain`, game/hl/trains.c): physical
    record. Keys: `wheels`, `height`, `startspeed`, `sounds`, `volume`,
    `bank`, `speed` as the map sets them, and the cook-made `#trackorigin`
    (`HLE_KV`, hl_entcook.h `hle_trackorigin`): the origin of the
    `path_track` its `target` names, raised by `height`, where its Find
    puts it 0.1 s after spawn. The record's wake box is that point ± the
    model's largest extent (engine/ent/rec.c `cooked_bounds`), not the
    model's storage place; a fresh one is spared the distance release for
    `REC_TRACK_HOLD` (0.5 s), else the release pass would dehydrate it at
    the storage place before its Find, out of reach of its wake box.
  - `infodecal` (HLSDK `CDecal`): physical record (class flags 0), every
    one kept. Keys: `texture` (`HLE_KS`, keys.def `TEXTURE`; id = decal id,
    above; `rec_key_decal`), `origin` (`HLE_KV`); the targetname is the
    record's `tname`. No targetname: the decal is placed at spawn
    (`StaticDecal`); with one: placed when used (`TriggerDecal`).
  - `env_glow` (HLSDK `CGlow`, effects.cpp): physical record. Keys:
    `model` (`HLE_KS`, sprite `res_t`), `origin`, `rendercolor` (`HLE_KV`),
    `rendermode`, `renderamt`, `renderfx`, `scale`, `framerate`
    (`HLE_KF`), as the map sets them (absent = HLSDK default).
- Pruned: logic classes that act only when used and have no targetname
  (`multi_manager`, `trigger_relay`, `multisource`, `env_render`,
  `env_shake`, `env_fade`, `env_message`, `player_weaponstrip`,
  `env_explosion`, `env_shooter`, `gibshooter`, `light`, `light_spot`,
  `env_beverage`), and `info_target` / `path_corner` / `path_track` /
  `info_teleport_destination` whose name no key in the map references
  (fixed point). Names are kept even when nothing
  in this map fires them: a neighbouring map's `changetarget` may.
- Keys: the entvars fields the game fills from entity keys (`hle_fields[]`:
  HLSDK `gEntvarsDescription`, set by `DispatchKeyValue`) and the keys
  HLSDK classes read in their own `KeyValue` (`hle_xkeys[]`); `_*` keys and
  `""`/`"0"`/`"0 0 0"` values dropped; `angle` → `angles 0 a 0`, `\n`
  escapes expanded. The loader rejects a record whose key names an index
  outside the keyname table; the game's spawn reads only the keys its class
  declares.
- Rewrites: trigger_changelevel `landmark` → `message`; info_landmark
  targetname also as `message`; multi_manager non-field keys → fire list
  (`name#n` → `name`).
- Pre-resolved: name → records (lump order); multisource inputs (entities
  whose `target` names it, then multi_managers listing it — GoldSrc
  `CMultiSource::Register` order); multi_manager fire lists (stable sort by
  delay, HLSDK bubble sort order).

## Runtime requirements (hl64 `engine/ent/rec.c`)

- One resident chunk per open map (the HLE slot = the world layer slot,
  2); global record id `gid = slot << 10 | lump`, carried in the entity's
  `id` component (architecture.md 6.2); names resolve in the caller's
  source map, callers without one use the home map (cross-map
  `changetarget` fires through the target map's name index). The chunk is
  read asynchronously at OPEN priority once the layer is ready; nothing of
  the map hydrates before it lands. A closed map keeps only its frozen
  runtime part (2-bit record states + logic blocks, 2 maps; a third
  freeze forgets the oldest) by CWG map; the chunk is re-read on reopen.
- Physical records: the game's spawn for the class reads the typed keys in
  place (strings point into the chunk, nothing allocated). Logic records:
  a small per-record state block plus a class handler; point records are
  queried by name or class.
- Delays (multi_manager steps, CBaseDelay `delay`, fire-and-die, logic
  thinks) run from a bounded 128-entry timer queue holding the f32
  nextthink HLSDK would write (specs/think-and-time.md 5), serviced at the
  start of the game frame — no helper entities are created for them.
- The `mem` debug command reports per-map chunk / record / runtime bytes and
  the timer peak. Measured (hl64 cook, M3): hldemo1 48,912 B chunk (447
  records: 166 logic, 275 physical) + 14,278 B runtime (24 B per record,
  16 B per logic record, the pending list); hldemo2 44,624 B (420 records).
  M4 (c6 cook): hldemo1 488 records (166 logic) frozen part 2,780 B,
  hldemo2 486 (133 logic) 2,252 B.

## Node graph (`HLN1`, optional tail)

A map with `info_node` / `info_node_air` entities carries its GoldSrc node
graph right after the HLE1 chunk, at `align16(header.size)` (the chunk
`size` stays the HLE1 size; the `ENTS` chunk holds both). Built by the cook
(`tools/w64c/hlecook`): HLSDK `CTestHull::BuildNodeGraph` run offline on
the BSP's hulls with GoldSrc's trace rules
([trace-and-hulls.md](../specs/trace-and-hulls.md)) — land nodes dropped
to the floor, `LinkVisibleNodes` (link entity: one brush entity blocking
both ways), walk rejection per hull (small / human / large world-only walk
move, fly hull trace), `RejectInlineLinks`. The Uplink cook equals the
shipped `maps/graphs/hldemo{1,2,3}.nod` and `t0a0d.nod` link for link.

| Section | Element | Notes |
|---|---|---|
| header | `hln_header_t` (24 B) | magic `"HLN1"`, version 1, flags (`HLN_F_BIGENDIAN`), size, `numnodes` (≤ 1024), `numlinks`, offsets |
| nodes | `hln_node_t` (16 B) | origin (map space; land nodes on the floor), `first_link` / `num_links`, info (`LAND` / `AIR` / `WATER`) |
| links | `hln_link_t` (6 B) | dest node, hull bits (`SMALL` / `HUMAN` / `LARGE` / `FLY`), link entity kind + door flags (`USE_ONLY`, `NO_AUTO_RETURN`, `NOMONSTERS`), its BSP model; weight = 2D node distance |

Links blocked by brush entities other than doors and `func_breakable` are
not kept (HandleLinkEnt never passes them). `hln_fixup` validates and
swaps it in place like `hle_fixup`; the record module keeps it per HLE
slot for the game's monster routing (`game/`).
hldemo1: 41 nodes, 122 links, 1,412 B.

## Sizes (Uplink, measured with the old cook)

| Map | BSP entity text | chunk | records (entity / logic / point) |
|---|---:|---:|---|
| hldemo1 | 105615 B | 45488 B | 433 |
| hldemo2 | 99045 B | 39536 B | 403 |
| hldemo3 | 66418 B | 29032 B | 285 |
