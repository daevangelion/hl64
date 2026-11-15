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
 * islave.c -- monster_alien_slave (HLSDK dlls/islave.cpp CISlave).
 *
 * The CBeam entities are fx.h beams from the strike point to the hand
 * attachment, kept until ClearBeams (BeamGlow re-issues them at the new
 * brightness). CheckRangeAttack2 returns FALSE first thing in HLSDK, so
 * the revive path (m_hDead, WackBeam) is unreachable and not carried.
 */
#include "hl64/fx.h"
#include "hl64/settings.h"
#include "game/hl/monster.h"
#include "game/hl/player.h"
#include "game/hl/savegame.h"

#define ISLAVE_AE_CLAW		1
#define ISLAVE_AE_CLAWRAKE	2
#define ISLAVE_AE_ZAP_POWERUP	3
#define ISLAVE_AE_ZAP_SHOOT	4
#define ISLAVE_AE_ZAP_DONE	5
#define ISLAVE_MAX_BEAMS	8
#define SKILL_HARD		3

static const float sk_slaveHealth[3] = { 30, 30, 60 };
static const float sk_slaveDmgClaw[3] = { 8, 10, 10 };
static const float sk_slaveDmgClawrake[3] = { 25, 25, 25 };
static const float sk_slaveDmgZap[3] = { 10, 10, 15 };

typedef struct {
	float		p0[3];		/* the strike point */
	float		p1[3];		/* the hand */
	int16_t		id;		/* fx beam, -1 none */
	uint8_t		width, noise;
	uint8_t		rgb[3], bright;
} slv_beam_t;

typedef struct {
	CBASEENTITY_FIELDS
	CBASEDELAY_FIELDS
	CBASEANIMATING_FIELDS
	CBASETOGGLE_FIELDS
	CBASEMONSTER_FIELDS
	CSQUADMONSTER_FIELDS
	int		m_iBravery;
	int		m_iBeams;
	float		slv_flNextAttack;	/* CISlave::m_flNextAttack (shadows the monster's) */
	int		m_voicePitch;
	slv_beam_t	m_pBeam[ISLAVE_MAX_BEAMS];
} CISlave;

#define SLV(e)	((CISlave *)ent_priv(e))
_Static_assert(ISLAVE_MAX_BEAMS <= FX_BEAM_KEYS_PER_ENT, "a slave's beams have keys");

static const int16_t slvAttackHit[] = { R_ZO_CLAW_STRIKE1, R_ZO_CLAW_STRIKE2, R_ZO_CLAW_STRIKE3 };
static const int16_t slvAttackMiss[] = { R_ZO_CLAW_MISS1, R_MON_CLAW_MISS2 };
static const int16_t slvPain[] = { R_SLV_PAIN1, R_SLV_PAIN2 };
static const int16_t slvDie[] = { R_SLV_DIE1, R_SLV_DIE2 };

#define T(t, d)	{ t, (float)(d) }

static const Task_t tlSlaveAttack1[] = {
	T(TASK_STOP_MOVING, 0), T(TASK_FACE_IDEAL, 0), T(TASK_RANGE_ATTACK1, 0),
};
static const Schedule_t slSlaveAttack1[] = {
	SCHED(tlSlaveAttack1, bits_COND_CAN_MELEE_ATTACK1 | bits_COND_HEAR_SOUND |
		bits_COND_HEAVY_DAMAGE, bits_SOUND_DANGER, "Slave Range Attack1")
};

/* saved games: append only (monster.h sched_list_t) */
SCHED_LIST(sched_islave, slSlaveAttack1);

static int Slv_Classify(int e) { (void)e; return CLASS_ALIEN_MILITARY; }

static int Slv_IRelationship(int e, int other)
{
	if (IsPlayer(other) && (CBase(e)->spawnflags & SF_MONSTER_WAIT_UNTIL_PROVOKED) &&
		!Mon_HasMemory(MON(e), bits_MEMORY_PROVOKED))
		return R_NO;
	return CBaseMonster_IRelationship(e, other);
}

