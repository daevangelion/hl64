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
 * aicombat.c -- the monsters' half of HLSDK dlls/combat.cpp: CGib, gibbing,
 * death and flinch activities, CBaseMonster TakeDamage / DeadTakeDamage /
 * TakeHealth / Killed / TraceAttack, CheckTraceHullAttack, the monster
 * FireBullets, TraceBleed, SpawnBlood, and the monster skill values
 * (skill.cfg defaults of the classes on the Uplink / Hazard maps).
 * Gibs are entities as in HLSDK (CGib), on the transient reserve.
 */
#include <math.h>

#include "game/hl/monster.h"

/* material hooks (GameCore material.c), used when present */
float	TEXTURETYPE_PlaySound(const col_trace_t *tr, const float src[3], const float end[3],
		int iBulletType) __attribute__((weak));
void	DecalGunshot(const col_trace_t *tr, int iBulletType) __attribute__((weak));

#define HUMAN_GIB_COUNT		6
#define ALIEN_GIB_COUNT		4

/* ---- skill (skill.cfg) -------------------------------------------------------- */

static const float sk_monHead[3] = { 3, 3, 3 };
static const float sk_monChest[3] = { 1, 1, 1 };
static const float sk_monStomach[3] = { 1, 1, 1 };
static const float sk_monArm[3] = { 1, 1, 1 };
static const float sk_monLeg[3] = { 1, 1, 1 };
static const float sk_monDmg9MM[3] = { 5, 5, 8 };
static const float sk_monDmgMP5[3] = { 3, 4, 5 };
static const float sk_monDmg12MM[3] = { 8, 10, 10 };

const float VECTOR_CONE_1DEGREES[3] = { 0.00873f, 0.00873f, 0.00873f };
const float VECTOR_CONE_2DEGREES[3] = { 0.01745f, 0.01745f, 0.01745f };
const float VECTOR_CONE_3DEGREES[3] = { 0.02618f, 0.02618f, 0.02618f };
const float VECTOR_CONE_4DEGREES[3] = { 0.03490f, 0.03490f, 0.03490f };
const float VECTOR_CONE_5DEGREES[3] = { 0.04362f, 0.04362f, 0.04362f };
const float VECTOR_CONE_6DEGREES[3] = { 0.05234f, 0.05234f, 0.05234f };
const float VECTOR_CONE_7DEGREES[3] = { 0.06105f, 0.06105f, 0.06105f };
const float VECTOR_CONE_8DEGREES[3] = { 0.06976f, 0.06976f, 0.06976f };
const float VECTOR_CONE_9DEGREES[3] = { 0.07846f, 0.07846f, 0.07846f };
const float VECTOR_CONE_10DEGREES[3] = { 0.08716f, 0.08716f, 0.08716f };
const float VECTOR_CONE_15DEGREES[3] = { 0.13053f, 0.13053f, 0.13053f };
const float VECTOR_CONE_20DEGREES[3] = { 0.17365f, 0.17365f, 0.17365f };

/* ---- CGib ------------------------------------------------------------------------ */

#define GIB(e)	((CGib *)ent_priv(e))

static int CGib_SpawnEntry(int e, gid_t16 gid)
{
	(void)e; (void)gid;
	return 0;
}

void CGib_Spawn(int e, res_t model)
{
	CGib *g = GIB(e);

	ent_set_movetype(e, MOVETYPE_BOUNCE);
	ent_phys(e)->friction = 0.55f;
	ent_rnd(e)->renderamt = 255;
	ent_cv(e)->rendermode = kRenderNormal;
	ent_rnd(e)->renderfx = kRenderFxNone;
	ent_set_solid(e, SOLID_SLIDEBOX);
	ent_set_model(e, model);
	ent_set_size(e, g_vecZero, g_vecZero);
	ThinkIn(e, 4);
	g->m_lifeTime = 25;
	SetThink(e, FN_CGib_WaitTillLand);
	SetTouch(e, FN_CGib_BounceGibTouch);
	g->m_material = -1;	/* matNone */
	g->m_cBloodDecals = 5;
}

static void CGib_LimitVelocity(int e)
{
	float	v[3], len = VecLen(ent_mot(e)->velocity);

	if (len > 1500.0f)
	{
		VecNorm(ent_mot(e)->velocity, v);
		VecScale(v, 1500, v);
		ent_set_velocity(e, v);
	}
}

