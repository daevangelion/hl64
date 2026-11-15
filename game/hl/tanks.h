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
 * tanks.h -- what the player (game/hl/player.c, HLSDK player.cpp) calls of
 * the mounted guns (game/hl/tanks.c, func_tank.cpp): CFuncTank::OnControls.
 * The rest goes through the tank's use function (UseEntity): USE_SET with
 * value 2 fires the gun (ControllerPostFrame), USE_OFF lets it go.
 */
#ifndef HL64_GAME_TANKS_H
#define HL64_GAME_TANKS_H

/* CFuncTank::OnControls(player): the player still stands where it took
 * the controls (a controllable tank only) */
int	CFuncTank_OnControls(int tank);

#endif