/* CallForHelp: provoke the monsters sharing my netname within flDist */
static void Slv_CallForHelp(int e, float flDist, int enemy, const float lkp[3])
{
	name_t	nn = CBase(e)->netname;
	float	d[3], ro[3];
	int	r;

	if (nn == NAME_NONE)
		return;
	for (r = 0; r < HL64_MAX_ENTITIES; r++)
	{
		if (!ent_is_live(r) || !ent_priv(r) || CBase(r)->netname != nn)
			continue;
		OriginIn(r, e, ro);
		VecSub(ent_cv(e)->origin, ro, d);
		if (VecLen(d) < flDist && mon_is_monster(r))
		{
			Mon_Remember(MON(r), bits_MEMORY_PROVOKED);
			Mon_PushEnemy(r, enemy, lkp);
		}
	}
}

static void Slv_AlertSound(int e)
{
	CISlave	*s = SLV(e);
	int	en = EHGet(s->m_hEnemy);

	if (en < 0)
		return;
	SENTENCEG_PlayRndSz(e, RESID(GRP_SLV_ALERT), 0.85f, ATTN_NORM, 0, s->m_voicePitch);
	Slv_CallForHelp(e, 512, en, s->m_vecEnemyLKP);
}

static void Slv_IdleSound(int e)
{
	if (RANDOM_LONG(0, 2) == 0)
		SENTENCEG_PlayRndSz(e, RESID(GRP_SLV_IDLE), 0.85f, ATTN_NORM, 0, SLV(e)->m_voicePitch);
}

static void Slv_PainSound(int e)
{
	if (RANDOM_LONG(0, 2) == 0)
		EMIT_SOUND_DYN(e, CHAN_WEAPON, RANDOM_SOUND_ARRAY(slvPain), 1.0f, ATTN_NORM, 0,
			SLV(e)->m_voicePitch);
}

static void Slv_DeathSound(int e)
{
	EMIT_SOUND_DYN(e, CHAN_WEAPON, RANDOM_SOUND_ARRAY(slvDie), 1.0f, ATTN_NORM, 0,
		SLV(e)->m_voicePitch);
}

static int Slv_ISoundMask(int e)
{
	(void)e;
	return bits_SOUND_WORLD | bits_SOUND_COMBAT | bits_SOUND_DANGER | bits_SOUND_PLAYER;
}

/* beam n of slave e: its key is FX_BEAM_KEY(e, n), game state (never an
 * fx pool slot: the block saves with the game) */
static void beam_issue(int e, int n)
{
	slv_beam_t *b = &SLV(e)->m_pBeam[n];

	b->id = (int16_t)fx_beam(FX_BEAM_KEY(e, n), -1, b->p0, -1, b->p1, RESID(SLV_LGTNING_SPR),
		b->width, b->noise, b->rgb[0], b->rgb[1], b->rgb[2], b->bright, 0, 0);
}

/* a beam from p0 to the hand (end attachment side < 0 ? 2 : 1) */
static void beam_add(int e, int side, const float p0[3], int width, int noise, int r, int g,
	int b, int bright)
{
	CISlave		*s = SLV(e);
	slv_beam_t	*bm = &s->m_pBeam[s->m_iBeams];

	VecCopy(p0, bm->p0);
	gs_attachment(e, side < 0 ? 1 : 0, bm->p1);
	bm->width = (uint8_t)width;
	bm->noise = (uint8_t)noise;
	bm->rgb[0] = (uint8_t)r;
	bm->rgb[1] = (uint8_t)g;
	bm->rgb[2] = (uint8_t)b;
	bm->bright = (uint8_t)bright;
	beam_issue(e, s->m_iBeams);
	s->m_iBeams++;
}

