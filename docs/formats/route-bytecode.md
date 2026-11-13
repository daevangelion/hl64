# Route sources and compiled route bytecode (`.rbc`), version 1

Test routes drive the game from a script at frame boundaries (architecture.md
15). Sources stay text (`test/routes/*.route`, the old route syntax);
`tools/routec` compiles them against `campaign.rtb`
([rtb-format.md](rtb-format.md)) into `.rbc` bytecode run by the route VM
(`engine/test`, frame loop step 13). Every name is resolved at compile
time, so a renamed entity fails the compile, not the run.

```
python3 -m tools.routec.routec --rtb campaign.rtb X.route -o X.rbc
python3 -m tools.routec.routec --rtb campaign.rtb --dump X.rbc   # validate + list
```

## File

Big-endian, read in place; CRC-32 is the zlib polynomial.

| off | field | rule |
|---:|---|---|
| 0 | `magic` u32 | `0x52424331` (`RBC1`) |
| 4 | `version` u16 | 1 |
| 6 | `flags` u16 | 0 |
| 8 | `rtb_id` u32 | `build_id` of the `campaign.rtb` it was compiled against; the VM refuses a route whose `rtb_id` differs from the loaded table |
| 12 | `file_size` u32 | equals the file length |
| 16 | `crc` u32 | CRC-32 of bytes `[48, file_size)` |
| 20 | `nops` u16 | 1–65535 |
| 22 | reserved u16 | 0 |
| 24 | `code_ofs` u32 | 48 |
| 28 | `code_size` u32 | multiple of 4 |
| 32 | `lines_ofs` u32 | `code_ofs + code_size` |
| 36 | `str_ofs` u32 | `lines_ofs + align4(2 · nops)` |
| 40 | `str_size` u32 | `str_ofs + str_size = file_size` |
| 44 | `src` u16 | string offset of the source file name |
| 46 | reserved u16 | 0 |

- **code**: `nops` ops back to back (below).
- **lines**: u16 source line per op (≥ 1), zero padding to 4: the debug
  table behind `route FAIL` messages.
- **strings**: NUL-terminated printable ASCII (no spaces): the source name
  and the `SHOT` / `STATS` labels (≤ 47 characters).

## Ops

