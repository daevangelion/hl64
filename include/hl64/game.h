/* SPDX-License-Identifier: MIT */
/*
 * game.h -- exports of game/ the engine calls (docs/architecture.md 7.2).
 * M2: the player movement adapter (game/pm, HLSDK pm_shared, D2), the
 * player's pre-move think (game/hl) and the view (game/hud). The rest of
 * 7.2 (spawn, think / touch / use dispatch, player postthink, HUD, studio)
 * joins with M3.
 */
#ifndef HL64_GAME_H
#define HL64_GAME_H

#include "hl64/pmove.h"

/* Once at boot: pm_shared's tables. */
void	game_pm_init(void);
/* Run pm_shared on the engine's move record (pmove-host.md 1 step 5):
 * copies pm into HLSDK playermove_t, PM_Move, writes the player block
 * (pm->pl) and the touch list (pm->touch, pm->ntouch) back. */
void	game_pm_move(phy_pm_t *pm);
/* The player's think before the move (pmove-host.md 1 step 2; HLSDK
 * CBasePlayer::PreThink): M2 keeps FL_INWATER with the water level. */
void	game_player_prethink(ent_player_t *pl);

/* The view (HLSDK client V_CalcRefdef for a first-person player): eye
 * origin = origin + view_ofs + bob, stair-step smoothing, water offset
 * (PM_PointContents), in the player's home-local frame; angles = command
 * angles + punch + movement roll. time and frametime in seconds of game
 * clock. */
typedef struct {
	float	org[3];
	float	ang[3];		/* pitch (down +), yaw, roll; degrees */
} game_view_t;
void	game_calc_view(const ent_player_t *pl, float time, float frametime, game_view_t *out);

#endif