static void Slv_ClearBeams(int e)
{
	CISlave	*s = SLV(e);
	int	i;

	for (i = 0; i < s->m_iBeams; i++)
		if (s->m_pBeam[i].id >= 0)
			fx_beam_kill(s->m_pBeam[i].id);
	s->m_iBeams = 0;
	ent_cv(e)->skin = 0;
	STOP_SOUND(e, CHAN_WEAPON, RESID(SLV_ZAP4));
}

/* ArmBeam: small beam from the arm to a nearby wall */
static void Slv_ArmBeam(int e, int side)
{
	CISlave		*s = SLV(e);
	col_trace_t	tr, tr1;
	float		src[3], aim[3], end[3], flDist = 1.0f;
	int		i;

	if (s->m_iBeams >= ISLAVE_MAX_BEAMS)
		return;
	UTIL_MakeAimVectors(ent_cv(e)->angles);
	VecMA(ent_cv(e)->origin, 36, gpGlobals.v_up, src);
	VecMA(src, side * 16, gpGlobals.v_right, src);
	VecMA(src, 32, gpGlobals.v_forward, src);
	for (i = 0; i < 3; i++)
	{
		VecScale(gpGlobals.v_right, side * RANDOM_FLOAT(0, 1), aim);
		VecMA(aim, RANDOM_FLOAT(-1, 1), gpGlobals.v_up, aim);
		VecMA(src, 512, aim, end);
		UTIL_TraceLine(src, end, dont_ignore_monsters, e, &tr1);
		if (flDist > tr1.frac)
		{
			tr = tr1;
			flDist = tr.frac;
		}
	}
	/* couldn't find anything close enough */
	if (flDist == 1.0f)
		return;
	DecalGunshot(&tr, BULLET_PLAYER_CROWBAR);
	beam_add(e, side, tr.end, 30, 80, 96, 128, 16, 64);
}

/* BeamGlow: brighten all beams */
static void Slv_BeamGlow(int e)
{
	CISlave	*s = SLV(e);
	int	b = s->m_iBeams * 32, i;

	if (b > 255)
		b = 255;
	for (i = 0; i < s->m_iBeams; i++)
	{
		if (s->m_pBeam[i].bright == 255)
			continue;
		s->m_pBeam[i].bright = (uint8_t)b;
		beam_issue(e, i);	/* replaces the beam of its key */
	}
}

/* ZapBeam: heavy damage directly forward */
static void Slv_ZapBeam(int e, int side)
{
	CISlave		*s = SLV(e);
	col_trace_t	tr;
	float		src[3], aim[3], end[3], deflection = 0.01f;
	int		hit;

	if (s->m_iBeams >= ISLAVE_MAX_BEAMS)
		return;
	VecMA(ent_cv(e)->origin, 36, gpGlobals.v_up, src);
	Mon_ShootAtEnemy(e, src, aim);
	VecMA(aim, side * RANDOM_FLOAT(0, deflection), gpGlobals.v_right, aim);
	VecMA(aim, RANDOM_FLOAT(-deflection, deflection), gpGlobals.v_up, aim);
	VecMA(src, 1024, aim, end);
	UTIL_TraceLine(src, end, dont_ignore_monsters, e, &tr);
	beam_add(e, side, tr.end, 50, 20, 180, 255, 96, 255);
	hit = tr.ent;
	if (hit >= 0 && ent_priv(hit) && CBase(hit)->takedamage)
		TraceAttack(hit, e, mon_skill(sk_slaveDmgZap), aim, &tr, DMG_SHOCK);
	UTIL_EmitAmbientSound(e, tr.end, RESID(SLV_ELECTRO4), 0.5f, ATTN_NORM, 0,
		RANDOM_LONG(140, 160));
}

static void Slv_Killed(int e, int attacker, int iGib)
{
	Slv_ClearBeams(e);
	CSquadMonster_Killed(e, attacker, iGib);
}

static void Slv_SetYawSpeed(int e)
{
	int ys;

	switch (MON(e)->m_Activity)
	{
	case ACT_WALK:
	case ACT_IDLE:
		ys = 50;
		break;
	case ACT_RUN:
		ys = 70;
		break;
	default:
		ys = 90;
		break;
	}
	ent_phys(e)->yaw_speed = (float)ys;
}

