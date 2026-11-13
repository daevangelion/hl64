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
 * rat.c -- monster_rat (HLSDK dlls/rat.cpp CRat), an environmental monster.
 */
#include "game/hl/monster.h"

static int Rat_Classify(int e) { (void)e; return CLASS_INSECT; }
static void Rat_SetYawSpeed(int e) { ent_phys(e)->yaw_speed = 45; }

static int CRat_Spawn(int e, gid_t16 gid)
{
	CBaseMonster	*m = Mon_SpawnKeys(e, gid);
	static const float z[3] = { 0, 0, 0 };

	PRECACHE(RESID(MON_BIGRAT_MDL));
	ent_set_model(e, RESID(MON_BIGRAT_MDL));
	ent_set_size(e, z, z);
	ent_set_solid(e, SOLID_SLIDEBOX);
	ent_set_movetype(e, MOVETYPE_STEP);
	m->m_bloodColor = BLOOD_COLOR_RED;
	m->health = 8;
	VecSet(m->view_ofs, 0, 0, 6);	/* position of the eyes relative to monster's origin */
	ent_phys(e)->view_ofs_z = 6;
	m->m_flFieldOfView = 0.5f;
	m->m_MonsterState = MONSTERSTATE_NONE;
	Mon_MonsterInit(e);
	return 0;
}

static const game_vt_t rat_vt = {
	MON_GAME_VT,
	.Classify = Rat_Classify,
};

static const mon_vt_t rat_mvt = {
	.SetYawSpeed = Rat_SetYawSpeed,
};

const game_class_t gc_MONSTER_RAT = {
	.size = sizeof(CBaseMonster),
	.Spawn = CRat_Spawn,
	.vt = &rat_vt,
	.ext = &rat_mvt,
};
