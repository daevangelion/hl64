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
int		phy_player_active(void);
/* The home map (CWG index) the player is in, WLD_NO_MAP before spawn. */
int		phy_player_map(void);
ent_player_t	*phy_player(void);

/* Frame step 5 (pmove-host.md 1): physents, game_pm_move, write-back,
 * link with trigger touches, impact touches for the pmove touch list. */
void		phy_player_frame(const in_cmd_t *cmd, uint64_t now_us);

typedef struct {
	uint32_t	frames;		/* moves in the window */
	uint32_t	us_sum, us_max;	/* step 5 CPU */
	uint32_t	physents_max;
	uint32_t	overflow;	/* frames that dropped physents / ladders */
	uint32_t	sounds;		/* pm_shared sound requests (no audio before M3) */
} phy_player_prof_t;
/* The window since the last call (reset = 1 starts a new one). */
void		phy_player_prof(phy_player_prof_t *p, int reset);

#endif
