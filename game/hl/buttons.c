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
 * buttons.c -- HLSDK dlls/buttons.cpp: func_button, func_rot_button,
 * momentary_rot_button and DoSpark. (multisource and env_global are logic
 * records: logic.c; env_spark: env.c.)
 */
#include "game/hl/doors.h"

#define SF_BUTTON_DONTMOVE	1
#define SF_ROTBUTTON_NOTSOLID	1
#define SF_BUTTON_TOGGLE	32
#define SF_BUTTON_SPARK_IF_OFF	64
#define SF_BUTTON_TOUCH_ONLY	256
#define SF_MOMENTARY_DOOR	0x0001
#define SF_PENDULUM_AUTO_RETURN	16

#define BUTTON_NOTHING		0
#define BUTTON_ACTIVATE		1
#define BUTTON_RETURN		2

typedef struct {
	CBASEENTITY_FIELDS
	CBASEDELAY_FIELDS
	CBASEANIMATING_FIELDS
	CBASETOGGLE_FIELDS
	locksound_t	m_ls;
	uint8_t		m_fStayPushed;
	uint8_t		m_fRotating;
	uint8_t		m_bLockedSound;
	uint8_t		m_bLockedSentence;
	uint8_t		m_bUnlockedSound;
	uint8_t		m_bUnlockedSentence;
	uint8_t		m_sounds;
	uint8_t		pad;
	name_t		m_strChangeTarget;
} CBaseButton;

/* DoSpark(pev, location): sparks at location + size / 2, a random spark
 * sound on the entity (e < 0: at the point, for logic records) */
void DoSpark(int e, const float location[3], const float size[3])
{
	float tmp[3], vol = RANDOM_FLOAT(0.25f, 0.75f) * 0.4f;
	res_t s;

	VecMA(location, 0.5f, size, tmp);
	UTIL_Sparks(tmp);
	switch ((int)(RANDOM_FLOAT(0, 1) * 6))
	{
	case 0: s = RESID(SPARK1); break;
	case 1: s = RESID(SPARK2); break;
	case 2: s = RESID(SPARK3); break;
	case 3: s = RESID(SPARK4); break;
	case 4: s = RESID(SPARK5); break;
	case 5: s = RESID(SPARK6); break;
	default: return;
	}
	if (e >= 0)
		EMIT_SOUND(e, CHAN_VOICE, s, vol, ATTN_NORM);
	else
		snd_ambient(tmp, s, vol, ATTN_NORM, 0, PITCH_NORM);
}

void DoSparkPrecache(void)
{
	PRECACHE(RESID(SPARK1));
	PRECACHE(RESID(SPARK2));
	PRECACHE(RESID(SPARK3));
	PRECACHE(RESID(SPARK4));
	PRECACHE(RESID(SPARK5));
	PRECACHE(RESID(SPARK6));
}

static void CBaseButton_KeyValue(int e, gid_t16 gid)
{
	CBaseButton *p = PRIV(CBaseButton, e);

	CBaseToggle_KeyValue(e, gid);
	p->m_hActivator = EH_NONE;
	p->m_strChangeTarget = NAME_NONE;
	if (gid == GID_NONE)
		return;
	p->m_strChangeTarget = rec_key_name(gid, K_CHANGETARGET);
	p->m_bLockedSound = (uint8_t)KeyF(gid, K_LOCKED_SOUND, 0);
	p->m_bLockedSentence = (uint8_t)KeyF(gid, K_LOCKED_SENTENCE, 0);
	p->m_bUnlockedSound = (uint8_t)KeyF(gid, K_UNLOCKED_SOUND, 0);
	p->m_bUnlockedSentence = (uint8_t)KeyF(gid, K_UNLOCKED_SENTENCE, 0);
	p->m_sounds = (uint8_t)(int)KeyF(gid, K_SOUNDS, 0);
}

static void CBaseButton_Precache(int e)
{
	CBaseButton *p = PRIV(CBaseButton, e);

	if (p->spawnflags & SF_BUTTON_SPARK_IF_OFF)
		DoSparkPrecache();
	LockSoundsPrecache(&p->m_ls, p->m_bLockedSound, p->m_bUnlockedSound,
		p->m_bLockedSentence, p->m_bUnlockedSentence);
}

