# CWG3 campaign graph and campaign seams

Imported from the quakegeneric tree (`n64/docs/cwg-format.md`, audit entry
CLEAN, docs/provenance-audit.md); the runtime fallback is restated as an
hl64 requirement. Runtime: architecture.md 5.4.

`tools/cook/hl_campaign.py` discovers the graph from BSP entity lumps and
writes `maps/campaign.cwg` with empty seam tables. After `.w64` packaging,
`tools/w64c/w64seam.py` rewrites that graph with certified closed switches,
or OPEN seams where no closed switch point exists (both run by `make cook`,
`tools/cook/stage.py`).
Nothing depends on map names. CWG1 and CWG2 are rejected (clean cutover).

All numbers are big-endian; tables are packed without alignment padding.
Counts must be at most `0xFFFE`; `0xFFFF` is the absent-index sentinel.
String offsets are u16 byte offsets into the final NUL-terminated string table.

## Layout

Header, 32 bytes:

| Offset | Type | Field |
|---:|---|---|
| 0 | u32 | magic `0x43574733` (`CWG3`) |
| 4 | u16 | version 3 |
| 6 | u16 | flags 0 |
| 8 | u16[3] | map, landmark, link counts |
| 14 | u16[2] | start map, training map (`0xFFFF` absent) |
| 18 | u16 | component count |
| 20 | u16[3] | seam, twin, sector-pool counts |
| 26 | u16 | reserved 0 |
| 28 | u32 | string table bytes |

Tables follow: maps (48 B), landmarks (16 B), links (8 B), seams (136 B),
twins (4 B), sector pool (2 B), then strings. Exact file size:
`32 + 48*nmaps + 16*nlandmarks + 8*nlinks + 136*nseams + 4*ntwins + 2*nsectpool + strbytes`.

Map: u16 name, component, first landmark, landmark count, first link, link
count (bytes 0–11); f32 global placement `g_M[3]` (12),
map-local world-model mins[3] (24), maxs[3] (36).
Landmark: u16 name, reserved zero; f32 origin[3].
Link: u16 destination map, landmark name, spawnflags, seam (`0xFFFF` absent).
Maps retain component/BFS order. The unused CWG2 landmark-frame map
`offset` is removed; landmark-frame placement remains in cook JSON diagnostics.

Placement uses BSP world-model bounds, rounded outward to a 16-unit grid
for packing only. Unrotated 3D shelves pack **all** maps of **all**
components disjoint with at least 128 units between their actual boxes.
Candidate shelf widths/depths minimize the longest axis, then box volume;
the result is centered near the origin with integer multiples-of-16
translations (no rotation or scale). Local geometry stays unchanged.
The cook reports every `g_M`, actual global bounds and binary32 ULP at
the farthest corner; ULP must be at most `DIST_EPSILON / 16` (0.001953125).
Integer local coordinates translate exactly within this precision range;
arbitrary sub-ULP fractional positions still obey ordinary float rounding.

## Seam record

| Offset | Type | Field |
|---:|---|---|
| 0 | u16[4] | from map, to map, landmark string, trigger entity lump |
| 8 | u16 | changetarget string (`0xFFFF` absent) |
| 10 | u16 | kind: 0 touch, 1 USEONLY (`spawnflags & 2`) |
| 12 | u16 | flags: 1 closed verified, 2 narrowed, 4 transition volume, 8 OPEN (exactly one of 1 / 8) |
| 14 | u16[2] | first twin, twin count |
| 18 | u16[2] | first sector-pool entry, entry count |
| 22 | u16 | OPEN draw count: the leading sector-pool entries drawn across layers (0 unless OPEN) |
| 24 | f32[3] | delta = landmark B minus landmark A |
| 36 | f32[6] | switch eye mins, maxs, A-local |
| 60 | f32[6] | carry mins, maxs, A-local |
| 84 | f32[6] | approach player-origin mins, maxs, A-local |
| 108 | u32[3] | need_render, need_texture (the crossing window), need_coll, bytes (Proof and residency policy) |
| 120 | u32 | B package directory CRC (`.w64` header offset 24) |
| 124 | f32[3] | portal translation `T = g_B + delta - g_A` |

Twins are u16 A lump, B lump. Lumps are original BSP entity indices, with
worldspawn 0, exactly HLE's `lump`/`lump2rec`, not compact HLE record indices.
Each entity occurs at most once in a seam's twin slice. HLE's former
`twin` field is now `trank` (trigger touch rank, hle-format-v1.md); twins
exist only here. Sector-pool entries below `0x8000` name a B sector;
`0x8000 | n` names B BSP model `n`, which must have a MOVR chunk.
Sector/model indices above `0x7FFF` cannot be encoded and are rejected.
The runtime rejects a seam if B's directory CRC differs: sector indices
are specific to the cooked package. The runtime clamps the eye into the
switch box and resolves its B visleaf through B's own pinned hull 0.
Visleaf indices in the report are 1-based; no leafpool is stored.

