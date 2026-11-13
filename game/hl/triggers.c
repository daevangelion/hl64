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
 * triggers.c -- HLSDK dlls/triggers.cpp brush triggers on the hl64 API:
 * trigger_multiple / once / counter / hurt / push / teleport / changelevel
 * / transition / endsection and func_ladder. The logic classes of
 * triggers.cpp (multi_manager, trigger_relay, trigger_auto) and buttons.cpp
 * (multisource, env_global) are records without entities: logic.c.
 *
 * M3 has no seams (architecture.md 17): trigger_changelevel fires its
 * targets and its changetarget as HLSDK does and logs
 * `changelevel: <map> landmark <name> (<touch|use>)`; the crossing itself
 * is M4's.
 */
#include <string.h>

#include "engine/in/in.h"
#include "engine/phys/player.h"
#include "game/hl/util.h"

#define SF_TRIGGER_PUSH_ONCE		1
#define SF_TRIGGER_PUSH_START_OFF	2
#define SF_TRIGGER_HURT_TARGETONCE	1
#define SF_TRIGGER_HURT_START_OFF	2
#define SF_TRIGGER_HURT_NO_CLIENTS	8
#define SF_TRIGGER_HURT_CLIENTONLYFIRE	16
#define SF_TRIGGER_HURT_CLIENTONLYTOUCH	32
#define SF_CHANGELEVEL_USEONLY		0x0002
#define SF_ENDSECTION_USEONLY		0x0001
#define SPAWNFLAG_NOMESSAGE		1

/* CBaseTrigger: CBaseToggle + the entvars HLSDK triggers use. */
typedef struct {
	CBASEENTITY_FIELDS
	CBASEDELAY_FIELDS
	CBASEANIMATING_FIELDS
	CBASETOGGLE_FIELDS
	float		pain_finished;
} CBaseTrigger;

/* CChangeLevel */
typedef struct {
	CBASEENTITY_FIELDS
	CBASEDELAY_FIELDS
	CBASEANIMATING_FIELDS
	CBASETOGGLE_FIELDS
	float		pain_finished;
	float		m_changeTargetDelay;
	name_t		m_changeTarget;		/* in this map, NAME_NONE otherwise */
	name_t		m_landmark;		/* landmark (cooked into message) */
	char		m_szMapName[32];
	char		m_szLandmarkName[32];
	char		m_szChangeTarget[32];
} CChangeLevel;

static void copy_key(gid_t16 gid, int k, char *out, int n)
{
	const char *s = gid == GID_NONE ? 0 : rec_key_str(gid, (key_t16)k);

	out[0] = 0;
	if (s)
	{
		strncpy(out, s, (size_t)n - 1);
		out[n - 1] = 0;
	}
}

/* CBaseTrigger::KeyValue (damage, count, damagetype) + CBaseToggle */
static void CBaseTrigger_KeyValue(int e, gid_t16 gid)
{
	CBaseTrigger *p = PRIV(CBaseTrigger, e);

	CBaseToggle_KeyValue(e, gid);
	p->m_hActivator = EH_NONE;
	if (gid == GID_NONE)
		return;
	p->dmg = KeyF(gid, K_DAMAGE, p->dmg);
	p->m_cTriggersLeft = (int)KeyF(gid, K_COUNT, 0);
	p->m_bitsDamageInflict = (int32_t)KeyF(gid, K_DAMAGETYPE, 0);
	p->noise = rec_key_res(gid, K_NOISE);
}

/* CBaseTrigger::InitTrigger */
static void InitTrigger(int e)
{
	if (!VecIsZero(ent_cv(e)->angles))
		SetMovedir(e);
	ent_set_movetype(e, MOVETYPE_NONE);
	ent_set_solid(e, SOLID_TRIGGER);
	ent_set_model(e, CBase(e)->model);
	ent_rnd(e)->effects |= EF_NODRAW;
}

static int CBaseTrigger_ObjectCaps(int e)
{
	(void)e;
	return 0;	/* CBaseToggle::ObjectCaps() & ~FCAP_ACROSS_TRANSITION */
}

static const game_vt_t CBaseTrigger_vt = {
	.ObjectCaps = CBaseTrigger_ObjectCaps,
};