static void slv_claw(int e, const float dmg[3])
{
	int hurt = Mon_CheckTraceHullAttack(e, 70, (int)mon_skill(dmg), DMG_SLASH);

	if (hurt >= 0)
	{
		if (ent_cv(hurt)->flags & (FL_MONSTER | FL_CLIENT))
			Mon_PunchPlayer(hurt, 5, -18);
		EMIT_SOUND_DYN(e, CHAN_WEAPON, RANDOM_SOUND_ARRAY(slvAttackHit), 1.0f, ATTN_NORM, 0,
			SLV(e)->m_voicePitch);
	}
	else
		EMIT_SOUND_DYN(e, CHAN_WEAPON, RANDOM_SOUND_ARRAY(slvAttackMiss), 1.0f, ATTN_NORM, 0,
			SLV(e)->m_voicePitch);
}

static void Slv_HandleAnimEvent(int e, const MonsterEvent_t *ev)
{
	CISlave	*s = SLV(e);
	float	src[3];

	switch (ev->event)
	{
	case ISLAVE_AE_CLAW:
		slv_claw(e, sk_slaveDmgClaw);
		break;
	case ISLAVE_AE_CLAWRAKE:
		slv_claw(e, sk_slaveDmgClawrake);
		break;
	case ISLAVE_AE_ZAP_POWERUP:
		/* speed up attack when on hard */
		if (settings.skill == SKILL_HARD)
			ent_rnd(e)->framerate = 1.5f;
		UTIL_MakeAimVectors(ent_cv(e)->angles);
		if (s->m_iBeams == 0)
		{
			VecMA(ent_cv(e)->origin, 2, gpGlobals.v_forward, src);
			fx_dlight(src, 12, 255, 180, 96,
				(int)(20 / (ent_rnd(e)->framerate != 0 ? ent_rnd(e)->framerate : 1)), 0);
		}
		Slv_ArmBeam(e, -1);
		Slv_ArmBeam(e, 1);
		Slv_BeamGlow(e);
		EMIT_SOUND_DYN(e, CHAN_WEAPON, RESID(SLV_ZAP4), 1, ATTN_NORM, 0, 100 + s->m_iBeams * 10);
		ent_cv(e)->skin = (int16_t)(s->m_iBeams / 2);
		break;
	case ISLAVE_AE_ZAP_SHOOT:
		Slv_ClearBeams(e);
		ClearMultiDamage();
		UTIL_MakeAimVectors(ent_cv(e)->angles);
		Slv_ZapBeam(e, -1);
		Slv_ZapBeam(e, 1);
		EMIT_SOUND_DYN(e, CHAN_WEAPON, RESID(SLV_HW_SHOOT1), 1, ATTN_NORM, 0,
			RANDOM_LONG(130, 160));
		ApplyMultiDamage(e, e);
		s->slv_flNextAttack = gtime() + RANDOM_FLOAT(0.5f, 4.0f);
		break;
	case ISLAVE_AE_ZAP_DONE:
		Slv_ClearBeams(e);
		break;
	default:
		CBaseMonster_HandleAnimEvent(e, ev);
		break;
	}
}

static int Slv_CheckRangeAttack1(int e, float flDot, float flDist)
{
	if (SLV(e)->slv_flNextAttack > gtime())
		return 0;
	return CBaseMonster_CheckRangeAttack1(e, flDot, flDist);
}

/* HLSDK: "return FALSE;" before the revive search */
static int Slv_CheckRangeAttack2(int e, float flDot, float flDist)
{
	(void)e; (void)flDot; (void)flDist;
	return 0;
}

static void Slv_StartTask(int e, const Task_t *t)
{
	Slv_ClearBeams(e);
	CBaseMonster_StartTask(e, t);
}

