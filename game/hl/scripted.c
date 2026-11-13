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
 * scripted.c -- scripted_sequence / aiscripted_sequence (HLSDK
 * dlls/scripted.cpp CCineMonster, CCineAI) and the CBaseMonster script
 * members (ExitScriptedSequence, CineCleanup).
 *
 * hl64 forms: m_iszEntity is a cooked name (or a class id when the key
 * holds a classname); m_iszPlay / m_iszIdle are copied at spawn and
 * resolved to sequence indices on the possessed monster's model once
 * (cached per model), so no string compare runs after the first start.
 * A scripted_sequence is not a monster (HLSDK derives it from
 * CBaseMonster only for m_hTargetEnt), so its block is CBaseToggle plus
 * the CCineMonster members. One map layer (M3); the old tree's twin
 * hand-off and dehydrate parts are not needed here.
 */
#include <string.h>

#include "game/hl/monster.h"

#define SCRIPT_NAME_MAX		32	/* mstudioseqdesc_t label */

typedef struct {
	CBASEENTITY_FIELDS
	CBASEDELAY_FIELDS
	CBASEANIMATING_FIELDS
	CBASETOGGLE_FIELDS
	ent_h		m_hTargetEnt;
	char		m_iszIdle[SCRIPT_NAME_MAX];
	char		m_iszPlay[SCRIPT_NAME_MAX];
	name_t		m_iszEntity;
	cls_t		m_clsEntity;	/* m_iszEntity as a classname, CLS_NONE */
	res_t		m_seqModel;	/* model the indices below belong to */
	int16_t		m_seqIdle, m_seqPlay;
	int		m_fMoveTo;
	int		m_iFinishSchedule;
	float		m_flRadius;
	float		m_flRepeat;
	int		m_iDelay;
	float		m_startTime;
	int		m_saved_movetype;
	int		m_saved_solid;
	int		m_saved_effects;
	uint8_t		m_interruptable;
	uint8_t		m_ai;		/* CCineAI */
	uint8_t		pad[2];
} CCineMonster;

#define CINE(e)		((CCineMonster *)ent_priv(e))

static int is_cine(int e)
{
	cls_t c;

	if (e < 0 || !ent_is_live(e))
		return 0;
	c = ent_idc(e)->cls;
	return c == CLS_SCRIPTED_SEQUENCE || c == CLS_AISCRIPTED_SEQUENCE;
}

/* ---- spawn ----------------------------------------------------------------- */

static void copy_name(char *d, const char *s)
{
	size_t n = 0;

	if (s)
		for (; s[n] && n < SCRIPT_NAME_MAX - 1; n++)
			d[n] = s[n];
	d[n] = 0;
}

static int CCineMonster_Spawn(int e, gid_t16 gid)
{
	CCineMonster *c = CINE(e);

	CBaseToggle_KeyValue(e, gid);
	c->m_ai = ent_idc(e)->cls == CLS_AISCRIPTED_SEQUENCE;
	c->m_hTargetEnt = EH_NONE;
	copy_name(c->m_iszIdle, rec_key_str(gid, K_M_ISZIDLE));
	copy_name(c->m_iszPlay, rec_key_str(gid, K_M_ISZPLAY));
	c->m_iszEntity = rec_key_name(gid, K_M_ISZENTITY);
	c->m_clsEntity = rec_key_cls(gid, K_M_ISZENTITY);
	c->m_fMoveTo = (int)KeyF(gid, K_M_FMOVETO, 0);
	c->m_flRadius = KeyF(gid, K_M_FLRADIUS, 0);
	c->m_flRepeat = KeyF(gid, K_M_FLREPEAT, 0);
	c->m_iFinishSchedule = (int)KeyF(gid, K_M_IFINISHSCHEDULE, 0);
	c->m_seqModel = RES_NONE;
	c->m_seqIdle = c->m_seqPlay = -1;

	ent_set_solid(e, SOLID_NOT);
	if (ent_idc(e)->targetname == NAME_NONE || c->m_iszIdle[0])
	{
		SetThink(e, FN_CCineMonster_CineThink);
		ThinkIn(e, 1.0f);
		if (ent_idc(e)->targetname != NAME_NONE)
			c->m_startTime = gtime() + 1E6f;
	}
	c->m_interruptable = !(c->spawnflags & SF_SCRIPT_NOINTERRUPT);
	SetUse(e, FN_CCineMonster_Use);
	mon_stats_script();
	return 0;
}

int Script_FCanOverrideState(int script)
{
	CCineMonster *c = CINE(script);

	return c->m_ai || (c->spawnflags & SF_SCRIPT_OVERRIDESTATE);
}