int CGib_Create(void)
{
	float z[3] = { 0, 0, 0 };

	return CBaseEntity_Create(CLS_GIB, z, z, -1);
}

static int new_gib(res_t model)
{
	int	g = CGib_Create();

	if (g >= 0)
		CGib_Spawn(g, model);
	return g;
}

static void gib_speed(int g, int victim)
{
	float h = CBase(victim)->health, v[3];

	VecCopy(ent_mot(g)->velocity, v);
	if (h > -50)
		VecScale(v, 0.7f, v);
	else if (h > -200)
		VecScale(v, 2, v);
	else
		VecScale(v, 4, v);
	ent_set_velocity(g, v);
}

void CGib_SpawnHeadGib(int victim)
{
	int	g = new_gib(RESID(MON_HGIBS)), pl;
	float	o[3], v[3], av[3], d[3];

	if (g < 0)
		return;
	ent_rnd(g)->body = 0;
	if (victim >= 0)
	{
		VecAdd(ent_cv(victim)->origin, CBase(victim)->view_ofs, o);
		ent_set_origin(g, o);
		pl = ent_client_in_pvs(g);
		if (RANDOM_LONG(0, 100) <= 5 && pl > 0)
		{
			EyePosition(pl, d);
			VecSub(d, o, d);
			VecNorm(d, v);
			VecScale(v, 300, v);
			v[2] += 100;
		}
		else
		{
			/* draws in source order: argument evaluation order is
			 * unspecified (host x86 and cart MIPS differ, K10) */
			v[0] = RANDOM_FLOAT(-100, 100);
			v[1] = RANDOM_FLOAT(-100, 100);
			v[2] = RANDOM_FLOAT(200, 300);
		}
		ent_set_velocity(g, v);
		av[0] = RANDOM_FLOAT(100, 200);
		av[1] = RANDOM_FLOAT(100, 300);
		av[2] = 0;
		ent_set_avelocity(g, av);
		GIB(g)->m_bloodColor = BloodColor(victim);
		gib_speed(g, victim);
	}
	CGib_LimitVelocity(g);
}

void CGib_SpawnRandomGibs(int victim, int cGibs, int human)
{
	int		i, g;
	float		o[3], v[3], av[3];
	const col_ent_t	*vc;

	for (i = 0; i < cGibs; i++)
	{
		g = new_gib(human ? RESID(MON_HGIBS) : RESID(MON_AGIBS));
		if (g < 0)
			return;
		ent_rnd(g)->body = (uint8_t)(human ? RANDOM_LONG(1, HUMAN_GIB_COUNT - 1) :
			RANDOM_LONG(0, ALIEN_GIB_COUNT - 1));
		if (victim >= 0)
		{
			vc = ent_cv(victim);
			o[0] = vc->absmin[0] + (vc->maxs[0] - vc->mins[0]) * RANDOM_FLOAT(0, 1);
			o[1] = vc->absmin[1] + (vc->maxs[1] - vc->mins[1]) * RANDOM_FLOAT(0, 1);
			o[2] = vc->absmin[2] + (vc->maxs[2] - vc->mins[2]) * RANDOM_FLOAT(0, 1) + 1;
			ent_set_origin(g, o);
			VecScale(g_vecAttackDir, -1, v);
			v[0] += RANDOM_FLOAT(-0.25f, 0.25f);
			v[1] += RANDOM_FLOAT(-0.25f, 0.25f);
			v[2] += RANDOM_FLOAT(-0.25f, 0.25f);
			VecScale(v, RANDOM_FLOAT(300, 400), v);
			ent_set_velocity(g, v);
			av[0] = RANDOM_FLOAT(100, 200);
			av[1] = RANDOM_FLOAT(100, 300);
			av[2] = 0;
			ent_set_avelocity(g, av);
			GIB(g)->m_bloodColor = BloodColor(victim);
			gib_speed(g, victim);
			ent_set_solid(g, SOLID_BBOX);
			ent_set_size(g, g_vecZero, g_vecZero);
		}
		CGib_LimitVelocity(g);
	}
}

