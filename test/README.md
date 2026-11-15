# hl64 tests

| path | what |
|---|---|
| `test/run/m0.sh` .. `m5.sh`, `coll.sh` | milestone gates (host, `--ares`); `m5.sh` runs the whole Uplink / Hazard / m4 routes |
| `test/run/scene.sh` | the scene runner (below) |
| `test/run/menu.sh` | the front end on ares, by hand: a playable ROM walked by the pad (architecture.md 13) |
| `test/routes/*.route` | whole routes ([route-bytecode.md](../docs/formats/route-bytecode.md)) |
| `test/scenes/*.route` | short scenes (below); `test/scenes/table.py` their perf table |
| `test/m1` .. `test/m5`, `test/m6`, `test/coll` | per-milestone suites and log checks the gates run (`test/m6/menu_host.sh`: a suite of `m5.sh`) |
| `test/oracle` | GoldSrc oracle (dirty side, [README](oracle/README.md)) |

## Scenes: check a change in minutes

A whole Uplink route on ares is ~1 h of wall time on this machine (plus the
queue for the ares lock), the m4 and Hazard routes as much again. Check a
change on the scenes it touches instead; the whole routes are the final gate
of a merge set (`test/run/m5.sh --ares`), not the iteration loop.

A scene is a route of one map visit or one seam switch: `startmap` at its
map, a short prelude that puts the player and the world where the whole
route has them (setpos; `drop item_suit` / weapons for what the route's
player carries; the triggers the route fires before the span), then the
whole route's own lines for the span, **unchanged**, between `stats begin`
and `stats end`: that window is what the table measures. A
`# scene-window: <route> <from> <to>` line names the same span in the whole
route (SHOT / STATS labels, or route op numbers of the current
`test/routes/<route>.route`), so a whole-route log gives the same window to
compare; `# scene-part: <name> <from> <to>` adds a sub-window by labels.
Scenes run New Game on their map: state the route built on earlier visits
(persisted level state, the open neighbour behind a seam) is not there; the
scene's header says what differs.

Game seconds are the whole scene (prelude + window) at the fixed 1/30 s
frame; wall seconds are one ares run on ares-host (`HL64_ARES_HOST=ares-host`, all
17 scenes 8 at a time: 18 min for the set, ROM builds included) and, for a
few, on build-vm (load 8-23, one at a time), measured on main
a09abb2 + m5-remote (ares-host) and 30f8f01 (build-vm).

