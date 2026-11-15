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
 * garg.c -- monster_gargantua (HLSDK dlls/gargantua.cpp CGargantua) and
 * its helpers garg_stomp (CStomp), streak_spiral (CSpiral), env_smoker
 * (CSmoker).
 *
 * Effects on the fx.h temp-entity API instead of CSprite / CBeam
 * entities: the eye glow is a TE_GLOWSPRITE renewed every think at the
 * eye attachment with the approached brightness; the four flame beams are
 * point-to-point beams from the flame end to the gun attachment, renewed
 * every FlameUpdate (0.2 s life); the stomp's flying sprites are glow
 * sprites (0.3 s) on the floor; TE_STREAK_SPLASH is fx_particles. The
 * death explosions (SpawnExplosion at 0, 0.6, 1.2, 1.8 s) fire from the
 * dying gargantua's TASK_DIE (the last one when it is removed, 1.75 s).
 */
#include <math.h>

#include "hl64/fx.h"
#include "engine/phys/player.h"
#include "game/hl/breakable.h"
#include "game/hl/gib.h"
#include "game/hl/monster.h"
#include "game/hl/savegame.h"

#define GARG_ATTACKDIST		80.0f
#define GARG_AE_SLASH_LEFT	1
#define GARG_AE_LEFT_FOOT	3
#define GARG_AE_RIGHT_FOOT	4
#define GARG_AE_STOMP		5
#define GARG_AE_BREATHE		6
/* damage types that can hurt the garg */
#define GARG_DAMAGE		(DMG_ENERGYBEAM | DMG_CRUSH | DMG_MORTAR | DMG_BLAST)
#define GARG_FLAME_LENGTH	330
#define ATTN_GARG		ATTN_NORM
#define STOMP_INTERVAL		0.025f
#define SPIRAL_INTERVAL		0.1f
#define BREAK_FLESH		0x04

static const float sk_gargantuaHealth[3] = { 800, 800, 1000 };
static const float sk_gargantuaDmgSlash[3] = { 10, 30, 30 };
static const float sk_gargantuaDmgFire[3] = { 3, 5, 5 };
static const float sk_gargantuaDmgStomp[3] = { 50, 100, 100 };

enum {
	TASK_SOUND_ATTACK = LAST_COMMON_TASK + 1,
	TASK_FLAME_SWEEP,
};

typedef struct {
	CBASEENTITY_FIELDS
	CBASEDELAY_FIELDS
	CBASEANIMATING_FIELDS
	CBASETOGGLE_FIELDS
	CBASEMONSTER_FIELDS
	int16_t		m_pFlame[4];	/* fx beam keys, -1 none */
	uint8_t		m_flameOn;
	uint8_t		m_eyeOn;	/* the eye glow exists (until Killed) */
	uint8_t		m_eyeRenderamt;	/* m_pEyeGlow->pev->renderamt */
	uint8_t		m_explosions;	/* death explosions fired */
	int		m_eyeBrightness;
	float		m_seeTime;	/* time to attack (when I see the enemy, I set this) */
	float		m_flameTime;	/* time of next flame attack */
	float		m_painSoundTime;
	float		m_streakTime;	/* last time we created streaks */
	float		m_flameX;	/* flame thrower aim */
	float		m_flameY;
	float		m_dieTime;	/* TASK_DIE start */
} CGargantua;

#define GARG(e)	((CGargantua *)ent_priv(e))

static const int16_t gaAttackHit[] = { R_ZO_CLAW_STRIKE1, R_ZO_CLAW_STRIKE2, R_ZO_CLAW_STRIKE3 };
static const int16_t gaBeamAttack[] = { R_GA_FLAMEOFF1, R_GA_FLAMEON1, R_GA_FLAMERUN1 };
static const int16_t gaAttackMiss[] = { R_ZO_CLAW_MISS1, R_MON_CLAW_MISS2 };
static const int16_t gaFoot[] = { R_GA_STEP1, R_GA_STEP2 };
static const int16_t gaAttack[] = { R_GA_ATTACK1, R_GA_ATTACK2, R_GA_ATTACK3 };
static const int16_t gaPain[] = { R_GA_PAIN1, R_GA_PAIN2, R_GA_PAIN3 };
static const int16_t gaStomp[] = { R_GA_STOMP1 };
static const int16_t gaBreathe[] = { R_GA_BREATHE1, R_GA_BREATHE2, R_GA_BREATHE3 };

#define T(t, d)	{ t, (float)(d) }

static const Task_t tlGargFlame[] = {
	T(TASK_STOP_MOVING, 0), T(TASK_FACE_ENEMY, 0), T(TASK_SOUND_ATTACK, 0),
	T(TASK_SET_ACTIVITY, ACT_MELEE_ATTACK2), T(TASK_FLAME_SWEEP, 4.5),
	T(TASK_SET_ACTIVITY, ACT_IDLE),
};
static const Schedule_t slGargFlame[] = { SCHED(tlGargFlame, 0, 0, "GargFlame") };

static const Task_t tlGargSwipe[] = {
	T(TASK_STOP_MOVING, 0), T(TASK_FACE_ENEMY, 0), T(TASK_MELEE_ATTACK1, 0),
};
static const Schedule_t slGargSwipe[] = {
	SCHED(tlGargSwipe, bits_COND_CAN_MELEE_ATTACK2, 0, "GargSwipe")
};

