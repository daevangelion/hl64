# Physics: movetypes, pushers, monster steps

Behaviour: per-frame motion of every entity except the player's own
movement (pmove-host.md), as GoldSrc moves them. Mechanism: only entities
in the active set are stepped (think-and-time.md 3). Traces and touches:
trace-and-hulls.md, area-links-and-touch.md. Conventions and oracles:
[README.md](README.md). GoldSrc probe captures: `$HL64_FIXTURES/goldsrc/
physics-movetypes/P-*`, `think-and-time/K-pusher`,
`trace-and-hulls/W-water` (Oracle2, hl64 `d2bf3d8`, 10 maps).

## 1. Parameters

Settings (HL defaults; `settings.h`, typed): gravity 800, maxvelocity
2000, friction 4, stopspeed 100, stepsize 18, edgefriction 2. Per entity:
`gravity` (scale, 0 means 1), `friction`.

Constants: floor = plane normal.z > 0.7; wall = normal.z = 0; clip stop
epsilon 0.1; slide iterations 4; clip planes 5; step height 18.
Calibrated on the probe captures (hl64 `test/m3/phys.py`, 10 maps): BOUNCE
rest speed 30 u/s (a vertical bounce at 16 u/s rests, 80 u/s flies on;
any value in (16, 60] fits), BOUNCE vertical stop below gravity·dt (8 u/s
at dt 0.01: 2.24 / 5.22 stop, 8.88 / 12.4 / 12.8 bounce on; the entity
gravity scale is not separated by the capture, hl64 uses sv_gravity),
BOUNCE creep 0.9, STEP water drag 0.8 per water level per second
**[probe:measured P-bounce, P-step]**.

The entity `friction` is literal (HLSDK entvars start at 0): BOUNCE
overbounce is `2 − friction`, and the STEP / PUSHSTEP ground friction
scales by it once and then sets it to 1 — a pushable or monster with
friction 0 keeps its speed on the first frame it moves, then slows by
`sv_friction` **[probe:measured P-pushable pushstep_f0 / _f1, every map
`func_pushable`, P-step pushed]**.

## 2. Dispatch and common operations

Entities in the active set are stepped once per frame in ascending index
(think-and-time.md 3); entities created during the pass at a higher index
are stepped this frame. Movetypes (HLSDK values): NONE 0, WALK 3 (player
only; others act as NONE), STEP 4, FLY 5, TOSS 6, PUSH 7, NOCLIP 8,
FLYMISSILE 9, BOUNCE 10, BOUNCEMISSILE 11, FOLLOW 12, PUSHSTEP 13; any
other value is a fatal error naming the entity.

**SOLID_BSP requires MOVETYPE_PUSH**: GoldSrc stops the server with a fatal
error when a SOLID_BSP entity has any other movetype **[probe:measured]**
(Oracle2 run logs). hl64: the engine checks the pair whenever solid or movetype is
set and calls `sys_error` naming the class (host and cart alike); the host
test suite spawns every game class so no such pair can ship, and the cook
rejects map records whose cooked keys would produce it.

- **check velocity**: NaN components → 0 (logged once per entity); each
  component clamped to ±maxvelocity.
- **add gravity**: `velocity.z −= scale · gravity · dt`.
- **base velocity**: an entity given `basevelocity` (a trigger_push touch
  sets FL_BASEVELOCITY with it; standing on an FL_CONVEYOR pusher sets the
  conveyor's speed) moves by velocity + basevelocity while the flag is set.
  On the frame the flag clears, the base velocity is folded into the
  velocity as `velocity += basevelocity · (1 + dt/2)` (100 u/s becomes
  100.5 at dt 0.01) **[probe:measured P-toss]** (9 of 10 maps).
- **push(e, d)**: one move query from origin to origin + d with e's box,
  pass e, type `missile` for FLYMISSILE and BOUNCEMISSILE,
  `ignore_monsters` for SOLID_TRIGGER/SOLID_NOT movers, else normal;
  origin := end; link with touch; impact touch if something was hit (the
  world included: A.touch(world)). An all-solid push still moves to the
  end (trace-and-hulls.md 3).
- **conveyor and base velocity**, before the step of the toss family,
  STEP and PUSHSTEP: an entity on the ground of an FL_CONVEYOR pusher gets
  basevelocity = the conveyor's speed·movedir (added when FL_BASEVELOCITY
  is already set) and the flag; without the flag a non-zero base velocity
  is folded in; the flag is then cleared. hl64: the game registers the
  conveyor velocity with `phy_set_conveyor` (no speed / movedir in the
  engine). A freed ground clears FL_ONGROUND and groundentity.