static int ButtonResponseToTouch(int e)
{
	CBaseButton *p = PRIV(CBaseButton, e);
	int ts = p->m_toggle_state;

	if (ts == TS_GOING_UP || ts == TS_GOING_DOWN ||
	    (ts == TS_AT_TOP && !p->m_fStayPushed && !(p->spawnflags & SF_BUTTON_TOGGLE)))
		return BUTTON_NOTHING;
	if (ts == TS_AT_TOP)
	{
		if ((p->spawnflags & SF_BUTTON_TOGGLE) && !p->m_fStayPushed)
			return BUTTON_RETURN;
	}
	else
		return BUTTON_ACTIVATE;
	return BUTTON_NOTHING;
}

static void ButtonActivate(int e)
{
	CBaseButton *p = PRIV(CBaseButton, e);

	EMIT_SOUND(e, CHAN_VOICE, p->noise, 1, ATTN_NORM);
	if (!UTIL_IsMasterTriggered(p->m_sMaster, EHGet(p->m_hActivator)))
	{
		PlayLockSounds(e, &p->m_ls, 1, 1);
		return;
	}
	PlayLockSounds(e, &p->m_ls, 0, 1);
	p->m_toggle_state = TS_GOING_UP;
	p->m_pfnCallWhenMoveDone = FN_CBaseButton_TriggerAndWait;
	if (!p->m_fRotating)
		LinearMove(e, p->m_vecPosition2, p->speed);
	else
		AngularMove(e, p->m_vecAngle2, p->speed);
}

void CBaseButton_ButtonSpark(int e)
{
	const col_ent_t *c = ent_cv(e);
	float size[3];

	SetThink(e, FN_CBaseButton_ButtonSpark);
	ThinkIn(e, 0.1f + RANDOM_FLOAT(0, 1.5f));
	VecSub(c->maxs, c->mins, size);
	DoSpark(e, c->mins, size);
}

void CBaseButton_ButtonUse(int e, int activator, int caller, int use_type, float value)
{
	CBaseButton *p = PRIV(CBaseButton, e);

	(void)caller;
	(void)use_type;
	(void)value;
	if (p->m_toggle_state == TS_GOING_UP || p->m_toggle_state == TS_GOING_DOWN)
		return;
	p->m_hActivator = EHANDLE(activator);
	if (p->m_toggle_state == TS_AT_TOP)
	{
		if (!p->m_fStayPushed && (p->spawnflags & SF_BUTTON_TOGGLE))
		{
			EMIT_SOUND(e, CHAN_VOICE, p->noise, 1, ATTN_NORM);
			CBaseButton_ButtonReturn(e);
		}
	}
	else
		ButtonActivate(e);
}

void CBaseButton_ButtonTouch(int e, int other)
{
	CBaseButton *p = PRIV(CBaseButton, e);
	int code;

	if (!IsPlayer(other))
		return;
	p->m_hActivator = EHANDLE(other);
	code = ButtonResponseToTouch(e);
	if (code == BUTTON_NOTHING)
		return;
	if (!UTIL_IsMasterTriggered(p->m_sMaster, other))
	{
		PlayLockSounds(e, &p->m_ls, 1, 1);
		return;
	}
	SetTouch(e, FN_NONE);
	if (code == BUTTON_RETURN)
	{
		EMIT_SOUND(e, CHAN_VOICE, p->noise, 1, ATTN_NORM);
		SUB_UseTargets(e, other, USE_TOGGLE, 0);
		CBaseButton_ButtonReturn(e);
	}
	else
		ButtonActivate(e);
}

void CBaseButton_TriggerAndWait(int e)
{
	CBaseButton *p = PRIV(CBaseButton, e);
	int act = EHGet(p->m_hActivator);

	if (!UTIL_IsMasterTriggered(p->m_sMaster, act))
		return;
	p->m_toggle_state = TS_AT_TOP;
	if (p->m_fStayPushed || (p->spawnflags & SF_BUTTON_TOGGLE))
		SetTouch(e, (p->spawnflags & SF_BUTTON_TOUCH_ONLY) ? FN_CBaseButton_ButtonTouch :
			FN_NONE);
	else
	{
		ThinkInLtime(e, p->m_flWait);
		SetThink(e, FN_CBaseButton_ButtonReturn);
	}
	ent_rnd(e)->frame = 1;		/* use alternate textures */
	SUB_UseTargets(e, act, USE_TOGGLE, 0);
}

