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
 * grenade.h -- CGrenade (HLSDK dlls/weapons.h, ggrenade.cpp): hand and
 * M203 grenades for the player's weapons and for monsters (hgrunt), and
 * the explosion monster_tripmine uses. Positions are map-local in the
 * caller's frame; owner is an entity slot (-1 none).
 */
#ifndef HL64_GAME_GRENADE_H
#define HL64_GAME_GRENADE_H

#include "game/hl/cbase.h"

typedef struct {
	CBASEENTITY_FIELDS
	CBASEDELAY_FIELDS
	CBASEANIMATING_FIELDS
	float		m_flNextAttack;		/* BounceTouch debounce */
	uint8_t		m_fRegisteredSound;
	uint8_t		cg_pad[3];
} CGrenade;

/* ShootTimed (bounces, detonates time s after the throw), ShootContact
 * (explodes on touch); the entity or -1 */
int	CGrenade_ShootTimed(int owner, const float start[3], const float vel[3], float time);
int	CGrenade_ShootContact(int owner, const float start[3], const float vel[3]);
/* CGrenade::Explode(pTrace, bitsDamageType) on any CGrenade-layout entity */
void	CGrenade_Explode(int e, const col_trace_t *tr, int bits);
/* CGrenade::Killed -> Detonate */
void	CGrenade_KilledVt(int e, int attacker, int gib);
/* CGrenade's save level (tripmines build on it; thrown grenades are
 * transients) */
extern const sv_table_t	sv_CGrenade;

#endif
