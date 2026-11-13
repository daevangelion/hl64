# HLE1 — GoldSrc binary entity chunk (v1)

Imported from the quakegeneric tree (`n64/docs/hle-format-v1.md`, audit
entry CLEAN, docs/provenance-audit.md); old-engine references restated.

One self-contained, position-independent chunk per GoldSrc map that replaces
the BSP entity text. Produced by the cook's `hle_build` (`tools/w64c/hlecook`,
imported per architecture.md 16.3) and stored as the `.w64` package's `ENTS`
chunk ([w64-format.md](w64-format.md)); the engine's record module
(`engine/ent`, architecture.md 6.4) uses it in place after one
endian/validation pass (`hle_fixup`).

hl64 extends the cook in M1 so that string-valued keys arrive as ids
(`name_t`, `res_t`, sentence and pattern ids from
[rtb-format.md](rtb-format.md)); until then the layout below is the cooked
byte format.

## Layout

All offsets are bytes from the chunk start; every section starts 4-aligned
(8-aligned except the hash). Indices, not pointers, everywhere.

| Section | Element | Count field | Notes |
|---|---|---|---|
| header | `hle_header_t` (96 B) | — | magic, version, flags, sizes, offsets |
| records | `hle_rec_t` (24 B) | `numrecs` | kept entities, BSP lump order |
| lump2rec | u16 | `numlumps` | BSP lump → record, 0xFFFF = not kept |
| classes | `{u32 str; u16 flags; u16 pad}` | `numclasses` | classname string + `HLE_CF_*` |
| keynames | `{u32 str; u16 kind; u16 pad}` | `numkeynames` | interned key names + value kind |
| names | `{u32 str; u16 first, count; u16 flags, pad}` | `numnames` | targetname-like strings → lumplist slice |
| hash | u16 | `hashsize` (pow2) | open-addressed FNV-1a of name strings → name index |
| keys | `{u16 kn; u16 pad; u32 val}` | `numkeys` | typed key values |
| lumplist | u16 | `numlumplist` | name lists, then multisource input lists (BSP lumps) |
| fire | `{u16 name; u16 pad; f32 delay}` | `numfire` | multi_manager fire lists, sorted by delay |
| floats | f32 | `numfloats` | vec3 values |
| strings | bytes | `strsize` | names region `[0, namestr_end)`, then other strings |

`hle_rec_t`: `lump` (BSP entity index), `cls`, `tname` (name index of the
targetname, 0xFFFF none), `flags`, `key_first`/`key_count`,
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

## Cook rules

- Classes: only `hle_classes[]` (what the native game spawns); others are
  dropped (`info_node`, `env_glow`, `env_beam`, `env_bubbles`,
  `infodecal`, `env_sound`, `trigger_autosave`, `func_tank*`,
  `func_tracktrain`, `monster_miniturret`, `monster_rat`, …).
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

## Runtime requirements (hl64 `engine/ent`)

- Up to 4 resident chunks (the HLE slots of architecture.md 5.3); global
  record id `gid = slot << 10 | lump`, carried in the entity's `id`
  component (architecture.md 6.2); names resolve in the caller's source
  map, callers without one use the home map (cross-map `changetarget`
  fires through the target map's name index).
- Physical records: the game's spawn for the class reads the typed keys in
  place (strings point into the chunk, nothing allocated). Logic records:
  a small per-record state block plus a class handler; point records are
  queried by name or class.
- Delays (multi_manager steps, CBaseDelay `delay`, fire-and-die, logic
  thinks) run from a bounded 128-entry timer queue in µs, serviced at the
  start of the game frame — no helper entities are created for them.
- The `mem` debug command reports per-map chunk / record / runtime bytes and
  the timer peak.

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
