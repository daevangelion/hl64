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
 * gib.h -- CGib (HLSDK dlls/monsters.h, combat.cpp), implemented in
 * game/hl/aicombat.c; used by monsters and GameCore's env_shooter.
 */
#ifndef HL64_GAME_GIB_H
#define HL64_GAME_GIB_H

#include "game/hl/cbase.h"

/* CGib (monsters.h): class "gib", a CBaseEntity with these members */
typedef struct {
	CBASEENTITY_FIELDS
	int		m_bloodColor;
	int		m_cBloodDecals;
	int		m_material;	/* -1 = matNone */
	float		m_lifeTime;
} CGib;
#define GIB(e)	((CGib *)ent_priv(e))
/* GetClassPtr((CGib *)NULL): a fresh gib entity (-1 none); then CGib_Spawn */
int	CGib_Create(void);
void	CGib_Spawn(int e, res_t model);	/* CGib::Spawn(szGibModel) */
void	CGib_SpawnHeadGib(int victim);
void	CGib_SpawnRandomGibs(int victim, int cGibs, int human);
void	CGib_SpawnStickyGibs(int victim, const float org[3], int cGibs);

#endif