void CGib_SpawnStickyGibs(int victim, const float org[3], int cGibs)
{
	int	i, g;
	float	o[3], v[3], av[3];

	for (i = 0; i < cGibs; i++)
	{
		g = new_gib(RESID(MON_STICKYGIB));
		if (g < 0)
			return;
		ent_rnd(g)->body = (uint8_t)RANDOM_LONG(0, 2);
		if (victim >= 0)
		{
			o[0] = org[0] + RANDOM_FLOAT(-3, 3);
			o[1] = org[1] + RANDOM_FLOAT(-3, 3);
			o[2] = org[2] + RANDOM_FLOAT(-3, 3);
			ent_set_origin(g, o);
			VecScale(g_vecAttackDir, -1, v);
			v[0] += RANDOM_FLOAT(-0.15f, 0.15f);
			v[1] += RANDOM_FLOAT(-0.15f, 0.15f);
			v[2] += RANDOM_FLOAT(-0.15f, 0.15f);
			VecScale(v, 900, v);
			ent_set_velocity(g, v);
			av[0] = RANDOM_FLOAT(250, 400);
			av[1] = RANDOM_FLOAT(250, 400);
			av[2] = 0;
			ent_set_avelocity(g, av);
			GIB(g)->m_bloodColor = BloodColor(victim);
			gib_speed(g, victim);
			ent_set_movetype(g, MOVETYPE_TOSS);
			ent_set_solid(g, SOLID_BBOX);
			ent_set_size(g, g_vecZero, g_vecZero);
			SetTouch(g, FN_CGib_StickyGibTouch);
			SetThink(g, FN_NONE);
		}
		CGib_LimitVelocity(g);
	}
}

void CGib_StickyGibTouch(int e, int other)
{
	col_trace_t	tr;
	float		end[3], n[3];

	SetThink(e, FN_SUB_Remove);
	ThinkIn(e, 10);
	if (other != ENT_WORLD)
	{
		ent_think_at(e, gtime());
		return;
	}
	VecMA(ent_cv(e)->origin, 32, ent_mot(e)->velocity, end);
	UTIL_TraceLine(ent_cv(e)->origin, end, ignore_monsters, e, &tr);
	UTIL_BloodDecalTrace(&tr, GIB(e)->m_bloodColor);
	VecScale(tr.normal, -1, n);
	UTIL_VecToAngles(n, ent_cv(e)->angles);
	ent_set_velocity(e, g_vecZero);
	ent_set_avelocity(e, g_vecZero);
	ent_set_movetype(e, MOVETYPE_NONE);
}

void CGib_WaitTillLand(int e)
{
	CGib *g = GIB(e);

	if (!IsInWorld(e))
	{
		UTIL_Remove(e);
		return;
	}
	if (VecIsZero(ent_mot(e)->velocity))
	{
		SetThink(e, FN_SUB_StartFadeOut);
		ThinkIn(e, g->m_lifeTime);
		if (g->m_bloodColor != DONT_BLEED)
			CSoundEnt_InsertSound(bits_SOUND_MEAT, ent_cv(e)->origin, 384, 25);
	}
	else
		ThinkIn(e, 0.5f);
}

void CGib_BounceGibTouch(int e, int other)
{
	CGib		*g = GIB(e);
	float		v[3], s[3], t[3], av[3];
	col_trace_t	tr;

	(void)other;
	if (ent_cv(e)->flags & FL_ONGROUND)
	{
		VecScale(ent_mot(e)->velocity, 0.9f, v);
		ent_set_velocity(e, v);
		ent_cv(e)->angles[0] = 0;
		ent_cv(e)->angles[2] = 0;
		VecCopy(ent_mot(e)->avelocity, av);
		av[0] = av[2] = 0;
		ent_set_avelocity(e, av);
	}
	else if (g->m_cBloodDecals > 0 && g->m_bloodColor != DONT_BLEED)
	{
		VecCopy(ent_cv(e)->origin, s);
		s[2] += 8;
		VecCopy(s, t);
		t[2] -= 24;
		UTIL_TraceLine(s, t, ignore_monsters, e, &tr);
		UTIL_BloodDecalTrace(&tr, g->m_bloodColor);
		g->m_cBloodDecals--;
	}
}

const game_class_t gc_GIB = {
	.size = sizeof(CGib),
	.Spawn = CGib_SpawnEntry,
	.save = &sv_Transient,
};


/* ---- monster gibbing and death ------------------------------------------------------ */

