/* SPDX-License-Identifier: MIT */
/*
 * pmove.h -- the player-move host contract (docs/specs/pmove-host.md,
 * docs/architecture.md 6.2 and 7, decision D2).
 *
 * The player's movement is HLSDK pm_shared, compiled in game/pm under the
 * HLSDK licence. The engine (engine/phys/pmove.c) owns the player block,
 * builds one frame record per move (the command, the physents taken from
 * the spatial grid, the ladders) and supplies the services pm_shared calls
 * back into (contents, player traces, position tests). The game's adapter
 * (game/pm/pm_host.c, game_pm_move) copies the record into HLSDK's
 * playermove_t, runs PM_Move and writes the player block and the touch
 * list back. Layout and names here are hl64's own.
 *
 * Coordinates are home-layer local (the player's home map's own frame;
 * global = local + the home layer's g_M): pm_shared's float arithmetic
 * then sees GoldSrc's numbers, not numbers several thousand units off
 * (pmove-host.md 2.1). The player block's origin, the physent copies
 * (origin, absmin, absmax), the services' points and the touch list are
 * in that frame; the entity layer and engine/coll stay global. Physent 0
 * is the home layer's world model at the origin; other open layers follow
 * as world physents (index 0) at their g_M minus the home g_M.
 */
#ifndef HL64_PMOVE_H
#define HL64_PMOVE_H

#include <stdint.h>

#include "hl64/coll.h"
#include "hl64/limits.h"

/* ---- usercmd (architecture.md 11; HLSDK usercmd_t meaning) ------------- */

/* Button bits: HLSDK in_buttons.h values. */
#define IN_BTN_ATTACK		(1u << 0)
#define IN_BTN_JUMP		(1u << 1)
#define IN_BTN_DUCK		(1u << 2)
#define IN_BTN_FORWARD		(1u << 3)
#define IN_BTN_BACK		(1u << 4)
#define IN_BTN_USE		(1u << 5)
#define IN_BTN_LEFT		(1u << 7)
#define IN_BTN_RIGHT		(1u << 8)
#define IN_BTN_MOVELEFT		(1u << 9)
#define IN_BTN_MOVERIGHT	(1u << 10)
#define IN_BTN_ATTACK2		(1u << 11)
#define IN_BTN_RUN		(1u << 12)
#define IN_BTN_RELOAD		(1u << 13)

typedef struct {
	float		viewangles[3];	/* pitch (down +), yaw, roll; degrees */
	float		forwardmove, sidemove, upmove;	/* units per second */
	uint16_t	buttons;	/* IN_BTN_* */
	uint8_t		msec;		/* 1..100 */
	uint8_t		impulse;
} in_cmd_t;

/* ---- player block (architecture.md 6.2 ent_player_t) -------------------- */

/* Player flags pm_shared reads or writes (HLSDK const.h FL_* values). */
#define PL_FL_CLIENT		(1u << 3)
#define PL_FL_INWATER		(1u << 4)
#define PL_FL_GODMODE		(1u << 6)
#define PL_FL_NOTARGET		(1u << 7)
#define PL_FL_ONGROUND		(1u << 9)
#define PL_FL_WATERJUMP		(1u << 11)
#define PL_FL_FROZEN		(1u << 12)
#define PL_FL_DUCKING		(1u << 14)
#define PL_FL_ONTRAIN		(1u << 24)

/* Movetypes the player uses (HLSDK values). */
#define PL_MOVE_NONE		0
#define PL_MOVE_WALK		3
#define PL_MOVE_FLY		5
#define PL_MOVE_NOCLIP		8

