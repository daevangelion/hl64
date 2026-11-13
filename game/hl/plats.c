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
 * plats.c -- HLSDK dlls/plats.cpp: func_plat (+ its runtime inside
 * trigger, class plat_trigger), func_train and func_guntarget. Path
 * corners are path_corner entities (world.c): message = the name fired on
 * arrival, speed, wait (GetDelay), spawnflags.
 */
#include "game/hl/util.h"

#define SF_PLAT_TOGGLE			0x0001
#define SF_TRAIN_WAIT_RETRIGGER		1
#define SF_TRACKTRAIN_PASSABLE		0x0008
#define SF_CORNER_WAITFORTRIG		0x001
#define SF_CORNER_TELEPORT		0x002
#define SF_CORNER_FIREONCE		0x004
#define FGUNTARGET_START_ON		0x0001

/* CBasePlatTrain: noise = noiseMoving, noise1 = noiseArrived */
typedef struct {
	CBASEENTITY_FIELDS
	CBASEDELAY_FIELDS
	CBASEANIMATING_FIELDS
	CBASETOGGLE_FIELDS
	float		m_volume;
	uint8_t		m_bMoveSnd;
	uint8_t		m_bStopSnd;
	uint8_t		m_activated;
	uint8_t		pad;
	ent_h		m_pevCurrentTarget;	/* func_train */
	int32_t		m_sounds;
} CBasePlatTrain;

static int NoTransition_ObjectCaps(int e)
{
	(void)e;
	return 0;
}

static int CBasePlatTrain_GetToggleState(int e)
{
	return PRIV(CBaseToggle, e)->m_toggle_state;
}

static float CBasePlatTrain_GetDelay(int e)
{
	return PRIV(CBaseToggle, e)->m_flWait;
}

static void CBasePlatTrain_KeyValue(int e, gid_t16 gid)
{
	CBasePlatTrain *p = PRIV(CBasePlatTrain, e);

	CBaseToggle_KeyValue(e, gid);
	p->m_hActivator = EH_NONE;
	p->m_pevCurrentTarget = EH_NONE;
	if (gid == GID_NONE)
		return;
	p->m_flHeight = KeyF(gid, K_HEIGHT, 0);
	p->m_bMoveSnd = (uint8_t)KeyF(gid, K_MOVESND, 0);
	p->m_bStopSnd = (uint8_t)KeyF(gid, K_STOPSND, 0);
	p->m_volume = KeyF(gid, K_VOLUME, 0);
}

static res_t plat_move_sound(int n)
{
	switch (n)
	{
	case 1: return RESID(PLAT_BIGMOVE1);
	case 2: return RESID(PLAT_BIGMOVE2);
	case 3: return RESID(PLAT_ELEVMOVE1);
	case 4: return RESID(PLAT_ELEVMOVE2);
	case 5: return RESID(PLAT_ELEVMOVE3);
	case 6: return RESID(PLAT_FREIGHTMOVE1);
	case 7: return RESID(PLAT_FREIGHTMOVE2);
	case 8: return RESID(PLAT_HEAVYMOVE1);
	case 9: return RESID(PLAT_RACKMOVE1);
	case 10: return RESID(PLAT_RAILMOVE1);
	case 11: return RESID(PLAT_SQUEEKMOVE1);
	case 12: return RESID(PLAT_TALKMOVE1);
	case 13: return RESID(PLAT_TALKMOVE2);
	default: return RESID(SND_NULL);
	}
}

static res_t plat_stop_sound(int n)
{
	switch (n)
	{
	case 1: return RESID(PLAT_BIGSTOP1);
	case 2: return RESID(PLAT_BIGSTOP2);
	case 3: return RESID(PLAT_FREIGHTSTOP1);
	case 4: return RESID(PLAT_HEAVYSTOP2);
	case 5: return RESID(PLAT_RACKSTOP1);
	case 6: return RESID(PLAT_RAILSTOP1);
	case 7: return RESID(PLAT_SQUEEKSTOP1);
	case 8: return RESID(PLAT_TALKSTOP1);
	default: return RESID(SND_NULL);
	}
}

static void CBasePlatTrain_Precache(int e)
{
	CBasePlatTrain *p = PRIV(CBasePlatTrain, e);

	p->noise = plat_move_sound(p->m_bMoveSnd);
	p->noise1 = plat_stop_sound(p->m_bStopSnd);
	PRECACHE(p->noise);
	PRECACHE(p->noise1);
}