void CBaseMonster_FadeMonster(int e)
{
	ent_rnd(e)->framerate = 0;
	ent_set_velocity(e, g_vecZero);
	ent_set_movetype(e, MOVETYPE_NONE);
	ent_set_avelocity(e, g_vecZero);
	ent_rnd(e)->animtime = gtime();
	ent_rnd(e)->effects |= EF_NOINTERP;
	SUB_StartFadeOut(e);
}

void CBaseMonster_GibMonster(int e)
{
	int gibbed = 0;

	EMIT_SOUND(e, CHAN_WEAPON, RESID(MON_BODYSPLAT), 1, ATTN_NORM);
	if (Mon_HasHumanGibs(e))
	{
		CGib_SpawnHeadGib(e);
		CGib_SpawnRandomGibs(e, 4, 1);
		gibbed = 1;
	}
	else if (Mon_HasAlienGibs(e))
	{
		CGib_SpawnRandomGibs(e, 4, 0);
		gibbed = 1;
	}
	if (!IsPlayer(e))
	{
		if (gibbed)
		{
			SetThink(e, FN_SUB_Remove);
			ent_think_at(e, gtime());
		}
		else
			Mon_FadeMonster(e);
	}
}

int CBaseMonster_GetDeathActivity(int e)
{
	CBaseMonster	*m = MON(e);
	int		act = ACT_DIESIMPLE, tried = 0;
	float		dot, src[3], d[3], end[3];
	col_trace_t	tr;

	if (m->deadflag != DEAD_NO)
		return m->m_IdealActivity;
	Center(e, src);
	UTIL_MakeVectors(ent_cv(e)->angles);
	VecScale(g_vecAttackDir, -1, d);
	dot = VecDot(gpGlobals.v_forward, d);
	switch (m->m_LastHitGroup)
	{
	case HITGROUP_HEAD:
		act = ACT_DIE_HEADSHOT;
		break;
	case HITGROUP_STOMACH:
		act = ACT_DIE_GUTSHOT;
		break;
	default:
		tried = 1;
		if (dot > 0.3f)
			act = ACT_DIEFORWARD;
		else if (dot <= -0.3f)
			act = ACT_DIEBACKWARD;
		break;
	}
	if (Mon_LookupActivity(e, act) == ACTIVITY_NOT_AVAILABLE)
	{
		if (tried)
			act = ACT_DIESIMPLE;
		else if (dot > 0.3f)
			act = ACT_DIEFORWARD;
		else if (dot <= -0.3f)
			act = ACT_DIEBACKWARD;
	}
	if (Mon_LookupActivity(e, act) == ACTIVITY_NOT_AVAILABLE)
		act = ACT_DIESIMPLE;
	if (act == ACT_DIEFORWARD)
	{
		VecMA(src, 64, gpGlobals.v_forward, end);
		UTIL_TraceHull(src, end, dont_ignore_monsters, head_hull, e, &tr);
		if (tr.frac != 1.0f)
			act = ACT_DIESIMPLE;
	}
	if (act == ACT_DIEBACKWARD)
	{
		VecMA(src, -64, gpGlobals.v_forward, end);
		UTIL_TraceHull(src, end, dont_ignore_monsters, head_hull, e, &tr);
		if (tr.frac != 1.0f)
			act = ACT_DIESIMPLE;
	}
	return act;
}

int Mon_GetSmallFlinchActivity(int e)
{
	int act;

	UTIL_MakeVectors(ent_cv(e)->angles);
	switch (MON(e)->m_LastHitGroup)
	{
	case HITGROUP_HEAD: act = ACT_FLINCH_HEAD; break;
	case HITGROUP_STOMACH: act = ACT_FLINCH_STOMACH; break;
	case HITGROUP_LEFTARM: act = ACT_FLINCH_LEFTARM; break;
	case HITGROUP_RIGHTARM: act = ACT_FLINCH_RIGHTARM; break;
	case HITGROUP_LEFTLEG: act = ACT_FLINCH_LEFTLEG; break;
	case HITGROUP_RIGHTLEG: act = ACT_FLINCH_RIGHTLEG; break;
	default: act = ACT_SMALL_FLINCH; break;
	}
	if (Mon_LookupActivity(e, act) == ACTIVITY_NOT_AVAILABLE)
		act = ACT_SMALL_FLINCH;
	return act;
}