void CCineMonster_Use(int e, int activator, int caller, int useType, float value)
{
	CCineMonster	*c = CINE(e);
	int		t = EHGet(c->m_hTargetEnt);

	(void)activator; (void)caller; (void)useType; (void)value;
	if (t >= 0 && mon_is_monster(t) && t != ENT_PLAYER)
	{
		if (MON(t)->m_scriptState == SCRIPT_PLAYING)
			return;
		c->m_startTime = gtime() + 0.05f;
	}
	else
	{
		SetThink(e, FN_CCineMonster_CineThink);
		ent_think_at(e, gtime());
	}
}

/* ---- finding and possessing ------------------------------------------------- */

static int monster_ok(int t)
{
	return t >= 0 && t != ENT_PLAYER && (ent_cv(t)->flags & FL_MONSTER) && mon_is_monster(t);
}

static int FindEntity(int e)
{
	CCineMonster	*c = CINE(e);
	int		t, it = 0;

	c->m_hTargetEnt = EH_NONE;
	if (c->m_iszEntity != NAME_NONE)
	{
		while ((t = rec_next_named(c->m_iszEntity, &it)) >= 0)
		{
			Mon_SettleSpawn(t);
			if (monster_ok(t) &&
				Mon_CanPlaySequence(t, Script_FCanOverrideState(e), SS_INTERRUPT_BY_NAME))
			{
				c->m_hTargetEnt = EHANDLE(t);
				return 1;
			}
		}
	}
	if (c->m_clsEntity != CLS_NONE)
	{
		t = -1;
		while ((t = UTIL_FindEntityInSphere(t, ent_cv(e)->origin, c->m_flRadius)) >= 0)
		{
			if (ent_idc(t)->cls == c->m_clsEntity && monster_ok(t) &&
				Mon_CanPlaySequence(t, Script_FCanOverrideState(e), SS_INTERRUPT_IDLE))
			{
				c->m_hTargetEnt = EHANDLE(t);
				return 1;
			}
		}
	}
	c->m_hTargetEnt = EH_NONE;
	return 0;
}

static void resolve_sequences(CCineMonster *c, int t)
{
	res_t model = ent_rnd(t)->model;

	if (c->m_seqModel == model)
		return;
	c->m_seqModel = model;
	c->m_seqIdle = (int16_t)(c->m_iszIdle[0] ? gs_lookup_sequence(model, c->m_iszIdle) : -1);
	c->m_seqPlay = (int16_t)(c->m_iszPlay[0] ? gs_lookup_sequence(model, c->m_iszPlay) : -1);
	if (c->m_iszPlay[0] && c->m_seqPlay < 0)
		log_printf("script %d: no sequence %s on %s\n", ent_idc(t)->gid, c->m_iszPlay,
			res_name(model));
}

int Script_HasIdle(int script) { return CINE(script)->m_iszIdle[0] != 0; }
int Script_IdleIsPlay(int script)
{
	CCineMonster *c = CINE(script);
	return strcmp(c->m_iszIdle, c->m_iszPlay) == 0;
}
int Script_MoveTo(int script) { return CINE(script)->m_fMoveTo; }
int Script_ReadyToStart(int script)
{
	CCineMonster *c = CINE(script);
	return c->m_iDelay <= 0 && gtime() >= c->m_startTime;
}

/* StartSequence (play = m_iszPlay, else m_iszIdle) */
int Script_StartSequence(int script, int t, int play, int completeOnEmpty)
{
	CCineMonster	*c = CINE(script);
	const char	*name = play ? c->m_iszPlay : c->m_iszIdle;
	int		seq;
	ent_rnd_t	*r = ent_rnd(t);

	if (!name[0] && completeOnEmpty)
	{
		Script_SequenceDone(script, t);
		return c->m_ai ? 1 : 0;
	}
	resolve_sequences(c, t);
	seq = play ? c->m_seqPlay : c->m_seqIdle;
	if (seq < 0)
		seq = 0;
	r->sequence = (uint16_t)seq;
	r->frame = 0;
	Mon_ResetSequenceInfo(t);
	if (play)
		log_printf("script: %s plays %s t=%.2f\n", res_name(r->model), c->m_iszPlay, gtime());
	return 1;
}

int Script_PlaySequence(int script) { return CINE(script)->m_seqPlay; }
int Script_IdleSequence(int script) { return CINE(script)->m_seqIdle; }