- **clip(v, n, k)**: `v − n·(v·n)·k`, components with |c| < 0.1 → 0.
- **slide(e, t)** (collide and slide): up to 4 iterations, each a move query
  over the remaining time; all-solid → velocity 0, stop; progress → move
  there and reset the clip-plane list to the new velocity; a floor hit sets
  FL_ONGROUND and groundentity when the hit entity is SOLID_BSP (world
  included) — for STEP slides onto a box entity this is expected not to
  set FL_ONGROUND **[probe]**; impact touch (stop if e was freed);
  remaining time shrinks by the fraction; more than 5 planes → stop; the
  velocity becomes the reference velocity clipped against the first plane
  whose result does not point into another plane, else (exactly 2 planes)
  projected on their crease, else 0; a result opposing the velocity at the
  start of the slide stops it.

## 3. Movetypes

### NONE
Think only.

### NOCLIP
Think; `angles += avelocity·dt`; `origin += velocity·dt`; link without
touch.

### Toss family: TOSS, BOUNCE, FLY, FLYMISSILE, BOUNCEMISSILE
1. Think; stop if freed.
2. velocity.z > 0 clears FL_ONGROUND. A ground entity with FL_MONSTER or
   FL_CLIENT does not hold it: FL_ONGROUND is cleared and it lands on the
   monster again every frame (impact touches each frame; 106 per
   capture, 10/10) **[probe:measured P-toss]**; on the world or a
   SOLID_BSP entity it rests (one touch). On ground with no velocity and
   no base velocity: avelocity 0, water level, nothing more. With a base
   velocity it moves as below: an item resting on an FL_CONVEYOR pusher
   (speed 100) never moves, although its basevelocity is 100, because its
   push (v + bv)·dt starts on the floor's stand-off and stops at fraction
   0 **[probe:measured P-toss]** (9/9, 274 touches). It leaves the active
   set until something moves it or removes its ground.
3. Check velocity; add gravity unless FLY, FLYMISSILE, BOUNCEMISSILE.
4. `angles += avelocity·dt`; push(e, (velocity + basevelocity)·dt); stop if
   no impact or e freed. An all-solid push (trapped in solid): velocity
   and avelocity 0, no FL_ONGROUND; it sinks by each frame's gravity move
   **[probe:measured P-toss t0a0b1, P-step hldemo3 water_toss]**.