void CBaseButton_ButtonReturn(int e)
{
	CBaseButton *p = PRIV(CBaseButton, e);

	p->m_toggle_state = TS_GOING_DOWN;
	p->m_pfnCallWhenMoveDone = FN_CBaseButton_ButtonBackHome;
	if (!p->m_fRotating)
		LinearMove(e, p->m_vecPosition1, p->speed);
	else
		AngularMove(e, p->m_vecAngle1, p->speed);
	ent_rnd(e)->frame = 0;		/* use normal textures */
}

void CBaseButton_ButtonBackHome(int e)
{
	CBaseButton *p = PRIV(CBaseButton, e);
	int act = EHGet(p->m_hActivator), i, n;
	gid_t16 g;

	p->m_toggle_state = TS_AT_BOTTOM;
	if (p->spawnflags & SF_BUTTON_TOGGLE)
		SUB_UseTargets(e, act, USE_TOGGLE, 0);
	if (p->target != NAME_NONE)
	{
		/* re-use only the multisource targets */
		n = rec_name_count(p->target);
		for (i = 0; i < n; i++)
		{
			g = rec_name_gid(p->target, i);
			if (rec_class(g) == CLS_MULTISOURCE && rec_state(g) == REC_LIVE)
				game_logic_use(g, act, e, USE_TOGGLE, 0);
		}
	}
	SetTouch(e, (p->spawnflags & SF_BUTTON_TOUCH_ONLY) ? FN_CBaseButton_ButtonTouch : FN_NONE);
	if (p->spawnflags & SF_BUTTON_SPARK_IF_OFF)
	{
		SetThink(e, FN_CBaseButton_ButtonSpark);
		ThinkIn(e, 0.5f);
	}
}

static int CBaseButton_TakeDamage(int e, int inflictor, int attacker, float damage, int bits)
{
	CBaseButton *p = PRIV(CBaseButton, e);
	int code = ButtonResponseToTouch(e);

	(void)inflictor;
	(void)damage;
	(void)bits;
	if (code == BUTTON_NOTHING)
		return 0;
	SetTouch(e, FN_NONE);
	p->m_hActivator = EHANDLE(attacker);
	if (attacker < 0)
		return 0;
	if (code == BUTTON_RETURN)
	{
		EMIT_SOUND(e, CHAN_VOICE, p->noise, 1, ATTN_NORM);
		if (!(p->spawnflags & SF_BUTTON_TOGGLE))
			SUB_UseTargets(e, attacker, USE_TOGGLE, 0);
		CBaseButton_ButtonReturn(e);
	}
	else
		ButtonActivate(e);
	return 0;
}

static int CBaseButton_ObjectCaps(int e)
{
	return CBase(e)->takedamage ? 0 : FCAP_IMPULSE_USE;
}

static int CBaseButton_GetToggleState(int e)
{
	return PRIV(CBaseToggle, e)->m_toggle_state;
}

static float CBaseButton_GetDelay(int e)
{
	return PRIV(CBaseToggle, e)->m_flWait;
}

static const game_vt_t CBaseButton_vt = {
	.ObjectCaps = CBaseButton_ObjectCaps,
	.TakeDamage = CBaseButton_TakeDamage,
	.GetToggleState = CBaseButton_GetToggleState,
	.GetDelay = CBaseButton_GetDelay,
};

static int CBaseButton_Spawn(int e, gid_t16 gid)
{
	CBaseButton *p = PRIV(CBaseButton, e);
	float d[3];

	CBaseButton_KeyValue(e, gid);
	p->noise = ButtonSound(p->m_sounds);
	PRECACHE(p->noise);
	CBaseButton_Precache(e);
	if (p->spawnflags & SF_BUTTON_SPARK_IF_OFF)
	{
		SetThink(e, FN_CBaseButton_ButtonSpark);
		ThinkIn(e, 0.5f);
	}
	SetMovedir(e);
	ent_set_movetype(e, MOVETYPE_PUSH);
	ent_set_solid(e, SOLID_BSP);
	ent_set_model(e, p->model);
	if (p->speed == 0)
		p->speed = 40;
	if (p->health > 0)
		p->takedamage = DAMAGE_YES;
	if (p->m_flWait == 0)
		p->m_flWait = 1;
	if (p->m_flLip == 0)
		p->m_flLip = 4;
	p->m_toggle_state = TS_AT_BOTTOM;
	VecCopy(ent_cv(e)->origin, p->m_vecPosition1);
	ToggleTravel(e, p->m_flLip, p->m_vecPosition2);
	VecSub(p->m_vecPosition2, p->m_vecPosition1, d);
	if (VecLen(d) < 1 || (p->spawnflags & SF_BUTTON_DONTMOVE))
		VecCopy(p->m_vecPosition1, p->m_vecPosition2);
	p->m_fStayPushed = p->m_flWait == -1;
	p->m_fRotating = 0;
	if (p->spawnflags & SF_BUTTON_TOUCH_ONLY)
		SetTouch(e, FN_CBaseButton_ButtonTouch);
	else
	{
		SetTouch(e, FN_NONE);
		SetUse(e, FN_CBaseButton_ButtonUse);
	}
	return 0;
}

