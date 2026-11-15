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
 * aisquad.c -- CSquadMonster (HLSDK dlls/squadmonster.cpp, plane.cpp):
 * squads, slots, shared enemy information. A squad monster's class sets
 * mon_vt_t.squad and the CSquadMonster overrides below in its tables.
 */
#include "game/hl/monster.h"

static int is_squad(int e) { return e >= 0 && ent_is_live(e) && mon_vt(e)->squad; }

int Sqm_InSquad(int e) { return EHGet(SQM(e)->m_hSquadLeader) >= 0; }
int Sqm_IsLeader(int e) { return EHGet(SQM(e)->m_hSquadLeader) == e; }

int Sqm_MySquadLeader(int e)
{
	int l = EHGet(SQM(e)->m_hSquadLeader);
	return l >= 0 ? l : e;
}

int Sqm_MySquadMember(int e, int i)
{
	if (i >= MAX_SQUAD_MEMBERS - 1)
		return e;
	return EHGet(SQM(e)->m_hSquadMember[i]);
}

int Sqm_OccupySlot(int e, int desired)
{
	CSquadMonster	*s = SQM(e), *l;
	int		i, mask;

	if (!Sqm_InSquad(e))
		return 1;
	if (Sqm_SquadEnemySplit(e))
	{
		s->m_iMySlot = bits_SLOT_SQUAD_SPLIT;
		return 1;
	}
	l = SQM(Sqm_MySquadLeader(e));
	if (!(desired ^ l->m_afSquadSlots))
		return 0;
	for (i = 0; i < NUM_SLOTS; i++)
	{
		mask = 1 << i;
		if ((desired & mask) && !(l->m_afSquadSlots & mask))
		{
			l->m_afSquadSlots |= mask;
			s->m_iMySlot = mask;
			return 1;
		}
	}
	return 0;
}

void Sqm_VacateSlot(int e)
{
	CSquadMonster *s = SQM(e);

	if (s->m_iMySlot != bits_NO_SLOT && Sqm_InSquad(e))
	{
		SQM(Sqm_MySquadLeader(e))->m_afSquadSlots &= ~s->m_iMySlot;
		s->m_iMySlot = bits_NO_SLOT;
	}
}

void CSquadMonster_ScheduleChange(int e) { Sqm_VacateSlot(e); }

void CSquadMonster_Killed(int e, int attacker, int iGib)
{
	Sqm_VacateSlot(e);
	if (Sqm_InSquad(e))
		Sqm_SquadRemove(Sqm_MySquadLeader(e), e);
	CBaseMonster_Killed(e, attacker, iGib);
}

/* called on the leader (e) */
void Sqm_SquadRemove(int e, int rem)
{
	CSquadMonster	*s = SQM(e), *l;
	int		i, mem, leader;

	if (rem == Sqm_MySquadLeader(e))
	{
		for (i = 0; i < MAX_SQUAD_MEMBERS - 1; i++)
		{
			mem = Sqm_MySquadMember(e, i);
			if (mem >= 0)
			{
				SQM(mem)->m_hSquadLeader = EH_NONE;
				s->m_hSquadMember[i] = EH_NONE;
			}
		}
	}
	else
	{
		leader = Sqm_MySquadLeader(e);
		l = SQM(leader);
		for (i = 0; i < MAX_SQUAD_MEMBERS - 1; i++)
		{
			/* HLSDK compares with this (the leader), not pRemove */
			if (EHGet(l->m_hSquadMember[i]) == e)
			{
				l->m_hSquadMember[i] = EH_NONE;
				break;
			}
		}
	}
	SQM(rem)->m_hSquadLeader = EH_NONE;
}

int Sqm_SquadAdd(int e, int add)
{
	CSquadMonster	*s = SQM(e);
	int		i;

	for (i = 0; i < MAX_SQUAD_MEMBERS - 1; i++)
	{
		if (EHGet(s->m_hSquadMember[i]) < 0)
		{
			s->m_hSquadMember[i] = EHANDLE(add);
			SQM(add)->m_hSquadLeader = EHANDLE(e);
			return 1;
		}
	}
	return 0;
}

/* LKPs between members: into the receiver's frame (a member carried
 * across a seam is in another layer than its squad) */
void Sqm_SquadPasteEnemyInfo(int e)
{
	int leader = Sqm_MySquadLeader(e);
	float d[3];

	if (LayerDelta(e, leader, d))
		VecAdd(SQM(e)->m_vecEnemyLKP, d, SQM(leader)->m_vecEnemyLKP);
	else
		VecCopy(SQM(e)->m_vecEnemyLKP, SQM(leader)->m_vecEnemyLKP);
}