/* saved games: append only (monster.h sched_list_t) */
SCHED_LIST(sched_garg, slGargFlame, slGargSwipe);

/* TE_STREAK_SPLASH */
static void StreakSplash(const float org[3], const float dir[3], int color, int count)
{
	fx_particles(org, dir, color, count);
}

/* ---- garg_stomp (CStomp) ------------------------------------------------------------- */

typedef struct {
	CBASEENTITY_FIELDS
	float		scale;		/* pev->scale: distance left */
	float		framerate;	/* pev->framerate: acceleration */
} CStomp;

#define STOMP(e)	((CStomp *)ent_priv(e))

void CStomp_Think(int e)
{
	CStomp		*s = STOMP(e);
	col_trace_t	tr;
	float		start[3], end[3], down[3], dt = (float)eng_frametime();
	int		owner = ent_cv(e)->owner != COL_NONE ? ent_cv(e)->owner : -1, hit, i;

	ThinkIn(e, 0.1f);
	/* exist for at most 1 second */
	VecCopy(ent_cv(e)->origin, start);
	start[2] += 30;
	VecMA(start, s->speed * dt, s->movedir, end);
	UTIL_TraceHull(start, end, dont_ignore_monsters, head_hull, e, &tr);
	hit = tr.ent;
	if (hit > 0 && hit != owner)
		TakeDamage(hit, e, owner >= 0 ? owner : e, mon_skill(sk_gargantuaDmgStomp), DMG_SONIC);
	/* accelerate the effect */
	s->speed += dt * s->framerate;
	s->framerate += dt * 1500;
	/* move and spawn trails */
	while (gtime() - s->dmgtime > STOMP_INTERVAL)
	{
		VecMA(ent_cv(e)->origin, s->speed * STOMP_INTERVAL, s->movedir, end);
		ent_set_origin(e, end);
		for (i = 0; i < 2; i++)
		{
			VecCopy(end, down);
			down[2] -= 500;
			UTIL_TraceLine(end, down, ignore_monsters, e, &tr);
			fx_glow_sprite(tr.end, RESID(GA_EYE_SPR), 3, 10, 255);
		}
		s->dmgtime += STOMP_INTERVAL;
		/* scale has the "life" of this effect */
		s->scale -= STOMP_INTERVAL * s->speed;
		if (s->scale <= 0)
		{
			/* life has run out, remove */
			UTIL_Remove(e);
			STOP_SOUND(e, CHAN_BODY, RESID(W_MINE_CHARGE));
			return;
		}
	}
}

static int CStomp_Spawn(int e, gid_t16 gid)
{
	(void)gid;
	ThinkIn(e, 0);
	STOMP(e)->dmgtime = gtime();
	STOMP(e)->framerate = 30;
	ent_set_solid(e, SOLID_NOT);
	ent_set_movetype(e, MOVETYPE_NONE);
	SetThink(e, FN_CStomp_Think);
	EMIT_SOUND_DYN(e, CHAN_BODY, RESID(W_MINE_CHARGE), 1, ATTN_NORM, 0, (int)(PITCH_NORM * 0.55f));
	return 0;
}

const game_class_t gc_GARG_STOMP = {
	.size = sizeof(CStomp),
	.Spawn = CStomp_Spawn,
	.save = &sv_Transient,
};

static void StompCreate(int owner, const float origin[3], const float end[3], float speed)
{
	float	dir[3], ang[3] = { 0, 0, 0 };
	int	s = CBaseEntity_Create(CLS_GARG_STOMP, origin, ang, owner);

	if (s < 0)
		return;
	VecSub(end, origin, dir);
	STOMP(s)->scale = VecLen(dir);
	VecNorm(dir, STOMP(s)->movedir);
	STOMP(s)->speed = speed;
}

/* ---- streak_spiral (CSpiral) ------------------------------------------------------------ */

/* pev->dmgtime (last step), scale (radius), dmg (height), speed
 * (duration), health (elapsed) */
void CSpiral_Think(int e)
{
	CBaseEntity	*p = CBase(e);
	float		t = gtime() - p->dmgtime, pos[3], ang[3] = { 0, 0, 0 }, up[3] = { 0, 0, 1 };
	float		fraction, radius;

	while (t > SPIRAL_INTERVAL)
	{
		VecCopy(ent_cv(e)->origin, pos);
		fraction = 1.0f / p->speed;
		radius = (p->max_health * p->health) * fraction;
		pos[2] += (p->health * p->dmg) * fraction;
		ang[1] = (p->health * 360 * 8) * fraction;
		UTIL_MakeVectors(ang);
		VecMA(pos, radius, gpGlobals.v_forward, pos);
		StreakSplash(pos, up, RANDOM_LONG(8, 11), 20);
		p->dmgtime += SPIRAL_INTERVAL;
		p->health += SPIRAL_INTERVAL;
		t -= SPIRAL_INTERVAL;
	}
	ThinkIn(e, 0);
	if (p->health >= p->speed)
		UTIL_Remove(e);
}

static int CSpiral_Spawn(int e, gid_t16 gid)
{
	(void)gid;
	ent_set_movetype(e, MOVETYPE_NONE);
	ent_set_solid(e, SOLID_NOT);
	ent_rnd(e)->effects |= EF_NODRAW;
	ThinkIn(e, 0);
	SetThink(e, FN_CSpiral_Think);
	return 0;
}

