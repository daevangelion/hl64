# Physics: movetypes, pushers, monster steps

Per-frame motion of every entity except the player's walk/duck/swim/ladder
movement, which is pmove (pmove-host.md). Traces and touches:
trace-and-hulls.md, area-links-and-touch.md. Think timing:
think-and-time.md. Conventions: [README.md](README.md).

## 1. Parameters

Cvars (HL defaults): `sv_gravity` 800, `sv_maxvelocity` 2000,
`sv_friction` 4, `sv_stopspeed` 100, `sv_stepsize` 18, `edgefriction` 2.
Per entity: `gravity` (scale; 0 means 1), `friction` (see 4.4).

Constants: floor = a plane with normal.z > 0.7; wall = normal.z exactly 0;
stop epsilon 0.1 (velocity components smaller in magnitude become 0 after a
clip); slide iterations 4; clip planes 5; step height 18.

## 2. Frame dispatch

After `game_start_frame` and the player (pmove-host.md), every live entity
with index ≥ 2 is processed once, in increasing index order, including
entities created earlier in this same loop at higher indices. Before its
step, if a force-retouch count is active, the entity is relinked with touch.
Entities of a map that is asleep (architecture.md 6.3: not the home map,
outside its stream window, beyond the seam awake radius) are skipped.
Dispatch by movetype (HLSDK values): NONE 0, WALK 3 (only the player; others
treated as NONE), STEP 4, FLY 5, TOSS 6, PUSH 7, NOCLIP 8, FLYMISSILE 9,
BOUNCE 10, BOUNCEMISSILE 11, FOLLOW 12, PUSHSTEP 13. Unknown values are a
fatal error naming the entity.

Common operations used below:

- **check velocity**: any NaN component → 0 (logged once per entity);
  every component clamped to ±`sv_maxvelocity`.
- **add gravity**: `velocity.z −= scale · sv_gravity · dt`.
- **push(e, d)**: one move query from origin to origin + d with e's box and
  pass = e, type MISSILE for FLYMISSILE, NOMONSTERS when e is SOLID_TRIGGER
  or SOLID_NOT, NORMAL otherwise; e.origin := endpos; link with touch; if
  the trace hit an entity, impact touch (area-links-and-touch.md 4).
  Velocity is not changed. Returns the trace.
- **clip(v, n, k)**: `v − n·(v·n)·k`, then zero components with |c| < 0.1;
  reports "floor" if n.z > 0 and "wall" if n.z = 0.
- **slide(e, t)**: collide-and-slide for time t:
  - repeat up to 4 times while velocity is non-zero: move query from origin
    to origin + velocity·(time left), type NORMAL, pass e;
  - allsolid → velocity := 0, stop ("trapped");
  - fraction > 0 → origin := endpos, and the velocity at this point
    becomes the reference for clipping, the plane list is emptied;
  - fraction = 1 → done;
  - hit a floor plane → "floor"; if the hit entity is SOLID_BSP (world
    included) set FL_ONGROUND and groundentity := hit entity. Landing on a
    box entity (a monster, a crate) does not set FL_ONGROUND.
  - hit a wall plane → "wall";
  - impact touch; if e was freed, stop;
  - time left −= time left · fraction;
  - add the plane (more than 5 → velocity := 0, stop);
  - new velocity: the reference velocity clipped (k = 1) against the first
    plane of the list whose clipped result does not point into any other
    listed plane (dot < 0); if none: with exactly 2 planes, the velocity
    projected on the crease (cross product of the two normals); with more,
    velocity := 0 and stop;
  - if the new velocity points against the velocity e had when slide
    started (dot ≤ 0), velocity := 0 and stop (no corner jitter).
  - slide links e with touch afterwards (the caller does it).

## 3. Movetypes

### NONE
Think only (think-and-time.md). No motion, no gravity.

### NOCLIP
Think; `angles += avelocity·dt`; `origin += velocity·dt`; link without touch.
No collision.