void Sqm_SquadCopyEnemyInfo(int e)
{
	int leader = Sqm_MySquadLeader(e);
	float d[3];

	if (LayerDelta(leader, e, d))
		VecAdd(SQM(leader)->m_vecEnemyLKP, d, SQM(e)->m_vecEnemyLKP);
	else
		VecCopy(SQM(leader)->m_vecEnemyLKP, SQM(e)->m_vecEnemyLKP);
}

void Sqm_SquadMakeEnemy(int e, int enemy)
{
	int		leader, i, mem;
	CSquadMonster	*m;

	if (!Sqm_InSquad(e) || enemy < 0)
		return;
	leader = Sqm_MySquadLeader(e);
	for (i = 0; i < MAX_SQUAD_MEMBERS; i++)
	{
		mem = Sqm_MySquadMember(leader, i);
		if (mem < 0)
			continue;
		m = SQM(mem);
		if (EHGet(m->m_hEnemy) != enemy && !Mon_HasConditions((CBaseMonster *)m, bits_COND_SEE_ENEMY))
		{
			if (EHGet(m->m_hEnemy) >= 0)
				Mon_PushEnemy(mem, EHGet(m->m_hEnemy), m->m_vecEnemyLKP);
			m->m_hEnemy = EHANDLE(enemy);
			OriginIn(enemy, mem, m->m_vecEnemyLKP);
			Mon_SetConditions((CBaseMonster *)m, bits_COND_NEW_ENEMY);
		}
	}
}

int Sqm_SquadCount(int e)
{
	int leader, i, n = 0;

	if (!Sqm_InSquad(e))
		return 0;
	leader = Sqm_MySquadLeader(e);
	for (i = 0; i < MAX_SQUAD_MEMBERS; i++)
		if (Sqm_MySquadMember(leader, i) >= 0)
			n++;
	return n;
}

int Sqm_SquadRecruit(int e, int radius, int maxMembers)
{
	CSquadMonster	*s = SQM(e);
	int		count, myClass = Classify(e), r;
	col_trace_t	tr;
	float		a[3], b[3];

	if (Sqm_InSquad(e))
		return 0;
	if (maxMembers < 2)
		return 0;
	s->m_hSquadLeader = EHANDLE(e);
	count = 1;
	if (s->netname != NAME_NONE)
	{
		/* UTIL_FindEntityByString(netname): every entity with it */
		for (r = 0; r < HL64_MAX_ENTITIES; r++)
		{
			if (!ent_is_live(r) || !ent_priv(r) || CBase(r)->netname != s->netname)
				continue;
			if (is_squad(r) && !Sqm_InSquad(r) && Classify(r) == myClass && r != e)
			{
				if (!Sqm_SquadAdd(e, r))
					break;
				count++;
			}
		}
	}
	else
	{
		r = -1;
		while ((r = UTIL_FindEntityInSphere(r, ent_cv(e)->origin, (float)radius)) >= 0)
		{
			if (!is_squad(r) || r == e || !IsAlive(r) || EHGet(SQM(r)->m_pCine) >= 0)
				continue;
			if (!Sqm_InSquad(r) && Classify(r) == myClass &&
				(myClass != CLASS_ALIEN_MONSTER || ent_idc(r)->cls == ent_idc(e)->cls) &&
				SQM(r)->netname == NAME_NONE)
			{
				VecAdd(ent_cv(e)->origin, s->view_ofs, a);
				OriginIn(r, e, b);
				VecAdd(b, s->view_ofs, b);
				UTIL_TraceLine(a, b, ignore_monsters, r, &tr);
				if (tr.frac == 1.0f)
				{
					if (!Sqm_SquadAdd(e, r))
						break;
					count++;
				}
			}
		}
	}
	if (count == 1)
		s->m_hSquadLeader = EH_NONE;
	return count;
}

int CSquadMonster_CheckEnemy(int e, int enemy)
{
	CSquadMonster	*s = SQM(e);
	int		upd, en = EHGet(s->m_hEnemy);

	(void)enemy;
	upd = CBaseMonster_CheckEnemy(e, en);
	en = EHGet(s->m_hEnemy);
	if (Sqm_InSquad(e) && en == EHGet(SQM(Sqm_MySquadLeader(e))->m_hEnemy))
	{
		if (upd)
			Sqm_SquadPasteEnemyInfo(e);
		else
			Sqm_SquadCopyEnemyInfo(e);
	}
	return upd;
}

void CSquadMonster_StartMonster(int e)
{
	CSquadMonster *s = SQM(e);

	CBaseMonster_StartMonster(e);
	if ((s->m_afCapability & bits_CAP_SQUAD) && !Sqm_InSquad(e))
	{
		if (s->netname != NAME_NONE && !(s->spawnflags & SF_SQUADMONSTER_LEADER))
			return;
		Sqm_SquadRecruit(e, 1024, 4);
		if (Sqm_IsLeader(e) && ent_idc(e)->cls == CLS_MONSTER_HUMAN_GRUNT)
		{
			Mon_SetBodygroup(e, 1, 1);
			ent_cv(e)->skin = 0;
		}
	}
}