void CBaseTrigger_ToggleUse(int e, int activator, int caller, int use_type, float value)
{
	(void)activator;
	(void)caller;
	(void)use_type;
	(void)value;
	if (ent_cv(e)->solid == SOLID_NOT)
		ent_set_solid(e, SOLID_TRIGGER);	/* gpGlobals->force_retouch: engine */
	else
		ent_set_solid(e, SOLID_NOT);
	ent_set_origin(e, ent_cv(e)->origin);
}

/* ---- trigger_multiple / trigger_once ------------------------------------- */

static void ActivateMultiTrigger(int e, int activator)
{
	CBaseTrigger *p = PRIV(CBaseTrigger, e);

	if (ent_tm(e)->nextthink > gtime())
		return;
	if (!UTIL_IsMasterTriggered(p->m_sMaster, activator))
		return;
	if (p->noise)
		EMIT_SOUND(e, CHAN_VOICE, p->noise, 1, ATTN_NORM);
	p->m_hActivator = EHANDLE(activator);
	SUB_UseTargets(e, activator, USE_TOGGLE, 0);
	if (p->m_flWait > 0)
	{
		SetThink(e, FN_CBaseTrigger_MultiWaitOver);
		ThinkIn(e, p->m_flWait);
	}
	else
	{
		SetTouch(e, FN_NONE);
		ThinkIn(e, 0.1f);
		SetThink(e, FN_SUB_Remove);
	}
}

void CBaseTrigger_MultiWaitOver(int e)
{
	SetThink(e, FN_NONE);
}

void CBaseTrigger_MultiTouch(int e, int other)
{
	uint32_t sf = CBase(e)->spawnflags, of = ent_cv(other)->flags;

	if (((IsPlayer(other) || (of & FL_CLIENT)) && !(sf & SF_TRIGGER_NOCLIENTS)) ||
	    ((of & FL_MONSTER) && (sf & SF_TRIGGER_ALLOWMONSTERS)) ||
	    ((sf & SF_TRIGGER_PUSHABLES) && ent_idc(other)->cls == CLS_FUNC_PUSHABLE))
		ActivateMultiTrigger(e, other);
}

static int CTriggerMultiple_Spawn(int e, gid_t16 gid)
{
	CBaseTrigger *p = PRIV(CBaseTrigger, e);

	CBaseTrigger_KeyValue(e, gid);
	if (p->m_flWait == 0)
		p->m_flWait = 0.2f;
	InitTrigger(e);
	SetTouch(e, FN_CBaseTrigger_MultiTouch);
	return 0;
}

static int CTriggerOnce_Spawn(int e, gid_t16 gid)
{
	CBaseTrigger *p = PRIV(CBaseTrigger, e);

	CBaseTrigger_KeyValue(e, gid);
	p->m_flWait = -1;
	InitTrigger(e);
	SetTouch(e, FN_CBaseTrigger_MultiTouch);
	return 0;
}

const game_class_t gc_TRIGGER_MULTIPLE = {
	.size = sizeof(CBaseTrigger), .Spawn = CTriggerMultiple_Spawn, .vt = &CBaseTrigger_vt,
};
const game_class_t gc_TRIGGER_ONCE = {
	.size = sizeof(CBaseTrigger), .Spawn = CTriggerOnce_Spawn, .vt = &CBaseTrigger_vt,
};

/* ---- trigger_counter ------------------------------------------------------ */

void CBaseTrigger_CounterUse(int e, int activator, int caller, int use_type, float value)
{
	CBaseTrigger *p = PRIV(CBaseTrigger, e);
	int tell;

	(void)caller;
	(void)use_type;
	(void)value;
	p->m_cTriggersLeft--;
	p->m_hActivator = EHANDLE(activator);
	if (p->m_cTriggersLeft < 0)
		return;
	tell = activator >= 0 && IsPlayer(activator) && !(p->spawnflags & SPAWNFLAG_NOMESSAGE);
	if (p->m_cTriggersLeft != 0)
	{
		if (tell)
			log_printf("trigger_counter: %s\n",
				p->m_cTriggersLeft == 1 ? "Only 1 more to go..." :
				p->m_cTriggersLeft == 2 ? "Only 2 more to go..." :
				p->m_cTriggersLeft == 3 ? "Only 3 more to go..." :
				"There are more to go...");
		return;
	}
	if (tell)
		log_printf("trigger_counter: Sequence completed!\n");
	ActivateMultiTrigger(e, activator);
}

