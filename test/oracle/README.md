<!-- SPDX-License-Identifier: MIT -->
# GoldSrc oracle (HLDS + HLSDK probe)

Oracle 1 of `docs/specs/README.md` (decision D10): retail GoldSrc observed
as a black box. Valve's Linux dedicated server (steamcmd app 90, anonymous)
runs a game DLL built from the Half Life 1 SDK plus a probe module; the
probe creates entities and fake clients, calls engine functions and logs
inputs and results as JSONL fixtures. Nothing of the engine is
disassembled or copied; the engine is only called through the HLSDK
interface (`pfnTraceLine`, `pfnTraceHull`, `pfnTraceModel`,
`pfnTraceMonsterHull`, `pfnPointContents`, `pfnSetOrigin`,
`pfnRunPlayerMove`, the pmove callbacks the engine hands to `PM_Move`,
physics and touches of probe entities).

## Files and licences

| path | licence | what |
|---|---|---|
| `probe/*.cpp`, `probe/*.h` | Half Life 1 SDK LICENSE (`LICENSE-HLSDK`) | probe module compiled into the HLSDK game DLL |
| `hlsdk-hooks.patch` | Half Life 1 SDK LICENSE | five hooks into HLSDK `dlls/` (GameDLLInit, StartFrame, CWorld::Precache, the `pfnPM_Move` table entry, DispatchTouch) |
| `run.sh`, `Makefile`, `tools/summarize.py`, this file | MIT | install, build, capture, report |

No Valve binary, map, model or fixture is committed. Fixtures derive from
Valve data and stay in `$HL64_FIXTURES` (default `/scratch/hl64/fixtures`).

## Running

```
test/oracle/run.sh            # all M2 captures on the Uplink + Hazard Course maps
test/oracle/run.sh all        # M2, M3 and live captures (three servers per map)
test/oracle/run.sh m3         # M3 captures (P-*, K-*, W-water, L-illum)
test/oracle/run.sh live       # A-order (map triggers left live)
test/oracle/run.sh T-points   # one capture; names below
HL64_MAPS="hldemo1" HL64_PROBE_SCALE=0.1 test/oracle/run.sh m2   # quick
test/oracle/tools/summarize.py   # coverage per spec test id + spec checks
```