static int Slv_TakeDamage(int e, int inflictor, int attacker, float flDamage, int bits)
{
	/* don't slash one of your own */
	if ((bits & DMG_SLASH) && attacker >= 0 && Mon_IRelationshipOf(e, attacker) < R_DL)
		return 0;
	Mon_Remember(MON(e), bits_MEMORY_PROVOKED);
	return CBaseMonster_TakeDamage(e, inflictor, attacker, flDamage, bits);
}

static void Slv_TraceAttack(int e, int attacker, float dmg, const float dir[3],
	const col_trace_t *tr, int bits)
{
	if (bits & DMG_SHOCK)
		return;
	CBaseMonster_TraceAttack(e, attacker, dmg, dir, tr, bits);
}

static const Schedule_t *Slv_GetScheduleOfType(int e, int type);

static const Schedule_t *Slv_GetSchedule(int e)
{
	CISlave		*s = SLV(e);
	CBaseMonster	*m = (CBaseMonster *)s;
	CSound		*snd;

	Slv_ClearBeams(e);
	if (Mon_HasConditions(m, bits_COND_HEAR_SOUND))
	{
		snd = Mon_PBestSound(e);
		if (snd && (snd->m_iType & bits_SOUND_DANGER))
			return Slv_GetScheduleOfType(e, SCHED_TAKE_COVER_FROM_BEST_SOUND);
		if (snd && (snd->m_iType & bits_SOUND_COMBAT))
			Mon_Remember(m, bits_MEMORY_PROVOKED);
	}
	if (s->m_MonsterState == MONSTERSTATE_COMBAT)
	{
		/* dead enemy */
		if (Mon_HasConditions(m, bits_COND_ENEMY_DEAD))
			return CBaseMonster_GetSchedule(e);
		if ((s->health < 20 || s->m_iBravery < 0) &&
			!Mon_HasConditions(m, bits_COND_CAN_MELEE_ATTACK1))
		{
			s->m_failSchedule = SCHED_CHASE_ENEMY;
			if (Mon_HasConditions(m, bits_COND_LIGHT_DAMAGE | bits_COND_HEAVY_DAMAGE))
				return Slv_GetScheduleOfType(e, SCHED_TAKE_COVER_FROM_ENEMY);
			if (Mon_HasConditions(m, bits_COND_SEE_ENEMY) &&
				Mon_HasConditions(m, bits_COND_ENEMY_FACING_ME))
				return Slv_GetScheduleOfType(e, SCHED_TAKE_COVER_FROM_ENEMY);
		}
	}
	return CBaseMonster_GetSchedule(e);
}

static const Schedule_t *Slv_GetScheduleOfType(int e, int type)
{
	switch (type)
	{
	case SCHED_FAIL:
		if (Mon_HasConditions(MON(e), bits_COND_CAN_MELEE_ATTACK1))
			return CSquadMonster_GetScheduleOfType(e, SCHED_MELEE_ATTACK1);
		break;
	case SCHED_RANGE_ATTACK1:
	case SCHED_RANGE_ATTACK2:
		return slSlaveAttack1;
	}
	return CSquadMonster_GetScheduleOfType(e, type);
}