static int CTriggerCounter_Spawn(int e, gid_t16 gid)
{
	CBaseTrigger *p = PRIV(CBaseTrigger, e);

	CBaseTrigger_KeyValue(e, gid);
	p->m_flWait = -1;
	if (p->m_cTriggersLeft == 0)
		p->m_cTriggersLeft = 2;
	SetUse(e, FN_CBaseTrigger_CounterUse);
	return 0;
}

const game_class_t gc_TRIGGER_COUNTER = {
	.size = sizeof(CBaseTrigger), .Spawn = CTriggerCounter_Spawn, .vt = &CBaseTrigger_vt,
};

/* ---- trigger_hurt ------------------------------------------------------------- */

/* The player's geiger range (Items, CBasePlayer::m_flgeigerRange). */
void	game_player_geiger_range(float range) __attribute__((weak));

void CTriggerHurt_RadiationThink(int e)
{
	const col_ent_t *c = ent_cv(e), *pc;
	float s1[3], s2[3], d[3];
	int i, pl;

	/* FIND_CLIENT_IN_PVS from the trigger's centre */
	pl = ent_client_in_pvs(e);
	if (pl > 0)
	{
		pc = ent_cv(pl);
		for (i = 0; i < 3; i++)
		{
			s1[i] = (c->absmin[i] + c->absmax[i]) * 0.5f;
			s2[i] = (pc->absmin[i] + pc->absmax[i]) * 0.5f;
		}
		VecSub(s1, s2, d);
		if (game_player_geiger_range)
			game_player_geiger_range(VecLen(d));
	}
	ThinkIn(e, 0.25f);
}

void CBaseTrigger_HurtTouch(int e, int other)
{
	CBaseTrigger *p = PRIV(CBaseTrigger, e);
	CBaseEntity *o = CBase(other);
	float dmg, t = gtime();

	if (!o || !o->takedamage)
		return;
	if ((p->spawnflags & SF_TRIGGER_HURT_CLIENTONLYTOUCH) && !IsPlayer(other))
		return;
	if ((p->spawnflags & SF_TRIGGER_HURT_NO_CLIENTS) && IsPlayer(other))
		return;
	if (p->dmgtime > t && t != p->pain_finished)
		return;
	dmg = p->dmg * 0.5f;
	if (dmg < 0)
		TakeHealth(other, -dmg, p->m_bitsDamageInflict);
	else
		TakeDamage(other, e, e, dmg, p->m_bitsDamageInflict);
	p->pain_finished = t;
	p->dmgtime = t + 0.5f;
	if (p->target != NAME_NONE)
	{
		if ((p->spawnflags & SF_TRIGGER_HURT_CLIENTONLYFIRE) && !IsPlayer(other))
			return;
		SUB_UseTargets(e, other, USE_TOGGLE, 0);
		if (p->spawnflags & SF_TRIGGER_HURT_TARGETONCE)
			p->target = NAME_NONE;
	}
}

static int CTriggerHurt_Spawn(int e, gid_t16 gid)
{
	CBaseTrigger *p = PRIV(CBaseTrigger, e);

	CBaseTrigger_KeyValue(e, gid);
	InitTrigger(e);
	SetTouch(e, FN_CBaseTrigger_HurtTouch);
	SetUse(e, ent_idc(e)->targetname != NAME_NONE ? FN_CBaseTrigger_ToggleUse : FN_NONE);
	if (p->m_bitsDamageInflict & DMG_RADIATION)
	{
		SetThink(e, FN_CTriggerHurt_RadiationThink);
		ThinkIn(e, RANDOM_FLOAT(0.0f, 0.5f));
	}
	if (p->spawnflags & SF_TRIGGER_HURT_START_OFF)
		ent_set_solid(e, SOLID_NOT);
	ent_set_origin(e, ent_cv(e)->origin);
	return 0;
}

const game_class_t gc_TRIGGER_HURT = {
	.size = sizeof(CBaseTrigger), .Spawn = CTriggerHurt_Spawn, .vt = &CBaseTrigger_vt,
};

/* ---- trigger_push ------------------------------------------------------------- */