`run.sh` is idempotent: it installs HLDS into `$HLDS_DIR`
(`/scratch/hlds`) with steamcmd if missing, clones the HLSDK at the pinned
revision `b1b5cf5892918535619b2937bb927e46cb097ba1` into
`/scratch/hl64/tmp/oracle/hlsdk` and applies `hlsdk-hooks.patch`, builds
`probe.so` (32-bit, gcc `-m32`, x87 maths as Valve's HLSDK Makefile) into
`/scratch/hl64/builds/oracle`, installs the game directory
`$HLDS_DIR/hl64probe` (`liblist.gam`, `dlls/probe.so`, the maps copied from
the player's data, default `/scratch/hl1/valve`, and a `server.cfg`), then
runs one server per map and batch. Each server exits by itself when its
captures are done; logs go to `$HL64_FIXTURES/goldsrc/logs/<map>[-m3|-live].log`.
Batches: the named captures and groups (`m2`, `m3`) in one server; `all`
runs `m2`, `m3` and `live` in servers of their own; live captures always
run apart (the probe refuses to mix them). The full M2 set on ten maps
takes about three minutes, M3 about two, live a few seconds.

Build needs gcc-multilib (32-bit C runtime) and the 32-bit `libstdc++.so.6`
(linked by path; g++-multilib is not required).

## Server setup (what the fixtures assume)

- `+maxplayers 1 +deathmatch 0 +coop 0`: single-player rules
  (`CHalfLifeRules`); with one client slot the engine honours
  `host_framerate`, so every frame is exactly 0.01 s (`+host_framerate 0.01
  +sys_ticrate 10000`: the run is as fast as the CPU allows and
  deterministic).
- `server.cfg` (written by `run.sh`) sets retail single-player movevars:
  `sv_maxspeed 320` (HLDS's own `valve/server.cfg` would set 270),
  `sv_clienttrace 1` (HLDS: 3.5), gravity 800, stepsize 18, friction 4,
  edgefriction 2, stopspeed 100, accelerate/airaccelerate/wateraccelerate
  10, waterfriction 1, bounce 1, maxvelocity 2000.
- Captures start 2 s after the map spawned (game time 3.0). First the probe
  removes every map monster (scripts and AI would move things) and sets
  every map trigger to SOLID_NOT (the bot must not change level or fire
  map logic); both lists are in each file's `meta`. Doors, trains and other
  brush entities stay in their spawn state; every file's second line lists
  them (`brush_ents`: index, model, origin, angles, bounds, solid, skin).
- Live captures (`A-order`) instead leave map triggers linked exactly as
  the map left them (listed as `live_triggers` in `meta`): the probe's
  `DispatchTouch` hook logs every touch of a map entity and does not
  dispatch it, so no level change, damage, teleport or pickup ever runs.
  Monsters are removed as above.
- World bounds are read from the map file (BSP model 0).
- Random inputs come from an xorshift generator seeded with
  (map, capture), so a rerun reproduces the inputs exactly.

## Output

`$HL64_FIXTURES/goldsrc/<spec>/<capture>/<map>.jsonl`. Line 1 `{"meta":…}`,
line 2 `{"brush_ents":[…]}`, then records. Vectors are `[x,y,z]`, floats
printed with 9 significant digits (round-trip exact for `float`). Trace
results everywhere use the HLSDK `TraceResult` fields:
`as` fAllSolid, `ss` fStartSolid, `io` fInOpen, `iw` fInWater, `fr`
flFraction, `end` vecEndPos, `n` vecPlaneNormal, `d` flPlaneDist, `hit`
pHit entity index (−1 none, 0 world), `hg` iHitgroup. Pmove traces (`pm`)
use the same keys with `pe` = physent index. Entities (`ent`) are
`{"i","class","model","o","a","mins","maxs","absmin","absmax","solid","movetype","flags","skin","rendermode","owner"}`.

| capture | spec tests | records |
|---|---|---|
| `trace-and-hulls/T-points` | T1 | `{"p","src","c","pmc","pmtc","tpc"}`: 20 000 points per map (`src` `u` uniform in the world bounds, `l` on the 1/16 u lattice, every 4th point) plus 64 points inside each brush entity with a contents skin (`src` `b`). `c` = `pfnPointContents`; `pmc`/`pmtc` = pmove `PM_PointContents` (contents, true contents; physents are those near the parked bot only); `tpc` = pmove `PM_TruePointContents` (world). |
| `trace-and-hulls/T-segments` | T2, T3, PM3 | `{"hull","kind","s","e","world","move","pm"?}` per hull 0–3: 2000 segments ≤ 192 u (`kind` `o`: start open for that hull, 80 %; `u`: start anywhere) and 200 segments 192–4096 u (`L`); 15 % axial, 15 % horizontal directions. `world` = `pfnTraceModel` against entity 0 (world only); `move` = `pfnTraceHull(ignore_monsters)` (world and brush entities, `hit` names the entity). `pm` (first 500 per hull) = pmove `PM_PlayerTrace(PM_WORLD_ONLY)` with `usehull` mapped from the hull (1→0, 3→1, 0→2, 2→3). |
| `trace-and-hulls/T-allsolid` | T9 | same record; 500 per hull starting in solid with a 1–64 u segment (`kind` `s`) and 500 starting in solid and ending in the open within 128 u (`x`). |
| `trace-and-hulls/T-entities` | T5, T6 | `{"spot"}`, then per configuration `{"setup",ents:[B,P]}` and traces `{"cfg","fn","mode","hull","pass","mover","s","e",…trace}`. B is a probe box (−16,−16,−36)…(16,16,36) at the spot, P a small box 100 u away used as pass entity. Configurations: bbox, slidebox, not, trigger, owned_by_pass (B.owner = P), owner_of_pass (P.owner = B), glass (kRenderTransTexture), monster (FL_MONSTER), zero_size. Line traces with modes 0, 1 (ignore_monsters), 2 (missile), 0x100, 0x101 (ignore glass) and pass none/P/B at 12 lateral offsets; hull 1–3 traces along and across B. `hullsel_*`: `pfnTraceMonsterHull` with a mover of size {0,8,8.01,32,36,36.01,64}² × {18,36,36.01,72}, centred (`hullsel_`) and feet-origin (`hullsel_feet_`), in six axis directions: +x against a probe brush entity (`hullsel_door` setup), the rest against the world; `hullsel_ref` = the same paths with hulls 0–3. |
| `trace-and-hulls/T-rotated` | T7 | `{"spot","model"}`, then per angle `{"angles","ent"}` (the probe brush entity, its linked box) and traces: `line`, `hull` 1–3, `monsterhull` with a feet-origin box (−16,−16,0)…(16,16,72), and `model` (`pfnTraceModel` hull 0/1/3 against that entity only). Angles (0,0,0) (0,30,0) (0,90,0) (0,135,0) (30,0,0) (15,45,0) (0,30,20). The model is the map brush model that best fills its bounds; the origin is set so the rotated model centre lands on the spot. |
| `trace-and-hulls/T-hitbox` | T10 | per monster (scientist, barney, human_grunt, zombie) and pose `{"monster","pose","sequence","frame","controller","blending","body","skin","ent"}` then line traces from 4 sides, heights −2…78 step 4, lateral −20…20 step 4, plus a few hull 3 traces (bounding box only). Monsters are spawned by the game DLL, then frozen (no think, MOVETYPE_NONE). |
| `area-links-and-touch/A-touch` | A1, A3, A4, A7 | `{"linkbox",ent}`: linked boxes read back for box entities of each solid type, with Quake's FL_ITEM bit (256; HLSDK has no FL_ITEM), yawed, and a brush entity unrotated / yaw 30 / pitch 20 / trigger yaw 30. Then a box trigger T (±32) and a mover M (MOVETYPE_FLY, (−16,−16,−36)…(16,16,36), zero velocity): per case `{"case","name","kind","axis","gap"}`, `{"ev":"placed"|"after",…}`, and every touch `{"ev":"touch","case","phase","self","other",…}` where `phase` is `set_origin` (during `pfnSetOrigin`), `physics` (the engine's physics frame that follows) or `pmove` (the fake client's `pfnRunPlayerMove`). Gap = distance between the real boxes. Kinds: fly, item (bit 256 set), not, trigger, none (MOVETYPE_NONE), toss, bot (fake client). `_order` cases add two more overlapping triggers (`{"setup":"order"}`). Cases ≥ 1000: impact scenarios (fly into a box, toucher sets itself SOLID_NOT, SOLID_NOT mover, toss onto, bounce into, target removes mover, mover removes itself) with per-frame mover state (`ev` `a`) for 40 frames. |
| `area-links-and-touch/A-shape` | A5 | `{"candidate",…,"fill"}` for every map brush model (fraction of interior lattice points inside the brush), `{"source"}` = the chosen non-box model (triggers first), then per variant (trigger unrotated, trigger yaw 30) `{"variant","trigger"}` and 320 sample points `{"case","p","in_hull0","in_hull1","in_hull1_unrotated","box_overlap"}` followed by the fake client's touches (phase `pmove`, after a 1 ms zero command at that origin, `{"ev":"bot","after"}`) and the physics mover's touches (phase `physics`). The source map entity is set SOLID_NOT. |
| `pmove-host/PM-bot` | PM2 (and §5) | Finder lines (`{"finder",…,"found":false}`, `{"step",…}` for the step brushes), then per run a header `{"run","kind","msec","start","yaw","where","reset"}` and 300 frames `{"run","f","cmd":{"a","fm","sm","um","b","ms"},"t","o","v","bv","fl","vo","wl","wt","ge","ind","dt","fv","pa","mt","mins","maxs","hull","pmt","onground","oldbuttons","pe":[[info,solid,skin]…],"me":[…],"touch":[…]}` — the command sent and the player state after `pfnRunPlayerMove`; `hull`…`touch` are read from `playermove_t` right after `PM_Move`. Kinds: stand, walk (150), run (400), strafe (400/400, turning), jump, standjump, duck, duckjump on level floor at msec 10 (run/jump/duck/duckjump also at 33, run/jump at 50); step18/step19 (a box brush sunk into the floor so its top is 18 or 19 u up, 64 u ahead); stairs (found by step-up search), ladder (pitch −45) and ladder_level (pitch 0), water (enter from 4 u above the surface, swim, then jump), slope; the varying kinds at msec 10 and 33. Command schedules are in milliseconds (`probe_pmove.cpp` `MakeCmd`). |
| `area-links-and-touch/A-order` | A9, 3.1 | Live. `{"trigger":{"i","class","model","targetname","lump","start_off","o","a","mins","maxs","absmin","absmax","solid","spawnflags"}}` per SOLID_TRIGGER entity (and per SOLID_NOT `trigger_hurt` / `trigger_push` / `trigger_monsterjump`, `start_off`); `lump` = block index in the map's entity lump (worldspawn 0), brush entities matched by model, point entities (items) by classname and nearest origin. Per pair of triggers whose linked boxes are within 64 u and per mover hull (0: 2 u cube, 1: 32×32×72, 3: 32×32×36, 2: 64 cube) `{"pair","phase","hull","mover","found","candidates","p"}`: origins where a mover touches both (box overlap, origin inside each brush trigger's clip hull, trigger angles ignored). Then per placement `{"place","phase","pair","hull","mover","p","after","touched":[…],"order":[first,second]}`: a FLY mover of that hull set there by `pfnSetOrigin`, `touched` = every trigger its next physics link touched, in order; `mover` `bot` = the fake client's 1 ms zero move at the hull-1 origin. Phases: 1 static (map triggers never relinked); 2 runtime (`{"runtime"}` lines: probe trigger B created, then A in a slot freed 0.6 s earlier, both inside map trigger `map_trigger`); 3 start-off triggers turned on as HLSDK `CBaseTrigger::ToggleUse` (`{"enabled"}` lines, then their pairs); 4 toggle: per phase-1 pair, `{"toggle"}` = the trigger touched first turned off and on (ToggleUse twice; toggles accumulate), then the pair re-measured (`after_toggle`). |
| `think-and-time/K-settle` | K8 | `{"precache_time","early_frames"}`, then `{"probe_run"}` and the first 40 StartFrames from map load `{"early","frame","t","ft","items":[{"i","class","o","v","fl","ge","mt","solid","nt"}…]}` (every `item_*`, `weapon_*`, `ammo_*`), then `{"item","class","lump","lump_o","o","fl"}`: each item's map-file origin and where it rests. |
| `think-and-time/K-alloc` | K6 | Raw `pfnCreateEntity` edicts (classname `hl64_raw` after the allocation is recorded): `{"snapshot"}` = every freed slot `[index, freetime, serial]` (the edict array read directly; `INDEXENT` hides free edicts); `{"alloc","t","i","serial","freetime","o","fl","classname","solid"}` (fields as allocated). Fillers take every reusable slot, then E0–E5; frame 1 frees E1, E3 (`pfnRemoveEntity`) and flags E5 FL_KILLME; one allocation at frames 2, 3, 26, 50–53, 61, 70. |
| `think-and-time/K-think`, `K-pusher`; `physics-movetypes/P-toss`, `P-bounce`, `P-step`, `P-push`, `P-rotate`, `P-pushable`, `P-walkmove`; `trace-and-hulls/W-water`; `lightstyles/L-illum` | K1–K5, P1–P12, 5, W, L | Scenario files. Line 3 `{"floor","radius","height"}`: a level world floor spot (probe placements are relative to it). Per scenario `{"scenario","name","frames","t"}` (or `{"scenario","skipped"}`), setup lines `{"sc",…}` (platforms, pushers, schedules, water spots, walk-move and drop-to-floor results), `{"sc","ents":[ent…]}`, then one line per server frame `{"sc","f","t","ft","e":[{"i","o","a","v","av","bv","fl","ge","lt","nt","wl","wt","solid","mt","mins","maxs","absmin","absmax"}…]}` (state after `f` physics frames), and events with `"case"` = scenario: `{"ev":"touch"…}`, `{"ev":"think","t","ft","self","ltime","nt","o","v"}` (time as the engine set it for the think), `{"ev":"blocked","self","other","ltime"}`, `{"ev":"use","self","target","caller","type"}`, `{"ev":"schedule"|"set_nextthink"|"pusher_start"|"push"|"fire"…}`. Platforms, pushers and blockers are probe copies of map brush models that fill their bounds (MOVETYPE_PUSH: the engine refuses SOLID_BSP without it). K-think changes `host_framerate` (1/30, 0.1) for some scenarios; the runner restores it. W-water spawns a `func_water` through the game DLL (none in these maps). L-illum samples `pfnGetEntityIllum` at 48 floor points every frame. |

## Limits

- Server-side only: client prediction, rendering and sound are oracle 2.
- pm_shared runs from this DLL, built from the HLSDK with Valve's flags;
  it is the same source as retail `hl.so`, not the same binary.
- Map monsters are removed and map triggers disabled, so hl64 replays
  must do the same (lists in `meta`).
- A5 needs a map brush model that does not fill its bounds: found in the
  Uplink maps (an angled `trigger_once`), not in the Hazard Course.
- Reruns with the same capture list are byte-identical. A different list
  (e.g. `run.sh PM-bot` alone) shifts the game time (`t`, `pmt`) and the
  probe entities' indices, because each capture starts where the previous
  one ended; positions, velocities and flags stay the same. The engine's
  random stream also carries over: `human_grunt` `skin` in T-hitbox
  differs when M3 captures share the server (hence separate batches).
- PM-bot writes `{"run","ents":[ent…]}` before a frame whenever a non-brush
  physent (an item, a pushable box) first appears in the run or changed.
  At its start (the instant of its `brush_ents` line) PM-bot freezes every
  map pusher (doors, plats, buttons, trains, rotating brushes: velocity,
  avelocity and nextthink cleared; `{"frozen_movers":[[i,class,was_moving]…]}`)
  and the probe swallows their touches until it ends, so `brush_ents`
  holds for every run (an earlier run can no longer open a door).
- M3 scenarios that need a feature the map lacks are skipped and say so:
  water (only hldemo3, t0a0, t0a0b, t0a0b1, t0a0b2 have world water), map
  `func_pushable`s, a brush model whose rotation sweep fits a floor spot.