Every op is one header word — `op` u8, `a` u8, `b` u16 — followed by the
op's fixed number of 32-bit operand words (u32, or IEEE f32 marked *f*).
Times are game-clock microseconds. Positions are **map-local** to the op's
map (the VM adds the map's CWG placement `g_M`; architecture.md 5.4).

| op | name | a | b | words | meaning |
|---:|---|---|---|---|---|
| 0x01 | `WAIT` | 0 | 0 | `us` | done when `us` of game time have passed since the op started |
| 0x02 | `WAIT_MAP` | 0 | map | `timeout` | the player's home map is `map` |
| 0x03 | `WAIT_NEAR` | 0 | map | x, y, z *f*, r *f* (≥ 0), `timeout` | home map is `map` and the player origin is within `r` (3D distance) of the point |
| 0x04 | `WAIT_MOVED` | 0 | name id | `timeout` | an entity with the name (`TARG`) is moving (velocity or avelocity non-zero) or its origin / angles differ from its record's spawn values; zero angles never count (HLSDK `SetMovedir` zeroes a linear mover's angles at spawn: a closed `func_door` with `angle 90` rests at 0 0 0) |
| 0x05 | `WAIT_GONE` | 0 | name id | `timeout` | every record carrying the name is killed or removed, or its live entity is not solid (`SOLID_NOT`: GoldSrc keeps body-less entities sharing a breakable's name — `ambient_generic`, `env_sprite`, a toggled-off trigger — for good; a pending record is not gone), and for a spawned name every spawner has spawned and none of its children with the name is alive |
| 0x06 | `WAIT_CLASS` | 0 | map | class id, x, y, z *f*, r *f*, `timeout` | a live, solid entity of the class lies within `r` of the point in `map` |
| 0x07 | `WAIT_GLOBAL` | state (0 off, 1 on, 2 dead) | global id | `timeout` | the global state (HLSDK `GLOBALESTATE`) has that value |
| 0x08 | `WAIT_END` | 0 | 0 | `timeout` | the campaign's end section has fired (`trigger_endsection` / end of demo) |
| 0x09 | `LOOK` | 0 | 0 | pitch *f*, yaw *f* | set the view angles (roll 0) |
| 0x0A | `LOOK_AT` | 0 name, 1 class | name or class id | map | aim the view at the centre of the nearest live entity of the name / class in `map`; fails if there is none |
| 0x0B | `SETPOS` | 1 = angles given | map | x, y, z, pitch, yaw *f* | test builds only (a debug teleport; a build without `HL64_TEST` rejects the route): move the player there with zero velocity (and set the view angles when `a` = 1); fails unless the home map is `map` |
| 0x0C | `BUTTONS` | 0 | down mask | up mask | held route buttons = (held \| down) & ~up; masks are HLSDK `IN_*` bits, disjoint, bit 6 unused |
| 0x0D | `IMPULSE` | impulse | 0 | — | the next usercmd carries the impulse |
| 0x0E | `DROP` | 0 | map | class id, x, y, z *f* | the game creates a runtime entity of the class there (HLSDK `CBaseEntity::Create` path) |
| 0x0F | `GOD` | 0 / 1 | 0 | — | debug invulnerability off / on (test builds) |
| 0x10 | `SHOT` | 0 | label | — | request a screenshot tagged with the label (ares harness dump / host recorder) |
| 0x11 | `STATS` | 0 | label | — | print one stats record line tagged with the label |
| 0x12 | `PASS` | 0 | 0 | — | the route passed: log `route PASS`, harness exit 0; runs in the frame after it is reached, so the `STATS` / `SHOT` commands queued just before it still run; a PROFILE build queues the `mem` command when it is reached (it runs with those: the gates' memory tables read it) |
| 0x13 | `START` | 0 | map | — | test builds only, first op only: boot runs New Game on `map` instead of the campaign start (its records, the player at its first `info_player_start`; the free camera's start layer is closed); the VM passes over it. A build without `HL64_TEST` rejects the route |

Ids: map = CWG map index, name id = `TARG` index (its map is the
entity's map), class id = `classes.def` line, global id = `GLOB` index.

Wait ops (`WAIT_*`, 0x02–0x08) have a timeout: the op fails if it is not
satisfied when more than `timeout` µs have passed since it started;
`timeout` 0 means it must hold when first checked.

## VM

- Runs once per frame after the frame's simulation and drawing (frame loop
  step 13). It executes ops in order until a wait op is not yet satisfied;
  every other op except `PASS` completes in the frame it is reached.
  `BUTTONS`, `LOOK`, `SETPOS` and `IMPULSE` take effect in the next
  frame's usercmd / player state; `STATS` and `SHOT` run at the next
  frame boundary.
- Entity and record ops never hydrate a pending record: a name's entities
  are its records' live entities (and, for a spawned name, the runtime
  holders once no record of the name is pending). A monstermaker "has
  spawned" (`WAIT_GONE`) when its record is killed or its entity has
  neither think nor use left (HLSDK `CMonsterMaker::MakeMonster` clears
  both when its count runs out). `WAIT_CLASS` and `LOOK_AT` measure from
  the entity's origin / box centre; `DROP` spawns at angles 0.
- Logs `route: <op#> <mnemonic> [label]` per op; a failure logs
  `route FAIL <op#> <source>:<line> <mnemonic> <reason>` (line from the
  debug table) and ends the run with a non-zero harness exit.
- Residency: the VM pins the destinations of the next `SETPOS` ops as
  route look-ahead pins (architecture.md 5.3). A `SETPOS` opens a
  30-frame teleport window (engine/io `io_tele_window`): its frames'
  synchronous reads stay blocked frame time; a frame > 1 ms in it counts
  as `blk1ms_tele=` on the `stats N stream:` line instead of `blk1ms=`.
- Load: the reader rules below; `rtb_id` must equal the loaded table's
  `build_id`.

## Validation (reader)

`tools/routec/rbc.py` and the VM loader reject a file unless: header fields
as above; `file_size` = bytes present; CRC matches; sections exactly where
the header says; every op has a known opcode and its operand words lie
inside the code; the ops end exactly at `code_size`; floats are finite and
radii ≥ 0; map / name / class / global ids lie inside the rtb's counts;
`LOOK_AT` by name names an entity of the given map; `SETPOS.a` ≤ 1,
`GOD.a` ≤ 1, `WAIT_GLOBAL.a` ≤ 2, other unused `a` / `b` fields are 0;
`BUTTONS` masks only use `IN_*` bits and do not overlap; label offsets are
string starts with printable ASCII ≤ 47 characters; every line entry ≥ 1
and the padding is 0; `START` only as the first op; `PASS` occurs exactly
once, as the last op.

## Source language (`.route`)

One step per line; `#` starts a comment (comments may hold any UTF-8; the
rest of a line must be ASCII). Numbers are decimal (`-12`, `0.3`, `1e3`);
times are seconds (0–4000). The current map is the one named by the latest
`waitmap` (or `startmap`): map-local positions and entity names refer to it,
and steps that need it fail to compile before the first `waitmap`.

| step | op |
|---|---|
| `startmap <map>` | `START` (first step only; sets the current map) |
| `wait <s>` | `WAIT` |
| `waitmap <map> <timeout>` | `WAIT_MAP` (sets the current map) |
| `waitnear <x> <y> <z> <r> <timeout>` | `WAIT_NEAR` |
| `waitmoved <name> <timeout>` | `WAIT_MOVED` |
| `waitgone <name> <timeout>` | `WAIT_GONE` |
| `waitclass <class> <x> <y> <z> <r> <timeout>` | `WAIT_CLASS` |
| `waitglobal <global> <timeout> [on\|off\|dead]` | `WAIT_GLOBAL` (default on) |
| `waitend <timeout>` | `WAIT_END` |
| `look <pitch> <yaw>` | `LOOK` |
| `lookat <name\|class>` | `LOOK_AT` (an entity name of the current map first, else a class) |
| `setpos <x> <y> <z> [<pitch> <yaw>]` | `SETPOS` |
| `+<button>` / `-<button>` | `BUTTONS`: `attack jump duck forward back use left right moveleft moveright attack2 speed reload` |
| `impulse <0-255>` | `IMPULSE` |
| `drop <class> <x> <y> <z>` | `DROP` |
| `god 0\|1` | `GOD` |
| `shot <label>` / `stats <label>` | `SHOT` / `STATS` |
| `pass` | `PASS` (once, last) |

Anything else is a compile error with `<file>:<line>`.

### Conversion of the old routes

`test/routes/uplink.route` is the old `n64/port/test/uplink_route.cfg` and
`test/routes/hazard.route` the old generated Hazard walk
(`out/hl-walk-t0a0_.cfg`), both audit CLEAN data. Every verb they use maps
to an op except one:

| old | new | note |
|---|---|---|
| `wait`, `waitmap`, `waitnear`, `waitmoved`, `waitgone`, `waitclass`, `waitglobal`, `waitend`, `look`, `lookat`, `setpos`, `+/-forward back moveleft moveright use attack duck`, `impulse`, `god 0/1`, `shot`, `stats`, `pass` | same | unchanged |
| `hl_drop` | `drop` | renamed |
| `god` (no argument; Hazard) | `god 1` | the generator's rule "set, never toggle" |
| `weapon_9mmar` | `weapon_9mmAR` | the classname as the maps and HLSDK spell it |
| `hl_trace 1` (Hazard) | — | **dropped**: an old-engine console switch that logged entity fires; not a route action (entity-fire logging is a debug log category) |

Other old route files (`dry_named_route.cfg`, `npc_anim_route.cfg`,
`hl_material_route.cfg`) used console passthrough (`ent_fire`, `map`,
`r_profile`, `zone_dry`, `hl_material`, `@…`) and are not converted.

Uplink: 535 ops (8,450 B); Hazard: 534 ops (7,996 B), compiled against the
M0 scratch table of the player's Uplink/Hazard entity lumps.
