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
 * breakable.h -- HLSDK dlls/func_break.h: Materials, Explosions, the
 * CBreakable block, the material sound helpers (breakable.c),
 * ExplosionCreate (explode.cpp, env.c) and the run-time CSprite helpers
 * (effects.h / effects.cpp, env.c).
 */
#ifndef HL64_GAME_BREAKABLE_H
#define HL64_GAME_BREAKABLE_H

#include "game/hl/util.h"

typedef enum { expRandom, expDirected } Explosions;
typedef enum {
	matGlass = 0, matWood, matMetal, matFlesh, matCinderBlock, matCeilingTile, matComputer,
	matUnbreakableGlass, matRocks, matNone, matLastMaterial
} Materials;

#define CBREAKABLE_FIELDS \
	CBASEENTITY_FIELDS \
	CBASEDELAY_FIELDS \
	int32_t		m_Material;	/* Materials */ \
	int32_t		m_Explosion;	/* Explosions */ \
	res_t		m_idShard;	\
	res_t		m_iszGibModel;	\
	float		m_angle;	\
	cls_t		m_iszSpawnObject; /* CLS_NONE */ \
	uint16_t	brk_pad;	\
	int32_t		m_iExplodeMagnitude;	/* pev->impulse */

typedef struct {
	CBREAKABLE_FIELDS
} CBreakable;

void		MaterialSoundPrecache(int material);
/* CBreakable::MaterialSoundRandom(edict, material, volume) */
void		MaterialSoundRandom(int e, int material, float volume);
/* ExplosionCreate(center, angles, owner, magnitude, doDamage) */
void		ExplosionCreate(const float center[3], const float angles[3], int owner,
			int magnitude, int do_damage);
/* CSprite::SpriteCreate(model, origin, animate): a temporary env_sprite
 * (never saved), the entity or -1; AnimateAndDie(framerate);
 * SetTransparency(rendermode, r, g, b, renderamt, renderfx) */
int		CSprite_SpriteCreate(res_t model, const float org[3], int animate);
void		CSprite_AnimateAndDie(int e, float framerate);
void		CSprite_SetTransparency(int e, int mode, int r, int g, int b, int a, int fx);
/* A breakable record's material key (matWood when absent / bad). */
int		Breakable_Material(gid_t16 gid);
/* CBreakable::Precache for a material and gibmodel key (RES_NONE none):
 * its gib and sound precaches; the shard model. */
res_t		Breakable_Precache(int material, res_t gibmodel);
/* A spawnobject key value as a class (CLS_NONE none / not in the game). */
cls_t		Breakable_SpawnObject(int i);

#endif