| scene | map | what | window (uplink.route) | game s | ares-host s | build-vm s |
|---|---|---|---|---:|---:|---:|
| `hl1-start` | hldemo1 | intro room until the teleport, start corridor | op 1 .. SHOT start | 26 | 34 | |
| `hl1-dome` | hldemo1 | dome trigger, scientists and barney's scripted talk | SHOT start .. SHOT dome-scene | 32 | 26 | 529 |
| `seam-1to2` | hldemo1 -> 2 | elevator button, doors, USEONLY switch, hldemo2 arrival (also the hl2 arrival scene) | SHOT elevator .. SHOT hldemo2-arrival | 59 | 58 | |
| `hl2-soda` | hldemo2 | soda room: crowbar, soda machine bust, valve, pipe duct crawl | SHOT hldemo2-arrival .. SHOT duct-dark | 20 | 29 | |
| `hl2-duct-flashlight` | hldemo2 | dark duct, HEV flashlight (night vision, D12); part `hold`: 8 s held | SHOT duct-dark .. SHOT vent-flashlight | 21 | 45 | |
| `hl2-yard` | hldemo2 | vent, shotgun room, barney, zombie, tripmines, warehouse gate grunt, crane, yard drop, fallback squad (prelude: hl2-soda + the duct crawl) | SHOT vent-flashlight .. SHOT grunts-fight | 70 | 92 | 1474-1539 (53 s version) |
| `fx-tripmine` | hldemo2 | zombie crates, barrier, tripmine chain explosions | ops 105 .. 124 | 12 | 11 | 313 |
| `seam-2to3` | hldemo2 -> 3 | east yard (sci_execution), dome road, touch switch, hldemo3 arrival | SHOT grunts-fight .. SHOT hldemo3-arrival | 27 | 39 | |
| `hl3-arrival` | hldemo3 | arrival corridor, hellbox grunt, detonator explosion, dome door | ops 203 .. 226 | 15 | 21 | |
| `hl3-welder` | hldemo3 | dish wheel / button, rappel grunts, domelift, welder grunts, steam valve | SHOT dish-activated .. op 298 | 57 | 69 | |
| `seam-3to2` | hldemo3 -> 2 | exit tunnel, touch switch, hldemo2 return crawl | ops 367 .. 393 | 9 | 7 | |
| `hl2-return-battle` | hldemo2 | second battle, warehouse, crate wall shot down (the route's AI-heaviest frames) | SHOT hldemo2-return .. SHOT elevator-return | 25 | 64 | |
| `seam-2to1` | hldemo2 -> 1 | elevator back, USEONLY switch, hldemo1 return until the doors open | SHOT elevator-return .. op 479 | 16 | 18 | |
| `hl1-garg` | hldemo1 | glimpse, cave-in, vent, garg_vent_break, the garg kills garg_sci_1 | op 512 .. SHOT garg-look | 37 | 50 | |
| `hl1-garg-end` | hldemo1 | garg chain, rollcar, ending, end section, restart (the one long scene) | SHOT garg-look .. op 534 | 103 | 286 (a09abb2: FAIL, the garg stalls after kickcar on ares) | |
| `hazard-seam` | t0a0c <-> t0a0d | Hazard touch switch c -> d and back d -> c | (hazard.route 538-552, then back) | 9 | 7 | 161 |
| `addglow` | hldemo1 | additive env_glow poses, TransTexture strips, MP5 grenade smoke ([addblend.route](routes/addblend.route)'s poses) | whole scene | 14 | 20 | 482 |
| `alpha-edge` | hldemo2 | '{' fence textures close up: shotgun-room door #659 window ({fence2), east walkway railing #230 ({rail2); shots door-near/far, rail-near/far (blue alpha-edge fringes) | whole scene (off-route poses) | 3 | 5 | |

Deviations from the whole route inside a window: `hl2-return-battle` puts
the player past the tripmine corridor (its barriers stand on New Game) and
opens the elevator with the outside button; `hl1-garg` starts the vent
crawl at x 1200; `seam-2to1` opens the car with the outside button before
the window.

### Does a scene reproduce the route?

`hl2-yard` against the same window (SHOT vent-flashlight .. SHOT
grunts-fight, 1465 frames) of whole-route ares runs (logs
`/scratch/hl64/builds/M6Render2/{main,b1}/ares.log`), scene run on the
same trees and cook (c10), ares on ares-host:

| tree | route avg / p90 ms | scene avg / p90 ms | route > 50 ms | scene > 50 ms |
|---|---|---|---:|---:|
| main ec821b9 | 61.6 / 91.8 | 61.1 / 91.2 | 752 | 720 |
| m6-render edeec4b | 55.0 / 82.2 | 54.6 / 82.3 | 534 | 528 |
| delta | -6.6 / -9.6 | -6.5 / -8.9 | -218 | -192 |

Every zone's average is within 0.6 ms of the route's on both trees (studio
14.2 vs 14.5, fx 10.5 vs 11.2, world 8.3 vs 8.4, mon 6.0 vs 6.1, ...), and
the change's zone deltas match (studio -2.4 / -2.4, fx -4.3 / -4.3, fx.decal
-3.8 / -3.9); maxima differ more (one seam frame of 22-29 ms: New Game on
hldemo2 opens and closes hldemo1 behind the elevator, the route had it
open). Two builds of the same tree whose ROMs differ only in the route's
strings measured 61.1 / 89.6 and 61.1 / 91.2 (701 / 720 frames > 50 ms):
that is the noise. The scene first put the player straight into the duct
end with the flashlight on: +3.0 ms (world +2.1 in the window's first 4 s,
the HEV logon's sound); a prelude that replays the route's own approach is
what makes a scene match. ares results are the same on build-vm and ares-host
(identical tables for the same ROM).

### Runner

```
test/run/scene.sh [--ares|--host] [--profile] [--psamp] [--before TREE]
                  [--route-log R=LOG ...] [--keep-rom] [--cmd CMD ...] [-j N] SCENE... | all
```

- host (default): `hl64-host` per scene at a fixed 1/30 s, `-j N` in
  parallel, seconds per scene: route PASS (does the scene still run on this
  tree). `--profile` adds the table from host CPU roll-ups (1 s of host
  time each: short windows may have none; not the cart's numbers).
- `--ares`: one `HL64_TEST PROFILE=1` ROM per scene (the ELF is built once,
  a scene re-packs the DFS with its `route.rbc`), each run through
  `test/run/ares.sh` `ares_run`: with `HL64_ARES_HOST=ares-host` on ares-host (8 at
  a time there; use it), else locally under the shared slot locks
  `/tmp/hl64-ares.slot*`; `-j N` scenes at a time; the emulator's wall time
  in `wall.txt` (slot wait and copies excluded). `--psamp`: a `PSAMP=1` ROM
  and `test/m6/psamp.py` per scene (trees that have them).
- `--before TREE`: also build another worktree into `$BUILD_DIR/before` and
  run the same scenes there (this tree's scene sources, compiled against
  `BEFORE_CONTENT`'s rtb when the two trees need different cooks), before
  and after alternating per scene; the table shows both and the delta.
- `--route-log uplink=<ares-uplink.log>`: beside each scene's window, the
  same window of a whole-route PROFILE log (`# scene-window`), to see that
  the scene reproduces the route.
- `--cmd "[@F] CMD"`: one more debug command (hl64-host `--cmd` syntax) for
  every scene, e.g. `--cmd "@2 set brightness 50"` to compare a setting's
  cost; repeatable.
- Env: `BUILD_DIR`, `HL64_CONTENT` (default the main cook
  `/scratch/hl64/tmp/Int6/c17`), `BEFORE_CONTENT`, `HL64_ARES_HOST`,
  `ARES_TIMEOUT` (per scene, 1800 s), `ARES_BIN` / `ARES_SLOTS` (local),
  `MAKE_JOBS`.
- Out: `$BUILD_DIR/scenes/<host|ares>/<scene>/` (`ares.log` / `host.log`,
  `table.md`, `wall.txt`, `shots/NN-<label>.png`, `psamp.md`) and
  `summary.md` (one row per scene and tree: result, game seconds, window
  frames, frame avg / p90 / max, frames > 50 ms, mon-dominant and
  AI-pushed frames, wall time).

`test/scenes/table.py LOG[@FROM:TO] [LOG2[@FROM:TO]]` makes the table of
any PROFILE log (a scene's `stats begin` .. `stats end` window, or FROM / TO
op numbers or SHOT / STATS labels of a whole-route log), one log or two
side by side: frame avg / p90 (of the 1 s roll-up means) / max (slowest
`seamprof: slow` frame), frames > 50 ms (pacing histograms), `seamprof:
slow` frames, mon-dominant (mon >= half the frame's CPU, the M5 AI gate) and
AI-pushed frames, and avg / max per zone.

Examples:

```
# does my change still run every scene (host, ~4 min for all)
BUILD_DIR=/scratch/hl64/builds/Me/sc test/run/scene.sh all
# cart numbers of the scenes a render change touches, against main (ares-host)
HL64_ARES_HOST=ares-host BUILD_DIR=/scratch/hl64/builds/Me/sc test/run/scene.sh --ares -j 8 \
    --before /scratch/hl64/wt/<main worktree> hl2-yard hl2-duct-flashlight addglow
# a scene against the same window of a whole-route run
test/scenes/table.py <ares-uplink.log>@vent-flashlight:grunts-fight \
    /scratch/hl64/builds/Me/sc/scenes/ares/hl2-yard/ares.log --label route --label scene
```
