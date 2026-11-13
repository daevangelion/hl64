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
| [dlights.md](dlights.md) | dynamic lights, flashlight, falloff on vertex-lit world and models | M3 |
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
   HLSDK in `test/goldsrc-probe/` (HLSDK licence) on the player's Uplink /
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
GoldSrc behaviour.

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
| A-touch, A-shape | probe | area-links-and-touch.md (M2) |
| P-toss, P-bounce, P-step, P-push, P-rotate, P-pushable, P-walkmove | probe | physics-movetypes.md (M3) |
| K-think, K-pusher, K-alloc, K-settle | probe | think-and-time.md (M3) |
| PM-bot runs (walk, run, jump, duck, ladder, water, stairs) | probe | pmove-host.md (M2) |
| X-fx, X-glow, S-mix, L-anim, D-flash, W-texanim | retail client | render-fx, sound, lightstyles, dlights, world-surfaces (M3) |
| old cook outputs, M1 poses, route logs and PROFILE logs | old tree | world-surfaces W1, M1, M4–M6 baselines |