const game_class_t gc_STREAK_SPIRAL = {
	.size = sizeof(CBaseEntity),
	.Spawn = CSpiral_Spawn,
	.save = &sv_Transient,
};

/* CSpiral::Create; the radius is kept in max_health (pev->scale) */
static void SpiralCreate(const float origin[3], float height, float radius, float duration)
{
	float	ang[3] = { 0, 0, 0 };
	int	s;

	if (duration <= 0)
		return;
	s = CBaseEntity_Create(CLS_STREAK_SPIRAL, origin, ang, -1);
	if (s < 0)
		return;
	CBase(s)->dmgtime = gtime();
	CBase(s)->max_health = radius;
	CBase(s)->dmg = height;
	CBase(s)->speed = duration;
	CBase(s)->health = 0;
}

/* ---- env_smoker (CSmoker) --------------------------------------------------------------- */

/* pev->health puffs left, max_health the size (pev->scale), dmg the spread */
void CSmoker_Think(int e)
{
	CBaseEntity	*p = CBase(e);
	float		org[3];

	VecCopy(ent_cv(e)->origin, org);
	org[0] += RANDOM_FLOAT(-p->dmg, p->dmg);
	org[1] += RANDOM_FLOAT(-p->dmg, p->dmg);
	{
		/* draws in source order (argument order is unspecified, K10) */
		int scale = RANDOM_LONG((int)p->max_health, (int)(p->max_health * 1.1f));

		fx_smoke(org, RESID(SPR_SMOKE), scale, RANDOM_LONG(8, 14));
	}
	p->health--;
	if (p->health > 0)
		ThinkIn(e, RANDOM_FLOAT(0.1f, 0.2f));
	else
		UTIL_Remove(e);
}

static int CSmoker_Spawn(int e, gid_t16 gid)
{
	static const float z[3] = { 0, 0, 0 };

	(void)gid;
	ent_set_movetype(e, MOVETYPE_NONE);
	ThinkIn(e, 0);
	ent_set_solid(e, SOLID_NOT);
	ent_set_size(e, z, z);
	ent_rnd(e)->effects |= EF_NODRAW;
	VecSet(ent_cv(e)->angles, 0, 0, 0);
	SetThink(e, FN_CSmoker_Think);
	return 0;
}

const game_class_t gc_ENV_SMOKER = {
	.size = sizeof(CBaseEntity),
	.Spawn = CSmoker_Spawn,
	.save = &sv_Transient,
};

/* ---- CGargantua ---------------------------------------------------------------------- */

static void Ga_EyeUpdate(int e)
{
	CGargantua	*g = GARG(e);
	float		pos[3];

	if (!g->m_eyeOn)
		return;
	g->m_eyeRenderamt = (uint8_t)UTIL_Approach((float)g->m_eyeBrightness,
		(float)g->m_eyeRenderamt, 26);
	if (g->m_eyeRenderamt == 0)
		return;	/* EF_NODRAW */
	/* a spread think may pause before the attachment's pose (~6 ms on
	 * the cart) */
	if (Ai_PauseDue(e, AI_POSE_RESERVE))
		Ai_Pause(e, 0);
	gs_attachment(e, 0, pos);
	fx_glow_sprite(pos, RESID(GA_EYE_SPR), 2, 10, g->m_eyeRenderamt);
}

static void Ga_StompAttack(int e)
{
	col_trace_t	tr;
	float		start[3], aim[3], end[3];

	UTIL_MakeVectors(ent_cv(e)->angles);
	VecCopy(ent_cv(e)->origin, start);
	start[2] += 60;
	VecMA(start, 35, gpGlobals.v_forward, start);
	Mon_ShootAtEnemy(e, start, aim);
	VecMA(start, 1024, aim, end);
	UTIL_TraceLine(start, end, ignore_monsters, e, &tr);
	StompCreate(e, start, tr.end, 0);
	UTIL_ScreenShake(ent_cv(e)->origin, 12.0f, 100.0f, 2.0f, 1000);
	{
		res_t s = RANDOM_SOUND_ARRAY(gaStomp);	/* draws in source order (K10) */

		EMIT_SOUND_DYN(e, CHAN_WEAPON, s, 1.0f, ATTN_GARG, 0, PITCH_NORM + RANDOM_LONG(-10, 10));
	}
	VecCopy(ent_cv(e)->origin, end);
	end[2] -= 20;
	UTIL_TraceLine(ent_cv(e)->origin, end, ignore_monsters, e, &tr);
	if (tr.frac < 1.0f)
		UTIL_DecalTrace(&tr, DECAL_GARGSTOMP1);
}

/* flame beam i of gargantua e: from the flame end to the gun (key
 * FX_BEAM_KEY(e, i): a new one replaces it) */
static int flame_beam(int e, int i, const float from[3], const float gun[3])
{
	if (i < 2)
		return fx_beam(FX_BEAM_KEY(e, i), -1, from, -1, gun, RESID(GA_BEAM_SPR), 240, 0, 255,
			130, 90, 190, 20, 2);
	return fx_beam(FX_BEAM_KEY(e, i), -1, from, -1, gun, RESID(GA_BEAM_SPR), 140, 0, 0, 120,
		255, 190, 20, 2);
}