5. Clip velocity on the hit plane with overbounce: BOUNCE `2 − friction`,
   BOUNCEMISSILE 2, others 1. Measured rebound ratios (vertical speed after
   / before a floor bounce): friction 0.8 → 0.205, 0.55 → ≈ 0.47, 0 →
   ≈ 1.03, BOUNCEMISSILE 1.0 **[probe:measured P-bounce]** (the excess over
   `1 − friction` is the frame's gravity).
6. Floor hit: TOSS / FLY / FLYMISSILE stop: FL_ONGROUND, groundentity =
   the hit entity — **a box entity too**: a TOSS box landing on a monster
   rests on it with groundentity = the monster **[probe:measured P-toss]**
   (10/10) — velocity and avelocity 0. BOUNCE stops **vertically only**
   once a floor bounce no longer lifts it (FL_ONGROUND, velocity.z 0) and
   keeps its horizontal velocity: there is no engine ground friction and no
   rest speed — a grenade sliding at 60 u/s keeps 60 u/s until a wall stops
   it **[probe:measured P-bounce]**. BOUNCE / BOUNCEMISSILE on a floor:
   velocity.z after the clip below gravity·dt → FL_ONGROUND, groundentity,
   velocity.z 0; then speed (velocity + basevelocity) below 30 u/s → full
   stop as TOSS; else the entity moves on for the rest of the frame,
   slowed: push(e, velocity·(1 − fraction)·dt·0.9). Sliding on the floor
   it therefore advances 0.9·v·dt per frame (0.54 u at 60 u/s, dt 0.01)
   **[probe:measured P-bounce]** (8 scenarios × 10 maps).
7. Water level (5).

### STEP (monsters)
1. In liquid (water level of the last frame > 0): FL_INWATER, velocity ×
   (1 − 0.8·waterlevel·dt) (a sinking monster: v' = 0.992 v − 8 at level
   1, v' = 0.976 v at level 3, dt 0.01); else FL_INWATER cleared
   **[probe:measured P-step]** (5 maps).
2. Not on ground at the frame start, not FL_FLY, not FL_SWIM in liquid,
   water level < 2: add gravity (no gravity at level 3; level 2 is not
   captured).
3. Velocity non-zero: the ground move of PUSHSTEP below. No landing sound
   **[probe:measured P-step]**.
4. Think. 5. Water level (5).
Ordinary monster walking is done by the game through walk moves (6).

### PUSHSTEP (func_pushable)
1. Not on ground at the frame start: add gravity (no friction in the air:
   a pushable shoved off a ledge keeps 150 u/s until it lands).
2. Velocity non-zero, the **ground move** (STEP too):
   - horizontal speed s > 0 consumes the entity friction scale (then 1);
     **ground friction** when the entity was on the ground at the frame
     start (FL_ONGROUND) and stands on the world there (a bottom corner,
     1 u under the box, in world SOLID, as below) or check_bottom (6)
     holds there: control = max(s, stopspeed), s' = max(0, s −
     dt·control·sv_friction·scale), velocity.xy ×= s'/s (200 → 192 →
     184.32 … at dt 0.01). A box sent over a **world** ledge brakes until
     its last corner leaves it, whatever its groundentity (t0a0a's crate
     at 400 u/s: 4 % a frame on, 162.94 u/s at frame 33 with 22 u of its
     66 u box over the pit, where check_bottom fails)
     **[probe:measured P-botpush slide_0 slidebsp_0]**;
   - FL_ONGROUND cleared; check velocity; slide; a slide onto a floor of
     the world or a SOLID_BSP entity sets FL_ONGROUND and groundentity;
     else FL_ONGROUND is set (groundentity unchanged) when any of the four
     bottom corners, 1 u under the box, is world SOLID (point contents:
     brush entities are not seen); link with touch.
   On a brush-entity platform FL_ONGROUND therefore alternates frame by
   frame, and the frames that start on the ground brake only where
   check_bottom holds: a box whose front corners are already past the
   platform's edge slides on unbraked (pushstep_ledge, ledge48); the
   hldemo3 `func_pushable` standing on the edges of two `momentary_door`s
   brakes every other frame (120 → 115.2 → 110.59 …); landing on the
   world brakes on the landing frame (FL_ONGROUND from the corner test
   the frame before); a monster sinking past a 45° slope gets FL_ONGROUND
   once a corner is 1 u into it, 17 u above the slope under its centre
   **[probe:measured P-step ledge48 water_step, P-pushable pushstep_ledge
   map_pushable_0, P-botpush]**. Pushed by the player (CPushable::Touch
   with the impact velocity, area-links-and-touch.md 4), t0a0a's crate
   leaves its ledge on frame 78 and rests on the pit floor against the
   west wall from frame 124 **[probe:measured P-botpush walk_0]**. hl64
   matches that (leaves on frame 78–79, rests from 124 at the same place,
   x within 0.5 u while pushed); GoldSrc's crate box is the model file's
   bounds 1 u wider on every side (66 u, hl64 keeps 64 u), so hl64's crate
   top rests 2 u lower: 19.97 u under the floor instead of 17.97.
