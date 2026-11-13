# Game clock, think timing, active sets, entity lifetime

Behaviour: when thinks run and what time they see, as in GoldSrc, to the
frame. Mechanism: a GoldSrc-shaped game clock (double seconds, float
`nextthink`), a think heap and active sets instead of a per-frame scan of
every entity. Conventions and oracles: [README.md](README.md); frame
order: architecture.md 3.2. GoldSrc probe captures: `$HL64_FIXTURES/
goldsrc/think-and-time/K-*` (Oracle2, hl64 `d2bf3d8`, 10 maps, frame time
0.01 and 1/30).

## 1. Clock

The frame on which a think runs depends on float rounding, so the game
clock keeps GoldSrc's number formats; integer microseconds remain only for
engine-internal timing that no game code observes (I/O, pacing,
residency, profiling).

- `time`: **f64 seconds**, advanced by adding `dt` (f64) once per frame,
  starting at **1.0** at New Game **[probe:measured K-settle]**. The game
  reads it as f32 (HLSDK `gpGlobals->time`).
- `dt` (frame time): f64 seconds = elapsed vblanks × VI period, clamped to
  [1/60, 0.1] s on the cart (D6 i; GoldSrc allows 0.25 s — only frames
  under 10 fps differ); `host_fixed_dt` in test builds (1/30 s; 0.01 s
  when replaying probe fixtures) so host and cart advance identically.
- `nextthink` and pusher `ltime`: **f32**, exactly the values the game
  wrote (HLSDK entvars are float).
- Every engine→game call sees the frame's time, except a think or a logic
  timer callback, which sees its own due time (2, 5). `time` advances by
  `dt` after the active-set pass.
- No clock jump at seams; the clock is saved.

## 2. Think rule (every movetype except PUSH)

An entity thinks at most once per frame, at the point of its movetype step
(physics-movetypes.md):

- not due if nextthink ≤ 0, or if `(f64) nextthink > time + dt`
  **evaluated in double**, as GoldSrc evaluates it;
- due otherwise: nextthink := 0, then the think runs with game time =
  `(f32) max(nextthink, time)`;
- a think that schedules itself due again runs next frame (one think per
  frame); a lower-index entity that sets another entity's nextthink to a
  value due this frame makes it think **this** frame
  **[probe:measured K-think]**;
- freeing the entity ends its step.

Rounding is observable and reproduced, not smoothed: game code writes
`nextthink = time + delay` in float, so `float(t + ft)` set by a
lower-index entity ran the **next** frame at frame time 0.01 (the float sum
rounded above `t + ft` in double) and the **same** frame at 1/30 (rounded
below) **[probe:measured K-think]**. Scheduled times are kept, not rounded
to frames (a think rescheduling itself at +0.12 s with dt 1/30 runs at
frames 0, 3, 7, 10, 14 and sees t0 + 0.12 k in float sums)
**[probe:measured K-think]**; a think whose due time passed during a long
frame sees the frame time **[probe:measured K-think]**.

## 3. Active sets (mechanism)

- **Think heap**: 4-ary min-heap of (nextthink f32 bit pattern, entity
  index), ≤ 512 entries (positive floats order like their bit patterns);
  setting or clearing nextthink updates the entry. Popping uses the same
  double comparison as rule 2, so the heap decides exactly the frames
  GoldSrc decides.
- **Movers bitset** (512 bits): set while the entity's movetype has motion
  to do (velocity or avelocity non-zero, falling types not on ground,
  BOUNCE sliding on the ground, pushers with velocity/avelocity, FOLLOW,
  PUSHSTEP not at rest); engine setters for velocity, flags, origin,
  groundentity and movetype keep it current; an entity leaves the set when
  its step ends at rest.
- Per frame: pop every due heap entry into a **due** bitset; walk
  `due | movers` word by word in ascending index and run each entity's
  movetype step. An entity created or made due during the walk at a higher
  index joins this frame, as GoldSrc's ascending-index loop would visit it
  (rule 2, lower-index case).
- Equivalence with GoldSrc's "visit every entity" loop: an entity that is
  neither due nor moving does nothing in its step; skipping it changes
  nothing (K9 checks this on full routes).

## 4. Pusher think
Pushers think on their local clock (physics-movetypes.md 4.1): due when
nextthink lies in (ltime before the move, ltime after]; the think sees the
frame time; a blocked pusher's ltime stalls and its think does not run.
Doors, plats and trains schedule `ltime + travel time` (HLSDK
`CBaseToggle::LinearMove`); a door's ltime ends exactly at its nextthink
(0.78125) **[probe:measured K-pusher]**.

## 5. Logic timers
Edict-less records (multi_manager steps, delayed fires, logic thinks;
architecture.md 6.4) are HLSDK entity thinks in GoldSrc, and their timing
is that of HLSDK's float arithmetic. hl64 reproduces it exactly:

