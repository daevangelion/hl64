# Player movement host (engine side of pmove)

The player's walk, duck, jump, swim, ladder and noclip movement is the
HLSDK shared player-movement code (`pm_shared/pm_shared.c`), compiled in
`game/pm/` under the HLSDK licence (decision D2, recommended). The engine
supplies its inputs and services through `include/hl64/pmove.h` (MIT,
our own layout); a small adapter in `game/pm/` copies them into the
HLSDK `playermove_t` and back. This spec is the contract of that adapter
and of the engine services. Conventions: [README.md](README.md).

If D2 rejects pm_shared, this spec still holds for the engine side and
the player movement itself must be specified separately (it is not covered
here).

## 1. Per-frame sequence (architecture.md 3.2 step 5)

1. Build the `usercmd`: `msec` = round(dt·1000) clamped to [1, 100];
   view angles; forward/side/up move (stick scaled to ±`cl_forwardspeed`
   400, `cl_sidespeed` 400, `cl_upspeed` 320, then the game's max speed
   clamps); buttons (`IN_ATTACK`, `IN_JUMP`, `IN_DUCK`, `IN_USE`,
   `IN_ATTACK2`, `IN_RELOAD`, forward/back/left/right bits per HLSDK
   `in_buttons.h`); impulse.
2. `game_player_prethink`.
3. Player think if due (think-and-time.md 2).
4. Base velocity: if the ground entity has FL_CONVEYOR, add
   `ground.speed · ground.movedir` to `basevelocity`; triggers that push
   set it themselves through their touch (HLSDK `trigger_push`).
   [GoldSrc documented behaviour, HLSDK `triggers.cpp` / `plats.cpp`;
   **probe** PM-bot run on a conveyor.]
5. Build the physent list (2) and call `game_pm_move` with the movevars
   (4), the player block and the usercmd.
6. Write back origin, velocity, basevelocity, view_ofs, flags
   (FL_ONGROUND, FL_DUCKING, FL_WATERJUMP…), groundentity (physent index →
   entity), waterlevel/watertype, `bInDuck`, `flDuckTime`, `flSwimTime`,
   `flTimeStepSound`, `iStepLeft`, `flFallVelocity`, `punchangle`,
   `oldbuttons`, `usehull` (box := hull 1 or hull 3 box).
7. Link the player with trigger touch (area-links-and-touch.md 3), then
   run impact touches for every entry of the pmove touch list in order
   (each pair once per frame: player.touch(other), other.touch(player)).
8. `game_player_postthink` (fall damage from `flFallVelocity`, weapons,
   use).

## 2. Physents

- Index 0 is the world: one entry per open layer is not possible in the
  HLSDK layout, so the world physent is the **home layer**; other open
  layers join as BSP physents with their `g_M` as origin (and zero angles)
  when their global box meets the player's move box.
- Then every linked entity whose linked box meets the player's move box
  for this frame (origin ± (box + |velocity|·dt + 18 + 1) on each axis),
  in ascending entity index (spatial grid query), except: the player,
  SOLID_NOT and SOLID_TRIGGER
  entities, entities owned by the player.
  - SOLID_BSP entities: model physent (origin, angles, hulls of its
    model, `skin` contents if < −1, rendermode).
  - SOLID_BBOX / SOLID_SLIDEBOX: box physent (mins, maxs, origin).
- Move-ents (ladders): every `func_ladder` (skin = CONTENTS_LADDER)
  whose box meets the move box.
- Brush entities whose `skin` is a contents value (func_water) are physents
  with `solid` = SOLID_NOT so pm_shared's point contents sees their
  contents but traces pass through them.
