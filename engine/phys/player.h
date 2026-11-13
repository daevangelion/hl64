/* SPDX-License-Identifier: MIT */
/*
 * player.h -- the player entity and the pmove host (engine/phys/pmove.c;
 * docs/specs/pmove-host.md, docs/architecture.md 3.2 step 5).
 *
 * The player is entity slot 1 (its collision view in engine/ent) plus the
 * player block ent_player_t (include/hl64/pmove.h). Spawning registers the
 * player's handlers for the setpos / look / noclip / god debug commands
 * (before the first spawn setpos / look move the free camera).
 */
#ifndef HL64_PLAYER_H
#define HL64_PLAYER_H

#include <stdint.h>

#include "hl64/pmove.h"

/* Boot: pm_shared tables, debug command handlers (noclip, god). */
void		phy_player_init(void);
/* Place the player in map (opened as needed) at a map-local origin with
 * view angles; the first call creates the player (slot 1, standing hull,
 * GoldSrc spawn state). Returns 0, or -1 for a bad map. */
int		phy_player_spawn(int map, const float org[3], const float ang[3]);
/* At the map's info_player_start (+1 u, as HLSDK spawns), or -1. */
int		phy_player_spawn_start(int map);
/* Test builds' debug teleport (route-bytecode.md SETPOS): phy_player_spawn,
 * then lifted (<= 64 u, 1 u steps) to the first z where the box of the
 * player's hull is free. Returns 0, or -1 for a bad map. */
int		phy_player_setpos(int map, const float org[3], const float ang[3]);
int		phy_player_active(void);
/* The home map (CWG index) the player is in, WLD_NO_MAP before spawn. */
int		phy_player_map(void);
ent_player_t	*phy_player(void);
/* Seam crossing (architecture.md 5.4 req. 3; the one place of the
 * re-base): home := map_b and local += g_M(A) - g_M(B) + T, so the global
 * position moves by exactly T; velocity, angles, punch, view, flags and
 * inventory kept; the ground entity becomes `ground_b` (B's twin of the
 * mover the player stands on, -1 none). Returns 0, -1 when B is not open. */
int		phy_player_rebase(int map_b, const float T[3], int ground_b);

/* Frame step 5 (pmove-host.md 1): physents, game_pm_move, write-back,
 * link with trigger touches, impact touches for the pmove touch list.
 * time: the frame's game clock (f64 s; pm_shared sees it as ms). */
void		phy_player_frame(const in_cmd_t *cmd, double time);

typedef struct {
	uint32_t	frames;		/* moves in the window */
	uint32_t	us_sum, us_max;	/* step 5 CPU */
	uint32_t	physents_max;
	uint32_t	overflow;	/* frames that dropped physents / ladders */
	uint32_t	sounds;		/* pm_shared sound requests (no audio before M3) */
	/* the services under the move, timed where pm_shared calls them */
	uint32_t	traces, trace_us;	/* player traces and position tests */
	uint32_t	clips;			/* physent clips inside them */
	uint32_t	contents, contents_us;	/* point contents */
} phy_player_prof_t;
/* The window since the last call (reset = 1 starts a new one). */
void		phy_player_prof(phy_player_prof_t *p, int reset);

#endif