/* the train / guntarget position at a corner: corner - (mins + maxs) / 2 */
static void corner_dest(int e, int corner, float out[3])
{
	const col_ent_t *c = ent_cv(e);
	int i;

	for (i = 0; i < 3; i++)
		out[i] = ent_cv(corner)->origin[i] - (c->mins[i] + c->maxs[i]) * 0.5f;
}

/* ---- func_plat ------------------------------------------------------------ */

void CFuncPlat_GoDown(int e)
{
	CBasePlatTrain *p = PRIV(CBasePlatTrain, e);

	if (p->noise)
		EMIT_SOUND(e, CHAN_STATIC, p->noise, p->m_volume, ATTN_NORM);
	p->m_toggle_state = TS_GOING_DOWN;
	p->m_pfnCallWhenMoveDone = FN_CFuncPlat_HitBottom;
	LinearMove(e, p->m_vecPosition2, p->speed);
}

void CFuncPlat_HitBottom(int e)
{
	CBasePlatTrain *p = PRIV(CBasePlatTrain, e);

	if (p->noise)
		STOP_SOUND(e, CHAN_STATIC, p->noise);
	if (p->noise1)
		EMIT_SOUND(e, CHAN_WEAPON, p->noise1, p->m_volume, ATTN_NORM);
	p->m_toggle_state = TS_AT_BOTTOM;
}

void CFuncPlat_GoUp(int e)
{
	CBasePlatTrain *p = PRIV(CBasePlatTrain, e);

	if (p->noise)
		EMIT_SOUND(e, CHAN_STATIC, p->noise, p->m_volume, ATTN_NORM);
	p->m_toggle_state = TS_GOING_UP;
	p->m_pfnCallWhenMoveDone = FN_CFuncPlat_HitTop;
	LinearMove(e, p->m_vecPosition1, p->speed);
}

void CFuncPlat_HitTop(int e)
{
	CBasePlatTrain *p = PRIV(CBasePlatTrain, e);

	if (p->noise)
		STOP_SOUND(e, CHAN_STATIC, p->noise);
	if (p->noise1)
		EMIT_SOUND(e, CHAN_WEAPON, p->noise1, p->m_volume, ATTN_NORM);
	p->m_toggle_state = TS_AT_TOP;
	if (!(p->spawnflags & SF_PLAT_TOGGLE))
	{
		SetThink(e, FN_CFuncPlat_GoDown);
		ThinkInLtime(e, 3);
	}
}

void CFuncPlat_Blocked(int e, int other)
{
	CBasePlatTrain *p = PRIV(CBasePlatTrain, e);

	TakeDamage(other, e, e, 1, DMG_CRUSH);
	if (p->noise)
		STOP_SOUND(e, CHAN_STATIC, p->noise);
	if (p->m_toggle_state == TS_GOING_UP)
		CFuncPlat_GoDown(e);
	else if (p->m_toggle_state == TS_GOING_DOWN)
		CFuncPlat_GoUp(e);
}

void CFuncPlat_PlatUse(int e, int activator, int caller, int use_type, float value)
{
	CBasePlatTrain *p = PRIV(CBasePlatTrain, e);

	(void)activator;
	(void)caller;
	(void)value;
	if (p->spawnflags & SF_PLAT_TOGGLE)
	{
		if (!ShouldToggle(use_type, p->m_toggle_state == TS_AT_BOTTOM))
			return;
		if (p->m_toggle_state == TS_AT_TOP)
			CFuncPlat_GoDown(e);
		else if (p->m_toggle_state == TS_AT_BOTTOM)
			CFuncPlat_GoUp(e);
	}
	else
	{
		SetUse(e, FN_NONE);
		if (p->m_toggle_state == TS_AT_TOP)
			CFuncPlat_GoDown(e);
	}
}

/* CPlatTrigger: m_pPlatform as a handle */
typedef struct {
	CBASEENTITY_FIELDS
	ent_h		m_pPlatform;
} CPlatTrigger;