/* HLSDK CBaseButton::m_SaveData (+ the locksound_t it keeps as members) */
static const sv_field_t f_CBaseButton[] = {
	SVF(CBaseButton, m_ls.sLockedSound, SV_RES, SVK),
	SVF(CBaseButton, m_ls.sLockedSentence, SV_RES, SVK),
	SVF(CBaseButton, m_ls.sUnlockedSound, SV_RES, SVK),
	SVF(CBaseButton, m_ls.sUnlockedSentence, SV_RES, SVK),
	SVF(CBaseButton, m_ls.iLockedSentence, SV_I16, 0),
	SVF(CBaseButton, m_ls.iUnlockedSentence, SV_I16, 0),
	SVF(CBaseButton, m_ls.flwaitSound, SV_TIME, 0),
	SVF(CBaseButton, m_ls.flwaitSentence, SV_TIME, 0),
	SVF(CBaseButton, m_ls.bEOFLocked, SV_U8, 0),
	SVF(CBaseButton, m_ls.bEOFUnlocked, SV_U8, 0),
	SVF(CBaseButton, m_fStayPushed, SV_U8, SVK),
	SVF(CBaseButton, m_fRotating, SV_U8, SVK),
	SVF(CBaseButton, m_bLockedSound, SV_U8, SVK),
	SVF(CBaseButton, m_bLockedSentence, SV_U8, SVK),
	SVF(CBaseButton, m_bUnlockedSound, SV_U8, SVK),
	SVF(CBaseButton, m_bUnlockedSentence, SV_U8, SVK),
	SVF(CBaseButton, m_sounds, SV_U8, SVK),
	SVF(CBaseButton, m_strChangeTarget, SV_NAME, SVK),
};
static const sv_table_t sv_CBaseButton = SV_TABLE(&sv_CBaseToggle, f_CBaseButton);

const game_class_t gc_FUNC_BUTTON = {
	.size = sizeof(CBaseButton), .Spawn = CBaseButton_Spawn, .vt = &CBaseButton_vt,
	.save = &sv_CBaseButton,
};

/* ---- func_rot_button ---------------------------------------------------------- */

static int CRotButton_Spawn(int e, gid_t16 gid)
{
	CBaseButton *p = PRIV(CBaseButton, e);

	CBaseButton_KeyValue(e, gid);
	p->noise = ButtonSound(p->m_sounds);
	PRECACHE(p->noise);
	AxisDir(e);
	if (p->spawnflags & SF_DOOR_ROTATE_BACKWARDS)
		VecScale(p->movedir, -1, p->movedir);
	ent_set_movetype(e, MOVETYPE_PUSH);
	ent_set_solid(e, (p->spawnflags & SF_ROTBUTTON_NOTSOLID) ? SOLID_NOT : SOLID_BSP);
	ent_set_model(e, p->model);
	if (p->speed == 0)
		p->speed = 40;
	if (p->m_flWait == 0)
		p->m_flWait = 1;
	if (p->health > 0)
		p->takedamage = DAMAGE_YES;
	p->m_toggle_state = TS_AT_BOTTOM;
	VecCopy(ent_cv(e)->angles, p->m_vecAngle1);
	VecMA(ent_cv(e)->angles, p->m_flMoveDistance, p->movedir, p->m_vecAngle2);
	p->m_fStayPushed = p->m_flWait == -1;
	p->m_fRotating = 1;
	if (!(p->spawnflags & SF_BUTTON_TOUCH_ONLY))
	{
		SetTouch(e, FN_NONE);
		SetUse(e, FN_CBaseButton_ButtonUse);
	}
	else
		SetTouch(e, FN_CBaseButton_ButtonTouch);
	return 0;
}

