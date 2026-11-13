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
 * world.c -- worldspawn (HLSDK dlls/world.cpp CWorld), the point classes
 * that become entities (subs.cpp CPointEntity, pathcorner.cpp path_corner
 * and path_track). info_player_start / info_landmark / info_target /
 * info_teleport_destination are cooked point records (no entity): their
 * origin and angles are read by name (util.c UTIL_NamedOrigin).
 */
#include "game/hl/util.h"

#define SF_WORLD_DARK		0x0001
#define SF_WORLD_TITLE		0x0002

/* lightstyles.def line order: 0..12 are styles 0..12, 13 is style 63 */
void world_lightstyles(void)
{
	int i;

	for (i = 0; i < 13; i++)
		fx_lightstyle(i, (uint16_t)i);
	fx_lightstyle(63, 13);
}

void	ShowChapterTitle(int title);

/* rec_logic-free state of the world: the chapter title shown 0.3 s after
 * the world spawned (CWorld::Precache's one-shot env_message) */
static int	world_chapter_title = -1;

void CWorld_ChapterTitle(int e)
{
	SetThink(e, FN_NONE);
	ShowChapterTitle(world_chapter_title);
	world_chapter_title = -1;
}

static int CWorld_Spawn(int e, gid_t16 gid)
{
	/* CWorld::Precache: the C-physics sounds, lightstyles (the gib models
	 * and ricochet sounds belong to the classes that use them) */
	PRECACHE(RESID(SND_NULL));
	world_lightstyles();
	if (gid != GID_NONE)
	{
		world_chapter_title = rec_key_title(gid, K_CHAPTERTITLE);
		if (world_chapter_title >= 0)
		{
			SetThink(e, FN_CWorld_ChapterTitle);
			ThinkIn(e, 0.3f);
		}
		if (KeyF(gid, K_STARTDARK, 0) != 0)
			log_printf("world: startdark (v_dark: engine fade from black)\n");
	}
	return 0;
}

const game_class_t gc_WORLDSPAWN = {
	.size = sizeof(CBaseEntity),
	.Spawn = CWorld_Spawn,
};

/* ---- CPointEntity ---------------------------------------------------------- */

int CPointEntity_Spawn(int e, gid_t16 gid)
{
	(void)gid;
	ent_set_solid(e, SOLID_NOT);
	return 0;
}

static int CPointEntity_ObjectCaps(int e)
{
	(void)e;
	return 0;	/* CBaseEntity::ObjectCaps() & ~FCAP_ACROSS_TRANSITION */
}

static const game_vt_t CPointEntity_vt = {
	.ObjectCaps = CPointEntity_ObjectCaps,
};

/* ---- path_corner (pathcorner.cpp CPathCorner) -------------------------------- */

typedef struct {
	CBASEENTITY_FIELDS
	float		m_flWait;
} CPathCorner;

static int CPathCorner_Spawn(int e, gid_t16 gid)
{
	PRIV(CPathCorner, e)->m_flWait = KeyF(gid, K_WAIT, 0);
	/* message: the name a train fires on arrival (plats.c) */
	PRIV(CPathCorner, e)->message = gid == GID_NONE ? NAME_NONE : rec_key_name(gid, K_MESSAGE);
	if (ent_idc(e)->targetname == NAME_NONE)
		log_printf("game: path_corner without a targetname (entity %d)\n", e);
	return CPointEntity_Spawn(e, gid);
}

static float CPathCorner_GetDelay(int e)
{
	return PRIV(CPathCorner, e)->m_flWait;
}

static const game_vt_t CPathCorner_vt = {
	.ObjectCaps = CPointEntity_ObjectCaps,
	.GetDelay = CPathCorner_GetDelay,
};

const game_class_t gc_PATH_CORNER = {
	.size = sizeof(CPathCorner),
	.Spawn = CPathCorner_Spawn,
	.vt = &CPathCorner_vt,
};

/* ---- path_track (pathcorner.cpp CPathTrack): no func_tracktrain is kept
 * by the cook, so only its toggle remains (no altpath key is cooked:
 * m_paltpath is always NULL) ------------------------------------------------ */

#define SF_PATH_DISABLED	0x00000001

typedef struct {
	CBASEENTITY_FIELDS
	float		m_length;
} CPathTrack;

void CPathTrack_Use(int e, int activator, int caller, int use_type, float value)
{
	CBaseEntity *pev = CBase(e);
	int on = !(pev->spawnflags & SF_PATH_DISABLED);

	(void)activator;
	(void)caller;
	(void)value;
	if (ShouldToggle(use_type, on))
	{
		if (on)
			pev->spawnflags |= SF_PATH_DISABLED;
		else
			pev->spawnflags &= ~SF_PATH_DISABLED;
	}
}

static int CPathTrack_Spawn(int e, gid_t16 gid)
{
	static const float mn[3] = { -8, -8, -8 }, mx[3] = { 8, 8, 8 };

	(void)gid;
	ent_set_solid(e, SOLID_TRIGGER);
	UTIL_SetSize(e, mn, mx);
	SetUse(e, FN_CPathTrack_Use);
	return 0;
}

const game_class_t gc_PATH_TRACK = {
	.size = sizeof(CPathTrack),
	.Spawn = CPathTrack_Spawn,
	.vt = &CPointEntity_vt,
};

/* ---- info_* points (subs.cpp / triggers.cpp CPointEntity): cooked point
 * records, never hydrated; the entries let a runtime Create spawn one ---- */

const game_class_t gc_INFO_PLAYER_START = { .size = sizeof(CBaseEntity), .Spawn = CPointEntity_Spawn, .vt = &CPointEntity_vt };
const game_class_t gc_INFO_PLAYER_DEATHMATCH = { .size = sizeof(CBaseEntity), .Spawn = CPointEntity_Spawn, .vt = &CPointEntity_vt };
const game_class_t gc_INFO_PLAYER_COOP = { .size = sizeof(CBaseEntity), .Spawn = CPointEntity_Spawn, .vt = &CPointEntity_vt };
const game_class_t gc_INFO_TELEPORT_DESTINATION = { .size = sizeof(CBaseEntity), .Spawn = CPointEntity_Spawn, .vt = &CPointEntity_vt };
const game_class_t gc_INFO_LANDMARK = { .size = sizeof(CBaseEntity), .Spawn = CPointEntity_Spawn, .vt = &CPointEntity_vt };
const game_class_t gc_INFO_TARGET = { .size = sizeof(CBaseEntity), .Spawn = CPointEntity_Spawn, .vt = &CPointEntity_vt };

/* ---- bodyque (world.cpp CCorpse: no Spawn, FCAP_DONT_SAVE) ------------ */

static int CCorpse_Spawn(int e, gid_t16 gid)
{
	(void)e;
	(void)gid;
	return 0;
}

static int CCorpse_ObjectCaps(int e)
{
	(void)e;
	return FCAP_DONT_SAVE;
}

static const game_vt_t CCorpse_vt = { .ObjectCaps = CCorpse_ObjectCaps };

const game_class_t gc_BODYQUE = { .size = sizeof(CBaseEntity), .Spawn = CCorpse_Spawn, .vt = &CCorpse_vt };