void Script_DelayStart(int e, int state)
{
	name_t	n = ent_idc(e)->targetname;
	int	t, it = 0;

	if (n == NAME_NONE)
	{
		t = e;
		goto one;
	}
	while ((t = rec_next_named(n, &it)) >= 0)
	{
		if (ent_idc(t)->cls != CLS_SCRIPTED_SEQUENCE)
			continue;
one:
		{
			CCineMonster *c = CINE(t);

			if (state)
				c->m_iDelay++;
			else
			{
				c->m_iDelay--;
				if (c->m_iDelay <= 0)
					c->m_startTime = gtime() + 0.05f;
			}
		}
		if (n == NAME_NONE)
			break;
	}
}

static void PossessEntity(int e)
{
	CCineMonster	*c = CINE(e);
	int		t = EHGet(c->m_hTargetEnt);
	CBaseMonster	*m;
	ent_rnd_t	*r;

	if (!monster_ok(t))
		return;
	if (c->m_ai && !Mon_CanPlaySequence(t, Script_FCanOverrideState(e), SS_INTERRUPT_AI))
		return;
	m = MON(t);
	r = ent_rnd(t);
	m->m_pGoalEnt = EHANDLE(e);
	m->m_pCine = EHANDLE(e);
	m->m_hTargetEnt = EHANDLE(e);
	c->m_saved_movetype = ent_cv(t)->movetype;
	c->m_saved_solid = ent_cv(t)->solid;
	c->m_saved_effects = r->effects;
	r->effects |= ent_rnd(e)->effects;
	switch (c->m_fMoveTo)
	{
	case 0:
		m->m_scriptState = SCRIPT_WAIT;
		break;
	case 5:
		if (c->m_ai)
			m->m_scriptState = SCRIPT_WAIT;
		break;
	case 1:
		m->m_scriptState = SCRIPT_WALK_TO_MARK;
		if (!c->m_ai)
			Script_DelayStart(e, 1);
		break;
	case 2:
		m->m_scriptState = SCRIPT_RUN_TO_MARK;
		if (!c->m_ai)
			Script_DelayStart(e, 1);
		break;
	case 4:
		ent_set_origin(t, ent_cv(e)->origin);
		ent_phys(t)->ideal_yaw = ent_cv(e)->angles[1];
		ent_set_avelocity(t, g_vecZero);
		ent_set_velocity(t, g_vecZero);
		r->effects |= EF_NOINTERP;
		ent_cv(t)->angles[1] = ent_cv(e)->angles[1];
		m->m_scriptState = SCRIPT_WAIT;
		c->m_startTime = gtime() + 1E6f;
		if (c->m_ai)
			EFlagsClear(t, FL_ONGROUND);
		break;
	}
	m->m_IdealMonsterState = MONSTERSTATE_SCRIPT;
	log_printf("script: %s takes %s (moveto %d) t=%.2f\n", res_name(ent_rnd(t)->model),
		c->m_iszPlay[0] ? c->m_iszPlay : "-", c->m_fMoveTo, gtime());
	if (!c->m_ai)
	{
		if (c->m_iszIdle[0])
		{
			Script_StartSequence(e, t, 0, 0);
			if (Script_IdleIsPlay(e))
				r->framerate = 0;
		}
	}
	else if (m->m_MonsterState == MONSTERSTATE_SCRIPT)
	{
		Mon_ChangeSchedule(t, Mon_GetScheduleOfType(t, SCHED_AISCRIPT));
	}
}

void CCineMonster_CineThink(int e)
{
	if (FindEntity(e))
		PossessEntity(e);
	else
	{
		Script_CancelScript(e);
		ThinkIn(e, 1.0f);
	}
}

void Script_SequenceDone(int e, int t)
{
	CCineMonster *c = CINE(e);

	log_printf("script: %s done %s t=%.2f\n", res_name(ent_rnd(t)->model),
		c->m_iszPlay[0] ? c->m_iszPlay : "-", gtime());
	if (!(c->spawnflags & SF_SCRIPT_REPEATABLE))
	{
		SetThink(e, FN_SUB_Remove);
		ThinkIn(e, 0.1f);
	}
	Mon_CineCleanup(t);
	Script_FixScriptMonsterSchedule(e, t);
	SUB_UseTargets(e, -1, USE_TOGGLE, 0);
}

void Script_FixScriptMonsterSchedule(int e, int t)
{
	CCineMonster	*c = CINE(e);
	CBaseMonster	*m = MON(t);

	if (!c->m_ai)
	{
		if (m->m_IdealMonsterState != MONSTERSTATE_DEAD)
			m->m_IdealMonsterState = MONSTERSTATE_IDLE;
		Mon_ClearSchedule(t);
		return;
	}
	if (c->m_iFinishSchedule == SCRIPT_FINISHSCHED_AMBUSH)
		Mon_ChangeSchedule(t, Mon_GetScheduleOfType(t, SCHED_AMBUSH));
	else
		Mon_ClearSchedule(t);
}