void CTriggerPush_Touch(int e, int other)
{
	CBaseTrigger *p = PRIV(CBaseTrigger, e);
	const col_ent_t *oc = ent_cv(other);
	float push[3], v[3];

	switch (oc->movetype)
	{
	case MOVETYPE_NONE:
	case MOVETYPE_PUSH:
	case MOVETYPE_NOCLIP:
	case MOVETYPE_FOLLOW:
		return;
	}
	if (oc->solid == SOLID_NOT || oc->solid == SOLID_BSP)
		return;
	VecScale(p->movedir, p->speed, push);
	if (IsPlayer(other))
	{
		ent_player_t *pl = phy_player();

		if (p->spawnflags & SF_TRIGGER_PUSH_ONCE)
		{
			VecAdd(pl->velocity, push, pl->velocity);
			if (pl->velocity[2] > 0)
				pl->flags &= ~PL_FL_ONGROUND;
			UTIL_Remove(e);
		}
		else
		{
			if (pl->flags & FL_BASEVELOCITY)
				VecAdd(push, pl->basevelocity, push);
			VecCopy(push, pl->basevelocity);
			pl->flags |= FL_BASEVELOCITY;
		}
		return;
	}
	if (p->spawnflags & SF_TRIGGER_PUSH_ONCE)
	{
		VecAdd(ent_mot(other)->velocity, push, v);
		ent_set_velocity(other, v);
		if (v[2] > 0)
			EFlagsClear(other, FL_ONGROUND);
		UTIL_Remove(e);
	}
	else
	{
		if (oc->flags & FL_BASEVELOCITY)
			VecAdd(push, ent_mot(other)->basevelocity, push);
		ent_set_basevelocity(other, push);
		EFlagsSet(other, FL_BASEVELOCITY);
	}
}

static int CTriggerPush_Spawn(int e, gid_t16 gid)
{
	CBaseTrigger *p = PRIV(CBaseTrigger, e);

	CBaseTrigger_KeyValue(e, gid);
	if (VecIsZero(ent_cv(e)->angles))
		ent_cv(e)->angles[1] = 360;
	InitTrigger(e);
	if (p->speed == 0)
		p->speed = 100;
	if (p->spawnflags & SF_TRIGGER_PUSH_START_OFF)
		ent_set_solid(e, SOLID_NOT);
	SetTouch(e, FN_CTriggerPush_Touch);
	SetUse(e, FN_CBaseTrigger_ToggleUse);
	ent_set_origin(e, ent_cv(e)->origin);
	return 0;
}

const game_class_t gc_TRIGGER_PUSH = {
	.size = sizeof(CBaseTrigger), .Spawn = CTriggerPush_Spawn, .vt = &CBaseTrigger_vt,
};

/* ---- trigger_teleport ---------------------------------------------------- */

void CBaseTrigger_TeleportTouch(int e, int other)
{
	CBaseTrigger *p = PRIV(CBaseTrigger, e);
	uint32_t of = ent_cv(other)->flags;
	float tmp[3], ang[3];

	if (!IsPlayer(other) && !(of & (FL_CLIENT | FL_MONSTER)))
		return;
	if (!UTIL_IsMasterTriggered(p->m_sMaster, other))
		return;
	if (!(p->spawnflags & SF_TRIGGER_ALLOWMONSTERS) && (of & FL_MONSTER))
		return;
	if ((p->spawnflags & SF_TRIGGER_NOCLIENTS) && IsPlayer(other))
		return;
	if (!UTIL_NamedOrigin(p->target, tmp, ang))
		return;
	if (IsPlayer(other))
	{
		ent_player_t *pl = phy_player();

		tmp[2] -= ent_cv(other)->mins[2];
		tmp[2]++;
		pl->flags &= ~PL_FL_ONGROUND;
		pl->groundentity = -1;
		VecCopy(tmp, pl->origin);
		ent_set_origin(other, tmp);
		VecCopy(ang, ent_cv(other)->angles);
		VecCopy(ang, pl->angles);
		VecCopy(ang, pl->view_angles);
		in_set_angles(ang);		/* v_angle + fixangle */
		VecSet(pl->velocity, 0, 0, 0);
		VecSet(pl->basevelocity, 0, 0, 0);
		log_printf("teleport: player to %.0f %.0f %.0f\n", tmp[0], tmp[1], tmp[2]);
		return;
	}
	tmp[2]++;
	EFlagsClear(other, FL_ONGROUND);
	ent_set_origin(other, tmp);
	VecCopy(ang, ent_cv(other)->angles);
	ent_set_velocity(other, g_vecZero);
	ent_set_basevelocity(other, g_vecZero);
}

