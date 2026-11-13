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
  origin := end; link with touch; impact touch if something was hit.
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
2. On ground (FL_ONGROUND) with no velocity: nothing more. A resting toss
   entity ignores its base velocity: an item resting on an FL_CONVEYOR
   pusher (speed 100) never moves, although its basevelocity is set to 100
   **[probe:measured P-toss]** (9/9). It leaves the active set until
   something moves it or removes its ground.
3. Check velocity; add gravity unless FLY, FLYMISSILE, BOUNCEMISSILE.
4. `angles += avelocity·dt`; push(e, (velocity + basevelocity)·dt); stop if
   no impact or e freed.
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
   it **[probe:measured P-bounce]**. The vertical stop threshold is
   calibrated from the same capture.
7. Water level (5).

### STEP (monsters)
1. Not on ground and not FL_FLY/FL_SWIM: add gravity, check velocity,
   slide(e, dt), link with touch. No landing sound
   **[probe:measured P-step]**.
2. On ground with horizontal velocity (pushed by an explosion or a door):
   ground friction as for PUSHSTEP below, then slide
   **[probe:measured P-step]**.
3. Think. 4. Water level (5).
Ordinary monster walking is done by the game through walk moves (6).

### PUSHSTEP (func_pushable)
1. On ground: horizontal friction — s = |velocity.xy|; if s > 0:
   control = max(s, stopspeed), s' = max(0, s − dt·control·friction),
   velocity.xy ×= s'/s (200 → 192 → 184.32 … at dt 0.01). Then a 1 u
   downward probe with its own box; no hit clears FL_ONGROUND.
2. Not on ground: add gravity (no friction in the air: a pushable shoved
   off a ledge keeps 150 u/s until it lands).
3. Velocity non-zero: check velocity, slide, link with touch.
4. Think. 5. Water level.
**[probe:measured P-pushable]**

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
   FOLLOW: riders (FL_ONGROUND on this pusher), and entities whose box
   intersects the pusher's own hull **at its new position** (a test against
   the pusher alone). An entity in the pusher's path is therefore pushed
   only from the frame the pusher has moved into it, by that frame's whole
   move, so it keeps the overlap of that first frame: a door with a 4 u gap
   to a crate and 100 u of travel moves the crate 95 u, and the crate ends
   overlapping the door by 1 u **[probe:measured P-push]**.
5. Non-player candidates lose FL_ONGROUND; each is pushed by `move` with the
   pusher temporarily non-solid (its own traces may stop it short; impact
   touches run).
6. Blocked: when a pushed candidate could not follow the move (its traces
   stopped it short) and is left inside the pusher — zero-width candidates
   are ignored; SOLID_NOT / SOLID_TRIGGER candidates (corpses, gibs)
   collapse to zero size and are ignored **[probe:measured P-push]** —
   the candidate returns (link with touch), the pusher returns (ltime −=
   movetime), the pusher's `blocked` runs once per blocked frame with other
   = candidate, and every candidate already moved returns
   **[probe:measured P-push]**. The residual first-frame overlap of step 4
   is not a block (the crate case); the exact block criterion is taken from
   the P-push records.

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

After toss-family, STEP and PUSHSTEP steps, a non-player entity gets a
water level 0–3 like the player's (feet, waist, eyes): toss, bounce and
step entities fully under water reach **waterlevel 3**, not 1
**[probe:measured W-water]** (the sample points are taken from the capture;
expected: bottom + 1, box centre, origin + view_ofs). watertype = the
contents at the lowest submerged point. Liquid = WATER, SLIME, LAVA, and
also SKY (−6) **[probe:measured W-water]**. **Entering liquid halves the
vertical velocity** (37/37 entries) **[probe:measured W-water]**. No splash
sound from the engine. Contents brushes (`func_water`, SOLID_NOT with a
contents skin) count as liquid (trace-and-hulls.md 1).

## 6. Monster step moves (engine services)

- **check_bottom(e)**: true when the four bottom corners at mins.z − 1 are
  SOLID world contents; else a line from the bottom centre down 36 u must
  hit, and each corner's line to the same depth must hit no more than 18 u
  below the centre hit.
- **walk_move(e, yaw, dist, mode)** (`pfnWalkMove`; modes NORMAL,
  WORLDONLY, CHECKONLY): displacement (cos, sin, 0)·dist.
  FL_FLY/FL_SWIM: try with vertical adjustment toward the enemy (−8 when
  more than 40 u above it, +8 when less than 30 u above), then without;
  a swimmer may not leave liquid. Walkers: box trace from new position +18
  down to −18; all-solid fails; start-solid retries from the unraised
  position; no ground hit fails unless FL_PARTIALGROUND (then fall); after a
  hit, check_bottom must pass unless FL_PARTIALGROUND; success clears
  FL_PARTIALGROUND, sets groundentity, links with touch (CHECKONLY never
  moves). Measured: world stairs (8 u risers) are climbed; an 18 u step
  made of a **brush entity** (SOLID_BSP) is never stepped onto — the move
  fails at contact **[probe:measured P-walkmove]** (7 maps). 18 u world
  steps are expected to be climbed **[probe]**.
- **move_to_origin(e, goal, dist, mode)** (`pfnMoveToOrigin`): turn toward
  the goal and walk_move along it; if blocked, try compass directions
  (45° steps) reducing the larger axis gap first, random order on ties
  (engine random stream), not the direct reverse until all else failed.
  **[probe P-walkmove: success/failure pattern, not the random choice]**
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
| P8 | corpses collapse instead of blocking | **[probe:measured P-push]** |
| P9 | pushable friction (200 → 192 → 184.32 at dt 0.01, 120-start map pushables likewise) and falling off a ledge with no air friction | **[probe:measured P-pushable]** |
| P10 | walk_move: world 8 u stairs climbed; an 18 u SOLID_BSP brush-entity step fails at contact; overhang > 18 u fails check_bottom; partial ground correction | **[probe:measured P-walkmove]** |
| P11 | drop_to_floor codes and origins | probe P-walkmove |
| P12 | conveyor: an item resting on an FL_CONVEYOR pusher (speed 100) never moves while its basevelocity is 100; on leaving a trigger_push, velocity += basevelocity·(1 + dt/2) | **[probe:measured P-toss]** |
| P13 | active set: a resting crate, a resting gib and an idle func_wall are not stepped (zone counters 0) yet react on the frame something moves them | host counters |
| P14 | water: toss/bounce/step entities reach waterlevel 3; entering liquid halves vz; SKY (−6) counts as liquid | **[probe:measured W-water]** |
| P15 | SOLID_BSP with a movetype other than PUSH is a fatal error naming the class | **[probe:measured]** (Oracle2 run logs) + host spawn-all test |
