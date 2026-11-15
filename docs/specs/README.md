# Behaviour specs

These files state what hl64 must do, as GoldSrc-observable results, for the
engine behaviour the old quakegeneric tree inherited from Quake or
approximated from GoldSrc. They were written by a dirty-side agent
(EngineSpec) that read the old tree; they contain prose, constants, tables
and testable properties only — no code. Implementers work from these files,
`docs/architecture.md`, `docs/formats/`, the Half Life 1 SDK and
libdragon/tiny3d docs.

**Target:** GoldSrc as observed (architecture.md rule 1). Each spec separates
the *behaviour* (what a player or the game code can observe; must match
GoldSrc) from the *mechanism* (how hl64 computes it; free, and chosen for
the N64). Where the old tree differs from GoldSrc, the spec says so and
follows GoldSrc.

| spec | covers | milestone |
|---|---|---|
| [trace-and-hulls.md](trace-and-hulls.md) | contents, clip hulls, hull selection, traces, brush transforms, hitboxes, move filters, layers | M2 |
| [area-links-and-touch.md](area-links-and-touch.md) | linked boxes, spatial grid, trigger and impact touches, wakes | M2/M3 |
| [physics-movetypes.md](physics-movetypes.md) | movetypes, gravity, slide move, pushers, step moves, water level, render interpolation | M3 |
| [think-and-time.md](think-and-time.md) | clock, frame time, think timing, active sets, pusher time, entity lifetime, timers | M3 |
| [pmove-host.md](pmove-host.md) | engine side of HLSDK player movement | M2 |
| [precache-and-resources.md](precache-and-resources.md) | cooked resource ids, precache semantics, residency interest | M3 |
| [lightstyles.md](lightstyles.md) | style patterns, timing, values, switchable lights, model lighting | M1/M3 |
| [dlights.md](dlights.md) | dynamic lights, night vision (the flashlight, D12), falloff on vertex-lit world and models | M3 |
| [sound.md](sound.md) | channels, attenuation, stereo, pitch, loops, sentences | M3 |
| [render-fx.md](render-fx.md) | render modes / fx blend, sprites, decals, temporary effects | M3 |
| [world-surfaces.md](world-surfaces.md) | cooked vertex light, light probes, texture animation | M1 |

## Conventions

- GoldSrc world units; +Z up. Angles in degrees (pitch, yaw, roll); their
  forward/right/up vectors follow HLSDK `AngleVectors`: with sp, cp, sy,
  cy, sr, cr the sines/cosines of pitch, yaw, roll,
  forward = (cp·cy, cp·sy, −sp),
  right = (−sr·sp·cy + cr·sy, −sr·sp·sy − cr·cy, −sr·cp),
  up = (cr·sp·cy + sr·sy, cr·sp·sy − sr·cy, cr·cp).
  Traces, pushers and brush drawing use entity `angles` as given; studio
  drawing follows HLSDK's studio renderer (D3); the player pitch
  convention (`angles.x = −v_angle.x / 3`) is the game's business.
- `CONTENTS_*`, `MOVETYPE_*`, `SOLID_*`, `FL_*`, `EF_*`, `CHAN_*`, `ATTN_*`,
  `kRender*` values are those of HLSDK `common/const.h` / `dlls/util.h`.
- Time: the engine clock is integer microseconds (`now`); `dt` is the frame
  step; the game's float `time` = now · 10⁻⁶ (think-and-time.md).

## Oracles

In order of authority:

1. **GoldSrc probe** (decision D10). Valve's Linux dedicated server
   (steamcmd app 90, anonymous download) running a game DLL built from the
   HLSDK in `test/oracle/` (HLSDK licence) on the player's Uplink /
   Hazard maps. The probe creates entities and calls engine functions
   (`pfnTraceLine`, `pfnTraceHull`, `pfnTraceModel`, `pfnPointContents`,
   `pfnSetOrigin`, `pfnWalkMove`, `pfnDropToFloor`, movetypes and
   `nextthink` on probe entities, fake clients driven by `pfnRunPlayerMove`
   for pmove) and logs inputs and results as JSONL. This is black-box
   observation of retail GoldSrc (allowed by PROVENANCE.md); nothing of the
   engine is disassembled. It covers server-side behaviour: traces,
   touches, physics, think timing, pushers, player movement.