void CPlatTrigger_Touch(int e, int other)
{
	int plat = EHGet(PRIV(CPlatTrigger, e)->m_pPlatform);
	CBasePlatTrain *p;

	if (!IsPlayer(other))
		return;
	if (!IsAlive(other) || plat < 0)
		return;
	p = PRIV(CBasePlatTrain, plat);
	if (p->m_toggle_state == TS_AT_BOTTOM)
		CFuncPlat_GoUp(plat);
	else if (p->m_toggle_state == TS_AT_TOP)
		ent_think_at(plat, ent_tm(plat)->ltime + 1);	/* delay going down */
}

static int CPlatTrigger_Spawn(int e, gid_t16 gid)
{
	(void)gid;
	PRIV(CPlatTrigger, e)->m_pPlatform = EH_NONE;
	return 0;
}

static int CPlatTrigger_ObjectCaps(int e)
{
	(void)e;
	return FCAP_DONT_SAVE;
}

static const game_vt_t CPlatTrigger_vt = {
	.ObjectCaps = CPlatTrigger_ObjectCaps,
};

const game_class_t gc_PLAT_TRIGGER = {
	.size = sizeof(CPlatTrigger), .Spawn = CPlatTrigger_Spawn, .vt = &CPlatTrigger_vt,
};

static void PlatSpawnInsideTrigger(int plat)
{
	CBasePlatTrain *p = PRIV(CBasePlatTrain, plat);
	const col_ent_t *pc = ent_cv(plat);
	float tmin[3], tmax[3];
	int t = CBaseEntity_Create(CLS_PLAT_TRIGGER, pc->origin, g_vecZero, -1);

	if (t < 0)
		return;
	PRIV(CPlatTrigger, t)->m_pPlatform = EHANDLE(plat);
	ent_set_movetype(t, MOVETYPE_NONE);
	ent_set_solid(t, SOLID_TRIGGER);
	VecSet(tmin, pc->mins[0] + 25, pc->mins[1] + 25, pc->mins[2]);
	VecSet(tmax, pc->maxs[0] + 25, pc->maxs[1] + 25, pc->maxs[2] + 8);
	tmin[2] = tmax[2] - (p->m_vecPosition1[2] - p->m_vecPosition2[2] + 8);
	if (pc->maxs[0] - pc->mins[0] <= 50)
	{
		tmin[0] = (pc->mins[0] + pc->maxs[0]) / 2;
		tmax[0] = tmin[0] + 1;
	}
	if (pc->maxs[1] - pc->mins[1] <= 50)
	{
		tmin[1] = (pc->mins[1] + pc->maxs[1]) / 2;
		tmax[1] = tmin[1] + 1;
	}
	UTIL_SetSize(t, tmin, tmax);
	SetTouch(t, FN_CPlatTrigger_Touch);
}

static int CFuncPlat_Spawn(int e, gid_t16 gid)
{
	CBasePlatTrain *p = PRIV(CBasePlatTrain, e);
	const col_ent_t *c = ent_cv(e);

	CBasePlatTrain_KeyValue(e, gid);
	/* Setup */
	if (p->m_flTLength == 0)
		p->m_flTLength = 80;
	if (p->m_flTWidth == 0)
		p->m_flTWidth = 10;
	VecSet(ent_cv(e)->angles, 0, 0, 0);
	ent_set_movetype(e, MOVETYPE_PUSH);
	ent_set_solid(e, SOLID_BSP);
	ent_set_origin(e, c->origin);
	ent_set_model(e, p->model);
	VecCopy(c->origin, p->m_vecPosition1);
	VecCopy(c->origin, p->m_vecPosition2);
	if (p->m_flHeight != 0)
		p->m_vecPosition2[2] = c->origin[2] - p->m_flHeight;
	else
		p->m_vecPosition2[2] = c->origin[2] - (c->maxs[2] - c->mins[2]) + 8;
	if (p->speed == 0)
		p->speed = 150;
	if (p->m_volume == 0)
		p->m_volume = 0.85f;
	/* Precache */
	CBasePlatTrain_Precache(e);
	if (!(p->spawnflags & SF_PLAT_TOGGLE))
		PlatSpawnInsideTrigger(e);
	SetBlocked(e, FN_CFuncPlat_Blocked);
	if (ent_idc(e)->targetname != NAME_NONE)
	{
		ent_set_origin(e, p->m_vecPosition1);
		p->m_toggle_state = TS_AT_TOP;
		SetUse(e, FN_CFuncPlat_PlatUse);
	}
	else
	{
		ent_set_origin(e, p->m_vecPosition2);
		p->m_toggle_state = TS_AT_BOTTOM;
	}
	return 0;
}