const game_class_t gc_FUNC_ROT_BUTTON = {
	.size = sizeof(CBaseButton), .Spawn = CRotButton_Spawn, .vt = &CBaseButton_vt,
	.save = &sv_CBaseButton,
};

/* ---- momentary_rot_button ----------------------------------------------------- */

typedef struct {
	CBASEENTITY_FIELDS
	CBASEDELAY_FIELDS
	CBASEANIMATING_FIELDS
	CBASETOGGLE_FIELDS
	int32_t		m_lastUsed;
	int32_t		m_direction;
	float		m_returnSpeed;
	float		m_start[3];
	float		m_end[3];
	int32_t		m_sounds;
} CMomentaryRotButton;

static void UpdateSelf(int e, float value)
{
	CMomentaryRotButton *p = PRIV(CMomentaryRotButton, e);
	ent_tm_t *tm = ent_tm(e);
	int playsound = 0;
	float av[3];

	if (!p->m_lastUsed)
	{
		playsound = 1;
		p->m_direction = -p->m_direction;
	}
	p->m_lastUsed = 1;
	ent_think_at(e, tm->ltime + 0.1f);
	if (p->m_direction > 0 && value >= 1.0f)
	{
		ent_set_avelocity(e, g_vecZero);
		VecCopy(p->m_end, ent_cv(e)->angles);
		ent_relink(e, 0);
		return;
	}
	else if (p->m_direction < 0 && value <= 0)
	{
		ent_set_avelocity(e, g_vecZero);
		VecCopy(p->m_start, ent_cv(e)->angles);
		ent_relink(e, 0);
		return;
	}
	if (playsound)
		EMIT_SOUND(e, CHAN_VOICE, p->noise, 1, ATTN_NORM);
	if (tm->nextthink < tm->ltime)
		ent_think_at(e, tm->ltime + 0.1f);
	else
		ent_think_at(e, tm->nextthink + 0.1f);
	VecScale(p->movedir, p->m_direction * p->speed, av);
	ent_set_avelocity(e, av);
	SetThink(e, FN_CMomentaryRotButton_Off);
}

static void UpdateSelfReturn(int e, float value)
{
	CMomentaryRotButton *p = PRIV(CMomentaryRotButton, e);
	float av[3];

	if (value <= 0)
	{
		ent_set_avelocity(e, g_vecZero);
		VecCopy(p->m_start, ent_cv(e)->angles);
		ent_relink(e, 0);
		ent_think_at(e, -1);
		SetThink(e, FN_NONE);
	}
	else
	{
		VecScale(p->movedir, -p->m_returnSpeed, av);
		ent_set_avelocity(e, av);
		ThinkInLtime(e, 0.1f);
	}
}

/* every momentary_rot_button with the same target (FIND_ENTITY_BY_STRING
 * "target" over the live entities of this map) */
static void UpdateAllButtons(int e, float value, int start)
{
	name_t tgt = CBase(e)->target;
	int b = -1;

	while ((b = ent_next_class(b, CLS_MOMENTARY_ROT_BUTTON, ent_idc(e)->layer)) >= 0)
	{
		if (CBase(b)->target != tgt)
			continue;
		if (start)
			UpdateSelf(b, value);
		else
			UpdateSelfReturn(b, value);
	}
}

static void UpdateTarget(int e, float value)
{
	name_t tgt = CBase(e)->target;
	int it = 0, t;

	if (tgt == NAME_NONE)
		return;
	while ((t = rec_next_named(tgt, &it)) >= 0)
		UseEntity(t, e, e, USE_SET, value);
}

void CMomentaryRotButton_Use(int e, int activator, int caller, int use_type, float value)
{
	CMomentaryRotButton *p = PRIV(CMomentaryRotButton, e);
	float dest[3], v1;
	ent_tm_t *tm = ent_tm(e);

	(void)activator;
	(void)caller;
	(void)use_type;
	(void)value;
	ent_phys(e)->ideal_yaw = AxisDelta(p->spawnflags, ent_cv(e)->angles, p->m_start) /
		p->m_flMoveDistance;
	UpdateAllButtons(e, ent_phys(e)->ideal_yaw, 1);
	VecMA(ent_cv(e)->angles, tm->nextthink - tm->ltime, ent_mot(e)->avelocity, dest);
	v1 = AxisDelta(p->spawnflags, dest, p->m_start) / p->m_flMoveDistance;
	UpdateTarget(e, v1);
}