3. Think. 4. Water level.
**[probe:measured P-pushable, P-step]**

### PUSH
Section 4.

### FOLLOW
After its think: origin := aiment's origin and angles := aiment's angles;
link without touch. The renderer draws a FOLLOW studio entity with the
aiment's skeleton (HLSDK uses it for attached models) **[probe]**.

### WALK
The player only.

## 4. Pushers

### 4.1 Local time and think
A pusher has its own clock `ltime` (f32 seconds, as HLSDK's
`pev->ltime`); its nextthink is in ltime units. movetime = nextthink −
ltime (≥ 0) if nextthink < ltime + dt, else dt. With movetime > 0: rotate
if avelocity ≠ 0, then (if the rotation was not blocked and velocity ≠ 0)
translate over the same movetime from the same start ltime; otherwise
translate. A blocked push does not advance ltime and runs no think. Then if
the original nextthink lies in (old ltime, new ltime]: nextthink := 0 and
the think runs (game time = the frame time). Measured: a door's ltime stops
exactly at its nextthink (0.78125) on arrival; a blocked door's ltime
stalls and its think does not run **[probe:measured K-pusher]**.

### 4.2 Translation
1. Zero velocity: ltime += movetime.
2. move = velocity·movetime; the pusher moves, ltime += movetime, link
   without touch.
3. SOLID_NOT pushers stop here (passable doors and toggled walls push
   nothing).
4. Candidates, ascending index, excluding movetypes PUSH, NONE, NOCLIP,
   FOLLOW: riders (FL_ONGROUND on this pusher), and entities whose linked
   box **strictly** overlaps the pusher's new linked box (faces in contact
   do not count: a pusher coming down on a crate starts pushing when its
   linked box overlaps the crate's by 1 u, 1 u short of the real boxes
   **[probe:measured P-push down_onto_crate]**) and whose box intersects
   the pusher's own hull **at its new position** (a test against the
   pusher alone). An entity in the pusher's path is therefore pushed
   only from the frame the pusher has moved into it, by that frame's whole
   move: a door whose padded bounds leave a 4 u gap to a crate (5 u
   between the real faces) and 100 u of travel moves the crate 95 u, faces
   in contact at the end **[probe:measured P-push]**.
5. Non-player candidates lose FL_ONGROUND; each is pushed by `move` with the
   pusher temporarily non-solid (its own traces may stop it short; impact
   touches run).
6. Blocked: after its push the candidate is **stuck** — the position test
   (trace-and-hulls.md 4.6) against everything with its own move type is
   start-solid. The move query only clips entities whose linked boxes its
   ±1 u box meets, so a crate whose hull (24 u box → hull 3, +8 u on one
   side) already reaches into a blocker is not stuck until its linked box
   meets the blocker's: a door pushing a 24 u crate toward a brush 8 u
   away moves it 4 u and then blocks **[probe:measured P-push
   door_block_rider, K-pusher blocked]** (10 maps). Zero-width candidates
   are ignored; SOLID_NOT / SOLID_TRIGGER candidates (corpses, gibs)
   collapse to zero size where they are and are ignored
   **[probe:measured P-push corpse]**; otherwise the candidate returns
   (link with touch), the pusher returns (ltime unchanged), the pusher's
   `blocked` runs once per blocked frame with other = candidate, and every
   candidate already moved returns; FL_ONGROUND they lost is not restored
   **[probe:measured P-push]**.
7. hl64 finds candidates in the spatial grid (solids and triggers over the
   old ∪ new linked boxes): SOLID_NOT entities are not in the grid, so a
   SOLID_NOT entity resting on a pusher is not carried (GoldSrc carries
   it) — a known deviation, no capture.