void CBaseMonster_BecomeDead(int e)
{
	CBaseMonster *m = MON(e);

	m->takedamage = DAMAGE_YES;
	m->health = m->max_health / 2;
	m->max_health = 5;
	ent_set_movetype(e, MOVETYPE_TOSS);
}

int Mon_ShouldGibMonster(int e, int iGib)
{
	return (iGib == GIB_NORMAL && MON(e)->health < GIB_HEALTH_VALUE) || iGib == GIB_ALWAYS;
}

void Mon_CallGibMonster(int e)
{
	CBaseMonster *m = MON(e);

	m->takedamage = DAMAGE_NO;
	ent_set_solid(e, SOLID_NOT);
	ent_rnd(e)->effects = EF_NODRAW;
	Mon_GibMonster(e);
	if (!ent_is_live(e))
		return;
	m->deadflag = DEAD_DEAD;
	Mon_FCheckAITrigger(e);
	if (m->health < -99)
		m->health = 0;
	if (Mon_ShouldFadeOnDeath(e))
		UTIL_Remove(e);
}

void Monstermaker_DeathNotice(gid_t16 maker, int child);

void CBaseMonster_Killed(int e, int attacker, int iGib)
{
	CBaseMonster	*m = MON(e);
	uint16_t	owner;

	(void)attacker;
	if (Mon_HasMemory(m, bits_MEMORY_KILLED))
	{
		if (Mon_ShouldGibMonster(e, iGib))
			Mon_CallGibMonster(e);
		return;
	}
	Mon_Remember(m, bits_MEMORY_KILLED);
	EMIT_SOUND(e, CHAN_WEAPON, RESID(SND_NULL), 1, ATTN_NORM);
	m->m_IdealMonsterState = MONSTERSTATE_DEAD;
	Mon_SetConditions(m, bits_COND_LIGHT_DAMAGE);
	/* pOwner->DeathNotice: monstermaker children (logic records) by gid,
	 * entity owners through their class */
	if (m->m_makerGid != GID_NONE)
		Monstermaker_DeathNotice(m->m_makerGid, e);
	owner = ent_cv(e)->owner;
	if (owner != COL_NONE && ent_is_live(owner) && ent_vt(owner) && ent_vt(owner)->DeathNotice)
		ent_vt(owner)->DeathNotice(owner, e);
	if (Mon_ShouldGibMonster(e, iGib))
	{
		Mon_CallGibMonster(e);
		return;
	}
	else if (ent_cv(e)->flags & FL_MONSTER)
	{
		SetTouch(e, FN_NONE);
		Mon_BecomeDead(e);
	}
	if (m->health < -99)
		m->health = 0;
	m->m_IdealMonsterState = MONSTERSTATE_DEAD;
}

/* ---- damage ------------------------------------------------------------------------- */

int CBaseMonster_TakeHealth(int e, float flHealth, int bits)
{
	CBaseMonster *m = MON(e);

	if (!m->takedamage)
		return 0;
	m->m_bitsDamageType &= ~(bits & ~DMG_TIMEBASED);
	return CBaseEntity_TakeHealth(e, flHealth, bits);
}

float Mon_DamageForce(int e, float damage)
{
	const col_ent_t	*c = ent_cv(e);
	float		vol = (c->maxs[0] - c->mins[0]) * (c->maxs[1] - c->mins[1]) * (c->maxs[2] - c->mins[2]);
	float		force = damage * ((32 * 32 * 72.0f) / vol) * 5;

	return force > 1000.0f ? 1000.0f : force;
}

static void attack_dir(int e, int inflictor, float out[3])
{
	float a[3], b[3];

	VecSet(out, 0, 0, 0);
	if (inflictor >= 0 && ent_is_live(inflictor))
	{
		Center(inflictor, a);
		a[2] -= 10;
		Center(e, b);
		VecSub(a, b, a);
		VecNorm(a, out);
		VecNorm(out, g_vecAttackDir);
		VecCopy(g_vecAttackDir, out);
	}
}

