# Linked boxes, spatial grid, touches

Behaviour: which entities a move collides with and which triggers it
touches, and in which order, as in GoldSrc. Mechanism: one hashed
multi-level grid instead of GoldSrc's area-node tree, plus a cooked touch
rank and a creation counter that reproduce GoldSrc's trigger touch order
(3.1).
Conventions and oracles: [README.md](README.md).

## 1. Linked box (absmin / absmax)

Recomputed on every link:

1. Rotated brush (SOLID_BSP, any angle non-zero): a cube centred on the
   origin whose half-size is the **largest** per-axis value of
   max(|mins[i]|, |maxs[i]|) — not the length of that vector, so the
   corners of a long rotated brush can stick out of its linked box — then
   widened as in 3; e.g. extents (124, 173, 17) give half-size 174
   **[probe:measured A-touch]** (rotated brush entities on all 10 maps).
2. Otherwise `origin + mins … origin + maxs`. Only SOLID_BSP entities use
   rule 1: a trigger (SOLID_TRIGGER) with angles keeps its unrotated box
   **[probe:measured A-shape]**.
3. Every linked box is then widened by 1 u on every axis, whatever its
   solid type and flags **[probe:measured A-touch]**. There is no item
   widening: HLSDK has no FL_ITEM, and bit 256 (FL_SKIPLOCALHOST) gets no
   extra 15 u (the old tree's Quake rule is not GoldSrc).

## 2. Spatial grid (mechanism)

As built (M2, `engine/ent/grid.c`, Grid):

- **One hashed grid over global coordinates** (not one per layer), so its
  size does not depend on the maps (t0a0 alone would need 19 × 18 × 8
  cells of 256 u) and entities outside every layer need no special case.
- **Three levels**: 256 u cells (the COLL grid's), 1024 u and 4096 u. An
  entity is a member of every cell its linked box touches, at the first
  level where the box spans at most two cells on every axis; a box too big
  for level 2 goes to the **huge** list, which every query walks.
- **Multi-cell membership**: an entity's first membership lives in its
  node, further ones come from a shared pool of 8 B entries (1485 entries,
  carved inside the COMPONENTS region); when the pool runs dry the entity
  goes to the huge list (slower, still exact).
- **Node**: 16 B per entity (kind, query stamp, rounded box), one cache line
  per rejected candidate. Cells map to hash buckets by one float-to-int
  conversion per coordinate (any non-decreasing map keeps "boxes that meet
  share a cell"); a 128 B bitmap of non-empty buckets lets queries skip
  empty cells without touching their heads.
- A query walks, on every non-empty level, the cells its box touches,
  reports each entity once (per-query stamp), and scans the slots instead
  when its box touches too many cells. Triggers, solids and contents
  brushes (SOLID_NOT brushes with a contents skin: `func_ladder`,
  `func_water`) share the grid; the node's kind filters them. Other
  SOLID_NOT entities are not linked.
- Order of results: solid candidates in ascending entity index (it only
  matters for exact ties, trace-and-hulls.md 4.4 step 7); trigger
  candidates in touch order (3.1).
- Memory: the grid tables use 2 KB of the THINK_GRID region; nodes and the
  membership pool are in COMPONENTS (architecture.md 4.1).
- Cost on the cart (ares, `test/m2/grid_bench.sh`): **18.6 µs per query,
  17.2 µs per relink** (measured, Grid).
- Leaf/sector membership for visibility is not stored per link
  (architecture.md 5.5: sectors for rendering, exact leaf on demand).

## 3. Trigger touch

When an entity is linked **with touch** (all physics motion, the player
move, walk moves; not `set_origin`, not `drop_to_floor`, not a pusher's own
link), every SOLID_TRIGGER entity with a touch function whose linked box
overlaps the mover's linked box (touching faces count) is touched,
`self` = trigger, `other` = mover, provided:

- it is not the mover;
- for a brush trigger (the trigger has a BSP model): the mover's origin is
  inside the trigger brush's solid in the **hull of the mover's size**,
  selected and placed as for a trace (trace-and-hulls.md 2: hull 0 for a
  point-size box, hull 1 / 3 / 2 by size, the brush at origin + hull.mins −
  mover.mins), tested in the model's unrotated frame — the trigger's
  angles are ignored (a yaw-30° trigger fires on its unrotated shape) — so
  a non-box trigger fires only inside its shape, not anywhere in its
  bounding box **[probe:measured A-shape]** (960 samples, player-size
  movers). The hull follows the mover's size **[probe:measured A-order]**:
  of the 499 brush-trigger touches of hull-2-size movers (64 × 64 × 64),
  384 have the origin outside the trigger's bounds grown by the hull-1
  extents and all 499 inside them grown by the hull-2 extents; with this
  rule the A9 replay touches exactly GoldSrc's ranked triggers at all
  3 452 placements (point 448, human 1 176, large 1 052, head 776 movers),
  hull 1 for every mover missed 218 large-mover placements.

Because both linked boxes are widened by 1 u, a mover and a box trigger
that overlap on two axes touch while the gap between their real boxes on
the third is ≤ 2.0 u, and not at 2.01 u **[probe:measured A-touch]** (each
axis; FLY movers and the fake client).

Touches may free or relink any entity; the pass continues over the
candidate list taken before the first call, skipping entities freed since.

### 3.1 Touch order

GoldSrc touches overlapping triggers in its own (area-node) order, not in
entity index order. Measured on A-order (Oracle2, hl64 `d2bf3d8`, 10 maps;
526 map-trigger pairs, all consistent between placements, no cycle;
analysis script `/scratch/quakegeneric/tmp/EngineSpec/acell4.py`):

- **Cell model** (fitted to the capture, prose only): halve the world
  model's box four times, each time along the longer horizontal axis at
  its midpoint (31 cells, a binary tree; no captured map has equal x and y
  extents at any split, hl64 then splits y **[probe]**); a trigger belongs
  to the deepest cell whose box contains its linked box (it stays at the
  first split plane it straddles, and a box face lying **on** a split
  plane also stays above it: the other choice breaks 1 pair). A trigger in
  an **ancestor cell** touches before one in a descendant cell: **666 of
  666** co-touched placements in ancestor/descendant cells
  **[probe:measured A-order]**. Triggers in disjoint cells touch in
  pre-order with the **upper child** (coordinates above the split) first,
  whatever the mover's position (lower child first breaks 11 pairs)
  **[probe:measured A-order]**.
- **Order inside a cell** is the order of each trigger's last link before
  play, entity index within a group: brush triggers, `path_track` and
  items (`item_*`: linked at spawn, dropped to the floor in their spawn),
  then ammo (`ammo_*`: MOVETYPE_TOSS, relinked when it lands in the first
  physics frames), then weapons (`weapon_*`: SOLID_TRIGGER only when their
  fall think materialises them, ≥ 0.1 s) **[probe:measured A-order]**.
  Linked boxes at that point: brush triggers model bounds + origin ± 2
  (GoldSrc pads model bounds by 1, the link by 1 more); `path_track`
  origin ± 9; items and ammo origin + (−17, −17, −1)…(17, 17, 17); weapons
  origin + (−24, −24, 0)…(24, 24, 16) (CBasePlayerItem's own collision
  box, not widened) — all equal to the captured boxes on the 10 maps.
  Ammo that falls longer than a weapon's first fall think is not
  captured; the groups are then a model, not a measurement **[probe]**.
- **Map triggers**: the `touchrank` cook step (`tools/w64c/touchrank.py`,
  after `hlecook`) computes `trank` (HLE1 field,
  `docs/formats/hle-format-v1.md`) **from geometry with the cell model**:
  `cell << 11 | place`, cells in pre-order, place in the cell's link
  order. It needs no capture, so it works on any player-supplied map; it
  ranks every GoldSrc SOLID_TRIGGER class (also those hlecook drops, such
  as `trigger_autosave`), so the kept ones keep GoldSrc's relative order.
  Check (`python3 -m tools.w64c.touchrank --check FIXTURES --maps-dir
  MAPS [--content CONTENT]`, A9): of the **526** captured map-trigger pairs
  (phases 1 and 3), **506** touch in `trank` order, **19** involve a
  trigger the game created (no lump), and **1** is known: hldemo2
  `item_healthkit` (lump 601) spawns inside `func_wall_toggle` `*80` and
  falls 8 u after spawn (z 168 → 160), so GoldSrc relinks it after
  `ammo_9mmAR` (lump 509) lands; geometry cannot see that relink. Every
  captured placement's full list of touched map triggers is in `trank`
  order except the 9 that hold that pair (3 443 of 3 452)
  **[probe:measured A-order]**.
- **Triggers created at run time** touch in **creation order** (the order
  they were allocated), not index order: a trigger of index 179 created
  earlier touches before one of index 7 created later
  **[probe:measured A-touch, A-order]** (all 10 maps). Whether they
  interleave with map triggers by cell is not yet measured **[probe]**:
  of the 19 A-order pairs with a game-created trigger (Hazard Course
  batteries and MP5s, a hldemo2 handgun), the 17 mixed pairs touch the map
  trigger first and the 2 between created triggers in index order, but the
  cell model predicts the same order for all 19.
- **Re-enabled / relinked triggers reorder**: a trigger that is unlinked
  and linked again changes its place within its cell; after the probe's
  sequential re-enables, the most recently re-enabled trigger touched
  **first** among same-cell partners in 392 of 421 placements, and order across
  cells never changed (88 of 88) **[probe:measured A-order]**. hl64 rule:
  the touch key is (cell, link sequence); spawn assigns ascending
  sequences in the cooked `trank` order; re-enabling a trigger
  (SOLID_NOT → SOLID_TRIGGER) gives it a sequence ahead of every trigger
  currently in its cell. The 29 re-enable pairs this rule misses are
  tracked by A9 until the rule is refined **[probe]** (the former 193
  settled-item exceptions are the link-order groups above). Which HLSDK classes can be re-enabled:
  `trigger_multiple` has **no** toggle Use; ToggleUse exists on
  `trigger_hurt` (only with a targetname), `trigger_push` and
  `trigger_monsterjump` (HLSDK).

**Force retouch**: HLSDK sets `gpGlobals->force_retouch` when triggers are
enabled; it lasts **one frame** **[probe:measured A-order]**
(`force_retouch_frames` 1). On that frame every live entity in the active
set and every entity resting inside a trigger box is relinked with touch,
and the touches of that frame come in a different order from an ordinary
link (the relinked triggers' new sequences above) **[probe:measured
A-order]**. Mechanism: the engine keeps, per trigger, nothing extra; it
relinks the entities whose boxes overlap any trigger (grid query per
trigger), which is what is observable.

**Pending-record wake**: before the touch pass, pending map records whose
cooked bounds meet the mover's box are hydrated (architecture.md 6.4) so a
trigger that was never spawned is touched in the same link.

## 4. Impact touch

When a physics move stops on entity B (trace `pHit` = B): first A.touch(B)
if A has a touch function and is not SOLID_NOT; then, **only if A still
exists**, B.touch(A) if B has a touch function and is not SOLID_NOT — an A
that removes itself in its own touch gets no B.touch(A)
**[probe:measured A-touch]**. A SOLID_NOT mover passes through box
entities without stopping and without any impact touch (its moves ignore
monsters, physics-movetypes.md 2) **[probe:measured A-touch]**. A caller
checks A's liveness before continuing.

## 5. Tests

| # | property | data |
|---|---|---|
| A1 | linked box: unrotated box + 1 u on every axis for every solid type and flag (no 15 u widening for bit 256); rotated SOLID_BSP cube half-size = largest per-axis extent + 1 (e.g. (124, 173, 17) → 174) on all 10 maps; angled triggers keep unrotated boxes | **[probe:measured A-touch]**, A-shape |
| A2 | grid: every entity found by any box query that overlaps its linked box, none returned twice, at every level and through the huge list and a dry membership pool; solids in ascending index, triggers in touch order | randomised host test |
| A3 | box trigger touched once per link when the real-box gap is ≤ 2.0 u on any one axis (and overlapping on the others), including face contact; not at 2.01 u; same for FLY movers and the player | **[probe:measured A-touch]** |
| A4 | `set_origin` into a trigger touches nothing; the next physics move does | probe A-touch |
| A5 | brush trigger fires iff the mover's origin is inside its brush in the hull of the mover's size (trace hull selection), trigger angles ignored (a yaw-30° trigger uses its unrotated shape); inside the bbox but outside the brush never fires; A-order movers of every hull touch exactly GoldSrc's triggers (A9 touched h0–h3) | **[probe:measured A-shape]** (960 samples), **[probe:measured A-order]** (3 452 placements) |
| A6 | touch that frees the trigger / another trigger / the toucher: no crash, no touch of freed entities | synthetic |
| A7 | impact touch: A.touch(B) then B.touch(A); no B.touch(A) when A removed itself in A.touch(B); a SOLID_NOT mover passes through box entities with no impact and no touch | **[probe:measured A-touch]** |
| A8 | pending record hydrated and touched in the same frame | synthetic |
| A9 | touch order: runtime-created triggers in creation order (index 179 created before index 7 touches first); map triggers in geometry-computed `trank` order (ancestor cell first 666/666, upper child first, link-order groups; 506 of 526 pairs, 19 with a game-created trigger, 1 known; `touchrank --check` on the rule, `test/m2/area.py` replaying every placement through the engine with the cooked `trank`); re-enabled trigger first in its cell (392/421); force retouch lasts one frame | **[probe:measured A-touch, A-order]** (all 10 maps); every A-order placement is a regression case for `touchrank` |
