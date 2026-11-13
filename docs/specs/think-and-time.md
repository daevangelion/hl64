# Game clock, think timing, active sets, entity lifetime

Behaviour: when thinks run and what time they see, as in GoldSrc.
Mechanism: integer clock, think heap and active sets instead of a
per-frame scan of every entity. Conventions and oracles:
[README.md](README.md); frame order: architecture.md 3.2.

## 1. Clock

- `now`: u64 microseconds of game time, starting at 1 000 000 (1.0 s) at New
  Game **[probe K-settle]**. The game's float `time` = now · 10⁻⁶; float
  delays convert once to µs (round to nearest).
- `dt`: elapsed vblanks × VI period, clamped to [1/60, 0.1] s on the cart
  (D6 i; GoldSrc allows 0.25 s — only frames under 10 fps differ);
  `host_fixed_dt` (default 1/30 s) in test builds so host and cart routes
  advance identically.
- Every engine→game call during a frame sees the frame's `now`, except a
  think or timer callback, which sees its own due time (2, 5). `now`
  advances by `dt` after the active-set pass.
- No clock jump at seams; the clock is saved.

## 2. Think rule (every movetype except PUSH)

An entity thinks at most once per frame, at the point of its movetype step
(physics-movetypes.md):

- not due if nextthink is unset (0) or `nextthink > now + dt`;
- due otherwise: nextthink := unset, then the think runs with time =
  max(nextthink, now) — so it can run in the frame *before* the clock
  passes its due time and see that (future) scheduled time; a think
  scheduled in the past sees the frame time;
- a think that schedules itself due again by `now + dt` runs next frame;
- freeing the entity ends its step.

Scheduled times are kept, not rounded to frames: a think rescheduling
itself at +0.12 s from frame 0 with dt = 1/30 runs at frames 0, 3, 7, 10,
14 … and sees t0 + 0.12 k. **[probe K-think]** (GoldSrc's float
`nextthink ≤ time + frametime` rule, reproduced in integers.)

## 3. Active sets (mechanism)

- **Think heap**: 4-ary min-heap of (nextthink µs, entity index), ≤ 512
  entries; setting or clearing nextthink updates the entry.
- **Movers bitset** (512 bits): set while the entity's movetype has motion
  to do (velocity or avelocity non-zero, falling types not on ground,
  pushers with velocity/avelocity, FOLLOW, PUSHSTEP not at rest); engine
  setters for velocity, flags, origin, groundentity and movetype keep it
  current; an entity leaves the set when its step ends at rest.
- Per frame: pop every heap entry due by `now + dt` into a **due** bitset;
  walk `due | movers` word by word in ascending index and run each
  entity's movetype step. An entity created during the walk joins if its
  index is above the walk position, as GoldSrc's ascending-index loop
  would visit it.
- Observable equivalence with GoldSrc's "visit every entity" loop: an
  entity that is neither due nor moving would do nothing in its step;
  skipping it changes nothing.

## 4. Pusher think
Pushers think on their local clock (physics-movetypes.md 4.1): due when
nextthink lies in (ltime before the move, ltime after]; the think sees the
frame time; a blocked pusher's think waits. Doors, plats and trains
schedule `ltime + travel time` (HLSDK `CBaseToggle::LinearMove`).
**[probe K-pusher]**

## 5. Logic timers
Edict-less records (multi_manager steps, delayed fires, logic thinks;
architecture.md 6.4) use one timer heap of 128 entries:

- at `game_start_frame` every timer due by `now + dt` runs in due order
  (ties: insertion order), seeing max(due, now); timers scheduled by a
  callback that are already due run in the same pass (≤ 1024 callbacks per
  frame, logged if reached);
- a closing map's timers freeze by remaining time (activator dropped) and
  are re-queued at now + remaining when it reopens (logic thinks and
  multi_manager steps when their record rehydrates);
- a full heap is fatal, naming map and record.

In GoldSrc these are entity thinks of `multi_manager` / `DelayedUse`
entities; the observable timing is the same as rule 2 **[probe K-think]**.

## 6. Entity lifetime

- Slot 0 world, slot 1 player, game entities from slot 2; cap 495.
- **Allocate**: lowest free slot ≥ 2 that was freed before time 2.0 s or at
  least 0.5 s ago; else the next never-used slot **[probe K-alloc]**. The
  new entity's components are zeroed, serial incremented.
- Reserves: optional hydration only while ≥ 48 slots are free; transient
  effects (gibs, debris, shells) only while ≥ 32 are free (otherwise
  skipped); game allocations may use every slot; exhaustion is fatal with a
  class census.
- **Free**: unlink, remove from the heap and active sets, zero components
  (keep serial and free time), serial += 1. References held elsewhere are
  validated through handles.
- **New Game settle**: after the start map's records spawn, two 0.1 s
  simulation frames run before the first presented frame (items and
  monsters drop to the floor) **[probe K-settle]**. Hydration later gets
  none.

## 7. Tests

| # | property | data |
|---|---|---|
| K1 | +0.12 s self-rescheduling think at dt 1/30: frames 0, 3, 7, 10, 14; times t0 + 0.12 k | probe K-think + host |
| K2 | `nextthink = time` from its own think → next frame; set by a lower-index entity → this frame; unset → never; `now + dt` boundary inclusive | probe K-think |
| K3 | a think whose due time passed during a 0.1 s frame sees the frame time | probe K-think |
| K4 | pusher thinks on ltime; blocked pusher postpones | probe K-pusher |
| K5 | timers: order, cascade, time seen, freeze/thaw keep remaining time | probe K-think (multi_manager entities) + synthetic |
| K6 | allocation: reuse delay, lowest index, serial increments | probe K-alloc |
| K7 | reserves: hydration stops at 48 free, transients at 32 | synthetic |
| K8 | settle: start time 1.0 s; items dropped 10 u above a floor rest on it at frame 1 | probe K-settle |
| K9 | active set: identical results with a debug mode that steps every live entity every frame (route checksum of all components at each stats step) | host route |
| K10 | host and cart clocks identical on a fixed-dt route | route stats |