8. hl64 records (architecture.md 6.4): GoldSrc's pusher meets every map
   entity where it came to rest after map start; an hl64 pending record
   still holds the mapper's place (hldemo3's dish func_pushable, 16 u above
   the dish_control momentary_doors that carry it to the `on` trigger, 1360
   u from the control room). So before the move (before step 2) the
   pending and dry records whose box meets the old ∪ new linked boxes, or
   the 256 u above them, hydrate; a woken STEP / PUSHSTEP / TOSS / BOUNCE
   entity not on the ground, not FL_FLY / FL_SWIM, settles by
   DROP_TO_FLOOR's trace onto the pusher at its old place (or whatever is
   beneath) and is a rider of this move; one that finds no floor falls on
   and release leaves it 1 s (a 256 u fall). Rotation (4.3) wakes over the
   old box ∪ the rotated-brush cube.

### 4.3 Rotation
As 4.2 with: amove = avelocity·movetime added to angles; swept box = the
rotated-brush cube at the new angles; each candidate's displacement is its
position relative to the pusher origin rotated by amove; non-client
candidates turn with the pusher (`yaw += amove.yaw`), clients keep their
view; a blocked rotation undoes those yaws **[probe:measured P-rotate]**.

### 4.4 Riders
Entities standing on a pusher move with it even outside the swept volume
(a crate on a lift rises with it **[probe:measured P-push]**); the player is
moved by the push, not by pmove.

## 5. Water level (non-player)

After toss-family, STEP and PUSHSTEP steps (resting toss entities too), a
non-player entity gets a water level 0–3 like the player's (feet, waist,
eyes): toss, bounce and step entities fully under water reach
**waterlevel 3**, not 1 **[probe:measured W-water]**. Sample points
**[probe:measured P-step]**: feet = (linked box centre x, y; absmin.z + 1,
the bottom of the real box: a monster whose feet are 0.68 u under the
surface is at level 1), waist = linked box centre, eyes = origin +
view_ofs (`ent_phys.view_ofs_z`; level 3 only once the waist is in: a
probe monster with view_ofs 0 goes 1 → 3). watertype = the feet contents
when liquid, else EMPTY (−1, also when the feet are in solid)
**[probe:measured P-bounce bounce_box]**. GoldSrc gives every entity it
steps watertype EMPTY on its first frame; hl64 sets it when a motion
movetype is set (slice Ent). Liquid = WATER, SLIME, LAVA, the currents,
and also SKY (−6) **[probe:measured W-water]**. **Entering liquid
(level 0 → > 0) halves the vertical velocity** (37/37 entries)
**[probe:measured W-water]**; the move of that frame used the full
velocity. No splash sound from the engine. Contents brushes
(`func_water`, SOLID_NOT with a contents skin) count as liquid
(trace-and-hulls.md 1).

## 6. Monster step moves (engine services)

- **check_bottom(e)**: true when the four bottom corners at mins.z − 1 are
  SOLID world contents; else a line (move query, `ignore_monsters`: brush
  entities count) at the bottom centre from **a step above the bottom to
  a step below it** (mins.z + 18 → mins.z − 18) must hit, and each
  corner's line over the same span must hit no more than 18 u below the
  centre hit **[probe:measured P-walkmove world_stairs]** (the lift: a
  corner line that starts at mins.z is all-solid where a corner pokes
  into a ramp the hull lets the box rest against — t0a0d climbs it; a
  corner whose ground is more than 18 u under the box fails the step
  even when the centre's ground is within 18 u of it — t0a0b, t0a0b1,
  t0a0d stop at the stair top; the 2006 walk results of 10 maps).
