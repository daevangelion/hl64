# Physics: movetypes, pushers, monster steps

Behaviour: per-frame motion of every entity except the player's own
movement (pmove-host.md), as GoldSrc moves them. Mechanism: only entities
in the active set are stepped (think-and-time.md 3). Traces and touches:
trace-and-hulls.md, area-links-and-touch.md. Conventions and oracles:
[README.md](README.md).

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

- **check velocity**: NaN components → 0 (logged once per entity); each
  component clamped to ±maxvelocity.
- **add gravity**: `velocity.z −= scale · gravity · dt`.
- **base velocity**: entities standing on an FL_CONVEYOR entity, or given
  `basevelocity` by a trigger_push touch, move by velocity + basevelocity
  for the frame; basevelocity is not added into velocity **[probe
  P-toss]**.
- **push(e, d)**: one move query from origin to origin + d with e's box,
  pass e, type `missile` for FLYMISSILE and BOUNCEMISSILE,
  `ignore_monsters` for SOLID_TRIGGER/SOLID_NOT movers, else normal;
  origin := end; link with touch; impact touch if something was hit.
- **clip(v, n, k)**: `v − n·(v·n)·k`, components with |c| < 0.1 → 0.
- **slide(e, t)** (collide and slide): up to 4 iterations, each a move query
  over the remaining time; all-solid → velocity 0, stop; progress → move
  there and reset the clip-plane list to the new velocity; a floor hit sets
  FL_ONGROUND and groundentity when the hit entity is SOLID_BSP (world
  included) — landing on a box entity (a monster, a crate) does not; impact
  touch (stop if e was freed); remaining time shrinks by the fraction;
  more than 5 planes → stop; the velocity becomes the reference velocity
  clipped against the first plane whose result does not point into another
  plane, else (exactly 2 planes) projected on their crease, else 0; a
  result opposing the velocity at the start of the slide stops it.

## 3. Movetypes

### NONE
Think only.

### NOCLIP
Think; `angles += avelocity·dt`; `origin += velocity·dt`; link without
touch.

### Toss family: TOSS, BOUNCE, FLY, FLYMISSILE, BOUNCEMISSILE
1. Think; stop if freed.
2. On ground (FL_ONGROUND) with no velocity: nothing more (and the entity
   leaves the active set until something moves it or removes its ground).
3. Check velocity; add gravity unless FLY, FLYMISSILE, BOUNCEMISSILE.
4. `angles += avelocity·dt`; push(e, (velocity + basevelocity)·dt); stop if
   no impact or e freed.
5. Clip velocity on the hit plane with overbounce: BOUNCE `2 − friction`
   (HLSDK grenades set friction 0.8 → 1.2, gibs 0.55 → 1.45; friction 0 →
   2), BOUNCEMISSILE 2, others 1 **[probe P-bounce]**.
6. Floor hit: TOSS / FLY / FLYMISSILE stop (FL_ONGROUND, groundentity,
   velocity and avelocity 0). BOUNCE and BOUNCEMISSILE lose the vertical
   component when it is smaller than one frame of gravity and keep sliding
   along the floor for the rest of the frame at reduced speed; they come to
   rest (FL_ONGROUND, velocity 0) once their speed falls below ≈ 30 u/s
   **[probe P-bounce: rest speed and slide damping]**. (The old tree's
   Quake rule — rebound with 1.5 and stop below 60 u/s vertical — is not
   GoldSrc and is not kept.)
7. Water level (5).

### STEP (monsters)
1. Not on ground and not FL_FLY/FL_SWIM: add gravity, check velocity,
   slide(e, dt), link with touch. No landing sound.
2. On ground with horizontal velocity (pushed by an explosion or a door):
   ground friction as for PUSHSTEP below, then slide **[probe P-step]**.
3. Think. 4. Water level (5).
Ordinary monster walking is done by the game through walk moves (6).

### PUSHSTEP (func_pushable)
1. On ground: horizontal friction — s = |velocity.xy|; if s > 0:
   control = max(s, stopspeed), s' = max(0, s − dt·control·friction),
   velocity.xy ×= s'/s. Then a 1 u downward probe with its own box; no hit
   clears FL_ONGROUND.