static void Ga_FlameCreate(int e)
{
	CGargantua	*g = GARG(e);
	col_trace_t	tr;
	float		gun[3], end[3];
	int		i;

	UTIL_MakeVectors(ent_cv(e)->angles);
	for (i = 0; i < 4; i++)
	{
		gs_attachment(e, i % 2 + 1, gun);
		VecMA(gun, GARG_FLAME_LENGTH, gpGlobals.v_forward, end);
		UTIL_TraceLine(gun, end, dont_ignore_monsters, e, &tr);
		g->m_pFlame[i] = (int16_t)flame_beam(e, i, tr.end, gun);
		CSoundEnt_InsertSound(bits_SOUND_COMBAT, gun, 384, 0.3f);
	}
	g->m_flameOn = 1;
	EMIT_SOUND_DYN(e, CHAN_BODY, res_def(gaBeamAttack[1]), 1.0f, ATTN_NORM, 0, PITCH_NORM);
	EMIT_SOUND_DYN(e, CHAN_WEAPON, res_def(gaBeamAttack[2]), 1.0f, ATTN_NORM, 0, PITCH_NORM);
}

static void Ga_FlameControls(int e, float angleX, float angleY)
{
	CGargantua *g = GARG(e);

	if (angleY < -180)
		angleY += 360;
	else if (angleY > 180)
		angleY -= 360;
	if (angleY < -45)
		angleY = -45;
	else if (angleY > 45)
		angleY = 45;
	g->m_flameX = UTIL_ApproachAngle(angleX, g->m_flameX, 4);
	g->m_flameY = UTIL_ApproachAngle(angleY, g->m_flameY, 8);
	Mon_SetBoneController(e, 0, g->m_flameY);
	Mon_SetBoneController(e, 1, g->m_flameX);
}

/* FlameDamage: damage along the flame (iClassIgnore CLASS_ALIEN_MONSTER) */
static void Ga_FlameDamage(int e, const float start[3], const float end[3], float flDamage,
	int iClassIgnore, int bits)
{
	col_trace_t	tr;
	float		mid[3], d[3], aim[3], spot[3], src[3], dist, radius, adj;
	int		s = -1;

	VecAdd(start, end, mid);
	VecScale(mid, 0.5f, mid);
	VecSub(start, mid, d);
	radius = VecLen(d);
	VecSub(end, start, d);
	VecNorm(d, aim);
	while ((s = UTIL_FindEntityInSphere(s, mid, radius)) >= 0)
	{
		if (!ent_priv(s) || CBase(s)->takedamage == DAMAGE_NO)
			continue;
		/* UNDONE: this should check a damage mask, not an ignore */
		if (iClassIgnore != CLASS_NONE && Classify(s) == iClassIgnore)
			continue;
		BodyTarget(s, mid, spot);
		VecSub(spot, mid, d);
		dist = VecDot(aim, d);
		if (dist > radius)
			dist = radius;
		else if (dist < -radius)
			dist = radius;
		VecMA(mid, dist, aim, src);
		UTIL_TraceLine(src, spot, dont_ignore_monsters, e, &tr);
		if (tr.frac != 1.0f && tr.ent != s)
			continue;
		VecSub(src, tr.end, d);
		dist = VecLen(d);
		adj = flDamage;
		if (dist > 64)
		{
			adj = flDamage - (dist - 64) * 0.4f;
			if (adj <= 0)
				continue;
		}
		if (tr.frac != 1.0f)
		{
			ClearMultiDamage();
			VecSub(tr.end, src, d);
			VecNorm(d, d);
			TraceAttack(s, e, adj, d, &tr, bits);
			ApplyMultiDamage(e, e);
		}
		else
			TakeDamage(s, e, e, adj, bits);
	}
}

static void Ga_FlameUpdate(int e)
{
	CGargantua	*g = GARG(e);
	col_trace_t	tr;
	float		aim[3], start[3], end[3], inner[3];
	int		i, streaks = 0;

	for (i = 0; i < 2; i++)
	{
		/* a spread think may pause before each flame: its attachment
		 * pose, hitbox trace, decal and burn (~5-15 ms on the cart) */
		if (Ai_PauseDue(e, AI_EVENT_RESERVE))
			Ai_Pause(e, 0);
		if (!g->m_flameOn)
			continue;
		VecCopy(ent_cv(e)->angles, aim);
		aim[0] += g->m_flameX;
		aim[1] += g->m_flameY;
		UTIL_MakeVectors(aim);
		gs_attachment(e, i + 1, start);
		VecMA(start, GARG_FLAME_LENGTH, gpGlobals.v_forward, end);
		UTIL_TraceLine(start, end, dont_ignore_monsters, e, &tr);
		g->m_pFlame[i] = (int16_t)flame_beam(e, i, tr.end, start);
		VecScale(start, 0.6f, inner);
		VecMA(inner, 0.4f, tr.end, inner);
		g->m_pFlame[i + 2] = (int16_t)flame_beam(e, i + 2, inner, start);
		if (tr.frac != 1.0f && gtime() > g->m_streakTime)
		{
			StreakSplash(tr.end, tr.normal, 6, 20);
			streaks = 1;
			UTIL_DecalTrace(&tr, DECAL_SMALLSCORCH1 + RANDOM_LONG(0, 2));
		}
		/* RadiusDamage(trace.vecEndPos, pev, pev, gSkillData.gargantuaDmgFire, CLASS_ALIEN_MONSTER, DMG_BURN) */
		Ga_FlameDamage(e, start, tr.end, mon_skill(sk_gargantuaDmgFire), CLASS_ALIEN_MONSTER,
			DMG_BURN);
		fx_elight(e, start, RANDOM_FLOAT(32, 48), 255, 255, 255, 2, 0);
	}
	if (streaks)
		g->m_streakTime = gtime();
}

