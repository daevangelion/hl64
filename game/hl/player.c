/***
*
*	Copyright (c) 1996-2002, Valve LLC. All rights reserved.
*	
*	This product contains software technology licensed from Id 
*	Software, Inc. ("Id Technology").  Id Technology (c) 1996 Id Software, Inc. 
*	All Rights Reserved.
*
*   Use, distribution, and modification of this source code and/or resulting
*   object code is restricted to non-commercial enhancements to products from
*   Valve LLC.  All other use, distribution, or modification is prohibited
*   without written permission from Valve LLC.
*
****/
/*
 * hl64 game/, Half Life 1 SDK licence (game/LICENSE-HLSDK).
 *
 * player.c -- the player's pre-move think (HLSDK dlls/player.cpp
 * CBasePlayer::PreThink -> WaterMove), M2 part: the FL_INWATER flag that
 * follows the water level the previous move left (pmove-host.md 1 step 2).
 * Air supply, drowning, bubbles and lava / slime damage need the player's
 * health and the damage and sound modules and join with them (M3).
 */
#include "hl64/game.h"

void game_player_prethink(ent_player_t *pl)
{
	/* WaterMove */
	if (pl->movetype == PL_MOVE_NOCLIP || pl->dead)
		return;
	if (!pl->waterlevel)
	{
		pl->flags &= ~PL_FL_INWATER;
		return;
	}
	pl->flags |= PL_FL_INWATER;
}