- **walk_move(e, yaw, dist, mode)** (`pfnWalkMove`; modes NORMAL,
  WORLDONLY, CHECKONLY): displacement (cos, sin, 0)·dist.
  FL_FLY/FL_SWIM: try with vertical adjustment toward the enemy (−8 when
  more than 40 u above it, +8 when less than 30 u above), then without;
  a swimmer may not leave liquid. Walkers: box trace from new position +18
  down to −18; all-solid fails; start-solid retries from the unraised
  position; no ground hit fails unless FL_PARTIALGROUND (then fall); after a
  hit, check_bottom must pass unless FL_PARTIALGROUND; success clears
  FL_PARTIALGROUND, sets groundentity, links with touch. Not on ground,
  FL_FLY or FL_SWIM: fails at once (a FL_PARTIALGROUND walker that walked
  off fails its next move) **[probe:measured P-walkmove partial48]**.
  CHECKONLY moves the origin like NORMAL but does not link, touch or
  change flags / ground (HLSDK CheckLocalMove restores the origin)
  **[probe:measured P-walkmove flat]** (mode 2 results equal mode 0).
  Measured: world stairs (8 u risers) are climbed; an 18 u step made of a
  **brush entity** (SOLID_BSP) is never stepped onto — the walker lands
  on its top, and the corners still over the floor are 18 1/32 u down,
  past the check_bottom span **[probe:measured P-walkmove step18]** (7
  maps).
- **move_to_origin(e, goal, dist, mode)** (`pfnMoveToOrigin`): walk_move
  straight at the goal; if blocked, the eight compass directions ordered
  by how much nearer each brings the goal (ties in a random order from
  the engine stream), the reverse of the straight direction last. Angles
  and ideal_yaw are not changed **[probe:measured P-walkmove]** (the
  straight moves exactly, the detours as compass steps of `dist`).
- **drop_to_floor(e)**: box trace down 256 u: all-solid → −1, no hit → 0,
  else move there, link without touch, FL_ONGROUND, groundentity → 1.
- **change_yaw(e)**: move yaw toward ideal_yaw by at most yaw_speed (the
  game scales by frame time), shortest way, normalised to [0, 360).

## 7. Render interpolation

STEP entities are drawn at origin and angles interpolated between their
previous and current step positions over the interval between those
steps (≤ 0.2 s; a jump > 64 u snaps), as GoldSrc's client smooths
monsters that think at 10 Hz. Other movetypes draw at their current
state. (Client-side: awaits the retail-client capture plan.)

## 8. Tests

| # | property | data |
|---|---|---|
| P1 | toss: drop from rest lands after √(2h/gravity) ± one frame, ends at rest on the world | **[probe:measured P-toss]** |
| P2 | bounce rebound ratios (friction 0.8 → 0.205, 0.55 → ≈ 0.47, 0 → ≈ 1.03; BOUNCEMISSILE 1.0); vertical stop on a floor with horizontal velocity kept (60 u/s until a wall); no ground friction, no rest speed | **[probe:measured P-bounce]** |
| P3 | slide: wall keeps the parallel component; 90° corner stops; crease follows the crease. A TOSS box landing on a monster gets FL_ONGROUND and groundentity = the monster | **[probe:measured P-toss]** (landing) + synthetic (slide) |
| P4 | STEP: falls off ledges, lands silently; a pushed monster slides to rest by the ground friction formula | **[probe:measured P-step]** |
| P5 | pusher timing: door ltime ends at nextthink (0.78125), think on the frame ltime reaches it; a blocked door's ltime stalls and its think does not run | **[probe:measured K-pusher]** |
| P6 | push: a crate on a lift rises; a door with a 4 u gap and 100 u travel moves a crate 95 u and leaves it overlapping by 1 u; a monster against a closing door blocks it, `blocked` once per blocked frame, earlier candidates return | **[probe:measured P-push]** |
| P7 | rotating door carries an entity round the hinge; non-client yaw follows, client yaw does not | **[probe:measured P-rotate]** |
| P8 | corpses collapse instead of blocking; a collapsed corpse the door has passed lands on the world every frame (its toss trace stops on the floor at fraction 0, where the move query clips no entities, trace-and-hulls.md 4.4) | **[probe:measured P-push corpse]** |
| P9 | pushable friction (200 → 192 → 184.32 at dt 0.01, 120-start map pushables likewise) and falling off a ledge with no air friction; friction only from a frame-start ground on the world (a bottom corner in world SOLID) or where check_bottom holds (hldemo3 pushable on two `momentary_door`s: every other frame); a map crate sent 400 u/s over its world ledge with groundentity the world or a brush entity (P-botpush slides); the player walking t0a0a's crate into its pit (P-botpush walk_0: test/m5/crate-pit.route, its end state) | **[probe:measured P-pushable, P-botpush]** |
| P10 | walk_move: world 8 u stairs climbed up to the step whose corners leave the check_bottom span; an 18 u SOLID_BSP brush-entity step fails; overhang > 18 u fails check_bottom; partial ground correction | **[probe:measured P-walkmove]** |
| P11 | drop_to_floor codes and origins | probe P-walkmove |
| P12 | conveyor: an item resting on an FL_CONVEYOR pusher (speed 100) never moves while its basevelocity is 100; on leaving a trigger_push, velocity += basevelocity·(1 + dt/2) | **[probe:measured P-toss]** |
| P13 | active set: a resting crate, a resting gib and an idle func_wall are not stepped (zone counters 0) yet react on the frame something moves them | host counters |
| P14 | water: toss/bounce/step entities reach waterlevel 3; entering liquid halves vz; SKY (−6) counts as liquid; a monster sinking past a slope gets FL_ONGROUND from the corner test | **[probe:measured W-water, P-step water_step]** |
| P15 | SOLID_BSP with a movetype other than PUSH is a fatal error naming the class | **[probe:measured]** (Oracle2 run logs) + host spawn-all test |