The runtime validates `T` against the stored map placements and delta,
rounded once to binary32. Crossing adds `T` to the global player/entity
position: `g_A + p → g_B + p + delta`. Velocity, angles and inventory are
unchanged; one world spans the open maps without a world rebuild.
Cooked switch, carry and approach boxes remain map-local; the controller
converts global positions into the source map's local frame when testing
them. The switch box / arming rules below are unchanged.

## Proof and residency policy

Touch eye bounds are the changelevel brush box plus entity origin, expanded
by `(16,16,36)` on each side, then shifted by view offset `(0,0,28)`.
USEONLY bounds are the named transition brush volume or a 256-unit cube
around the landmark. Non-solid eyes are sampled on a <=16-unit grid with
box endpoints included. Each emits >=400 Fibonacci directions; first sky
or solid hits are sampled every 6 units through 4096. BSP interval traversal
skips empty intervals without changing that lattice. Two skies agree;
solids agree only within 8 units. Unbounded rays and sky/solid mismatches fail.
Only USEONLY proofs include opaque spawn-position brush occluders: closed
doors, walls, breakables, buttons and momentary doors. Touch proofs never
assume a mover is still closed. Narrowing starts from the good-eye bounding
box. If it encloses a failing sample, cuts retain a rectangular agreeing
subset (largest surviving sample count, then volume) until no failing eye
is enclosed. Thin X/Y boxes are allowed (swept eye/box crossing), but if Z
is less than 8 units thick the original region is resampled at 4-unit Z
spacing. The result must contain an at-least-8-unit slab including the
standing eye height over the floor there (floor + 64). The stored box is
ray-tested again, including its endpoints. Zero good eyes is an error.

If the touch region cannot provide that proof, an armed seam may switch
later: touch arms the changelevel; a subsequent swept eye segment entering
the switch box switches home; leaving the approach box disarms it. The cook
searches within 256 units of the trigger eye region for standing eyes over
world floors with the standing BSP hull clear. A 16-unit XY floor grid and
18-unit step adjacency identify closed components. Nearest candidate X/Y
cross-sections are fully ray-proved with a 4-unit Z grid and an 8-unit
standing-eye slab. Walkable floor samples under the footprint expanded by
32 units must all fit the section for that connected corridor: a player
walking onward cannot skirt the stored switch box. The report preserves
both the original trigger region and the chosen box, floor samples and rays.
No qualifying section is a cook error, not permission to relax visibility.

Runtime requirement (GoldSrc changes level on any touch): an armed seam
whose eye never reaches the switch box (a narrowed box, the player crossing
a trigger slab along its unproven edge) must still switch once the player
stops touching the trigger, or after 1 s inside it. Such a switch is logged
as forced (`forced: left the trigger outside the switch box` / `in the
trigger outside the switch box`) and counted in the stats record's
`forced=` field; the view may differ there by the rays the proof rejected.

Carry bounds are the named transition volume or switch bounds minus the
view offset; OPEN seams clip their cross-layer draw to them. Multiple transition volumes with the same name are rejected:
a single CWG carry box cannot faithfully encode their union.

Twin candidates are within 512 units of the aligned landmark; classnames
must agree. A shared nonempty globalname wins, otherwise targetnames,
centres (8-unit distance) and brush sizes (8-unit per-axis tolerance) agree.
Point entities compare origins. Edict-less HLE class flags and the explicit
info/env/light/transition/changelevel/ambient exclusions prevent logic twins.

Budgets use the staged packages (CRC-checked with `w64check.W64`), not
estimated BSP sizes. Render and texture bytes are summed per distinct
chunk, each rounded up to 16 bytes as `engine/world/world.c` allocates it.

B's stored seam set uses actual visibility from every final sampled eye.
Every sector whose world bbox (expanded by 1 unit) contains a B ray's
first-hit surface point is included, together with its TEXP pages.
Every ray-hit MOVR is included with its pages. For touch seams, movers are
not ray occluders, but every MOVR placement intersecting the switch eye box
expanded by 128 units is also included (doors may already be open).
Surface points use exact BSP entry distances; the closed-check comparison
still uses the 6-unit point-sampling lattice.

The render / texture proof is the **crossing window**: what
`engine/world/stream.c` names at its open and pin classes (prefetch
excluded) while the seam is approached and after it switches, both layers
at once.