static void Ga_FlameDestroy(int e)
{
	CGargantua	*g = GARG(e);
	int		i;

	EMIT_SOUND_DYN(e, CHAN_WEAPON, res_def(gaBeamAttack[0]), 1.0f, ATTN_NORM, 0, PITCH_NORM);
	for (i = 0; i < 4; i++)
	{
		if (g->m_pFlame[i] >= 0)
			fx_beam_kill(g->m_pFlame[i]);
		g->m_pFlame[i] = -1;
	}
	g->m_flameOn = 0;
}

static void Ga_PrescheduleThink(int e)
{
	CGargantua *g = GARG(e);

	if (!Mon_HasConditions((CBaseMonster *)g, bits_COND_SEE_ENEMY))
	{
		g->m_seeTime = gtime() + 5;
		g->m_eyeBrightness = 0;
	}
	else
		g->m_eyeBrightness = 200;
	Ga_EyeUpdate(e);
}

static int Ga_Classify(int e) { (void)e; return CLASS_ALIEN_MONSTER; }

static void Ga_SetYawSpeed(int e)
{
	int ys;

	switch (MON(e)->m_Activity)
	{
	case ACT_TURN_LEFT:
	case ACT_TURN_RIGHT:
		ys = 180;
		break;
	default:
		ys = 60;
		break;
	}
	ent_phys(e)->yaw_speed = (float)ys;
}

static void Ga_TraceAttack(int e, int attacker, float dmg, const float dir[3],
	const col_trace_t *tr, int bits)
{
	CGargantua *g = GARG(e);

	if (!IsAlive(e))
	{
		CBaseMonster_TraceAttack(e, attacker, dmg, dir, tr, bits);
		return;
	}
	/* UNDONE: hit group specific damage? */
	if (bits & (GARG_DAMAGE | DMG_BLAST))
	{
		if (g->m_painSoundTime < gtime())
		{
			EMIT_SOUND_DYN(e, CHAN_VOICE, RANDOM_SOUND_ARRAY(gaPain), 1.0f, ATTN_GARG, 0,
				PITCH_NORM);
			g->m_painSoundTime = gtime() + RANDOM_FLOAT(2.5f, 4);
		}
	}
	bits &= GARG_DAMAGE;
	if (bits == 0)
	{
		if (g->dmgtime != gtime() || RANDOM_LONG(0, 100) < 20)
		{
			UTIL_Ricochet(tr->end, RANDOM_FLOAT(0.5f, 1.5f));
			g->dmgtime = gtime();
		}
		dmg = 0;
	}
	CBaseMonster_TraceAttack(e, attacker, dmg, dir, tr, bits);
}

static int Ga_TakeDamage(int e, int inflictor, int attacker, float flDamage, int bits)
{
	if (IsAlive(e))
	{
		if (!(bits & GARG_DAMAGE))
			flDamage *= 0.01f;
		if (bits & DMG_BLAST)
			Mon_SetConditions(MON(e), bits_COND_LIGHT_DAMAGE);
	}
	return CBaseMonster_TakeDamage(e, inflictor, attacker, flDamage, bits);
}

/* SpawnExplosion: an env_explosion of magnitude near center (no damage) */
static void SpawnExplosion(const float center[3], float randomRange, int magnitude)
{
	float c[3], ang[3] = { 0, 0, 0 };

	VecCopy(center, c);
	c[0] += RANDOM_FLOAT(-randomRange, randomRange);
	c[1] += RANDOM_FLOAT(-randomRange, randomRange);
	ExplosionCreate(c, ang, -1, magnitude, 0);
}

/* the explosions DeathEffect schedules at i * 0.3 s (i = 0, 2, 4, 6),
 * fired when due */
static void Ga_DeathExplosions(int e, int all)
{
	CGargantua	*g = GARG(e);
	float		pos[3];
	int		i;

	while (g->m_explosions < 4)
	{
		i = g->m_explosions * 2;
		if (!all && gtime() < g->m_dieTime + i * 0.3f)
			break;
		VecCopy(ent_cv(e)->origin, pos);
		pos[2] += 32 + 15 * g->m_explosions;
		SpawnExplosion(pos, 70, 60 + i * 20);
		g->m_explosions++;
	}
}

static void Ga_DeathEffect(int e)
{
	CGargantua	*g = GARG(e);
	float		pos[3], z[3] = { 0, 0, 0 };
	int		s;

	UTIL_MakeVectors(ent_cv(e)->angles);
	VecMA(ent_cv(e)->origin, 100, gpGlobals.v_forward, pos);
	SpiralCreate(pos, (ent_cv(e)->absmax[2] - ent_cv(e)->absmin[2]) * 0.6f, 125, 1.5f);
	g->m_dieTime = gtime();
	g->m_explosions = 0;
	Ga_DeathExplosions(e, 0);
	s = CBaseEntity_Create(CLS_ENV_SMOKER, ent_cv(e)->origin, z, -1);
	if (s >= 0)
	{
		CBase(s)->health = 1;		/* 1 smoke balls */
		CBase(s)->max_health = 46;	/* 4.6x normal size */
		CBase(s)->dmg = 0;		/* 0 radial distribution */
		ThinkIn(s, 2.5f);		/* start in 2.5 seconds */
	}
}