int CBaseMonster_TakeDamage(int e, int inflictor, int attacker, float flDamage, int bits)
{
	CBaseMonster	*m = MON(e);
	float		dir[3], v[3];
	int		en;

	if (!m->takedamage)
		return 0;
	if (!IsAlive(e))
		return CBaseMonster_DeadTakeDamage(e, inflictor, attacker, flDamage, bits);
	if (m->deadflag == DEAD_NO)
		Mon_PainSound(e);
	m->m_bitsDamageType |= bits;
	attack_dir(e, inflictor, dir);
	if (inflictor >= 0 && ent_cv(e)->movetype == MOVETYPE_WALK &&
		(attacker < 0 || ent_cv(attacker)->solid != SOLID_TRIGGER))
	{
		VecMA(ent_mot(e)->velocity, -Mon_DamageForce(e, flDamage), dir, v);
		ent_set_velocity(e, v);
	}
	m->health -= flDamage;
	if (m->m_MonsterState == MONSTERSTATE_SCRIPT)
	{
		Mon_SetConditions(m, bits_COND_LIGHT_DAMAGE);
		return 0;
	}
	if (m->health <= 0)
	{
		if (bits & DMG_ALWAYSGIB)
			Killed(e, attacker, GIB_ALWAYS);
		else if (bits & DMG_NEVERGIB)
			Killed(e, attacker, GIB_NEVER);
		else
			Killed(e, attacker, GIB_NORMAL);
		return 0;
	}
	if ((ent_cv(e)->flags & FL_MONSTER) && attacker >= 0 &&
		(ent_cv(attacker)->flags & (FL_MONSTER | FL_CLIENT)))
	{
		if (inflictor >= 0)
		{
			en = EHGet(m->m_hEnemy);
			if (en < 0 || inflictor == en || !Mon_HasConditions(m, bits_COND_SEE_ENEMY))
				VecCopy(ent_cv(inflictor)->origin, m->m_vecEnemyLKP);
		}
		else
			VecMA(ent_cv(e)->origin, 64, g_vecAttackDir, m->m_vecEnemyLKP);
		Mon_MakeIdealYaw(e, m->m_vecEnemyLKP);
		if (flDamage > 0)
			Mon_SetConditions(m, bits_COND_LIGHT_DAMAGE);
		if (flDamage >= 20)
			Mon_SetConditions(m, bits_COND_HEAVY_DAMAGE);
	}
	return 1;
}

int CBaseMonster_DeadTakeDamage(int e, int inflictor, int attacker, float flDamage, int bits)
{
	CBaseMonster	*m = MON(e);
	float		dir[3];

	attack_dir(e, inflictor, dir);
	if (bits & DMG_GIB_CORPSE)
	{
		if (m->health <= flDamage)
		{
			m->health = -50;
			Killed(e, attacker, GIB_ALWAYS);
			return 0;
		}
		m->health -= flDamage * 0.1f;
	}
	return 1;
}

void Mon_MakeDamageBloodDecal(int e, int cCount, float flNoise, const col_trace_t *ptr, const float dir[3])
{
	CBaseMonster	*m = MON(e);
	col_trace_t	btr;
	float		td[3], end[3];
	int		i;

	if (!IsAlive(e))
	{
		if (m->max_health <= 0)
			return;
		m->max_health--;
	}
	for (i = 0; i < cCount; i++)
	{
		VecCopy(dir, td);
		td[0] += RANDOM_FLOAT(-flNoise, flNoise);
		td[1] += RANDOM_FLOAT(-flNoise, flNoise);
		td[2] += RANDOM_FLOAT(-flNoise, flNoise);
		VecMA(ptr->end, 172, td, end);
		UTIL_TraceLine(ptr->end, end, ignore_monsters, e, &btr);
		if (btr.frac != 1.0f)
			UTIL_BloodDecalTrace(&btr, BloodColor(e));
	}
}

void CBaseMonster_TraceAttack(int e, int attacker, float flDamage, const float dir[3],
	const col_trace_t *tr, int bits)
{
	CBaseMonster *m = MON(e);

	if (!m->takedamage)
		return;
	m->m_LastHitGroup = tr->hitgroup;
	switch (tr->hitgroup)
	{
	case HITGROUP_HEAD: flDamage *= mon_skill(sk_monHead); break;
	case HITGROUP_CHEST: flDamage *= mon_skill(sk_monChest); break;
	case HITGROUP_STOMACH: flDamage *= mon_skill(sk_monStomach); break;
	case HITGROUP_LEFTARM:
	case HITGROUP_RIGHTARM: flDamage *= mon_skill(sk_monArm); break;
	case HITGROUP_LEFTLEG:
	case HITGROUP_RIGHTLEG: flDamage *= mon_skill(sk_monLeg); break;
	default: break;
	}
	SpawnBlood(tr->end, BloodColor(e), flDamage);
	TraceBleed(e, flDamage, dir, tr, bits);
	AddMultiDamage(attacker, e, flDamage, bits);
}