static int CTriggerTeleport_Spawn(int e, gid_t16 gid)
{
	CBaseTrigger_KeyValue(e, gid);
	InitTrigger(e);
	SetTouch(e, FN_CBaseTrigger_TeleportTouch);
	return 0;
}

const game_class_t gc_TRIGGER_TELEPORT = {
	.size = sizeof(CBaseTrigger), .Spawn = CTriggerTeleport_Spawn, .vt = &CBaseTrigger_vt,
};

/* ---- func_ladder ------------------------------------------------------------ */

static int CLadder_Spawn(int e, gid_t16 gid)
{
	CBaseTrigger_KeyValue(e, gid);
	ent_set_solid(e, SOLID_NOT);
	ent_cv(e)->skin = CONTENTS_LADDER;
	ent_cv(e)->rendermode = kRenderTransTexture;
	ent_rnd(e)->renderamt = 0;
	ent_rnd(e)->effects &= ~EF_NODRAW;
	ent_set_model(e, CBase(e)->model);
	ent_set_movetype(e, MOVETYPE_PUSH);
	return 0;
}

const game_class_t gc_FUNC_LADDER = {
	.size = sizeof(CBaseTrigger), .Spawn = CLadder_Spawn, .vt = &CBaseTrigger_vt,
};

/* ---- trigger_transition (CTriggerVolume) ------------------------------------ */

static int CTriggerVolume_Spawn(int e, gid_t16 gid)
{
	(void)gid;
	ent_set_solid(e, SOLID_NOT);
	ent_set_movetype(e, MOVETYPE_NONE);
	ent_set_model(e, CBase(e)->model);	/* size and link */
	CBase(e)->model = RES_NONE;
	ent_rnd(e)->model = RES_NONE;
	return 0;
}

const game_class_t gc_TRIGGER_TRANSITION = {
	.size = sizeof(CBaseEntity), .Spawn = CTriggerVolume_Spawn, .vt = &CBaseTrigger_vt,
};

/* ---- trigger_changelevel ---------------------------------------------------- */

/* InTransitionVolume: no trigger_transition named like the landmark, or
 * the player intersects one of them. */
static int InTransitionVolume(int ent, name_t volume)
{
	int it = 0, v, in = 1;

	if (ent_cv(ent)->movetype == MOVETYPE_FOLLOW && ent_phys(ent)->aiment != ENT_NONE)
		ent = ent_phys(ent)->aiment;
	if (volume == NAME_NONE)
		return 1;
	while ((v = rec_next_named(volume, &it)) >= 0)
	{
		if (ent_idc(v)->cls != CLS_TRIGGER_TRANSITION)
			continue;
		if (Intersects(v, ent))
			return 1;
		in = 0;
	}
	return in;
}

static void ChangeLevelNow(int e, int activator, const char *how)
{
	CChangeLevel *p = PRIV(CChangeLevel, e);
	float t = gtime();

	if (t == p->dmgtime)
		return;
	p->dmgtime = t;
	if (!InTransitionVolume(ENT_PLAYER, p->m_landmark))
	{
		log_printf("changelevel: player isn't in the transition volume %s, aborting\n",
			p->m_szLandmarkName);
		return;
	}
	/* CFireAndDie: changetarget after changedelay (always crosses; the
	 * name is in the next map, so M4's seam delivers it there) */
	if (p->m_changeTarget != NAME_NONE)
		rec_fire_at(t + p->m_changeTargetDelay, p->m_changeTarget, NAME_NONE, -1, -1,
			USE_TOGGLE, 0);
	else if (p->m_szChangeTarget[0])
		log_printf("changelevel: changetarget %s (delay %g) is in %s: delivered by the "
			"crossing (M4)\n", p->m_szChangeTarget, (double)p->m_changeTargetDelay,
			p->m_szMapName);
	p->m_hActivator = EHANDLE(activator);
	SUB_UseTargets(e, activator, USE_TOGGLE, 0);
	log_printf("changelevel: %s landmark %s (%s)\n", p->m_szMapName, p->m_szLandmarkName, how);
}