static int CISlave_Spawn(int e, gid_t16 gid)
{
	CBaseMonster	*m = Mon_SpawnKeys(e, gid);
	CISlave		*s = (CISlave *)m;
	static const int16_t pre[] = {
		R_SLV_LGTNING_SPR, R_SLV_ZAP1, R_SLV_ZAP4, R_SLV_ELECTRO4, R_SLV_HW_SHOOT1,
		R_ZO_PAIN2, R_HC_HEADBITE, R_W_CBAR_MISS1, R_ZO_CLAW_STRIKE1, R_ZO_CLAW_STRIKE2,
		R_ZO_CLAW_STRIKE3, R_ZO_CLAW_MISS1, R_MON_CLAW_MISS2, R_SLV_PAIN1, R_SLV_PAIN2,
		R_SLV_DIE1, R_SLV_DIE2,
	};

	Sqm_Init(e);
	PRECACHE(RESID(MON_ISLAVE_MDL));
	PRECACHE_SOUND_ARRAY(pre);
	ent_set_model(e, RESID(MON_ISLAVE_MDL));
	ent_set_size(e, VEC_HUMAN_HULL_MIN, VEC_HUMAN_HULL_MAX);
	ent_set_solid(e, SOLID_SLIDEBOX);
	ent_set_movetype(e, MOVETYPE_STEP);
	m->m_bloodColor = BLOOD_COLOR_GREEN;
	ent_rnd(e)->effects = 0;
	m->health = mon_skill(sk_slaveHealth);
	VecSet(m->view_ofs, 0, 0, 64);	/* position of the eyes relative to monster's origin */
	ent_phys(e)->view_ofs_z = 64;
	m->m_flFieldOfView = VIEW_FIELD_WIDE;
	m->m_MonsterState = MONSTERSTATE_NONE;
	m->m_afCapability = bits_CAP_HEAR | bits_CAP_TURN_HEAD | bits_CAP_RANGE_ATTACK2 |
		bits_CAP_DOORS_GROUP;
	s->m_voicePitch = RANDOM_LONG(85, 110);
	Mon_MonsterInit(e);
	return 0;
}

static const game_vt_t slv_vt = {
	MON_GAME_VT_BASE,
	.TraceAttack = Slv_TraceAttack,
	.TakeDamage = Slv_TakeDamage,
	.Killed = Slv_Killed,
	.IRelationship = Slv_IRelationship,
	.Classify = Slv_Classify,
};

static const mon_vt_t slv_mvt = {
	.squad = 1,
	.SetYawSpeed = Slv_SetYawSpeed,
	.ISoundMask = Slv_ISoundMask,
	.HandleAnimEvent = Slv_HandleAnimEvent,
	.CheckRangeAttack1 = Slv_CheckRangeAttack1,
	.CheckRangeAttack2 = Slv_CheckRangeAttack2,
	.DeathSound = Slv_DeathSound,
	.PainSound = Slv_PainSound,
	.AlertSound = Slv_AlertSound,
	.IdleSound = Slv_IdleSound,
	.StartTask = Slv_StartTask,
	.GetSchedule = Slv_GetSchedule,
	.GetScheduleOfType = Slv_GetScheduleOfType,
	.CheckEnemy = CSquadMonster_CheckEnemy,
	.StartMonster = CSquadMonster_StartMonster,
	.ScheduleChange = CSquadMonster_ScheduleChange,
	.FValidateCover = CSquadMonster_FValidateCover,
	.GetIdealState = CSquadMonster_GetIdealState,
};

/* HLSDK CISlave::m_SaveData; its beams are effects (HLSDK saves the beam
 * entities): cleared with the release, none after a restore (m_iBeams 0
 * from the spawn) */
static const sv_field_t f_CISlave[] = {
	SVF(CISlave, m_iBravery, SV_I32, 0),
	SVF(CISlave, slv_flNextAttack, SV_TIME, 0),
	SVF(CISlave, m_voicePitch, SV_I32, 0),
};
static const sv_table_t sv_CISlave = SV_TABLE(&sv_CSquadMonster, f_CISlave,
	.released = Slv_ClearBeams);

/* After a saved game's load (frame layer: the slave's): its zap beams
 * (until killed) issued again from what the block keeps of them, so the
 * kills of its next ClearBeams / BeamGlow name its own beams. */
void CISlave_LoadDone(int e)
{
	CISlave *s = SLV(e);
	int i;

	if (s->m_iBeams < 0 || s->m_iBeams > ISLAVE_MAX_BEAMS)
		s->m_iBeams = 0;
	for (i = 0; i < s->m_iBeams; i++)
		beam_issue(e, i);
}

const game_class_t gc_MONSTER_ALIEN_SLAVE = {
	.size = sizeof(CISlave),
	.Spawn = CISlave_Spawn,
	.vt = &slv_vt,
	.ext = &slv_mvt,
	.save = &sv_CISlave,
};