### FLY, FLYMISSILE, TOSS, BOUNCE, BOUNCEMISSILE ("toss family")
1. Think; stop if freed.
2. TOSS and BOUNCE with FL_ONGROUND set do nothing more this frame.
3. Check velocity.
4. Add gravity unless FLY, FLYMISSILE or BOUNCEMISSILE.
5. `angles += avelocity·dt`.
6. push(e, velocity·dt); stop if fraction = 1 or e freed.
7. Clip velocity against the hit plane with overbounce k: BOUNCE 1.5,
   BOUNCEMISSILE 2, others 1.
8. If the plane is a floor: for TOSS (and FLY/FLYMISSILE) always, for
   BOUNCE only when the clipped velocity.z < 60: set FL_ONGROUND,
   groundentity := hit, velocity := 0, avelocity := 0.
   BOUNCEMISSILE never comes to rest on its own.
9. Water level update (5).

GoldSrc documents BOUNCE as using the entity's `friction` ("inverse
elasticity", HLSDK `progdefs.h`) and its `gravity` scale; the oracle's
BOUNCE uses the fixed 1.5 and the game integrates its grenades itself
(gravity 0.5, overbounce 1.2 = 2 − friction 0.8). hl64 implements the
oracle rule for BOUNCE until M5 parity passes; decision D6 then switches
BOUNCE to overbounce `2 − friction` (friction 0 → 1.5 kept for parity of
gibs) and grenades to BOUNCE.

### STEP (monsters)
1. If none of FL_ONGROUND, FL_FLY, FL_SWIM: add gravity, check velocity,
   slide(e, dt), link with touch. No landing sound.
2. Think.
3. Water level update (5).
Horizontal monster motion is done by the game through walk moves (6), not
by velocity.

### PUSHSTEP (func_pushable)
1. If FL_ONGROUND: horizontal ground friction — speed s = |velocity.xy|;
   if s > 0: control = max(s, sv_stopspeed); s' = max(0, s −
   dt·control·sv_friction); scale velocity.xy by s'/s. Then probe 1 u down
   with its own box (NORMAL, pass self): no hit (fraction 1, not allsolid)
   clears FL_ONGROUND (pushed off a ledge or the floor went away).
2. If not FL_ONGROUND: add gravity.
3. If velocity non-zero: check velocity, slide(e, dt), link with touch.
4. Think. 5. Water level update.
(The oracle memoises the probe while nothing relevant changes; the memo is
an optimisation with no observable effect.)

### PUSH (doors, plats, trains, rotating brushes)
See 4.

### FOLLOW
Each frame (after its think), origin := aiment's origin + the stored
offset in aiment's frame and angles := aiment's angles; link without touch.
Not used by the Uplink/Hazard game today [GoldSrc documented; no oracle].

### WALK
The player only (pmove-host.md). Any other entity with WALK is stepped as
NONE.

## 4. Pushers (MOVETYPE_PUSH)

### 4.1 Local time and think
A pusher keeps its own clock `ltime`; its `nextthink` is in `ltime` units.

- movetime = `nextthink − ltime` (≥ 0) if `nextthink < ltime + dt`, else dt.
- If movetime > 0: with non-zero avelocity, rotate first (4.3); if the
  rotation succeeded and velocity is non-zero, translate (4.2) over the same
  movetime starting from the pre-rotation `ltime` (a combined mover's
  rotation and translation share their start time; `ltime` ends at the
  later of the two results). Otherwise translate.
- A push that is blocked leaves `ltime` unchanged (the mover stalls and
  catches up nothing).
- Then if the original `nextthink` lies in `(old ltime, new ltime]`:
  nextthink := 0 and the think runs with game time = the frame's time.

### 4.2 Translation
1. Zero velocity: `ltime += movetime`, done.
2. move = velocity·movetime; swept box = pusher's linked box + move.
3. The pusher moves to its final position, `ltime += movetime`, link
   without touch.