- Caps (decision D2 changes HLSDK's 600/600/64): physents 64, visents 0
  (not used by pm_shared's movement), move-ents 8, touch list 32. Overflow
  keeps the nearest entries and logs once per frame.

## 3. Services (callbacks the adapter wires into `playermove_t`)

| service | semantics |
|---|---|
| point contents (p) | `PM_PointContents`: world contents at p unless a physent with a contents skin contains p (box test then hull 0 of its model), whose skin then wins; the "true" output is the world's |
| true point contents (p) | `PM_TruePointContents`: world contents only, CURRENT_* kept; brush entities, ladders included, never contribute **[probe:measured PM-bot]** |
| hull point contents (hull, node, p) | walk a hull handle from node (trace-and-hulls.md 1); the handle is opaque to the game except for the root field pm_shared reads |
| hull for bsp (physent, offset out) | the hull of that physent's model for the current `usehull` (0 → hull 1, 1 → hull 3, 2 → hull 0, 3 → hull 2), and the offset as trace-and-hulls.md 4.2 |
| player trace (start, end, flags, ignore physent) | trace the player box for `usehull` against every physent in index order (flags: studio-ignore, glass-ignore, world-only per HLSDK `pm_defs.h`), combined as trace-and-hulls.md 4.4 step 5; **then if allsolid, set startsolid; if startsolid, fraction := 0** (the pmove rule, unlike server traces); `ent` = physent index, −1 none |
| trace line (start, end, flags, hull, ignore) | the same with an explicit hull |
| test player position (pos) | physent index of the first solid the player box at pos is inside, −1 if free |
| stuck touch (hit index, trace) | append to the touch list (deduplicated) |
| play sound (channel, name, volume, attenuation, flags, pitch) | engine emit on the player entity; pm_shared passes literal names (footsteps, ladder, wade, jump-land), so the adapter maps them to `res_t` through a small cooked table (≈ 40 names, perfect hash built by `restab`) |
| trace texture (physent, start, end) | material code of the surface hit (trace-and-hulls.md 4.7) as the HLSDK texture-type character |
| random long / float | engine random stream |
| model type / bounds | from the resource table / submodel table |
| info value for key | reads "slj" and similar physinfo keys: always empty |
| console print | engine log |

## 4. Movevars (HL defaults, typed fields of `settings.h`)

gravity 800, stopspeed 100, maxspeed 320, spectatormaxspeed 500,
accelerate 10, airaccelerate 10, wateraccelerate 10, friction 4,
edgefriction 2, waterfriction 1, entgravity 1, bounce 1, stepsize 18,
maxvelocity 2000, zmax 4096, waveHeight 0, footsteps 1, rollangle 0,
rollspeed 0 (view roll is the game's view code).

## 5. Behaviour expected from the composition (black-box, retail HL)

Not engine rules: properties the host tests check to catch adapter
mistakes. All on flat ground unless stated.

- Running speed caps at 320 u/s (maxspeed); ducked at ⅓ of the command
  speed; within 1 % of the cap from rest in under 0.5 s with accelerate 10.
- Jump: initial vz = √(2·800·45) ≈ 268.3 u/s; apex 45 u above the start.
- Steps up to 18 u are climbed without jumping; 19 u are not.
- Ducking takes 0.4 s to switch hulls on the ground (eyes ease down) and
  is instant in the air (feet lift 18 u).
- Standing up is refused where the standing hull does not fit.
- Ladder (pm_shared `PM_LadderMove`): holding forward on a ladder climbs at
  200 u/s with a level view and 282.84 u/s (200·√2) looking 45° up
  (pitch −45) **[probe:measured PM-bot]**.
- Water: waterlevel 1/2/3 by feet/waist/eyes; swimming speed ≤ 0.8 ×
  maxspeed; no gravity at waterlevel ≥ 2.
- Started inside solid: pm_shared's stuck test nudges the player out, as
  in GoldSrc.

## 6. Reference behaviour

GoldSrc is the target, and pm_shared is GoldSrc's movement; the GoldSrc
probe drives fake clients through `pfnRunPlayerMove` on the same maps with
the same command streams and logs origin, velocity, flags and view height
per frame (PM-bot runs). hl64 must reproduce those traces. The old tree
(Quake movement with GoldSrc patches) is not a reference for movement;
route radii that the old route needed for its own movement are re-derived
from the probe runs.

## 7. Tests

| # | property | data |
|---|---|---|
| PM1 | adapter round trip: every field of section 1 step 6 written back unchanged when pm_shared does not touch it (identity move with zero cmd) | self-contained |
| PM2 | section 5 properties and the PM-bot runs (walk, run, jump, duck, duck-jump, stairs, ladder, water, slopes): per-frame origin within 0.01 u and flags equal for 300 frames each | probe PM-bot runs on the player's maps |
| PM3 | player trace: startsolid ⇒ fraction 0; allsolid ⇒ startsolid; hull per `usehull` mapping | self-contained |
| PM4 | physents: a crate and a door near the player are physents; a trigger, a SOLID_NOT entity and the player's own grenades are not; a func_water volume changes waterlevel but not collision | synthetic |
| PM5 | touch list: walking into a crate calls crate.touch once per frame; walking into a trigger calls trigger.touch through linking | synthetic |
| PM6 | riding: standing on a moving lift keeps the player on it (pushed by the lift; FL_ONGROUND kept; no velocity added) | synthetic |
| PM7 | caps: 70 crates around the player keep the 64 nearest, log once | synthetic |
| PM8 | compiled route `m2-walk` in hldemo1 (stairs, duck crawl under pipes, ladder, drop) passes its `WAIT_NEAR` checks | route |
| PM9 | ladder climb speed: 200 u/s at pitch 0 and 282.84 u/s at pitch −45 (±0.01), constant over the climb | **[probe:measured PM-bot]** ladder runs |
