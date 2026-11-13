# Area links and touch

How entities are registered in space for collision queries and trigger
touches. Conventions: [README.md](README.md).

## 1. Linked box (absmin / absmax)

Recomputed every time an entity is linked:

1. **Rotated brush** (SOLID_BSP with any non-zero angle): a cube centred on
   the origin whose half-size r is the length of the vector whose
   components are, per axis, max(|mins[i]|, |maxs[i]|). (It bounds the model
   box under any rotation about the origin.)
2. Otherwise `origin + mins` … `origin + maxs`.
3. Then: if FL_ITEM is set, x and y extend by 15 u on both sides (z
   unchanged) so items are easy to pick up off shelves; otherwise every axis
   extends by 1 u on both sides (movement stops 1/32 u short of contact, so
   contact must still overlap).

`absmin/absmax` in the core record are these linked values; they are only
written by linking.

## 2. Linking rules

- The world (entity 0) and free entities are never linked.
- Linking first unlinks the entity from wherever it was.
- The entity's PVS leaf set is recomputed when it has a model index (any
  solid type): up to 8 leaves of its layer that its linked box touches,
  otherwise flagged "large" (visibility then tests its box against the
  visible set). Its layer = the open layer whose global box contains the
  linked box's centre (−1 if none).
- SOLID_NOT entities are not put in area lists (traces and touches never
  see them).
- SOLID_TRIGGER entities go into the node's trigger list; every other solid
  type into the node's solid list.
- The node is the deepest area-tree node whose split plane the linked box
  does not cross (a box straddling a split stays at the parent).
- Insertion is at the tail of the node list.

### 2.1 Area tree

Built once at New Game over the home campaign component's global box (all
maps of that component at their `g_M`). Each node splits its box at the
midpoint of its longest axis (x, y or z) until a node's longest side is
≤ 1024 u or depth 7 (≤ 256 nodes). Leaves have no split. Entities in other
components never link (they cannot be open at the same time).

## 3. Trigger touch on link

When an entity is linked **with touch** (all physics movement, the player
move, walk/step moves; not `set_origin`, not `drop_to_floor`, not pusher
self-links), every trigger is touched whose linked box overlaps the moved
entity's linked box, where overlap means no axis has `a.min > b.max` or
`a.max < b.min` (faces in contact count as overlapping), and which:

- is not the moved entity itself,
- is SOLID_TRIGGER at the time of the test and has a touch function.

For each, the trigger's touch runs with `self` = trigger, `other` = moved
entity, game time = current frame time. Traversal: from the root, a node's
trigger list in list order, then the front child (if the box's max on the
split axis is beyond the split) and the back child (if the box's min is
below it). The traversal tolerates any entity being freed or relinked by a
touch: a freed entity is not touched again in this pass and the walk
continues with the entity that followed it when the touch began; an
entity relinked during the pass may or may not be visited again (unspecified,
not observable in the Uplink/Hazard routes).

Trigger shape: the oracle tests boxes only (a trigger brush that is not an
axis-aligned box touches anything inside its bounding box). GoldSrc may
refine brush-shaped triggers against the trigger's hull [not measured];
hl64 implements the box test (D6).

**Force retouch**: the game can request that for the next N frames every
live entity is relinked with touch at the start of its physics step even if
it did not move (used when triggers are enabled under a standing entity).

**Pending-record wake**: before the touch pass, pending map records
(architecture.md 6.3) whose cooked bounds meet the moved entity's linked box
are hydrated, so a trigger that was never spawned is touched in the same
link. The same wake runs for every move query's swept box
(trace-and-hulls.md 4.4).

## 4. Impact touch

When a physics move (fly/toss/step slide, pusher push, walk move) stops on
an entity `B` (trace `ent` = B, B ≠ world or world included — the world
has no touch), the mover `A` and `B` touch:

1. if A has a touch function and A is not SOLID_NOT: A.touch(other = B);
2. then, if B has a touch function and B is not SOLID_NOT (re-checked after
   step 1): B.touch(other = A).

Both run with the current frame time. Either may free A or B; the caller of
the move checks A's liveness before continuing.

## 5. Unlink

Unlinking removes the entity from its node list and clears its leaf set.
Freeing an entity unlinks it.

## 6. Tests

| # | property | data |
|---|---|---|
| A1 | linked box rules 1–3 for: unrotated box, FL_ITEM box, rotated brush at several angles (cube half-size = |max-abs-extent vector|), SOLID_NOT with model (leaf set but no list) | self-contained |
| A2 | node choice: a box straddling each split stays at the parent; a box fully inside a leaf cell goes to that leaf; tree depth ≤ 7, ≤ 256 nodes, cells ≤ 1024 u on Uplink and Hazard component boxes | self-contained on cooked CWG3 |
| A3 | trigger touch fires exactly once per link for every overlapping trigger, including face contact (boxes sharing a face), and not for an adjacent trigger 0.01 u away | synthetic |
| A4 | `set_origin` into a trigger fires nothing; the next physics move fires it | synthetic |
| A5 | a touch that frees the trigger, another trigger, or the toucher does not crash, does not touch a freed entity, and continues with the remaining triggers | synthetic |
| A6 | impact touch order: mover first, then the hit entity; a hit SOLID_NOT entity's touch is not called; a mover freed by its own touch skips step 2 | synthetic |
| A7 | trigger_once / trigger_multiple / trigger_changelevel / trigger_teleport fire on the same route frames as the oracle along the Uplink route (first-fire frame per trigger lump, ±1 frame) | fixture A-links: `(map, lump, frame, activator class)` of every trigger touch that called a fire, from the oracle's host route |
| A8 | pending records: a trigger whose record is pending and whose bounds overlap the player is hydrated and touched in the same frame | synthetic two-record map |