static void Ga_Killed(int e, int attacker, int iGib)
{
	(void)iGib;
	GARG(e)->m_eyeBrightness = 0;
	GARG(e)->m_eyeOn = 0;
	CBaseMonster_Killed(e, attacker, GIB_NEVER);
}

static int Ga_CheckMeleeAttack1(int e, float flDot, float flDist)
{
	(void)e;
	return flDot >= 0.7f && flDist <= GARG_ATTACKDIST;
}

/* Flame thrower madness! */
static int Ga_CheckMeleeAttack2(int e, float flDot, float flDist)
{
	return gtime() > GARG(e)->m_flameTime && flDot >= 0.8f && flDist > GARG_ATTACKDIST &&
		flDist <= GARG_FLAME_LENGTH;
}

/* stomp attack */
static int Ga_CheckRangeAttack1(int e, float flDot, float flDist)
{
	return gtime() > GARG(e)->m_seeTime && flDot >= 0.7f && flDist > GARG_ATTACKDIST;
}

/* GargantuaCheckTraceHullAttack: the entity hit (damaged), -1 none */
static int Ga_CheckTraceHullAttack(int e, float flDist, int iDamage, int iDmgType)
{
	col_trace_t	tr;
	float		start[3], end[3];

	UTIL_MakeVectors(ent_cv(e)->angles);
	VecCopy(ent_cv(e)->origin, start);
	start[2] += 64;
	VecMA(start, flDist, gpGlobals.v_forward, end);
	VecMA(end, -flDist * 0.3f, gpGlobals.v_up, end);
	UTIL_TraceHull(start, end, dont_ignore_monsters, head_hull, e, &tr);
	if (tr.ent < 0 || (tr.ent == 0 && tr.frac == 1.0f))
		return -1;	/* nothing hit (pHit NULL) */
	if (iDamage > 0)
		TakeDamage(tr.ent, e, e, (float)iDamage, iDmgType);
	return tr.ent;
}

static void Ga_HandleAnimEvent(int e, const MonsterEvent_t *ev)
{
	float	v[3];
	int	hurt;
	res_t	s;	/* drawn before the pitch: argument order is unspecified (K10) */

	switch (ev->event)
	{
	case GARG_AE_SLASH_LEFT:
		hurt = Ga_CheckTraceHullAttack(e, GARG_ATTACKDIST + 10.0f,
			(int)mon_skill(sk_gargantuaDmgSlash), DMG_SLASH);
		if (hurt >= 0)
		{
			if (ent_cv(hurt)->flags & (FL_MONSTER | FL_CLIENT))
			{
				if (IsPlayer(hurt))
				{
					VecSet(phy_player()->punchangle, -30, -30, 30);
					VecMA(phy_player()->velocity, -100, gpGlobals.v_right,
						phy_player()->velocity);
				}
				else
				{
					VecMA(ent_mot(hurt)->velocity, -100, gpGlobals.v_right, v);
					ent_set_velocity(hurt, v);
				}
			}
			s = RANDOM_SOUND_ARRAY(gaAttackHit);
			EMIT_SOUND_DYN(e, CHAN_WEAPON, s, 1.0f, ATTN_NORM, 0, 50 + RANDOM_LONG(0, 15));
		}
		else	/* play a random attack miss sound */
		{
			s = RANDOM_SOUND_ARRAY(gaAttackMiss);
			EMIT_SOUND_DYN(e, CHAN_WEAPON, s, 1.0f, ATTN_NORM, 0, 50 + RANDOM_LONG(0, 15));
		}
		break;
	case GARG_AE_RIGHT_FOOT:
	case GARG_AE_LEFT_FOOT:
		UTIL_ScreenShake(ent_cv(e)->origin, 4.0f, 3.0f, 1.0f, 750);
		s = RANDOM_SOUND_ARRAY(gaFoot);
		EMIT_SOUND_DYN(e, CHAN_BODY, s, 1.0f, ATTN_GARG, 0, PITCH_NORM + RANDOM_LONG(-10, 10));
		break;
	case GARG_AE_STOMP:
		Ga_StompAttack(e);
		GARG(e)->m_seeTime = gtime() + 12;
		break;
	case GARG_AE_BREATHE:
		s = RANDOM_SOUND_ARRAY(gaBreathe);
		EMIT_SOUND_DYN(e, CHAN_VOICE, s, 1.0f, ATTN_GARG, 0, PITCH_NORM + RANDOM_LONG(-10, 10));
		break;
	default:
		CBaseMonster_HandleAnimEvent(e, ev);
		break;
	}
}

static const Schedule_t *Ga_GetScheduleOfType(int e, int type)
{
	/* HACKHACK - turn off the flames if they are on and garg goes scripted / dead */
	if (GARG(e)->m_flameOn)
		Ga_FlameDestroy(e);
	switch (type)
	{
	case SCHED_MELEE_ATTACK2:
		return slGargFlame;
	case SCHED_MELEE_ATTACK1:
		return slGargSwipe;
	}
	return CBaseMonster_GetScheduleOfType(e, type);
}