- each timer stores the f32 `nextthink` the HLSDK code would have written
  and becomes due by rule 2 (double compare against `time + dt`);
- the callback runs the HLSDK comparison itself — a multi_manager fires a
  target when `(f32)(time − start) ≥ delay` in float at one of its think
  times, then schedules `start + next delay` as f32. Measured: a 0.33 s
  target fired at +0.34 s at frame time 0.01 (at the +0.33 think the float
  difference was just under 0.33) and at +0.333 s at 1/30
  **[probe:measured K-think]**. Timers must not be "simplified" to exact
  due times;
- due timers run at `game_start_frame` in due order (ties: insertion
  order), seeing `(f32) max(due, time)`; timers scheduled by a callback that
  are already due run in the same pass (≤ 1024 callbacks per frame, logged
  if reached);
- a closing map's timers freeze by remaining time (activator dropped) and
  are re-queued at time + remaining when it reopens (logic thinks and
  multi_manager steps when their record rehydrates);
- 128 entries; a full heap is fatal, naming map and record.

## 6. Entity lifetime

- Slot 0 world, slot 1 player, game entities from slot 2; cap 495.
- **Allocate**: lowest free slot ≥ 2 that was freed before time 2.0 s or at
  least 0.5 s ago; else the next never-used slot **[probe:measured
  K-alloc]** (90/90 allocations). The new entity's components are zeroed
  and a creation serial assigned (area-links-and-touch.md 3.1); the slot's
  serial is unchanged (it was incremented when the slot was freed): a
  reused slot reports the serial and free time of its last free
  **[probe:measured K-alloc]**. Slots taken by map entities between
  probe allocations follow the same rule (hldemo2: 5 of 5).
- Reserves: optional hydration only while ≥ 48 slots are free; transient
  effects (gibs, debris, shells) only while ≥ 32 are free (otherwise
  skipped); game allocations may use every slot; exhaustion is fatal with a
  class census.
- **Free**: unlink, remove from the heap and active sets, zero components
  (keep serial and free time), serial += 1; free time = the frame's time
  (f32). References held elsewhere are validated through handles.
  `FL_KILLME` (HLSDK `UTIL_Remove`) frees the entity when the active-set
  pass reaches its slot, after its step (set at StartFrame: freed in that
  frame's pass with that frame's time **[probe:measured K-alloc]**).
- **New Game settle**: after the start map's records spawn and precache at
  time 1.0, the first two frames run `game_start_frame` and the active-set
  pass with **time 1.0 and frame time 0** (not 0.1): nothing moves under
  gravity in them, thinks due at ≤ 1.0 run **[probe:measured K-settle]**.
  The clock starts advancing on the third frame. Hydration later gets no
  settle frames.

## 7. Tests

| # | property | data |
|---|---|---|
| K1 | +0.12 s self-rescheduling think at dt 1/30: frames 0, 3, 7, 10, 14; times t0 + 0.12 k (f32 sums) | **[probe:measured K-think]** + host |
| K2 | own think → next frame; set by a lower-index entity → this frame when due in the double compare; `float(t + ft)` set by a lower-index entity runs next frame at dt 0.01 and this frame at 1/30; nextthink ≤ 0 never | **[probe:measured K-think]** |
| K3 | a think whose due time passed during a long frame sees the frame time | **[probe:measured K-think]** |
| K4 | pusher thinks on ltime (door ltime = nextthink 0.78125 on arrival); blocked pusher's ltime stalls, no think | **[probe:measured K-pusher]** |
| K5 | logic timers reproduce HLSDK float arithmetic: multi_manager 0.33 s target at +0.34 (dt 0.01) and +0.333 (dt 1/30); order, cascade, time seen; freeze/thaw keep remaining time | **[probe:measured K-think]** + synthetic (freeze/thaw) |
| K6 | allocation: reuse delay, lowest index, serial increments | **[probe:measured K-alloc]** (90/90) |
| K7 | reserves: hydration stops at 48 free, transients at 32 | synthetic |
| K8 | settle: the first two frames after New Game have time 1.0 and frame time 0; the clock advances from the third | **[probe:measured K-settle]** |
| K9 | active set: identical results with a debug mode that steps every live entity every frame (route checksum of all components at each stats step) | host route |
| K10 | host and cart clocks bit-identical on a fixed-dt route | route stats |

Host runner: `test/m3/ent.py --bin BUILD/host/hl64-ent` (`make enttest`)
replays K-think scenarios 0–7 (K1–K3; 8–9 are K5), K-alloc (K6, slot
indices compressed in order to fit the 495 cap) and K-settle (K8) on the
real engine/ent code, and runs K7, K9 (a randomised NONE / NOCLIP /
FOLLOW world with thinks, spawns, removes and cross-entity nextthinks,
checksummed every frame with and without `phy_set_step_all`, plus a
control that bypasses the setters and must differ), the heap / active-set
unit test and P13's resting counters. K9 on full game routes waits for the
game's entities (M3 gate route).
