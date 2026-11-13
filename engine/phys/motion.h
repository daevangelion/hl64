/* SPDX-License-Identifier: MIT */
/*
 * motion.h -- engine/phys internals shared by the movetype steps
 * (move.c), the pushers (push.c) and the monster moves (walk.c):
 * docs/specs/physics-movetypes.md. Every position is map-local to the
 * frame of eng_frame_layer(), which the active-set pass sets to the
 * stepped entity's layer.
 */
#ifndef HL64_MOTION_H
#define HL64_MOTION_H

#include <stdint.h>

#include "hl64/coll.h"

/* The move query (trace-and-hulls.md 4.4), counted for the roll-up. */
void	phy_trace(const float start[3], const float mins[3], const float maxs[3],
		const float end[3], int type, const col_ent_t *pass, col_trace_t *tr);
/* The trace type of e's own moves: missile for FLYMISSILE / BOUNCEMISSILE,
 * ignore_monsters for SOLID_TRIGGER / SOLID_NOT movers, else normal. */
int	phy_move_type(const col_ent_t *v);
/* push(e, d) (spec 2): one move query from the origin to origin + d,
 * origin := end, link with touch, impact touch on a hit. Returns 1 when e
 * is still the same live entity. */
int	phy_shove(int e, const float d[3], col_trace_t *tr);
/* clip(v, n, k) (spec 2). */
void	phy_clip(const float in[3], const float n[3], float out[3], float k);
/* Slot is still the entity whose serial was taken. */
int	phy_alive(int e, uint16_t serial);

/* push.c: the PUSH step (spec 4). */
void	phy_pusher_step(int e, double time, double dt);

/* ---- roll-up (`prof phys_us`) -------------------------------------------- */

enum { PHY_K_TOSS, PHY_K_STEP, PHY_K_PUSH, PHY_K_PUSHSTEP, PHY_K_N };

typedef struct {
	uint32_t	frames;			/* frames with at least one step */
	uint32_t	us, us_max;		/* motion CPU per frame */
	uint32_t	steps[PHY_K_N];		/* steps per movetype class */
	uint32_t	kind_us[PHY_K_N];	/* their CPU */
	uint32_t	traces;			/* move queries and clips */
} phy_motion_prof_t;
/* The window since the last call (reset = 1 starts a new one). */
void	phy_motion_prof(phy_motion_prof_t *p, int reset);
/* Counted by push.c / walk.c for their own clips. */
extern uint32_t	phy_trace_count;

#endif