void Script_AllowInterrupt(int e, int fAllow)
{
	CCineMonster *c = CINE(e);

	if (c->spawnflags & SF_SCRIPT_NOINTERRUPT)
		return;
	c->m_interruptable = (uint8_t)(fAllow != 0);
}

int Script_CanInterrupt(int e)
{
	CCineMonster	*c = CINE(e);
	int		t;

	if (!c->m_interruptable)
		return 0;
	t = EHGet(c->m_hTargetEnt);
	return t >= 0 && ent_priv(t) && CBase(t)->deadflag == DEAD_NO;
}

int Script_IgnoreConditions(int e) { return Script_CanInterrupt(e) ? 0 : SCRIPT_BREAK_CONDITIONS; }

static void ScriptEntityCancel(int s)
{
	int t;

	if (!is_cine(s) || ent_idc(s)->cls != CLS_SCRIPTED_SEQUENCE)
		return;
	t = EHGet(CINE(s)->m_hTargetEnt);
	if (monster_ok(t) && MON(t)->m_MonsterState == MONSTERSTATE_SCRIPT)
	{
		MON(t)->m_scriptState = SCRIPT_CLEANUP;
		Mon_CineCleanup(t);
	}
}

void Script_CancelScript(int e)
{
	name_t	n = ent_idc(e)->targetname;
	int	t, it = 0;

	if (n == NAME_NONE)
	{
		ScriptEntityCancel(e);
		return;
	}
	while ((t = rec_next_named(n, &it)) >= 0)
		ScriptEntityCancel(t);
}

/* Activate: SequencePrecache -> the sequences' animation chunks */
static void CCineMonster_Activate(int e)
{
	CCineMonster	*c = CINE(e);
	int		t = -1, it = 0;

	if (c->m_iszEntity != NAME_NONE)
		while ((t = rec_next_named(c->m_iszEntity, &it)) >= 0 && !monster_ok(t))
			;
	if (t < 0 && c->m_clsEntity != CLS_NONE)
		t = ent_next_class(-1, c->m_clsEntity, ent_idc(e)->layer);
	if (monster_ok(t) && ent_rnd(t)->model)
	{
		resolve_sequences(c, t);
		if (c->m_seqIdle >= 0)
			studio_want(c->m_seqModel, c->m_seqIdle);
		if (c->m_seqPlay >= 0)
			studio_want(c->m_seqModel, c->m_seqPlay);
	}
}

/* ---- CBaseMonster script members ---------------------------------------------- */

int Mon_ExitScriptedSequence(int e)
{
	CBaseMonster	*m = MON(e);
	int		cine = EHGet(m->m_pCine);

	if (m->deadflag == DEAD_DYING)
	{
		m->m_IdealMonsterState = MONSTERSTATE_DEAD;
		return 0;
	}
	if (cine >= 0 && is_cine(cine))
		Script_CancelScript(cine);
	return 1;
}