static const game_vt_t CBasePlatTrain_vt = {
	.ObjectCaps = NoTransition_ObjectCaps,
	.GetToggleState = CBasePlatTrain_GetToggleState,
	.GetDelay = CBasePlatTrain_GetDelay,
};

const game_class_t gc_FUNC_PLAT = {
	.size = sizeof(CBasePlatTrain), .Spawn = CFuncPlat_Spawn, .vt = &CBasePlatTrain_vt,
};

/* ---- func_train ---------------------------------------------------------- */

void CFuncTrain_Blocked(int e, int other)
{
	CBasePlatTrain *p = PRIV(CBasePlatTrain, e);
	float t = gtime();

	if (t < p->m_flActivateFinished)
		return;
	p->m_flActivateFinished = t + 0.5f;
	TakeDamage(other, e, e, p->dmg, DMG_CRUSH);
}

static void train_stop_sounds(int e)
{
	CBasePlatTrain *p = PRIV(CBasePlatTrain, e);

	if (p->noise)
		STOP_SOUND(e, CHAN_STATIC, p->noise);
	if (p->noise1)
		EMIT_SOUND(e, CHAN_VOICE, p->noise1, p->m_volume, ATTN_NORM);
}

void CFuncTrain_Next(int e)
{
	CBasePlatTrain *p = PRIV(CBasePlatTrain, e);
	int targ = GetNextTarget(e), cur;
	float dest[3];

	if (targ < 0)
	{
		train_stop_sounds(e);
		return;
	}
	p->message = p->target;
	p->target = CBase(targ)->target;
	p->m_flWait = GetDelay(targ);
	cur = EHGet(p->m_pevCurrentTarget);
	if (cur >= 0 && CBase(cur)->speed != 0)
		p->speed = CBase(cur)->speed;
	p->m_pevCurrentTarget = EHANDLE(targ);
	p->enemy = EHANDLE(targ);
	corner_dest(e, targ, dest);
	if (CBase(targ)->spawnflags & SF_CORNER_TELEPORT)
	{
		ent_rnd(e)->effects |= EF_NOINTERP;
		ent_set_origin(e, dest);
		CFuncTrain_Wait(e);
	}
	else
	{
		if (p->noise)
		{
			STOP_SOUND(e, CHAN_STATIC, p->noise);
			EMIT_SOUND(e, CHAN_STATIC, p->noise, p->m_volume, ATTN_NORM);
		}
		ent_rnd(e)->effects &= ~EF_NOINTERP;
		p->m_pfnCallWhenMoveDone = FN_CFuncTrain_Wait;
		LinearMove(e, dest, p->speed);
	}
}

void CFuncTrain_Wait(int e)
{
	CBasePlatTrain *p = PRIV(CBasePlatTrain, e);
	int cur = EHGet(p->m_pevCurrentTarget);
	CBaseEntity *c = cur >= 0 ? CBase(cur) : 0;

	if (c && c->message != NAME_NONE)
	{
		FireTargets(c->message, e, e, USE_TOGGLE, 0);
		if (c->spawnflags & SF_CORNER_FIREONCE)
			c->message = NAME_NONE;
	}
	if ((c && (c->spawnflags & SF_CORNER_WAITFORTRIG)) ||
	    (p->spawnflags & SF_TRAIN_WAIT_RETRIGGER))
	{
		p->spawnflags |= SF_TRAIN_WAIT_RETRIGGER;
		train_stop_sounds(e);
		ent_think_at(e, 0);
		return;
	}
	if (p->m_flWait != 0)
	{
		ThinkInLtime(e, p->m_flWait);
		train_stop_sounds(e);
		SetThink(e, FN_CFuncTrain_Next);
	}
	else
		CFuncTrain_Next(e);
}

void CFuncTrain_Use(int e, int activator, int caller, int use_type, float value)
{
	CBasePlatTrain *p = PRIV(CBasePlatTrain, e);
	int en;

	(void)activator;
	(void)caller;
	(void)use_type;
	(void)value;
	if (p->spawnflags & SF_TRAIN_WAIT_RETRIGGER)
	{
		p->spawnflags &= ~SF_TRAIN_WAIT_RETRIGGER;
		CFuncTrain_Next(e);
	}
	else
	{
		p->spawnflags |= SF_TRAIN_WAIT_RETRIGGER;
		en = EHGet(p->enemy);
		if (en >= 0)
			p->target = ent_idc(en)->targetname;
		ent_think_at(e, 0);
		ent_set_velocity(e, g_vecZero);
		if (p->noise1)
			EMIT_SOUND(e, CHAN_VOICE, p->noise1, p->m_volume, ATTN_NORM);
	}
}