2. **Retail client captures** (needs the user's Half-Life install on
   Steam): screenshots and audio for client-side behaviour (render modes,
   fx blends, sprites, decals, dlights, flashlight, sound mixing,
   lightstyle and texture animation rates).
3. **HLSDK source** for anything the game DLL computes (game/ semantics,
   `pm_shared`, effect parameters).
4. **Old tree** (its `hl_parity` host build or cart ROM, observed by
   running it) only where it already matches GoldSrc — chiefly the cooked
   world itself (the same `.w64` data), seam behaviour, streaming and
   memory baselines, CPU baselines.

A property marked **[probe]** is calibrated or confirmed by an oracle-1
capture before its milestone; until then the stated value is the expected
GoldSrc behaviour. A property marked **[probe:measured <capture>]** states
what that capture measured (`$HL64_FIXTURES/goldsrc/<spec>/<capture>/`);
where a measurement contradicted an earlier expectation, the measurement
replaced it.

## Tests and fixtures

Each spec ends with numbered tests: a property plus its data —
self-contained (algebraic, synthetic scenes), fixture (JSONL inputs and
expected outputs in `test/fixtures/<oracle>/<spec>/`), or route (compiled
routes on host and ares). Tolerances unless stated: positions ±0.001 u,
fractions ±1e-5, angles ±0.01°, times exact to the frame.

Fixtures that embed game data (cooked `.w64`, screenshots, audio) are never
committed; they live in the local `HL64_FIXTURES` directory and are
regenerated from the player's own data. Number-only fixtures may be
committed.

Captures owed before the milestone that needs them:

| capture | oracle | for |
|---|---|---|
| T-points, T-segments, T-entities, T-rotated, T-hitbox, T-allsolid | probe | trace-and-hulls.md (M2) |
| A-touch, A-shape, A-order (taken: creation order of runtime triggers; map-trigger placements validate the geometry-computed `touchrank` cell model) | probe | area-links-and-touch.md (M2) |
| P-toss, P-bounce, P-step, P-push, P-rotate, P-pushable, P-walkmove (taken) | probe | physics-movetypes.md (M3) |
| K-think, K-pusher, K-alloc, K-settle (taken); W-water: `func_water` contents (taken); L-illum (taken: no server-side light observable, below) | probe | think-and-time.md, trace-and-hulls.md, lightstyles.md (M3) |
| PM-bot runs (walk, run, jump, duck, ladder, water, stairs) | probe | pmove-host.md (M2) |
| X-fx, X-glow, S-mix, L-anim, D-flash, W-texanim | retail client | render-fx, sound, lightstyles, dlights, world-surfaces (M3) |
| old cook outputs, M1 poses, route logs and PROFILE logs | old tree | world-surfaces W1, M1, M4–M6 baselines |

### How the GoldSrc probe captures were taken

The probe is in `test/oracle/` (HLSDK licence for the DLL sources; record
formats and server setup in `test/oracle/README.md`); `test/oracle/run.sh`
reproduces every capture byte for byte and `test/oracle/tools/summarize.py`
prints coverage per test id. Fixtures:
`$HL64_FIXTURES/goldsrc/<spec>/<capture>/<map>.jsonl`, maps hldemo1–3 and
t0a0, t0a0a, t0a0b, t0a0b1, t0a0b2, t0a0c, t0a0d. Server: HLDS build 10211,
one client slot (so `host_framerate 0.01` holds every frame at 10 ms),
single-player rules and movevars (`sv_maxspeed 320`, `sv_clienttrace 1`),
map monsters removed and map triggers set SOLID_NOT before capturing (both
listed in each file's `meta`), brush entities in their spawn state (listed
on line 2). Inputs come from a generator seeded by (map, capture).

