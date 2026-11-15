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
 * trains.h -- HLSDK dlls/trains.h: the track train the player drives
 * (player.c, player.cpp PlayerUse / PreThink).
 */
#ifndef HL64_GAME_TRAINS_H
#define HL64_GAME_TRAINS_H

#define SF_TRACKTRAIN_NOPITCH		0x0001
#define SF_TRACKTRAIN_NOCONTROL		0x0002
#define SF_TRACKTRAIN_FORWARDONLY	0x0004
#define SF_TRACKTRAIN_PASSABLE		0x0008

/* CBaseEntity::OnControls (FALSE but for func_tracktrain): pos (map-local
 * in train's frame) inside the train's control volume */
int	TrackTrain_OnControls(int train, const float pos[3]);
/* pev->impulse of a func_tracktrain: its full speed (TrainSpeed's max);
 * 0 for any other entity */
int	TrackTrain_Impulse(int e);

#endif