2. Not on ground: add gravity.
3. Velocity non-zero: check velocity, slide, link with touch.
4. Think. 5. Water level.
**[probe P-pushable]**

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
A pusher has its own clock `ltime` (µs); its nextthink is in ltime units.
movetime = nextthink − ltime (≥ 0) if nextthink < ltime + dt, else dt. With
movetime > 0: rotate if avelocity ≠ 0, then (if the rotation was not
blocked and velocity ≠ 0) translate over the same movetime from the same
start ltime; otherwise translate. A blocked push does not advance ltime.
Then if the original nextthink lies in (old ltime, new ltime]: nextthink := 0
and the think runs (game time = the frame time) **[probe K-pusher]**.

### 4.2 Translation
1. Zero velocity: ltime += movetime.
2. move = velocity·movetime; the pusher moves, ltime += movetime, link
   without touch.
3. SOLID_NOT pushers stop here (passable doors and toggled walls push
   nothing).
4. Candidates, ascending index, excluding movetypes PUSH, NONE, NOCLIP,
   FOLLOW: riders (FL_ONGROUND on this pusher), and entities whose linked
   box overlaps the swept box (strict) and whose box at their origin
   intersects the pusher's own hull at its new position (a test against
   the pusher alone).
5. Non-player candidates lose FL_ONGROUND; each is pushed by `move` with the
   pusher temporarily non-solid (its own traces may stop it short; impact
   touches run).
6. Still intersecting the pusher afterwards: zero-width candidates are
   ignored; SOLID_NOT / SOLID_TRIGGER candidates (corpses, gibs) collapse
   to zero size and are ignored; otherwise the push is **blocked**: the
   candidate returns (link with touch), the pusher returns (ltime −=
   movetime), the pusher's `blocked` runs with other = candidate, every
   candidate already moved returns. **[probe P-push]**

### 4.3 Rotation
As 4.2 with: amove = avelocity·movetime added to angles; swept box = the
rotated-brush cube at the new angles; each candidate's displacement is its
position relative to the pusher origin rotated by amove; non-client
candidates turn with the pusher (`yaw += amove.yaw`), clients keep their
view; a blocked rotation undoes those yaws. **[probe P-rotate]**

### 4.4 Riders
Entities standing on a pusher move with it even outside the swept volume;
the player is moved by the push, not by pmove.

## 5. Water level (non-player)

After toss-family, STEP and PUSHSTEP steps: c = point contents at the
origin; first check after spawn sets watertype = c; afterwards a liquid
gives watertype c and waterlevel 1, anything else EMPTY and 0. No splash
sound from the engine **[probe P-step]**.

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
  moves). **[probe P-walkmove]**
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
state.

## 8. Tests

| # | property | data |
|---|---|---|
| P1 | toss: drop from rest lands after √(2h/gravity) ± one frame, ends at rest on the world | probe P-toss |
| P2 | bounce: rebound speed per friction (0, 0.55, 0.8), slide damping and rest speed on a floor; BOUNCEMISSILE reflects with factor 2 and no gravity | probe P-bounce |
| P3 | slide: wall keeps the parallel component; 90° corner stops; crease follows the crease; landing on a monster leaves the box not on ground | synthetic + probe P-toss |
| P4 | STEP: falls off ledges, lands silently; a pushed monster slides to rest by ground friction | probe P-step |
| P5 | pusher timing: door end time d/s, think on the frame ltime passes nextthink, blocked doors stall ltime | probe K-pusher |
| P6 | push: crate on a lift rises; crate beside a door is pushed; a monster against a closing door blocks it, `blocked` runs once per blocked frame, earlier candidates return | probe P-push |
| P7 | rotating door carries an entity round the hinge; non-client yaw follows, client yaw does not | probe P-rotate |
| P8 | corpses collapse instead of blocking | probe P-push |
| P9 | pushable friction and falling off a ledge | probe P-pushable |
| P10 | walk_move: 18 u steps climbed, 19 u not; overhang > 18 u fails check_bottom; partial ground correction | probe P-walkmove |
| P11 | drop_to_floor codes and origins | probe P-walkmove |
| P12 | conveyor: an item on a func_conveyor moves at the conveyor speed without its velocity changing | probe P-toss |
| P13 | active set: a resting crate, a resting gib and an idle func_wall are not stepped (zone counters 0) yet react on the frame something moves them | host counters |
