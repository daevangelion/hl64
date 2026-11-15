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
 * and path_track), infodecal (world.cpp CDecal, on fx_decal_static).
 * info_player_start / info_landmark / info_target /
 * info_teleport_destination are cooked point records (no entity): their
 * origin and angles are read by name (util.c UTIL_NamedOrigin).
 */
#include "game/hl/util.h"
#include "game/hl/savegame.h"

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

uint32_t World_SaveState(int op)
{
	uint32_t n = sg_io(op, &world_chapter_title, sizeof(world_chapter_title));

	if (op == SG_GET && (world_chapter_title < -1 || world_chapter_title > 0xFFFF))
	{
		sv_fail("game: chapter title out of range");
		world_chapter_title = -1;
	}
	return n;
}

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

/* the map's worldspawn lives while its map is open (closing destroys it) */
static int CWorld_quiet(int e)
{
	(void)e;
	return 0;
}

static const sv_table_t sv_CWorld = { .base = &sv_CBaseEntity, .quiet = CWorld_quiet };

const game_class_t gc_WORLDSPAWN = {
	.size = sizeof(CBaseEntity),
	.Spawn = CWorld_Spawn,
	.save = &sv_CWorld,
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

/* HLSDK CPathCorner::m_SaveData; message is a name (fired by trains,
 * cleared by fire-once corners) */
static const sv_field_t f_CPathCorner[] = {
	SVF(CPathCorner, m_flWait, SV_F32, SVK),
};
static const sv_table_t sv_CPathCorner = SV_TABLE(&sv_CBaseEntity, f_CPathCorner,
	.flags = SVT_MSG_NAME);

const game_class_t gc_PATH_CORNER = {
	.size = sizeof(CPathCorner),
	.Spawn = CPathCorner_Spawn,
	.vt = &CPathCorner_vt,
	.save = &sv_CPathCorner,
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

/* HLSDK CPathTrack::m_SaveData (no next / alt path: see above) */
static const sv_field_t f_CPathTrack[] = {
	SVF(CPathTrack, m_length, SV_F32, 0),
};
static const sv_table_t sv_CPathTrack = SV_TABLE(&sv_CBaseEntity, f_CPathTrack);

const game_class_t gc_PATH_TRACK = {
	.size = sizeof(CPathTrack),
	.Spawn = CPathTrack_Spawn,
	.vt = &CPointEntity_vt,
	.save = &sv_CPathTrack,
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

/* ---- infodecal (world.cpp CDecal) ---------------------------------------- */

/* pfnStaticDecal / TE_BSPDECAL: the decal at the origin on the model the
 * +-5 u trace meets; < 0 when the engine cannot place it yet. */
static int CDecal_Spray(int e)
{
	const float *o = ent_cv(e)->origin;
	float a[3] = { o[0] - 5, o[1] - 5, o[2] - 5 }, b[3] = { o[0] + 5, o[1] + 5, o[2] + 5 };
	col_trace_t tr;

	UTIL_TraceLine(a, b, ignore_monsters, e, &tr);
	return fx_decal_static(tr.ent > 0 ? tr.ent : 0, o, ent_cv(e)->skin);
}

/* placed, then gone (SUB_Remove); hl64: the engine may not place it yet
 * (sector or sheet not resident, placement budget): tried again 0.1 s on */
void CDecal_StaticDecal(int e)
{
	if (CDecal_Spray(e) < 0)
	{
		ThinkIn(e, 0.1f);
		return;
	}
	UTIL_Remove(e);
}

void CDecal_TriggerDecal(int e, int activator, int caller, int use_type, float value)
{
	(void)activator;
	(void)caller;
	(void)use_type;
	(void)value;
	if (CDecal_Spray(e) < 0)
	{
		SetThink(e, FN_CDecal_StaticDecal);
		ThinkIn(e, 0.1f);
		return;
	}
	SetThink(e, FN_SUB_Remove);
	ThinkIn(e, 0.1f);
}

/* KeyValue "texture" -> pev->skin (DECAL_INDEX: the cooked decal id, -1
 * when decals.wad lacks it); single player: SF_DECAL_NOTINDEATHMATCH is
 * a deathmatch rule */
static int CDecal_Spawn(int e, gid_t16 gid)
{
	int d = rec_key_decal(gid, K_TEXTURE);

	if (d < 0)
		return SpawnRefuse("infodecal: decal not in decals.wad");
	ent_cv(e)->skin = (int16_t)d;
	if (ent_idc(e)->targetname == NAME_NONE)
	{
		/* sprays itself on as soon as the world is done spawning */
		SetThink(e, FN_CDecal_StaticDecal);
		ent_think_at(e, gtime());
	}
	else
	{
		/* sprays itself on when triggered */
		SetThink(e, FN_SUB_DoNothing);
		SetUse(e, FN_CDecal_TriggerDecal);
	}
	return 0;
}

const game_class_t gc_INFODECAL = { .size = sizeof(CBaseEntity), .Spawn = CDecal_Spawn };
