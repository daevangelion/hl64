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
 * monstermaker.c -- monstermaker (HLSDK dlls/monstermaker.cpp CMonsterMaker)
 * as a logic record (HLE_CF_LOGIC, pinned): the maker has no entity; its
 * members live in the record's rec_logic_t and keys, MakerThink is a record
 * timer, and children remember the maker's gid (pev->owner's role for
 * DeathNotice and ShouldFadeOnDeath). Children take the maker's netname
 * with rec_name_hold. Runtime persistence of children (M4) is not here.
 */
#include "game/hl/monster.h"

#define SF_MONSTERMAKER_START_ON	1
#define SF_MONSTERMAKER_CYCLIC		4
#define SF_MONSTERMAKER_MONSTERCLIP	8

/* rec_logic_t use: a = m_cNumMonsters, b = m_cLiveChildren, f = m_flGround */
#define MM_ACTIVE	1	/* m_fActive */
#define MM_FADE		2	/* m_fFadeChildren */
#define MM_THINK	4	/* SetThink(MakerThink) (else NULL / SUB_DoNothing) */
#define MM_USE		8	/* use function present */
#define MM_GROUND	16	/* m_flGround measured */

/* spawnflags the next monster spawn adds (CREATE_NAMED_ENTITY, then the
 * maker sets them before DispatchSpawn) */
uint32_t	mon_spawn_extra_flags;

static void maker_think_at(gid_t16 gid, float t)
{
	rec_timer_cancel(gid);
	rec_timer(gid, t, 0, -1);
}

static void MakeMonster(gid_t16 gid)
{
	rec_logic_t	*l = rec_logic(gid);
	int		maxLive = (int)KeyF(gid, K_M_IMAXLIVECHILDREN, 0), c;
	uint32_t	sf = (uint32_t)KeyF(gid, K_SPAWNFLAGS, 0);
	float		org[3], ang[3], mn[3], mx[3], end[3];
	uint16_t	list[2];
	col_trace_t	tr;
	cls_t		cls = rec_key_cls(gid, K_MONSTERTYPE);
	name_t		target, net;

	if (maxLive > 0 && l->b >= maxLive)
		return;
	rec_origin(gid, org);
	rec_angles(gid, ang);
	if (!(l->flags & MM_GROUND))
	{
		VecCopy(org, end);
		end[2] -= 2048;
		UTIL_TraceLine(org, end, ignore_monsters, -1, &tr);
		l->f = tr.end[2];
		l->flags |= MM_GROUND;
	}
	VecSet(mn, org[0] - 34, org[1] - 34, l->f);
	VecSet(mx, org[0] + 34, org[1] + 34, org[2]);
	if (UTIL_EntitiesInBox(list, 2, mn, mx, FL_CLIENT | FL_MONSTER))
		return;
	if (cls == CLS_NONE)
		return;
	target = rec_key_name(gid, K_TARGET);
	if (target != NAME_NONE)
		rec_fire(target, -1, -1, gid, USE_TOGGLE, 0);
	mon_spawn_extra_flags = SF_MONSTER_FALL_TO_GROUND;
	if (sf & SF_MONSTERMAKER_MONSTERCLIP)
		mon_spawn_extra_flags |= SF_MONSTER_HITMONSTERCLIP;
	c = CBaseEntity_Create(cls, org, ang, -1);
	mon_spawn_extra_flags = 0;
	if (c < 0)
		return;
	if (mon_is_monster(c))
		MON(c)->m_makerGid = gid;
	net = rec_key_name(gid, K_NETNAME);
	if (net != NAME_NONE)
	{
		ent_idc(c)->targetname = net;
		rec_name_hold(c, net);
	}
	log_printf("monstermaker: %s child %d t=%.2f\n", res_name(ent_rnd(c)->model), c, gtime());
	l->b++;
	l->a--;
	if (l->a == 0)
	{
		l->flags &= ~(MM_THINK | MM_USE);
		rec_timer_cancel(gid);
	}
}

static void CMonsterMaker_LogicSpawn(gid_t16 gid)
{
	rec_logic_t	*l = rec_logic(gid);
	uint32_t	sf = (uint32_t)KeyF(gid, K_SPAWNFLAGS, 0);
	cls_t		cls = rec_key_cls(gid, K_MONSTERTYPE);
	const game_class_t *gc = game_class(cls);

	l->a = (int)KeyF(gid, K_MONSTERCOUNT, 0);
	l->b = 0;
	l->f = 0;
	l->flags = 0;
	if (cls == CLS_NONE || !gc)
		log_printf("monstermaker %u: no class for its monstertype\n", (unsigned)gid);
	if (rec_targetname(gid) != NAME_NONE)
	{
		l->flags |= MM_USE;
		if (sf & SF_MONSTERMAKER_START_ON)
			l->flags |= MM_ACTIVE | MM_THINK;
	}
	else
	{
		maker_think_at(gid, gtime() + KeyF(gid, K_DELAY, 0));
		l->flags |= MM_ACTIVE | MM_THINK;
	}
	if (l->a != 1)
		l->flags |= MM_FADE;
}

static void CMonsterMaker_LogicTimer(gid_t16 gid, uint16_t arg, int activator)
{
	rec_logic_t *l = rec_logic(gid);

	(void)arg; (void)activator;
	if (!(l->flags & MM_THINK))
		return;
	maker_think_at(gid, gtime() + KeyF(gid, K_DELAY, 0));
	MakeMonster(gid);
}

static void CMonsterMaker_LogicUse(gid_t16 gid, int activator, int caller, int useType, float value)
{
	rec_logic_t	*l = rec_logic(gid);
	uint32_t	sf = (uint32_t)KeyF(gid, K_SPAWNFLAGS, 0);

	(void)activator; (void)caller; (void)value;
	if (!(l->flags & MM_USE))
		return;
	if (sf & SF_MONSTERMAKER_CYCLIC)
	{
		MakeMonster(gid);
		return;
	}
	/* ToggleUse */
	if (!ShouldToggle(useType, (l->flags & MM_ACTIVE) != 0))
		return;
	if (l->flags & MM_ACTIVE)
	{
		l->flags &= ~(MM_ACTIVE | MM_THINK);
		rec_timer_cancel(gid);
		return;
	}
	l->flags |= MM_ACTIVE | MM_THINK;
	maker_think_at(gid, gtime());
}

/* CMonsterMaker::DeathNotice */
void Monstermaker_DeathNotice(gid_t16 gid, int child)
{
	rec_logic_t *l = rec_logic(gid);

	if (!l)
		return;
	l->b--;
	if (!(l->flags & MM_FADE) && mon_is_monster(child))
		MON(child)->m_makerGid = GID_NONE;
}

const game_class_t gc_MONSTERMAKER = {
	.LogicSpawn = CMonsterMaker_LogicSpawn,
	.LogicUse = CMonsterMaker_LogicUse,
	.LogicTimer = CMonsterMaker_LogicTimer,
};