/* CPlane */
typedef struct { float n[3], d; } plane_t;
static void plane_init(plane_t *p, const float n[3], const float pt[3])
{
	VecCopy(n, p->n);
	p->d = VecDot(n, pt);
}
static int plane_front(const plane_t *p, const float pt[3]) { return VecDot(p->n, pt) - p->d >= 0; }

int Sqm_NoFriendlyFire(int e)
{
	CSquadMonster	*s = SQM(e);
	plane_t		back, left, right;
	float		c[3], ang[3], ls[3], rs[3], vl[3], sx;
	int		en, leader, i, mem;

	if (!Sqm_InSquad(e))
		return 1;
	en = EHGet(s->m_hEnemy);
	if (en < 0)
		return 0;
	CenterIn(en, e, c);
	VecSub(c, ent_cv(e)->origin, c);
	UTIL_VecToAngles(c, ang);
	UTIL_MakeVectors(ang);
	sx = (ent_cv(e)->maxs[0] - ent_cv(e)->mins[0]) * 1.5f;
	VecMA(ent_cv(e)->origin, -sx, gpGlobals.v_right, ls);
	VecMA(ent_cv(e)->origin, sx, gpGlobals.v_right, rs);
	VecScale(gpGlobals.v_right, -1, vl);
	plane_init(&left, gpGlobals.v_right, ls);
	plane_init(&right, vl, rs);
	plane_init(&back, gpGlobals.v_forward, ent_cv(e)->origin);
	leader = Sqm_MySquadLeader(e);
	for (i = 0; i < MAX_SQUAD_MEMBERS; i++)
	{
		mem = Sqm_MySquadMember(leader, i);
		if (mem >= 0 && mem != e)
		{
			float o[3];

			OriginIn(mem, e, o);
			if (plane_front(&back, o) && plane_front(&left, o) && plane_front(&right, o))
				return 0;
		}
	}
	return 1;
}

int CSquadMonster_GetIdealState(int e)
{
	CSquadMonster *s = SQM(e);

	(void)Mon_IScheduleFlags(e);
	switch (s->m_MonsterState)
	{
	case MONSTERSTATE_IDLE:
	case MONSTERSTATE_ALERT:
		if (Mon_HasConditions((CBaseMonster *)s, bits_COND_NEW_ENEMY) && Sqm_InSquad(e))
			Sqm_SquadMakeEnemy(e, EHGet(s->m_hEnemy));
		break;
	}
	return CBaseMonster_GetIdealState(e);
}

int CSquadMonster_FValidateCover(int e, const float cover[3])
{
	if (!Sqm_InSquad(e))
		return 1;
	return !Sqm_SquadMemberInRange(e, cover, 128);
}

int Sqm_SquadEnemySplit(int e)
{
	int leader, en, i, mem, me;

	if (!Sqm_InSquad(e))
		return 0;
	leader = Sqm_MySquadLeader(e);
	en = EHGet(SQM(leader)->m_hEnemy);
	for (i = 0; i < MAX_SQUAD_MEMBERS; i++)
	{
		mem = Sqm_MySquadMember(leader, i);
		if (mem < 0)
			continue;
		me = EHGet(SQM(mem)->m_hEnemy);
		if (me >= 0 && me != en)
			return 1;
	}
	return 0;
}

int Sqm_SquadMemberInRange(int e, const float loc[3], float dist)
{
	int leader, i, mem;

	if (!Sqm_InSquad(e))
		return 0;
	leader = Sqm_MySquadLeader(e);
	for (i = 0; i < MAX_SQUAD_MEMBERS; i++)
	{
		mem = Sqm_MySquadMember(leader, i);
		if (mem >= 0)
		{
			float o[3];

			OriginIn(mem, e, o);
			if (mv_dist2d(loc, o) <= dist)
				return 1;
		}
	}
	return 0;
}

const Schedule_t *CSquadMonster_GetScheduleOfType(int e, int type)
{
	if (type == SCHED_CHASE_ENEMY_FAILED)
		return &slChaseEnemyFailed[0];
	return CBaseMonster_GetScheduleOfType(e, type);
}

/* the squad members' private blocks start EHANDLEs as EH_NONE (GameCore
 * spawn inits CBaseEntity handles only) */
void Sqm_Init(int e)
{
	CSquadMonster	*s = SQM(e);
	int		i;

	s->m_hSquadLeader = EH_NONE;
	for (i = 0; i < MAX_SQUAD_MEMBERS - 1; i++)
		s->m_hSquadMember[i] = EH_NONE;
	s->m_iMySlot = bits_NO_SLOT;
}