- A leaf's view set (stream.c `name_leaf`): the sectors of its vis list
  (REND + TEXP pages) and the MOVR, with every page of its textures'
  animation / alternate chains, of each brush entity whose reach meets one
  of those sectors' boxes (`each_mover`). The reach is the entity's ENTS
  placement (sub bbox + origin key) and, live, anywhere on its travel:
  sliding doors / buttons movedir × (size − lip), plats their height down,
  trains / gun targets / track trains every path point of their chain,
  rotating classes the sphere about their origin; grown by speed × 0.5 s
  (`MOVER_LEAD_S`) and 128 (`MOVER_MARGIN`). A pushable keeps its placement.
- A home eye's open set: the view sets of the eye leaf and of the drawn
  eye's leaves (the eye ± 24 per axis, `VIEW_SLACK`, and ± 32, the bound
  of the last frame's bob / punch offset, `TELE_SLACK`), plus the point
  pins of the trigger_teleports the eye leaf sees (destination origin
  + 37, eye + 28: `name_point`). Eye leaf 0 names nothing.
- B's seam pin (`name_seam`): the stored sector-pool slice plus the view
  sets of the arrival leaves (`arrival_leaves`: the switch box's centre
  and X/Y corners at its mid height, mapped by delta).
- Approach window: A's open set at every approach eye plus B's seam pin.
  Approach eyes are a 16-unit grid (endpoints included) of every eye a
  player whose origin is in the approach box can have: standing (eye − 28,
  hull 1 clear) or ducked (eye − 12, hull 3 clear), plus the final eyes.
- Arrival window: after the switch B is home, its open set at each final
  eye mapped by delta, plus the largest seam pin of a seam from B whose
  approach box holds that arrival origin (standing or ducked): seam.c
  approaches it at once.
- No seam pinned (`map_open` in the report): every map's open set at every
  player eye of a 16-unit grid over its world bounds.

`need_render` / `need_texture` are the larger of the approach and arrival
windows. Collision uses each map's pinned hull 0 plus grid lists
overlapping the player box (origin = eye minus 28; hull ±16 XY, ±36 Z),
expanded by 128. Traced submodel placements touching that window plus
w64c's 64-unit margin are included. `need_coll` is the maximum paired A/B
sum over the final eyes; both pinned sets are always included. The report
records the worst eye, leaves and bytes per pool (`worst_eyes`,
`arrival_eyes`), the seam pin, the arrival leaves, the reverse seams whose
pins the arrival window takes, and `window_worst` (the worst seam or map
per pool, also with every approach at its widest).

Approach bounds start at switch-minus-view expanded by 512, then try
384/256/128/0 (`approach_windows` records each tried). The first whose
approach window × 1.25 fits the render and texture carves and whose
collision (every A leaf there, windows bounded per grid cell clipped to
the approach/leaf overlap, plus B's worst final-eye window) fits the
collision carve is kept. For a searched armed switch, the approach must
also contain the trigger's firing eye region minus the view offset;
otherwise it could disarm before the player reaches the chosen
cross-section. The cook fails when a seam's render or texture need, or a
map's open set, × 1.25 exceeds its carve (`HL64_POOL_RENDER_KB`,
`HL64_POOL_TEXTURE_KB`; the carves are set from `window_worst`), or a
seam's collision need exceeds `HL64_POOL_COLLISION_KB`.
`--no-budget` records overages without rejecting; staging never uses it.

Self links and USEONLY changelevels without a targetname cannot switch and
retain seam `0xFFFF` with a report note. Other missing landmarks or pool
overages fail the cook and preserve its input graph.
`<staging>/seams.json` includes proofs, twins, budgets, narrowing statistics,
approach expansion, excluded links and explicit errors. Staging checks
that every executable non-self graph link references a seam.

## OPEN seams

A touch or USEONLY link whose closed check (and armed-switch search) fails
is cooked OPEN (flag 8, `open_reason` in the report) instead of failing.
The switch box stays the trigger eye region; the runtime switches at the
changelevel itself. Eyes are the region's samples empty in both maps. The
draw set is every B sector whose bbox (expanded by 1 unit) contains the B
first-hit point of a disagreeing ray from those eyes, plus every ray-hit B
MOVR; it leads the seam's sector-pool slice (offset 22 counts it), followed
by the normal visible seam set. While the player is in the approach box the
cart draws the draw set from B's layer translated into A's frame and scaled
about the eye by 1 + 1/256 (a render debug setting disables this cross-layer
draw or logs the meshlets drawn), so where both maps model the
same surface A's copy wins the depth test. Only meshlets touching the carry
box draw: beyond the shared transition volume B models its own space (at
t0a0d→t0a0c, t0a0c's room west of the elevator where t0a0d has another),
and drawing it would overlay two rooms. After any switch, changelevel
touches are ignored until a frame without one: a landmark arrival may
stand in the trigger back (t0a0c↔t0a0d).