typedef struct {
	float		origin[3];	/* home-layer local (map-local) */
	float		velocity[3];
	float		basevelocity[3];
	float		movedir[3];	/* water-jump push */
	float		view_ofs[3];
	float		punchangle[3];
	float		view_angles[3];	/* the command's view angles */
	float		angles[3];	/* pmove's movement angles (with roll) */
	float		mins[3], maxs[3];	/* box of usehull */
	float		fall_velocity;	/* flFallVelocity */
	float		duck_time;	/* flDuckTime, ms */
	float		swim_time;	/* flSwimTime, ms */
	float		waterjump_time;	/* waterjumptime, ms */
	float		gravity;	/* 0 = 1 */
	float		friction;
	float		maxspeed;	/* client max speed, 0 = movevars only */
	uint32_t	flags;		/* PL_FL_* and the other HLSDK FL_* bits */
	int32_t		step_time;	/* flTimeStepSound, ms */
	uint16_t	oldbuttons;	/* pm_shared's oldbuttons after the move */
	uint16_t	buttons;	/* the last command's buttons */
	int16_t		groundentity;	/* entity index, -1 none */
	uint8_t		movetype;	/* PL_MOVE_* */
	uint8_t		usehull;	/* 0 stand, 1 duck, 2 point, 3 large */
	uint8_t		in_duck;	/* bInDuck */
	uint8_t		step_left;	/* iStepLeft */
	int8_t		waterlevel;
	int8_t		watertype;
	uint8_t		dead;
	uint8_t		pad[3];
} ent_player_t;

/* ---- one move (spec 1 step 5) -------------------------------------------- */

/* Player trace flags (HLSDK pm_defs.h PM_* values). */
#define PHY_PM_STUDIO_IGNORE	0x01
#define PHY_PM_GLASS_IGNORE	0x04
#define PHY_PM_WORLD_ONLY	0x08

typedef struct {
	ent_player_t	*pl;
	in_cmd_t	cmd;
	float		time_ms;	/* game clock, milliseconds */
	int		npe, nme, ntouch;
	/* physents: copies of the entities' collision views translated into
	 * the home frame; physent 0 and the other layers' world models carry
	 * index 0 (the world) */
	col_ent_t	pe[HL64_PM_PHYSENTS];
	col_ent_t	me[HL64_PM_MOVEENTS];	/* ladders */
	/* pmove touch list, in order: tr.ent = physent index */
	col_trace_t	touch[HL64_PM_TOUCH];
} phy_pm_t;

/* The move being run (engine-owned; valid during game_pm_move). */
extern phy_pm_t		phy_pm;

/* Player boxes per usehull (HLSDK pm player_mins / player_maxs). */
extern const float	phy_pm_mins[4][3];
extern const float	phy_pm_maxs[4][3];

/* ---- services (spec 3) ----------------------------------------------------- */

/* PM_PointContents: world contents at p unless a contents physent contains
 * p (its skin wins); *truecont (may be NULL) = the world's. */
int		phy_pm_contents(const float p[3], int *truecont);
/* pm_shared's console messages (NaN / velocity clamps, touch overflow):
 * engine log, once per frame. */
void		phy_pm_print(const char *msg);
/* PM_TruePointContents: world only, CURRENT_* kept. */
int		phy_pm_true_contents(const float p[3]);
/* PM_PlayerTrace: the box of usehull against every physent in index order,
 * combined as trace-and-hulls.md 4.4 step 5, then allsolid => startsolid,
 * startsolid => fraction 0; tr->ent = physent index, -1 none. */
void		phy_pm_trace(const float start[3], const float end[3], int flags, int usehull,
			int ignore_pe, col_trace_t *tr);
/* PM_TestPlayerPosition: physent index of the first solid the box of
 * usehull at pos is inside, -1 if free; tr (may be NULL) gets that trace. */
int		phy_pm_test(const float pos[3], int usehull, col_trace_t *tr);
/* One physent alone (PM_TraceModel passes usehull 2: the point). */
void		phy_pm_trace_ent(const col_ent_t *pe, int usehull, const float start[3],
			const float end[3], col_trace_t *tr);
/* PM_GetModelBounds: a brush model's bounds in the home frame (its
 * layer's map coordinates + that layer's g_M - the home g_M); 0 for a bad
 * model. */
int		phy_pm_model_bounds(uint16_t model, float mins[3], float maxs[3]);
/* PM_PlaySound: the player's footstep / ladder / wade / jump-land sounds
 * (literal HLSDK sample names). */
void		phy_pm_sound(int channel, const char *sample, float volume, float attenuation,
			int flags, int pitch);
/* Engine random stream (one stream, HLSDK RandomLong / RandomFloat
 * ranges: [lo, hi] inclusive). */
int32_t		hl64_rand_long(int32_t lo, int32_t hi);
float		hl64_rand_float(float lo, float hi);

#endif