4. A SOLID_NOT pusher stops here (passable doors and toggled-off walls
   move without pushing anything).
5. For every other live entity in index order, except movetypes PUSH,
   NONE, NOCLIP (and FOLLOW): it is a candidate if it stands on the pusher
   (FL_ONGROUND and groundentity = pusher), or if its linked box overlaps
   the swept box (strict: touching faces do not count) **and** its box at
   its origin intersects the pusher's own hull at the pusher's final
   position (a position test against the pusher alone — not against the
   world or other entities).
6. A candidate that is not the player (movetype WALK) loses FL_ONGROUND.
   It is pushed by `move` with the pusher temporarily non-solid
   (push(e, move): its own traces may stop it short against the world or
   other entities, and impact touches run).
7. If after the push the candidate still intersects the pusher:
   - a zero-width candidate (mins.x = maxs.x) is ignored;
   - a SOLID_NOT or SOLID_TRIGGER candidate (a corpse or gib) has its box
     collapsed to zero width and height (mins.xy := 0, maxs := mins) and is
     ignored;
   - otherwise the push is **blocked**: the candidate returns to its
     pre-push origin (link with touch), the pusher returns to its start
     (link without touch, `ltime −= movetime`), the pusher's `blocked`
     function runs with other = candidate, then every candidate already
     moved this push returns to its pre-push origin (link without touch).
     Processing stops.

### 4.3 Rotation
Same as 4.2 with these differences:
- amove = avelocity·movetime is added to the pusher's angles; the swept box
  is the pusher's linked box at its final angles (rotated-brush cube).
- Each candidate's displacement is the rotation of its position relative to
  the pusher origin by amove: with forward/right/up of `−amove`, the
  relative position r becomes `(r·forward, −r·right, r·up)`, and the push
  is the difference.
- A pushed candidate that is not a client turns with the pusher:
  `angles.yaw += amove.yaw` (clients keep their view angles). A blocked
  rotation undoes that yaw on every candidate it moved.

### 4.4 Riders
Entities standing on a pusher (including the player and resting TOSS
objects) move with it even when their boxes do not overlap the swept
volume. The player's pmove ground entity keeps carrying him (pmove-host.md:
the pusher's velocity is not added to his; he is moved by the push).

## 5. Water level (non-player)

At the end of the step of TOSS-family, STEP and PUSHSTEP entities:
contents c = point contents at origin. On the first check after spawn
(`watertype` 0) set watertype = c. Otherwise watertype := c and waterlevel
:= 1 when c is a liquid (WATER/SLIME/LAVA), else watertype := EMPTY and
waterlevel := 0. No splash sound (HL plays splashes from the game).
The oracle stores the contents value in waterlevel when leaving water (a
Quake bug); nothing in the game reads it for non-players, so hl64 uses 0.

## 6. Monster step moves (engine services)

- **check_bottom(e)**: true if the four bottom corners (at mins.z − 1) are
  all in SOLID world contents. Otherwise trace a line (NOMONSTERS) from the
  bottom centre down 2·18 u; no hit → false; then from each corner down to
  the same depth: any corner with no hit, or whose hit is more than 18 u
  below the centre hit, → false; else true.
- **walk_move(e, yaw, dist, mode)**: displacement (cos yaw, sin yaw, 0)·dist.
  - FL_FLY/FL_SWIM: try the displacement with a vertical adjustment toward
    the enemy (if it has one: −8 when more than 40 u above it, +8 when
    less than 30 u above it), then without; the first trace that completes
    (fraction 1) moves it (a swimmer may not leave liquid: a completed move
    into EMPTY fails). Otherwise fail.
  - Walkers: trace the box from new position + 18 up to new position − 18
    (NORMAL, pass e). allsolid → fail. startsolid → retry from the
    unraised position; allsolid or startsolid again → fail. No hit
    (fraction 1): with FL_PARTIALGROUND the monster moves by the plain
    displacement and loses FL_ONGROUND (falls); else fail (would walk off an
    edge). Hit: move to endpos; if check_bottom fails: with
    FL_PARTIALGROUND keep the move, else restore and fail. Success clears
    FL_PARTIALGROUND and sets groundentity := hit entity.
  - Link with touch on success (mode CHECKONLY: never move, only report;
    WORLDONLY: traces with NOMONSTERS).
