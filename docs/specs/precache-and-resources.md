# Resources: cooked ids, precache, residency interest

Behaviour: every model, sprite and sound the game or a map refers to is
available when used, as in GoldSrc. Mechanism: a cooked, campaign-wide
resource table with stable 16-bit ids replaces GoldSrc's per-level
precache-by-name tables (and the old tree's shared slot tables with reuse).
Conventions: [README.md](README.md).

## 1. The resource table (`campaign.rtb`, cooked by `tools/restab`)

- One entry per distinct model (`.mdl`), sprite (`.spr`, cooked `.sc4`),
  sound (`.wav` → `.wav64`) and studio animation chunk set (D3) used by any
  map of the campaign or by the game: names from the maps' entity lumps
  (cooked keys), from `game/defs/res.def` (the game's own precache lists:
  weapons, monsters, effects, HUD sounds) and from sentence words.
- Entry (12 B): kind u8, flags u8, file index u16 (cart file table),
  offset u32 within the file, size u32. Ids are dense per kind; `res_t` =
  kind << 13 | index (13-bit index: 8191 per kind; full Half-Life needs
  ≈ 600 models/sprites and ≈ 2500 sounds [EST]).
- Names are not on the cart; the host and debug builds carry a side table
  for printing.
- Brush submodels are not entries: they are (map slot, submodel number),
  per map (`*3` of hldemo1 is not `*3` of hldemo2), encoded as the `res_t`
  kind SUBMODEL with index = slot << 10 | submodel.
- The game refers to its own resources by generated constants
  (`RES_MODEL_SCIENTIST`, …) from `res.def`; map keys holding resource
  names (`model`, `noise*`, `message` for ambient_generic, sprites…) are
  cooked to `res_t` in HLE1.
- The cook fails if a name cannot be resolved to a file in the player's
  data (report lists the referencing map and record).

## 2. Precache semantics

- `precache(res)` is a residency **interest**: the model/sprite/sound is
  prefetched by distance and kept while referenced. It never allocates an
  index, never fails, and is legal at any time (in GoldSrc only during
  level load; one world keeps "loading" maps).
- Using a resource that was not precached works (it streams on first use)
  and logs once in debug builds; GoldSrc would error — not reachable by the
  shipped game, and the debug log keeps the game honest.
- The studio pool's eviction classes (SA2 §8.1) apply: models of live
  entities, the player and the view model are pinned; client-visible and
  home-map interests are soft; others cold.

## 3. Carried weapons

The player's carried weapons keep interest in their view model, world
model, fire/reload sounds and (MP5, hand grenade) grenade/explosion assets
for as long as they are carried, independent of the maps that are open
(HLSDK `W_Precache` keeps them for every level). The old tree died with
`v_crowbar.mdl not precached` after two maps closed; with static ids this
failure mode does not exist, and the interest rule keeps the data warm.

## 4. Reporting

Stats records carry resident models / sounds and studio pool use/peak.

## 5. Tests

| # | property | data |
|---|---|---|
| R1 | `restab` gives the same ids for the same inputs (deterministic order: kind, then name), fails on unresolved names with the referencing record | self-contained |
| R2 | every `res_t` in every HLE1 blob and in `res.def` resolves to an entry whose file range lies inside the ROM file | cook check |
| R3 | `*3` from hldemo1 and hldemo2 records are different models with their own bounds | two cooked layers |
| R4 | after picking up the MP5 in hldemo1 and closing hldemo1 and hldemo2, every weapon switch draws and sounds without a synchronous read stall > 1 frame | route segment hldemo3 → hldemo2-return |
| R5 | a rehydrated monster whose model was evicted draws within the studio prefetch rule (no T-pose frame, ≤ 3 frames late) | route stats |
| R6 | the cart has no string compare on the resource path (provcheck: no `strcmp`/`strcasecmp` in `engine/res/`, `engine/snd/`, `engine/render/`) | static check |
