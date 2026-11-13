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
  inside the trigger brush's **hull 1** (standing-player hull) solid,
  tested in the model's unrotated frame — the trigger's angles are ignored
  (a yaw-30° trigger fires on its unrotated shape) — so a non-box trigger
  fires only inside its shape, not anywhere in its bounding box
  **[probe:measured A-shape]** (960 samples, player-size movers). Movers of
  other sizes are not yet measured; until they are, hl64 uses hull 1 for
  every mover **[probe]**.

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
  its midpoint (31 cells, a binary tree); a trigger belongs to the
  deepest cell whose box contains its linked box (it stays at the first
  split plane it straddles). A trigger in an **ancestor cell** touches
  before one in a descendant cell: **666 of 666** co-touched placements in
  ancestor/descendant cells **[probe:measured A-order]**. Within one cell,
  brush triggers touch in ascending entity index (**411 of 411**
  placements); pairs involving point triggers (items, SOLID_TRIGGER
  weapons) follow ascending index in 2 212 of 2 405 placements; the 193
  exceptions all involve items, which relink after
  spawn when they settle **[probe:measured A-order]**. 33 placements
  co-touched triggers in disjoint cells (order there is the tree walk:
  the cell nearer the mover's box first) **[probe]**.
- **Map triggers**: the `touchrank` cook step computes `trank` (HLE1 field,
  `docs/formats/hle-format-v1.md`) **from geometry with the cell model**:
  cells in pre-order, then entity index. It needs no capture, so it works
  on any player-supplied map; the A-order captures are its regression test
  (A9), and a mismatch on a probed map fails the cook check. Where point
  triggers end up after settling is part of the open item below.
- **Triggers created at run time** touch in **creation order** (the order
  they were allocated), not index order: a trigger of index 179 created
  earlier touches before one of index 7 created later
  **[probe:measured A-touch, A-order]** (all 10 maps). Whether they
  interleave with map triggers by cell is not yet measured **[probe]**;
  Uplink and Hazard create none.
- **Re-enabled / relinked triggers reorder**: a trigger that is unlinked
  and linked again changes its place within its cell; after the probe's
  sequential re-enables, the most recently re-enabled trigger touched
  **first** among same-cell partners in 392 of 421 placements, and order across
  cells never changed (88 of 88) **[probe:measured A-order]**. hl64 rule:
  the touch key is (cell, link sequence); spawn assigns ascending
  sequences by index; re-enabling a trigger (SOLID_NOT → SOLID_TRIGGER)
  gives it a sequence ahead of every trigger currently in its cell. The
  29 re-enable pairs this rule misses and the 193 settled-item pairs are
  tracked by A9 until the rule is refined **[probe]**. Which HLSDK classes can be re-enabled:
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
| A5 | brush trigger fires iff the mover's origin is inside its hull-1 brush, trigger angles ignored (a yaw-30° trigger uses its unrotated shape); inside the bbox but outside the brush never fires | **[probe:measured A-shape]** (960 samples) |
| A6 | touch that frees the trigger / another trigger / the toucher: no crash, no touch of freed entities | synthetic |
| A7 | impact touch: A.touch(B) then B.touch(A); no B.touch(A) when A removed itself in A.touch(B); a SOLID_NOT mover passes through box entities with no impact and no touch | **[probe:measured A-touch]** |
| A8 | pending record hydrated and touched in the same frame | synthetic |
| A9 | touch order: runtime-created triggers in creation order (index 179 created before index 7 touches first); map triggers in geometry-computed `trank` order (ancestor cell first 666/666, same-cell brush triggers by index 411/411; 526 pairs consistent, no cycle); re-enabled trigger first in its cell (392/421); force retouch lasts one frame | **[probe:measured A-touch, A-order]** (all 10 maps); every A-order placement is a regression case for `touchrank` |