static void Ga_StartTask(int e, const Task_t *t)
{
	CGargantua *g = GARG(e);

	switch (t->iTask)
	{
	case TASK_FLAME_SWEEP:
		Ga_FlameCreate(e);
		g->m_flWaitFinished = gtime() + t->flData;
		g->m_flameTime = gtime() + 6;
		g->m_flameX = 0;
		g->m_flameY = 0;
		break;
	case TASK_SOUND_ATTACK:
		if (RANDOM_LONG(0, 100) < 30)
			EMIT_SOUND_DYN(e, CHAN_VOICE, RANDOM_SOUND_ARRAY(gaAttack), 1.0f, ATTN_GARG, 0,
				PITCH_NORM);
		Mon_TaskComplete((CBaseMonster *)g);
		break;
	case TASK_DIE:
		g->m_flWaitFinished = gtime() + 1.6f;
		Ga_DeathEffect(e);
		/* fall through */
	default:
		CBaseMonster_StartTask(e, t);
		break;
	}
}

static void Ga_GibDeath(int e)
{
	float	v[3], size[3] = { 200, 200, 128 }, z[3] = { 0, 0, 0 };
	int	i, gib;

	for (i = 0; i < 10; i++)
	{
		gib = CGib_Create();
		if (gib < 0)
			break;
		CGib_Spawn(gib, RESID(MDL_METALPLATEGIBS));
		ent_rnd(gib)->body = 0;
		GIB(gib)->m_bloodColor = BLOOD_COLOR_YELLOW;
		GIB(gib)->m_material = -1;	/* matNone */
		ent_set_origin(gib, ent_cv(e)->origin);
		/* UTIL_RandomBloodVector */
		v[0] = RANDOM_FLOAT(-1, 1);	/* draws in source order (K10) */
		v[1] = RANDOM_FLOAT(-1, 1);
		v[2] = RANDOM_FLOAT(0, 1);
		VecScale(v, RANDOM_FLOAT(300, 500), v);
		ent_set_velocity(gib, v);
		ThinkIn(gib, 1.25f);
		SetThink(gib, FN_SUB_FadeOut);
	}
	fx_break_model(ent_cv(e)->origin, size, z, 200, RESID(MDL_METALPLATEGIBS), 50, 20, BREAK_FLESH);
}

static void Ga_RunTask(int e, const Task_t *t)
{
	CGargantua	*g = GARG(e);
	float		org[3], dir[3], ang[3], tgt[3];
	int		en, cancel;

	switch (t->iTask)
	{
	case TASK_DIE:
		Ga_DeathExplosions(e, 0);
		if (gtime() > g->m_flWaitFinished)
		{
			Ga_DeathExplosions(e, 1);
			ent_rnd(e)->renderfx = kRenderFxExplode;
			ent_rnd(e)->rendercolor[0] = 255;
			ent_rnd(e)->rendercolor[1] = 0;
			ent_rnd(e)->rendercolor[2] = 0;
			ent_rnd(e)->framerate = 0;	/* StopAnimation */
			ThinkIn(e, 0.15f);
			SetThink(e, FN_SUB_Remove);
			Ga_GibDeath(e);
			return;
		}
		CBaseMonster_RunTask(e, t);
		break;
	case TASK_FLAME_SWEEP:
		if (gtime() > g->m_flWaitFinished)
		{
			Ga_FlameDestroy(e);
			Mon_TaskComplete((CBaseMonster *)g);
			Ga_FlameControls(e, 0, 0);
			Mon_SetBoneController(e, 0, 0);
			Mon_SetBoneController(e, 1, 0);
			break;
		}
		cancel = 0;
		VecSet(ang, 0, 0, 0);
		Ga_FlameUpdate(e);
		en = EHGet(g->m_hEnemy);
		if (en >= 0)
		{
			VecCopy(ent_cv(e)->origin, org);
			org[2] += 64;
			BodyTarget(en, org, tgt);
			VecSub(tgt, org, dir);
			UTIL_VecToAngles(dir, ang);
			ang[0] = -ang[0];
			ang[1] -= ent_cv(e)->angles[1];
			if (VecLen(dir) > 400)
				cancel = 1;
		}
		if (fabsf(ang[1]) > 60)
			cancel = 1;
		if (cancel)
		{
			g->m_flWaitFinished -= 0.5f;
			g->m_flameTime -= 0.5f;
		}
		Ga_FlameControls(e, ang[0], ang[1]);
		break;
	default:
		CBaseMonster_RunTask(e, t);
		break;
	}
}