hl64 host results (`test/m3/phys.py`, every capture scenario rebuilt on
the cooked maps and replayed frame by frame; origin / velocity ±0.001,
flags, ground, water level and type, size, ltime / nextthink, thinks and
blocks exact; 56 070 frames, 10 maps): P1 50/50, P2 80/80, P3 20/20
(10 landings + 10 slide self-tests), P4 39/39, P5 = K4 52/52, P6 35/35,
P7 3/3 (the bot rides by the pmove host), P8 7/7, P9 39/39, P10
2006/2006 walk results, P11 181/181, P12 19/19, P13 10/10, P14 32/32,
P15 10/10; impact touches 31 568 (the probe logged 31 537; 32 485
before the fraction-0 rule of trace-and-hulls.md 4.4). The former
residuals were engine rules, measured on these captures: P8 the move
query's fraction-0 rule (the corpse, 1/32 u to 2 u inside the passing
door, lands on the floor each frame); P9 the friction gate (3 STEP /
PUSHSTEP); P10 the check_bottom lift (6; it also explains the brush
step, so hl64's former "a level destination meeting a SOLID_BSP entity
fails" rule is gone); P14 the corner ground test (3 PUSHSTEP).
Map data, not an engine bug (c3a2d cook of slice Items,
`/scratch/hl64/tmp/Items/c3a2d`): the three `weapon_tripmine`s at
(1528, −600, −248), (1528, −640, −248) and (1528, −716, −160) start with
their HLSDK single-player box (−16 −16 0)…(16 16 28) inside a world
shelf block (hull-3 move query from the origin all-solid, ent 0; the
same box dropped from 40 u above rests at −215.97 / −215.97 / −167.97),
so they take the trapped toss step 4 of 3: velocity 0, no FL_ONGROUND,
sinking by gravity·dt² per frame — 8 u/s at dt 0.01, 26.7 u/s at 1/30 s
(the reported −248 → −303 in about 2 s). GoldSrc does the same on the
same hull: P-toss t0a0b1 scenario 6, entity 85: −164 → −164.08 →
−164.16 …, velocity 0, flags 0. Decision (Main, via M3Lead2): keep the
HLSDK / GoldSrc behaviour; neither engine nor game compensates for
entities placed in solid.
