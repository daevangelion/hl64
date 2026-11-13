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
 * lights.c -- HLSDK dlls/lights.cpp CLight (light, light_spot) as logic
 * records: a named light owns switchable style 32-62 and sets it to its
 * pattern, "m" or "a" (fx_lightstyle with cooked pattern ids:
 * lightstyles.def line 0 is "m", line 13 "a"). Lights without a
 * targetname are inert (CLight::Spawn removes them; the cook prunes them).
 * light_fluoro / light_fluorospark have no HLSDK class: GoldSrc does not
 * spawn them.
 */
#include "game/hl/util.h"

#define SF_LIGHT_START_OFF	1
#define PATTERN_M		0
#define PATTERN_A		13

/* rec_logic_t: a = m_iStyle, b = m_iszPattern (pattern id, -1 none),
 * flags = spawnflags & SF_LIGHT_START_OFF */
static void light_set(rec_logic_t *l)
{
	if (l->flags & SF_LIGHT_START_OFF)
		fx_lightstyle(l->a, PATTERN_A);
	else
		fx_lightstyle(l->a, (uint16_t)(l->b >= 0 ? l->b : PATTERN_M));
}

static void CLight_LogicSpawn(gid_t16 gid)
{
	rec_logic_t *l = rec_logic(gid);

	if (rec_targetname(gid) == NAME_NONE)
	{
		game_refuse_log(rec_class(gid), gid, -1,
			"CLight::Spawn: no targetname (inert light, REMOVE_ENTITY)");
		rec_kill(gid);
		return;
	}
	l->a = (int)KeyF(gid, K_STYLE, 0);
	l->b = rec_key_pattern(gid, K_PATTERN);
	l->flags = (uint8_t)((uint32_t)KeyF(gid, K_SPAWNFLAGS, 0) & SF_LIGHT_START_OFF);
	if (l->a >= 32)
		light_set(l);
}

static void CLight_LogicUse(gid_t16 gid, int activator, int caller, int use_type, float value)
{
	rec_logic_t *l = rec_logic(gid);

	(void)activator;
	(void)caller;
	(void)value;
	if (l->a < 32)
		return;
	if (!ShouldToggle(use_type, !(l->flags & SF_LIGHT_START_OFF)))
		return;
	l->flags ^= SF_LIGHT_START_OFF;
	light_set(l);
}

const game_class_t gc_LIGHT = {
	.LogicSpawn = CLight_LogicSpawn,
	.LogicUse = CLight_LogicUse,
};
const game_class_t gc_LIGHT_SPOT = {
	.LogicSpawn = CLight_LogicSpawn,
	.LogicUse = CLight_LogicUse,
};

static void NoClass_LogicSpawn(gid_t16 gid)
{
	game_refuse_log(rec_class(gid), gid, -1, "no HLSDK class (GoldSrc does not spawn it)");
	rec_kill(gid);
}

const game_class_t gc_LIGHT_FLUORO = { .LogicSpawn = NoClass_LogicSpawn };
const game_class_t gc_LIGHT_FLUOROSPARK = { .LogicSpawn = NoClass_LogicSpawn };