static void CFuncTrain_Activate(int e)
{
	CBasePlatTrain *p = PRIV(CBasePlatTrain, e);
	float dest[3];
	int targ;

	if (p->m_activated)
		return;
	p->m_activated = 1;
	targ = UTIL_FindByTargetname1(p->target);
	if (targ < 0)
	{
		log_printf("game: func_train %d: no first corner\n", e);
		return;
	}
	p->target = CBase(targ)->target;
	p->m_pevCurrentTarget = EHANDLE(targ);
	corner_dest(e, targ, dest);
	ent_set_origin(e, dest);
	if (ent_idc(e)->targetname == NAME_NONE)
	{
		ThinkInLtime(e, 0.1f);
		SetThink(e, FN_CFuncTrain_Next);
	}
	else
		p->spawnflags |= SF_TRAIN_WAIT_RETRIGGER;
}

static int CFuncTrain_Spawn(int e, gid_t16 gid)
{
	CBasePlatTrain *p = PRIV(CBasePlatTrain, e);

	CBasePlatTrain_KeyValue(e, gid);
	p->m_sounds = (int)KeyF(gid, K_SOUNDS, 0);
	CBasePlatTrain_Precache(e);
	if (p->speed == 0)
		p->speed = 100;
	if (p->target == NAME_NONE)
		log_printf("game: FuncTrain with no target\n");
	if (p->dmg == 0)
		p->dmg = 2;
	ent_set_movetype(e, MOVETYPE_PUSH);
	ent_set_solid(e, (p->spawnflags & SF_TRACKTRAIN_PASSABLE) ? SOLID_NOT : SOLID_BSP);
	ent_set_model(e, p->model);
	ent_set_origin(e, ent_cv(e)->origin);
	p->m_activated = 0;
	if (p->m_volume == 0)
		p->m_volume = 0.85f;
	SetUse(e, FN_CFuncTrain_Use);
	SetBlocked(e, FN_CFuncTrain_Blocked);
	return 0;
}

static const game_vt_t CFuncTrain_vt = {
	.ObjectCaps = NoTransition_ObjectCaps,
	.GetToggleState = CBasePlatTrain_GetToggleState,
	.GetDelay = CBasePlatTrain_GetDelay,
	.Activate = CFuncTrain_Activate,
};

const game_class_t gc_FUNC_TRAIN = {
	.size = sizeof(CBasePlatTrain), .Spawn = CFuncTrain_Spawn, .vt = &CFuncTrain_vt,
};

/* ---- func_guntarget (CGunTarget) ------------------------------------------ */

typedef struct {
	CBASEENTITY_FIELDS
	CBASEDELAY_FIELDS
	CBASEANIMATING_FIELDS
	CBASETOGGLE_FIELDS
	ent_h		m_hTargetEnt;
	uint8_t		m_on;
	uint8_t		pad[3];
} CGunTarget;

static void CGunTarget_Stop(int e)
{
	ent_set_velocity(e, g_vecZero);
	ent_think_at(e, 0);
	CBase(e)->takedamage = DAMAGE_NO;
}

void CGunTarget_Next(int e)
{
	CGunTarget *p = PRIV(CGunTarget, e);
	float dest[3];
	int t;

	SetThink(e, FN_NONE);
	t = GetNextTarget(e);
	p->m_hTargetEnt = EHANDLE(t);
	if (t < 0)
	{
		CGunTarget_Stop(e);
		return;
	}
	p->m_pfnCallWhenMoveDone = FN_CGunTarget_Wait;
	corner_dest(e, t, dest);
	LinearMove(e, dest, p->speed);
}

