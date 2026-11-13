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
 * material.h -- surface materials: HLSDK pm_materials.h texture types,
 * weapons.h Bullet types, and TEXTURETYPE_PlaySound (sound.cpp) over the
 * cooked material table (engine/coll col_material, trace-and-hulls.md 4.7)
 * instead of texture names and materials.txt.
 */
#ifndef HL64_GAME_MATERIAL_H
#define HL64_GAME_MATERIAL_H

#include "game/hl/util.h"

#define CHAR_TEX_CONCRETE	'C'
#define CHAR_TEX_METAL		'M'
#define CHAR_TEX_DIRT		'D'
#define CHAR_TEX_VENT		'V'
#define CHAR_TEX_GRATE		'G'
#define CHAR_TEX_TILE		'T'
#define CHAR_TEX_SLOSH		'S'
#define CHAR_TEX_WOOD		'W'
#define CHAR_TEX_COMPUTER	'P'
#define CHAR_TEX_GLASS		'Y'
#define CHAR_TEX_FLESH		'F'
#define CHAR_TEX_SNOW		'N'

/* weapons.h Bullet */
typedef enum {
	BULLET_NONE = 0,
	BULLET_PLAYER_9MM,
	BULLET_PLAYER_MP5,
	BULLET_PLAYER_357,
	BULLET_PLAYER_BUCKSHOT,
	BULLET_PLAYER_CROWBAR,
	BULLET_MONSTER_9MM,
	BULLET_MONSTER_MP5,
	BULLET_MONSTER_12MM
} Bullet;

/* TEXTURETYPE_PlaySound(ptr, vecSrc, vecEnd, iBulletType): the material
 * hit sound at the trace end; returns the volume bar (fvolbar). */
float		TEXTURETYPE_PlaySound(const col_trace_t *tr, const float src[3], const float end[3],
			int bullet);
/* The material character of a trace's surface (flesh for a classified
 * entity, else the cooked material), 0 when none. */
int		TEXTURETYPE_Get(const col_trace_t *tr, const float src[3], const float end[3]);

#endif