- **move_to_origin(e, goal, dist, mode)**: step toward goal: first try
  walk_move along the direction to goal (turning the entity's yaw toward
  it at its yaw speed); if that fails, try the compass directions (multiples
  of 45°) that reduce the larger of |dx|, |dy| first, then the other
  axis, in random order when both are equal, never the direct reverse of
  the previous direction until everything else failed; if all fail, mark
  FL_PARTIALGROUND when the monster is not fully supported. Randomness from
  the engine stream. Used by 3 fallback paths of the game's AI only; its
  exact choice order is not part of any parity gate.
- **drop_to_floor(e)**: trace the box down 256 u (NORMAL, pass e):
  allsolid → −1; no hit → 0; else origin := endpos, link without touch,
  FL_ONGROUND, groundentity := hit, return 1.
- **change_yaw(e)**: move `angles.yaw` toward `ideal_yaw` by at most
  `yaw_speed` (degrees per call; the game scales it by frame time),
  shortest way round; angles normalised to [0, 360).

## 7. Render interpolation

STEP entities move in discrete think steps (often 0.1 s). The renderer
draws them at origin/angles interpolated linearly between the previous and
the current think position over the interval between those thinks,
capped at 0.2 s; teleports (a jump > 64 u in one step) snap. Other
movetypes draw at their current origin. [GoldSrc documented client
behaviour; the oracle client lerps entity origins between server messages.]

## 8. Tests

| # | property | data |
|---|---|---|
| P1 | toss on flat floor: an object dropped from rest at height h lands after t = √(2h/800) ± one frame, ends with velocity 0, FL_ONGROUND, groundentity = world | synthetic + fixture P-toss |
| P2 | bounce: dropped at 300 u/s onto a floor rebounds with vz = 0.5·300 (overbounce 1.5) and settles once a rebound would be < 60 u/s | synthetic |
| P3 | slide: a box moving diagonally into a wall keeps the parallel component; into a 90° corner it stops; into a 2-plane crease it moves along the crease; a box landing on a monster does not become FL_ONGROUND | synthetic hulls |
| P4 | STEP monster walking off a ledge falls under gravity and lands with FL_ONGROUND; no sound is emitted | synthetic + fixture P-step |
| P5 | pusher timing: a door with speed s and distance d reaches its end in d/s, fires its think on the frame where `ltime` passes nextthink, and its `ltime` stops while blocked | synthetic + fixture P-push (hldemo1 doors along the route: open/close frame per lump) |
| P6 | push: a crate on a lift rises with it; a crate beside a door is pushed; a monster against a closing door blocks it, the door's blocked function runs once per blocked frame, all already-pushed entities return | synthetic |
| P7 | rotating door: an entity in its sweep is carried around the hinge, gains the door's yaw (non-client), the player's yaw is unchanged | synthetic + fixture T-rotated scenes |
| P8 | corpses (SOLID_NOT/TRIGGER with size) inside a closing door collapse to zero size and do not block | synthetic |
| P9 | pushable: pushed crate decelerates by the friction rule and stops; pushed off a ledge falls | synthetic |
| P10 | walk_move: 18 u steps are climbed, 19 u are not; a corner hanging more than 18 u over a drop fails check_bottom; FL_PARTIALGROUND allows the correcting move | synthetic |
| P11 | drop_to_floor return codes −1/0/1 and resulting origin | synthetic |
| P12 | Uplink route: the per-frame origins of the 20 movers and 10 monsters the oracle fixture lists match within 0.5 u at every 1 s sample until the first divergence caused by AI randomness (monsters only) | fixture P-route |