void CMomentaryRotButton_Off(int e)
{
	CMomentaryRotButton *p = PRIV(CMomentaryRotButton, e);

	ent_set_avelocity(e, g_vecZero);
	p->m_lastUsed = 0;
	if ((p->spawnflags & SF_PENDULUM_AUTO_RETURN) && p->m_returnSpeed > 0)
	{
		SetThink(e, FN_CMomentaryRotButton_Return);
		ThinkInLtime(e, 0.1f);
		p->m_direction = -1;
	}
	else
		SetThink(e, FN_NONE);
}

void CMomentaryRotButton_Return(int e)
{
	CMomentaryRotButton *p = PRIV(CMomentaryRotButton, e);
	float value = AxisDelta(p->spawnflags, ent_cv(e)->angles, p->m_start) / p->m_flMoveDistance;

	UpdateAllButtons(e, value, 0);
	if (value > 0)
		UpdateTarget(e, value);
}

static int CMomentaryRotButton_ObjectCaps(int e)
{
	return (CBase(e)->spawnflags & SF_MOMENTARY_DOOR) ? 0 : FCAP_CONTINUOUS_USE;
}

static const game_vt_t CMomentaryRotButton_vt = {
	.ObjectCaps = CMomentaryRotButton_ObjectCaps,
	.GetToggleState = CBaseButton_GetToggleState,
	.GetDelay = CBaseButton_GetDelay,
};

static int CMomentaryRotButton_Spawn(int e, gid_t16 gid)
{
	CMomentaryRotButton *p = PRIV(CMomentaryRotButton, e);
	float *ang = ent_cv(e)->angles;

	CBaseToggle_KeyValue(e, gid);
	p->m_hActivator = EH_NONE;
	p->m_returnSpeed = KeyF(gid, K_RETURNSPEED, 0);
	p->m_sounds = (int)KeyF(gid, K_SOUNDS, 0);
	AxisDir(e);
	if (p->speed == 0)
		p->speed = 100;
	if (p->m_flMoveDistance < 0)
	{
		VecMA(ang, p->m_flMoveDistance, p->movedir, p->m_start);
		VecCopy(ang, p->m_end);
		p->m_direction = 1;
		p->m_flMoveDistance = -p->m_flMoveDistance;
	}
	else
	{
		VecCopy(ang, p->m_start);
		VecMA(ang, p->m_flMoveDistance, p->movedir, p->m_end);
		p->m_direction = -1;
	}
	ent_set_movetype(e, MOVETYPE_PUSH);
	ent_set_solid(e, (p->spawnflags & SF_MOMENTARY_DOOR) ? SOLID_BSP : SOLID_NOT);
	ent_set_origin(e, ent_cv(e)->origin);
	ent_set_model(e, p->model);
	p->noise = ButtonSound(p->m_sounds);
	PRECACHE(p->noise);
	p->m_lastUsed = 0;
	SetUse(e, FN_CMomentaryRotButton_Use);
	return 0;
}

/* HLSDK CMomentaryRotButton::m_SaveData */
static const sv_field_t f_CMomentaryRotButton[] = {
	SVF(CMomentaryRotButton, m_lastUsed, SV_I32, 0),
	SVF(CMomentaryRotButton, m_direction, SV_I32, 0),
	SVF(CMomentaryRotButton, m_returnSpeed, SV_F32, SVK),
	SVF(CMomentaryRotButton, m_start, SV_VEC, SVK),
	SVF(CMomentaryRotButton, m_end, SV_VEC, SVK),
	SVF(CMomentaryRotButton, m_sounds, SV_I32, SVK),
};
static const sv_table_t sv_CMomentaryRotButton = SV_TABLE(&sv_CBaseToggle,
	f_CMomentaryRotButton);

const game_class_t gc_MOMENTARY_ROT_BUTTON = {
	.size = sizeof(CMomentaryRotButton), .Spawn = CMomentaryRotButton_Spawn,
	.vt = &CMomentaryRotButton_vt, .save = &sv_CMomentaryRotButton,
};
