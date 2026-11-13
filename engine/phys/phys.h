/* SPDX-License-Identifier: MIT */
/*
 * phys.h -- engine-internal entry points between the active-set pass
 * (engine/ent/pass.c), the movetype steps (engine/phys) and the game
 * clock (docs/specs/think-and-time.md, physics-movetypes.md).
 */
#ifndef HL64_PHYS_H
#define HL64_PHYS_H

/* ---- game clock (think-and-time.md 1; engine/ent/pass.c) ----------------- */

/* New Game: time 1.0; the next two frames are settle frames (frame time
 * 0, K-settle). */
void	eng_clock_new_game(void);
/* Frame start (step 4): the frame's time (f64 s) and frame time: dt, or 0
 * in a settle frame; sets eng_time() / eng_frametime(). Returns the frame
 * time. */
double	eng_clock_frame(double dt);
/* After step 6: time += frame time. */
void	eng_clock_advance(void);
/* The frame's start time (not changed by eng_set_clock). */
double	eng_clock_time(void);
/* What eng_time() / eng_frametime() return until the next call (thinks
 * and logic timers see their due time; callers restore the frame's). */
void	eng_set_clock(double time, double frametime);

/* ---- per-entity steps ------------------------------------------------------ */

/* The movetype step of TOSS, BOUNCE, FLY, FLYMISSILE, BOUNCEMISSILE, STEP,
 * PUSH and PUSHSTEP entities (physics-movetypes.md 3, 4), think included
 * (phy_run_think / phy_call_think below); slice Phys, engine/phys/move.c
 * and push.c. time = frame start, dt = frame time. */
void	phy_step_motion(int e, double time, double dt);

/* Think rule (think-and-time.md 2, every movetype except PUSH): when
 * tm.nextthink > 0 and (f64) nextthink <= time + dt: nextthink := 0 and
 * the think runs with eng_time() = (f32) max(nextthink, time). Returns 1
 * when e is still the same live entity afterwards. */
int	phy_run_think(int e, double time, double dt);
/* Pusher think (physics-movetypes.md 4.1): nextthink := 0, the think runs
 * with eng_time() = t. Returns 1 when e is still the same live entity. */
int	phy_call_think(int e, double t);
/* Blocked dispatch (pushers): e's blocked function with other. */
void	phy_blocked(int e, int other);
/* Touch dispatch (touch.c): self's touch function with other, frame layer
 * = self's layer. */
void	phy_dispatch_touch(int self, int other);

#endif
