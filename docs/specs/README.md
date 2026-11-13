# Behaviour specs

These files describe engine behaviour that the old quakegeneric tree
inherited from Quake (or approximated from GoldSrc) and that hl64 must
re-implement **without** reading Quake, Xash3D or GoldSrc engine code. They
were written by a dirty-side agent (EngineSpec) that read the old tree; they
contain prose, constants, tables and testable properties only — no code.
Implementers work from these files, `docs/architecture.md`,
`docs/formats/`, the Half Life 1 SDK and libdragon/tiny3d docs.

| spec | covers | milestone |
|---|---|---|
| [trace-and-hulls.md](trace-and-hulls.md) | contents, clip hulls, hull selection, segment/box traces, brush entity transforms (rotated brushes), entity clipping filters, layers | M2 |
| [area-links-and-touch.md](area-links-and-touch.md) | absolute boxes, area tree, linking, trigger touch, impact touch, wakes | M2/M3 |
| [physics-movetypes.md](physics-movetypes.md) | per-movetype stepping, gravity, slide move, pushers, step moves, water level, render interpolation | M3 |
| [think-and-time.md](think-and-time.md) | game clock, frame time, think/nextthink, pusher local time, entity alloc/free/reuse, frame order, timers | M3 |
| [pmove-host.md](pmove-host.md) | the engine side of player movement (HLSDK pm_shared contract) | M2 |
| [precache-and-resources.md](precache-and-resources.md) | model/sound/lightstyle index tables, ownership, reuse | M3/M4 |
| [lightstyles.md](lightstyles.md) | style patterns, timing, values, switchable lights, per-layer remap | M1/M3 |
| [dlights.md](dlights.md) | dynamic lights, flashlight, muzzle/effect lights, falloff on vertex-lit world and models | M3 |
| [sound.md](sound.md) | channels, attenuation, spatialisation, pitch, loops/ambients, sentences | M3 |
| [render-fx.md](render-fx.md) | render modes/fx blend, sprites, decals, temp effects, particles (behaviour level) | M3 |
| [world-surfaces.md](world-surfaces.md) | cooked vertex light and light probes (cook side), texture animation sequencing (cook + runtime) | M1 |

## Conventions used by every spec

- Units are GoldSrc world units; +Z is up. Angles are degrees, stored as
  (pitch, yaw, roll). The forward/right/up vectors of an angle triple are
  the HLSDK `AngleVectors` convention: with sp, cp, sy, cy, sr, cr the
  sines/cosines of pitch, yaw, roll,
  forward = (cp·cy, cp·sy, −sp),
  right = (−sr·sp·cy + cr·sy, −sr·sp·sy − cr·cy, −sr·cp),
  up = (cr·sp·cy + sr·sy, cr·sp·sy − sr·cy, cr·cp).
  Traces, pushers and brush drawing use entity `angles` as given; studio
  drawing follows the HLSDK studio renderer's convention (architecture.md
  D3); HLSDK's player pitch convention (`angles.x = −v_angle.x / 3`,
  `UTIL_MakeAimVectors`) is the game's business.
- Constants named `CONTENTS_*`, `MOVETYPE_*`, `SOLID_*`, `FL_*`, `EF_*`,
  `CHAN_*`, `ATTN_*`, `kRender*` take the values in HLSDK `common/const.h`
  and `dlls/util.h` (allowed source).
- "Oracle" means the old tree's behaviour, observed by running its
  `hl_parity` host build or its cart ROM, never by reading its source.
  Where the oracle differs from documented GoldSrc behaviour, the spec says
  so and states which one hl64 implements (architecture.md decision D6).
- `dt` is the frame's simulation step (architecture.md 3.2); `time` is the
  game clock at the start of the frame (think-and-time.md).

## Tests and oracle fixtures

Each spec ends with a numbered test list. A test is a property plus the
data it runs on:

- **Self-contained** tests run on hl64 alone (algebraic properties,
  invariants, synthetic hulls built in the test).
- **Fixture** tests compare hl64 against oracle captures stored as data in
  `test/fixtures/oracle/<spec>/<name>.jsonl` (one JSON object per line:
  inputs and expected outputs). Fixtures are produced by a dirty-side agent
  that builds the old tree in a scratch worktree with a temporary probe
  (never committed anywhere), runs it on the cooked Uplink/Hazard content,
  and writes the JSONL plus a `MANIFEST` (old-tree commit, build flags,
  content cook hashes, capture command). Clean-side agents read fixtures,
  never the probe.
- **Route** tests run route files (architecture.md 15) on host and ares.

Tolerances unless a test says otherwise: positions ±0.001 u, fractions
±1e-5, angles ±0.01°, times exact to the frame. A fixture mismatch beyond
tolerance fails the test and prints the input line.

Fixture captures owed (one dirty-side agent; the M2 ones before M2 starts):

| capture | for | content |
|---|---|---|
| T-points, T-segments, T-entities, T-rotated | trace-and-hulls.md | contents/trace results on all 10 maps |
| A-links | area-links-and-touch.md A7 | trigger fires along the Uplink route |
| P-toss, P-step, P-push, P-route | physics-movetypes.md | synthetic scenes run in the oracle + mover/monster origins along the route |
| K-think | think-and-time.md, lightstyles.md L6 | timer fires and lightstyle changes along the route |
| S-route | sound.md S6–S7 | emit log along the route |
| X-fx | render-fx.md X3 | blend values per fx over time |
| old cook outputs | world-surfaces.md W1 | the old cook's `.w64` files for the 10 maps (data only) |
| screenshots | M1, lightstyles L5, dlights D5–D6, render-fx X4/X7, M5, M6 | ares PPMs at fixed poses / route `shot` steps |
| route logs | M4–M6 | `stats` lines and PROFILE logs of the old cart and host routes |

Fixtures that embed game data (cooked `.w64` files, screenshots) are never
committed: they live under the local `HL64_FIXTURES` directory and are
regenerated from the player's own data, like the cooked content itself.
Number-only fixtures (trace results, timings, emit logs with sample names)
may be committed under `test/fixtures/oracle/`.