int Mon_CheckTraceHullAttack(int e, float flDist, int iDamage, int iDmgType)
{
	col_trace_t	tr;
	float		s[3], end[3];
	const col_ent_t	*c = ent_cv(e);

	if (IsPlayer(e))
		UTIL_MakeVectors(c->angles);
	else
		UTIL_MakeAimVectors(c->angles);
	VecCopy(c->origin, s);
	s[2] += (c->maxs[2] - c->mins[2]) * 0.5f;
	VecMA(s, flDist, gpGlobals.v_forward, end);
	UTIL_TraceHull(s, end, dont_ignore_monsters, head_hull, e, &tr);
	if (tr.ent > 0 || (tr.ent == 0 && tr.frac < 1.0f))
	{
		if (iDamage > 0)
			TakeDamage(tr.ent, e, e, (float)iDamage, iDmgType);
		return tr.ent;
	}
	return -1;
}

/* CBaseEntity::FireBullets, the monsters' version */
void FireBullets(int e, int cShots, const float src[3], const float dirShooting[3],
	const float spread[3], float flDistance, int iBulletType, int iTracerFreq, int iDamage,
	int attacker)
{
	static int	tracerCount;
	col_trace_t	tr;
	float		right[3], up[3], dir[3], end[3], x, y, z;
	int		shot, tracer, hit;

	VecCopy(gpGlobals.v_right, right);
	VecCopy(gpGlobals.v_up, up);
	if (attacker < 0)
		attacker = e;
	ClearMultiDamage();
	for (shot = 1; shot <= cShots; shot++)
	{
		do {
			x = RANDOM_FLOAT(-0.5f, 0.5f) + RANDOM_FLOAT(-0.5f, 0.5f);
			y = RANDOM_FLOAT(-0.5f, 0.5f) + RANDOM_FLOAT(-0.5f, 0.5f);
			z = x * x + y * y;
		} while (z > 1);
		VecMA(dirShooting, x * spread[0], right, dir);
		VecMA(dir, y * spread[1], up, dir);
		VecMA(src, flDistance, dir, end);
		UTIL_TraceLine(src, end, dont_ignore_monsters, e, &tr);
		tracer = 0;
		if (iTracerFreq != 0 && (tracerCount++ % iTracerFreq) == 0)
		{
			if (iTracerFreq != 1)
				tracer = 1;
			fx_tracer(src, tr.end);
		}
		if (tr.frac != 1.0f)
		{
			hit = tr.ent >= 0 ? tr.ent : 0;
			if (iDamage)
			{
				TraceAttack(hit, attacker, (float)iDamage, dir, &tr,
					DMG_BULLET | (iDamage > 16 ? DMG_ALWAYSGIB : DMG_NEVERGIB));
				if (TEXTURETYPE_PlaySound)
					TEXTURETYPE_PlaySound(&tr, src, end, iBulletType);
				if (DecalGunshot)
					DecalGunshot(&tr, iBulletType);
			}
			else
			{
				float dmg;

				switch (iBulletType)
				{
				default:
				case BULLET_MONSTER_9MM: dmg = mon_skill(sk_monDmg9MM); break;
				case BULLET_MONSTER_MP5: dmg = mon_skill(sk_monDmgMP5); break;
				case BULLET_MONSTER_12MM: dmg = mon_skill(sk_monDmg12MM); break;
				}
				TraceAttack(hit, attacker, dmg, dir, &tr, DMG_BULLET);
				if (iBulletType != BULLET_MONSTER_12MM || !tracer)
				{
					if (TEXTURETYPE_PlaySound)
						TEXTURETYPE_PlaySound(&tr, src, end, iBulletType);
					if (DecalGunshot)
						DecalGunshot(&tr, iBulletType);
				}
			}
		}
		/* UTIL_BubbleTrail: no underwater bubbles in the effects API */
	}
	ApplyMultiDamage(e, attacker);
}