static int CGargantua_Spawn(int e, gid_t16 gid)
{
	CBaseMonster	*m = Mon_SpawnKeys(e, gid);
	CGargantua	*g = (CGargantua *)m;
	static const float mn[3] = { -32, -32, 0 }, mx[3] = { 32, 32, 64 };
	static const int16_t pre[] = {
		R_GA_EYE_SPR, R_GA_BEAM_SPR, R_MDL_METALPLATEGIBS, R_W_MINE_CHARGE,
		R_ZO_CLAW_STRIKE1, R_ZO_CLAW_STRIKE2, R_ZO_CLAW_STRIKE3, R_GA_FLAMEOFF1,
		R_GA_FLAMEON1, R_GA_FLAMERUN1, R_ZO_CLAW_MISS1, R_MON_CLAW_MISS2, R_GA_METAL4,
		R_GA_METAL6, R_GA_RIC4, R_GA_RIC5, R_GA_STEP1, R_GA_STEP2, R_GA_IDLE1, R_GA_IDLE2,
		R_GA_IDLE3, R_GA_IDLE4, R_GA_IDLE5, R_GA_ALERT1, R_GA_ALERT2, R_GA_ALERT3,
		R_GA_PAIN1, R_GA_PAIN2, R_GA_PAIN3, R_GA_ATTACK1, R_GA_ATTACK2, R_GA_ATTACK3,
		R_GA_STOMP1, R_GA_BREATHE1, R_GA_BREATHE2, R_GA_BREATHE3, R_SPR_SMOKE,
	};
	int i;

	PRECACHE(RESID(MON_GARG_MDL));
	PRECACHE_SOUND_ARRAY(pre);
	ent_set_model(e, RESID(MON_GARG_MDL));
	ent_set_size(e, mn, mx);
	ent_set_solid(e, SOLID_SLIDEBOX);
	ent_set_movetype(e, MOVETYPE_STEP);
	m->m_bloodColor = BLOOD_COLOR_GREEN;
	m->health = mon_skill(sk_gargantuaHealth);
	m->m_flFieldOfView = -0.2f;	/* width of forward view cone (as a dotproduct result) */
	m->m_MonsterState = MONSTERSTATE_NONE;
	for (i = 0; i < 4; i++)
		g->m_pFlame[i] = -1;
	Mon_MonsterInit(e);
	/* the eye glow (CSprite kRenderGlow, kRenderFxNoDissipation, attachment 1) */
	g->m_eyeOn = 1;
	g->m_eyeRenderamt = 0;
	g->m_eyeBrightness = 0;
	g->m_seeTime = gtime() + 5;
	g->m_flameTime = gtime() + 2;
	return 0;
}

static const game_vt_t ga_vt = {
	MON_GAME_VT_BASE,
	.TraceAttack = Ga_TraceAttack,
	.TakeDamage = Ga_TakeDamage,
	.Killed = Ga_Killed,
	.IRelationship = CBaseMonster_IRelationship,
	.Classify = Ga_Classify,
};

static const mon_vt_t ga_mvt = {
	.SetYawSpeed = Ga_SetYawSpeed,
	.HandleAnimEvent = Ga_HandleAnimEvent,
	.CheckMeleeAttack1 = Ga_CheckMeleeAttack1,
	.CheckMeleeAttack2 = Ga_CheckMeleeAttack2,
	.CheckRangeAttack1 = Ga_CheckRangeAttack1,
	.GetScheduleOfType = Ga_GetScheduleOfType,
	.StartTask = Ga_StartTask,
	.RunTask = Ga_RunTask,
	.PrescheduleThink = Ga_PrescheduleThink,
};

/* HLSDK CGargantua::m_SaveData; the flame beams are effects (HLSDK saves
 * the beam entities): gone with the release, none after a restore */
static void CGargantua_released(int e)
{
	CGargantua *g = GARG(e);
	int i;

	for (i = 0; i < 4; i++)
		if (g->m_pFlame[i] >= 0)
		{
			fx_beam_kill(g->m_pFlame[i]);
			g->m_pFlame[i] = -1;
		}
}

static void CGargantua_restored(int e)
{
	CGargantua *g = GARG(e);
	int i;

	for (i = 0; i < 4; i++)
		g->m_pFlame[i] = -1;
	g->m_flameOn = 0;
}

/* After a saved game's load: the flame beams (0.2 s each, made again by
 * every FlameUpdate) are gone with the effects; their ids name nothing,
 * and killing them would kill another beam. m_flameOn stays: the next
 * FlameUpdate makes them, as the saving game's would. */
void CGargantua_LoadDone(int e)
{
	CGargantua *g = GARG(e);
	int i;

	for (i = 0; i < 4; i++)
		g->m_pFlame[i] = -1;
}

static const sv_field_t f_CGargantua[] = {
	SVF(CGargantua, m_eyeOn, SV_U8, 0),
	SVF(CGargantua, m_eyeRenderamt, SV_U8, 0),
	SVF(CGargantua, m_explosions, SV_U8, 0),
	SVF(CGargantua, m_eyeBrightness, SV_I32, 0),
	SVF(CGargantua, m_seeTime, SV_TIME, 0),
	SVF(CGargantua, m_flameTime, SV_TIME, 0),
	SVF(CGargantua, m_painSoundTime, SV_TIME, 0),
	SVF(CGargantua, m_streakTime, SV_TIME, 0),
	SVF(CGargantua, m_flameX, SV_F32, 0),
	SVF(CGargantua, m_flameY, SV_F32, 0),
	SVF(CGargantua, m_dieTime, SV_TIME, 0),
};
static const sv_table_t sv_CGargantua = SV_TABLE(&sv_CBaseMonster, f_CGargantua,
	.released = CGargantua_released, .restored = CGargantua_restored);

const game_class_t gc_MONSTER_GARGANTUA = {
	.size = sizeof(CGargantua),
	.Spawn = CGargantua_Spawn,
	.vt = &ga_vt,
	.ext = &ga_mvt,
	.save = &sv_CGargantua,
};