void CGunTarget_Wait(int e)
{
	CGunTarget *p = PRIV(CGunTarget, e);
	int t = EHGet(p->m_hTargetEnt);
	CBaseEntity *c;

	if (t < 0)
	{
		CGunTarget_Stop(e);
		return;
	}
	c = CBase(t);
	if (c->message != NAME_NONE)
	{
		FireTargets(c->message, e, e, USE_TOGGLE, 0);
		if (c->spawnflags & SF_CORNER_FIREONCE)
			c->message = NAME_NONE;
	}
	p->m_flWait = GetDelay(t);
	p->target = c->target;
	SetThink(e, FN_CGunTarget_Next);
	if (p->m_flWait != 0)
		ThinkInLtime(e, p->m_flWait);
	else
		CGunTarget_Next(e);
}

void CGunTarget_Start(int e)
{
	CGunTarget_Use(e, e, e, USE_ON, 0);
}

void CGunTarget_Use(int e, int activator, int caller, int use_type, float value)
{
	CGunTarget *p = PRIV(CGunTarget, e);
	int t;

	(void)activator;
	(void)caller;
	(void)value;
	if (!ShouldToggle(use_type, p->m_on))
		return;
	if (p->m_on)
		CGunTarget_Stop(e);
	else
	{
		p->takedamage = DAMAGE_AIM;
		t = GetNextTarget(e);
		p->m_hTargetEnt = EHANDLE(t);
		if (t < 0)
			return;
		p->health = p->max_health;
		CGunTarget_Next(e);
	}
}

static int CGunTarget_TakeDamage(int e, int inflictor, int attacker, float damage, int bits)
{
	CGunTarget *p = PRIV(CGunTarget, e);

	(void)inflictor;
	(void)attacker;
	(void)bits;
	if (p->health > 0)
	{
		p->health -= damage;
		if (p->health <= 0)
		{
			p->health = 0;
			CGunTarget_Stop(e);
			if (p->message != NAME_NONE)
				FireTargets(p->message, e, e, USE_TOGGLE, 0);
		}
	}
	return 0;
}

static int CGunTarget_BloodColor(int e)
{
	(void)e;
	return DONT_BLEED;
}

static int CGunTarget_Classify(int e)
{
	(void)e;
	return CLASS_MACHINE;
}

static void CGunTarget_BodyTarget(int e, const float src[3], float out[3])
{
	(void)src;
	VecCopy(ent_cv(e)->origin, out);
}

static void CGunTarget_Activate(int e)
{
	CGunTarget *p = PRIV(CGunTarget, e);
	float dest[3];
	int t = GetNextTarget(e);

	if (t >= 0)
	{
		p->m_hTargetEnt = EHANDLE(t);
		corner_dest(e, t, dest);
		ent_set_origin(e, dest);
	}
}

static int CGunTarget_Spawn(int e, gid_t16 gid)
{
	CGunTarget *p = PRIV(CGunTarget, e);

	CBaseToggle_KeyValue(e, gid);
	p->m_hActivator = EH_NONE;
	p->m_hTargetEnt = EH_NONE;
	p->message = gid == GID_NONE ? NAME_NONE : rec_key_name(gid, K_MESSAGE);
	ent_set_movetype(e, MOVETYPE_PUSH);
	ent_set_solid(e, SOLID_BSP);
	ent_set_origin(e, ent_cv(e)->origin);
	ent_set_model(e, p->model);
	if (p->speed == 0)
		p->speed = 100;
	p->takedamage = DAMAGE_NO;
	EFlagsSet(e, FL_MONSTER);
	p->m_on = 0;
	p->max_health = p->health;
	SetUse(e, FN_CGunTarget_Use);
	if (p->spawnflags & FGUNTARGET_START_ON)
	{
		SetThink(e, FN_CGunTarget_Start);
		ThinkInLtime(e, 0.3f);
	}
	return 0;
}

static const game_vt_t CGunTarget_vt = {
	.ObjectCaps = NoTransition_ObjectCaps,
	.Classify = CGunTarget_Classify,
	.BloodColor = CGunTarget_BloodColor,
	.TakeDamage = CGunTarget_TakeDamage,
	.BodyTarget = CGunTarget_BodyTarget,
	.Activate = CGunTarget_Activate,
	.GetToggleState = CBasePlatTrain_GetToggleState,
	.GetDelay = CBasePlatTrain_GetDelay,
};

const game_class_t gc_FUNC_GUNTARGET = {
	.size = sizeof(CGunTarget), .Spawn = CGunTarget_Spawn, .vt = &CGunTarget_vt,
};