void CChangeLevel_UseChangeLevel(int e, int activator, int caller, int use_type, float value)
{
	(void)caller;
	(void)use_type;
	(void)value;
	ChangeLevelNow(e, activator, "use");
}

void CChangeLevel_TouchChangeLevel(int e, int other)
{
	if (!IsPlayer(other))
		return;
	ChangeLevelNow(e, other, "touch");
}

static int CChangeLevel_Spawn(int e, gid_t16 gid)
{
	CChangeLevel *p = PRIV(CChangeLevel, e);

	CBaseTrigger_KeyValue(e, gid);
	copy_key(gid, K_MAP, p->m_szMapName, sizeof p->m_szMapName);
	copy_key(gid, K_MESSAGE, p->m_szLandmarkName, sizeof p->m_szLandmarkName);
	copy_key(gid, K_CHANGETARGET, p->m_szChangeTarget, sizeof p->m_szChangeTarget);
	p->m_landmark = gid == GID_NONE ? NAME_NONE : rec_key_name(gid, K_MESSAGE);
	p->m_changeTarget = gid == GID_NONE ? NAME_NONE : rec_key_name(gid, K_CHANGETARGET);
	p->m_changeTargetDelay = KeyF(gid, K_CHANGEDELAY, 0);
	if (!p->m_szMapName[0])
		log_printf("game: a trigger_changelevel doesn't have a map\n");
	if (!p->m_szLandmarkName[0])
		log_printf("game: trigger_changelevel to %s doesn't have a landmark\n",
			p->m_szMapName);
	if (ent_idc(e)->targetname != NAME_NONE)
		SetUse(e, FN_CChangeLevel_UseChangeLevel);
	InitTrigger(e);
	if (!(p->spawnflags & SF_CHANGELEVEL_USEONLY))
		SetTouch(e, FN_CChangeLevel_TouchChangeLevel);
	return 0;
}

const game_class_t gc_TRIGGER_CHANGELEVEL = {
	.size = sizeof(CChangeLevel), .Spawn = CChangeLevel_Spawn, .vt = &CBaseTrigger_vt,
};

/* ---- trigger_endsection ----------------------------------------------------- */

typedef struct {
	CBASEENTITY_FIELDS
	CBASEDELAY_FIELDS
	CBASEANIMATING_FIELDS
	CBASETOGGLE_FIELDS
	char		section[32];
} CTriggerEndSection;

/* pfnEndSection: the menu's end of a section (M5); logged. */
static void EndSection(int e)
{
	CTriggerEndSection *p = PRIV(CTriggerEndSection, e);

	if (p->section[0])
		log_printf("endsection: %s\n", p->section);
}

void CTriggerEndSection_EndSectionUse(int e, int activator, int caller, int use_type,
	float value)
{
	(void)caller;
	(void)use_type;
	(void)value;
	if (activator >= 0 && !IsPlayer(activator))
		return;
	SetUse(e, FN_NONE);
	EndSection(e);
	UTIL_Remove(e);
}

void CTriggerEndSection_EndSectionTouch(int e, int other)
{
	if (!IsPlayer(other))
		return;
	SetTouch(e, FN_NONE);
	EndSection(e);
	UTIL_Remove(e);
}

static int CTriggerEndSection_Spawn(int e, gid_t16 gid)
{
	CTriggerEndSection *p = PRIV(CTriggerEndSection, e);

	CBaseTrigger_KeyValue(e, gid);
	copy_key(gid, K_SECTION, p->section, sizeof p->section);
	InitTrigger(e);
	SetUse(e, FN_CTriggerEndSection_EndSectionUse);
	if (!(p->spawnflags & SF_ENDSECTION_USEONLY))
		SetTouch(e, FN_CTriggerEndSection_EndSectionTouch);
	return 0;
}

const game_class_t gc_TRIGGER_ENDSECTION = {
	.size = sizeof(CTriggerEndSection), .Spawn = CTriggerEndSection_Spawn,
	.vt = &CBaseTrigger_vt,
};