int Mon_CineCleanup(int e)
{
	CBaseMonster	*m = MON(e);
	col_ent_t	*cv = ent_cv(e);
	ent_rnd_t	*r = ent_rnd(e);
	int		old = EHGet(m->m_pCine), drop;
	CCineMonster	*oc = (old >= 0 && is_cine(old)) ? CINE(old) : 0;
	float		no[3], na[3], oo[3], mx[3];

	if (oc)
	{
		oc->m_hTargetEnt = EH_NONE;
		ent_set_movetype(e, oc->m_saved_movetype);
		ent_set_solid(e, oc->m_saved_solid);
		r->effects = (uint16_t)oc->m_saved_effects;
	}
	else
	{
		ent_set_movetype(e, MOVETYPE_STEP);
		ent_set_solid(e, SOLID_SLIDEBOX);
	}
	m->m_pCine = EH_NONE;
	m->m_hTargetEnt = EH_NONE;
	m->m_pGoalEnt = EH_NONE;
	if (m->deadflag == DEAD_DYING)
	{
		m->health = 0;
		r->framerate = 0.0f;
		ent_set_solid(e, SOLID_NOT);
		Mon_SetState(e, MONSTERSTATE_DEAD);
		m->deadflag = DEAD_DEAD;
		VecSet(mx, cv->maxs[0], cv->maxs[1], cv->mins[2] + 2);
		ent_set_size(e, cv->mins, mx);
		if (oc && (oc->spawnflags & SF_SCRIPT_LEAVECORPSE))
		{
			SetUse(e, FN_NONE);
			SetThink(e, FN_NONE);
			SetTouch(e, FN_NONE);
		}
		else
			SUB_StartFadeOut(e);
		r->framerate = 0;
		ent_set_movetype(e, MOVETYPE_NONE);
		r->effects |= EF_NOINTERP;
		return 0;
	}
	if (oc && oc->m_iszPlay[0])
	{
		if (!(oc->spawnflags & SF_SCRIPT_NOSCRIPTMOVEMENT))
		{
			gs_bone_position(e, 0, no);
			(void)na;
			VecCopy(cv->origin, oo);
			if (mv_dist2d(oo, no) < 8.0f)
				VecCopy(oo, no);
			cv->origin[0] = no[0];
			cv->origin[1] = no[1];
			cv->origin[2] += 1;
			EFlagsSet(e, FL_ONGROUND);
			drop = phy_drop_to_floor(e);
			if (drop < 0)
				VecCopy(oo, cv->origin);
			else if (drop == 0)
			{
				cv->origin[2] = no[2];
				EFlagsClear(e, FL_ONGROUND);
			}
			ent_set_origin(e, cv->origin);
			r->effects |= EF_NOINTERP;
		}
		m->m_Activity = ACT_RESET;
	}
	m->enemy = EH_NONE;
	if (m->health > 0)
		m->m_IdealMonsterState = MONSTERSTATE_IDLE;
	else
	{
		m->m_IdealMonsterState = MONSTERSTATE_DEAD;
		Mon_SetConditions(m, bits_COND_LIGHT_DAMAGE);
		m->deadflag = DEAD_DYING;
		Mon_FCheckAITrigger(e);
		m->deadflag = DEAD_NO;
	}
	m->spawnflags &= ~SF_MONSTER_WAIT_FOR_SCRIPT;
	return 1;
}

/* ---- class entries ---------------------------------------------------------------- */

static int CCineMonster_ObjectCaps(int e) { (void)e; return 0; }	/* & ~FCAP_ACROSS_TRANSITION */

static const game_vt_t cine_vt = {
	.ObjectCaps = CCineMonster_ObjectCaps,
	.Activate = CCineMonster_Activate,
};

/* HLSDK CCineMonster::m_SaveData (+ m_hTargetEnt, HLSDK's CBaseMonster
 * member, and the sequence lookup cache of our layout). A sequence bound
 * to a monster (playing or waiting) is not quiescent. */
static int CCineMonster_quiet(int e)
{
	return EHGet(CINE(e)->m_hTargetEnt) < 0;
}

static const sv_field_t f_CCineMonster[] = {
	SVF(CCineMonster, m_hTargetEnt, SV_EH, 0),
	SVF(CCineMonster, m_iszIdle, SV_U8, SVK),
	SVF(CCineMonster, m_iszPlay, SV_U8, SVK),
	SVF(CCineMonster, m_iszEntity, SV_NAME, SVK),
	SVF(CCineMonster, m_clsEntity, SV_U16, SVK),
	SVF(CCineMonster, m_seqModel, SV_RES, 0),
	SVF(CCineMonster, m_seqIdle, SV_I16, 0),
	SVF(CCineMonster, m_seqPlay, SV_I16, 0),
	SVF(CCineMonster, m_fMoveTo, SV_I32, SVK),
	SVF(CCineMonster, m_iFinishSchedule, SV_I32, SVK),
	SVF(CCineMonster, m_flRadius, SV_F32, SVK),
	SVF(CCineMonster, m_flRepeat, SV_F32, SVK),
	SVF(CCineMonster, m_iDelay, SV_I32, 0),
	SVF(CCineMonster, m_startTime, SV_TIME, 0),
	SVF(CCineMonster, m_saved_movetype, SV_I32, 0),
	SVF(CCineMonster, m_saved_solid, SV_I32, 0),
	SVF(CCineMonster, m_saved_effects, SV_I32, 0),
	SVF(CCineMonster, m_interruptable, SV_U8, 0),
	SVF(CCineMonster, m_ai, SV_U8, SVK),
};
static const sv_table_t sv_CCineMonster = SV_TABLE(&sv_CBaseToggle, f_CCineMonster,
	.quiet = CCineMonster_quiet);

const game_class_t gc_SCRIPTED_SEQUENCE = {
	.size = sizeof(CCineMonster),
	.Spawn = CCineMonster_Spawn,
	.vt = &cine_vt,
	.save = &sv_CCineMonster,
};

const game_class_t gc_AISCRIPTED_SEQUENCE = {
	.size = sizeof(CCineMonster),
	.Spawn = CCineMonster_Spawn,
	.vt = &cine_vt,
	.save = &sv_CCineMonster,
};