| capture | taken as |
|---|---|
| T-points | 20 000 points per map in the BSP world bounds, every 4th on the 1/16 u lattice, plus 64 inside each contents-skin brush entity; `pfnPointContents` and the pmove `PM_PointContents` / `PM_TruePointContents` of a parked fake client |
| T-segments | per hull 2000 segments ≤ 192 u (80 % starting open) and 200 of 192–4096 u; `pfnTraceModel` on the world and `pfnTraceHull(ignore_monsters)`; 500 per hull also as pmove `PM_PlayerTrace(PM_WORLD_ONLY)` |
| T-allsolid | per hull 500 segments inside solid and 500 leaving solid, same three traces |
| T-entities | probe box B and pass entity P at an open spot: B as bbox / slidebox / not / trigger / owned by P / owner of P / glass / FL_MONSTER / zero size; line traces in modes 0, 1, 2, 0x100, 0x101 with pass none/P/B at lateral offsets around B's faces and ±15; hull 1–3 traces; `pfnTraceMonsterHull` boxes {0,8,8.01,32,36,36.01,64}² × {18,36,36.01,72} (centred and feet-origin) against a probe brush entity and the world |
| T-rotated | a map brush model as probe SOLID_BSP entity at seven angle sets; line, hull 1–3, feet-origin box and single-model (`pfnTraceModel`) traces aimed through it |
| T-hitbox | scientist, barney, human_grunt, zombie spawned by the game DLL and frozen in four poses (sequence, frame, yaw, controllers, blending); line traces on a 4-side × 21-height × 11-lateral grid, a few hull 3 traces |
| A-touch | linked boxes read back per solid type, bit 256 (Quake's FL_ITEM; HLSDK has none), yaw, rotated SOLID_BSP / SOLID_TRIGGER brushes; box trigger vs. FLY / bit-256 / SOLID_NOT / SOLID_TRIGGER / MOVETYPE_NONE / TOSS movers and the fake client at real-box gaps −8…3 u per axis, touches tagged set_origin / physics / pmove; three overlapping triggers; seven impact scenarios |
| A-shape | the map brush model that least fills its bounds (an angled `trigger_once` in hldemo1–3; none in the Hazard Course) as a probe trigger, unrotated and at yaw 30; 320 sample origins, each probed by the fake client's 1 ms zero move and by a physics mover |
| PM-bot | fake client via `pfnRunPlayerMove`, 300 frames per run: stand, walk, run, strafe, jump, standjump, duck, duckjump on level floor (msec 10; run/jump/duck/duckjump at 33; run/jump at 50), 18 u and 19 u step brushes, stairs, ladder (pitch −45 and 0), water, slope; per frame the command and the player state after the move, plus hull, physents and touches from `playermove_t` |
| A-order | a server of its own with map triggers left linked as the map left them (a `DispatchTouch` hook logs and swallows map touches): every pair of SOLID_TRIGGER entities whose linked boxes are within 64 u, per mover hull 0/1/3/2 up to two origins where a mover touches both; a FLY mover set there and the touches of its next physics link in order, plus the fake client's 1 ms move; then runtime probe triggers, start-off `trigger_hurt` / `trigger_push` turned on as `ToggleUse` does, and every pair re-measured after its first-touched trigger was toggled off and on; placements wait until `force_retouch` is 0 |
| K-think, K-pusher | probe entities thinking at frame times 0.01, 1/30 and 0.1 s (`host_framerate`), thinks set by other entities, the `nextthink` boundary, `multi_manager` / `trigger_relay` delays; probe pushers (copies of map brush models) moving with `nextthink` on `ltime`, blocked, sub-frame, idle |
| K-alloc, K-settle | raw edict allocation and reuse read from the edict array; map-load time and the first 40 StartFrames with every item's state |
| P-toss … P-walkmove | scenarios on a level world floor spot: drops, an arc, landing on a monster, a conveyor, base velocity; BOUNCE at friction 0 / 0.55 / 0.8 / 1, a grenade, BOUNCEMISSILE; STEP falls, slides, ledges, water; a lift, doors, blocked doors with riders, a corpse, a monster; a rotating pusher with riders and the fake client; PUSHSTEP boxes and the maps' `func_pushable`s; `pfnWalkMove` (flat, probe steps, ledges, world stairs, fly, swim), `pfnDropToFloor`, `pfnMoveToOrigin` |
| W-water | a `func_water` spawned by the game DLL (none in these maps): `pfnPointContents` and pmove contents on a lattice, toss / step / fake client inside it, contents after it moved; probe brush entities with each contents skin and solid type |
| L-illum | `pfnGetEntityIllum` at 48 floor points every frame, with light style 0 changed and next to an EF_BRIGHTLIGHT entity: 128 everywhere (the client 0), so light styles, dlights, sound and render fx have no server-side observable on HLDS and stay with the retail client oracle |

M2 and M3 captures run with map triggers disabled in servers of their
own (`run.sh all`: m2, m3, live), so one group's time, entity slots and
engine random stream do not depend on the other.
