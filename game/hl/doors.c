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
 * doors.c -- HLSDK dlls/doors.cpp: func_door, func_water, func_door_rotating,
 * momentary_door, and the door / button lock sounds (PlayLockSounds,
 * ButtonSound; doors.h spawnflags). Sound names are res.def ids.
 */
#include <math.h>

#include "game/hl/doors.h"

/* ---- sounds --------------------------------------------------------------- */

res_t ButtonSound(int sound)
{
	switch (sound)
	{
	case 0: return RESID(SND_NULL);
	case 1: return RESID(BUTTON1);
	case 2: return RESID(BUTTON2);
	case 3: return RESID(BUTTON3);
	case 4: return RESID(BUTTON4);
	case 5: return RESID(BUTTON5);
	case 6: return RESID(BUTTON6);
	case 7: return RESID(BUTTON7);
	case 8: return RESID(BUTTON8);
	case 9: return RESID(BUTTON9);
	case 10: return RESID(BUTTON10);
	case 11: return RESID(BUTTON11);
	case 12: return RESID(LATCHLOCKED1);
	case 13: return RESID(LATCHUNLOCKED1);
	case 14: return RESID(LIGHTSWITCH2);
	case 21: return RESID(LEVER1);
	case 22: return RESID(LEVER2);
	case 23: return RESID(LEVER3);
	case 24: return RESID(LEVER4);
	case 25: return RESID(LEVER5);
	default: return RESID(BUTTON9);
	}
}

static res_t LockedSentence(int n)
{
	switch (n)
	{
	case 1: return RESID(SG_NA);
	case 2: return RESID(SG_ND);
	case 3: return RESID(SG_NF);
	case 4: return RESID(SG_NFIRE);
	case 5: return RESID(SG_NCHEM);
	case 6: return RESID(SG_NRAD);
	case 7: return RESID(SG_NCON);
	case 8: return RESID(SG_NH);
	case 9: return RESID(SG_NG);
	default: return RES_NONE;
	}
}

static res_t UnlockedSentence(int n)
{
	switch (n)
	{
	case 1: return RESID(SG_EA);
	case 2: return RESID(SG_ED);
	case 3: return RESID(SG_EF);
	case 4: return RESID(SG_EFIRE);
	case 5: return RESID(SG_ECHEM);
	case 6: return RESID(SG_ERAD);
	case 7: return RESID(SG_ECON);
	case 8: return RESID(SG_EH);
	default: return RES_NONE;
	}
}

/* CBaseButton / CBaseDoor Precache's lock sound part (the button's sentence
 * table lacks 9 for unlocked; both stop at 8 there). */
void LockSoundsPrecache(locksound_t *ls, int locked_sound, int unlocked_sound,
	int locked_sentence, int unlocked_sentence)
{
	ls->sLockedSound = locked_sound ? ButtonSound(locked_sound) : RES_NONE;
	ls->sUnlockedSound = unlocked_sound ? ButtonSound(unlocked_sound) : RES_NONE;
	ls->sLockedSentence = LockedSentence(locked_sentence);
	ls->sUnlockedSentence = UnlockedSentence(unlocked_sentence);
	PRECACHE(ls->sLockedSound);
	PRECACHE(ls->sUnlockedSound);
}

/* SENTENCEG_PlaySequentialSz(entity, group, ...): play sentence ipick of
 * the group (clamped to its last), return the next pick. */
int SENTENCEG_PlaySequential(int e, res_t group, float vol, float attn, int flags, int pitch,
	int ipick, int freset)
{
	uint16_t first;
	int count;

	(void)freset;
	if (RES_KIND(group) != RES_K_GROUP)
		return -1;
	count = res_group((uint16_t)RES_INDEX(group), &first);
	if (count <= 0)
		return -1;
	if (ipick >= count)
		ipick = count - 1;
	snd_sentence(e, CHAN_VOICE, (uint16_t)(first + ipick), vol, attn, flags, pitch);
	return ipick + 1;
}

#define DOOR_SENTENCEWAIT	6
#define DOOR_SOUNDWAIT		3
#define BUTTON_SOUNDWAIT	0.5f

void PlayLockSounds(int e, locksound_t *pls, int flocked, int fbutton)
{
	float wait = fbutton ? BUTTON_SOUNDWAIT : DOOR_SOUNDWAIT, vol, t = gtime();
	int playsound, playsentence, prev;

	if (flocked)
	{
		playsound = pls->sLockedSound && t > pls->flwaitSound;
		playsentence = pls->sLockedSentence && !pls->bEOFLocked && t > pls->flwaitSentence;
		vol = playsound && playsentence ? 0.25f : 1.0f;
		if (playsound)
		{
			EMIT_SOUND(e, CHAN_ITEM, pls->sLockedSound, vol, ATTN_NORM);
			pls->flwaitSound = t + wait;
		}
		if (playsentence)
		{
			prev = pls->iLockedSentence;
			pls->iLockedSentence = SENTENCEG_PlaySequential(e, pls->sLockedSentence, 0.85f,
				ATTN_NORM, 0, 100, pls->iLockedSentence, 0);
			pls->iUnlockedSentence = 0;
			pls->bEOFLocked = prev == pls->iLockedSentence;
			pls->flwaitSentence = t + DOOR_SENTENCEWAIT;
		}
	}
	else
	{
		playsound = pls->sUnlockedSound && t > pls->flwaitSound;
		playsentence = pls->sUnlockedSentence && !pls->bEOFUnlocked && t > pls->flwaitSentence;
		vol = playsound && playsentence ? 0.25f : 1.0f;
		if (playsound)
		{
			EMIT_SOUND(e, CHAN_ITEM, pls->sUnlockedSound, vol, ATTN_NORM);
			pls->flwaitSound = t + wait;
		}
		if (playsentence)
		{
			prev = pls->iUnlockedSentence;
			pls->iUnlockedSentence = SENTENCEG_PlaySequential(e, pls->sUnlockedSentence,
				0.85f, ATTN_NORM, 0, 100, pls->iUnlockedSentence, 0);
			pls->iLockedSentence = 0;
			pls->bEOFUnlocked = prev == pls->iUnlockedSentence;
			pls->flwaitSentence = t + DOOR_SENTENCEWAIT;
		}
	}
}

/* pos2 = pos1 + movedir * (|movedir.x (size.x - 2)| + ... - lip): the
 * fabs terms are double in HLSDK, the product goes through float */
void ToggleTravel(int e, float lip, float out[3])
{
	const col_ent_t *c = ent_cv(e);
	const float *md = CBase(e)->movedir;
	float size[3], d;

	VecSub(c->maxs, c->mins, size);
	d = (float)(fabs(md[0] * (size[0] - 2)) + fabs(md[1] * (size[1] - 2)) +
		fabs(md[2] * (size[2] - 2)) - lip);
	VecMA(c->origin, d, md, out);
}

/* ---- func_door / func_water ---------------------------------------------- */

static res_t DoorMoveSound(int n)
{
	switch (n)
	{
	case 1: return RESID(DOORMOVE1);
	case 2: return RESID(DOORMOVE2);
	case 3: return RESID(DOORMOVE3);
	case 4: return RESID(DOORMOVE4);
	case 5: return RESID(DOORMOVE5);
	case 6: return RESID(DOORMOVE6);
	case 7: return RESID(DOORMOVE7);
	case 8: return RESID(DOORMOVE8);
	case 9: return RESID(DOORMOVE9);
	case 10: return RESID(DOORMOVE10);
	default: return RESID(SND_NULL);
	}
}

static res_t DoorStopSound(int n)
{
	switch (n)
	{
	case 1: return RESID(DOORSTOP1);
	case 2: return RESID(DOORSTOP2);
	case 3: return RESID(DOORSTOP3);
	case 4: return RESID(DOORSTOP4);
	case 5: return RESID(DOORSTOP5);
	case 6: return RESID(DOORSTOP6);
	case 7: return RESID(DOORSTOP7);
	case 8: return RESID(DOORSTOP8);
	default: return RESID(SND_NULL);
	}
}

static void CBaseDoor_KeyValue(int e, gid_t16 gid)
{
	CBaseDoor *p = PRIV(CBaseDoor, e);

	CBaseToggle_KeyValue(e, gid);
	p->m_hActivator = EH_NONE;
	if (gid == GID_NONE)
		return;
	p->m_bMoveSnd = (uint8_t)KeyF(gid, K_MOVESND, 0);
	p->m_bStopSnd = (uint8_t)KeyF(gid, K_STOPSND, 0);
	p->m_bHealthValue = (uint8_t)KeyF(gid, K_HEALTHVALUE, 0);
	p->m_bLockedSound = (uint8_t)KeyF(gid, K_LOCKED_SOUND, 0);
	p->m_bLockedSentence = (uint8_t)KeyF(gid, K_LOCKED_SENTENCE, 0);
	p->m_bUnlockedSound = (uint8_t)KeyF(gid, K_UNLOCKED_SOUND, 0);
	p->m_bUnlockedSentence = (uint8_t)KeyF(gid, K_UNLOCKED_SENTENCE, 0);
}

static void CBaseDoor_Precache(int e)
{
	CBaseDoor *p = PRIV(CBaseDoor, e);

	p->noise1 = DoorMoveSound(p->m_bMoveSnd);	/* noiseMoving */
	p->noise2 = DoorStopSound(p->m_bStopSnd);	/* noiseArrived */
	PRECACHE(p->noise1);
	PRECACHE(p->noise2);
	LockSoundsPrecache(&p->m_ls, p->m_bLockedSound, p->m_bUnlockedSound,
		p->m_bLockedSentence, p->m_bUnlockedSentence);
}

static int is_rot_door(int e)
{
	return ent_idc(e)->cls == CLS_FUNC_DOOR_ROTATING;
}

void CBaseDoor_DoorTouch(int e, int other)
{
	CBaseDoor *p = PRIV(CBaseDoor, e);

	if (!IsPlayer(other))
		return;
	if (p->m_sMaster != NAME_NONE && !UTIL_IsMasterTriggered(p->m_sMaster, other))
		PlayLockSounds(e, &p->m_ls, 1, 0);
	if (ent_idc(e)->targetname != NAME_NONE)
	{
		PlayLockSounds(e, &p->m_ls, 1, 0);
		return;
	}
	p->m_hActivator = EHANDLE(other);
	if (DoorActivate(e))
		SetTouch(e, FN_NONE);
}

void CBaseDoor_Use(int e, int activator, int caller, int use_type, float value)
{
	CBaseDoor *p = PRIV(CBaseDoor, e);

	(void)caller;
	(void)use_type;
	(void)value;
	p->m_hActivator = EHANDLE(activator);
	if (p->m_toggle_state == TS_AT_BOTTOM ||
	    ((p->spawnflags & SF_DOOR_NO_AUTO_RETURN) && p->m_toggle_state == TS_AT_TOP))
		DoorActivate(e);
}

int DoorActivate(int e)
{
	CBaseDoor *p = PRIV(CBaseDoor, e);
	int act = EHGet(p->m_hActivator);

	if (!UTIL_IsMasterTriggered(p->m_sMaster, act))
		return 0;
	if ((p->spawnflags & SF_DOOR_NO_AUTO_RETURN) && p->m_toggle_state == TS_AT_TOP)
		CBaseDoor_DoorGoDown(e);
	else
	{
		if (act >= 0 && IsPlayer(act))
			TakeHealth(act, p->m_bHealthValue, DMG_GENERIC);
		PlayLockSounds(e, &p->m_ls, 0, 0);
		CBaseDoor_DoorGoUp(e);
	}
	return 1;
}

void CBaseDoor_DoorGoUp(int e)
{
	CBaseDoor *p = PRIV(CBaseDoor, e);
	float sign = 1.0f, ang[3], vec[3], vnext[3], fwd[3];
	int act;

	if (!(p->spawnflags & SF_DOOR_SILENT) && p->m_toggle_state != TS_GOING_UP &&
	    p->m_toggle_state != TS_GOING_DOWN)
		EMIT_SOUND(e, CHAN_STATIC, p->noise1, 1, ATTN_NORM);
	p->m_toggle_state = TS_GOING_UP;
	p->m_pfnCallWhenMoveDone = FN_CBaseDoor_DoorHitTop;
	if (is_rot_door(e))
	{
		act = EHGet(p->m_hActivator);
		if (act >= 0 && !(p->spawnflags & SF_DOOR_ONEWAY) && p->movedir[1] != 0)
		{
			const col_ent_t *ac = ent_cv(act);

			VecSub(ac->origin, ent_cv(e)->origin, vec);
			UTIL_MakeVectors(ac->angles);
			VecMA(ac->origin, 10, gpGlobals.v_forward, fwd);
			VecSub(fwd, ent_cv(e)->origin, vnext);
			if (vec[0] * vnext[1] - vec[1] * vnext[0] < 0)
				sign = -1.0f;
		}
		VecScale(p->m_vecAngle2, sign, ang);
		AngularMove(e, ang, p->speed);
	}
	else
		LinearMove(e, p->m_vecPosition2, p->speed);
}

void CBaseDoor_DoorHitTop(int e)
{
	CBaseDoor *p = PRIV(CBaseDoor, e);
	int act = EHGet(p->m_hActivator);

	if (!(p->spawnflags & SF_DOOR_SILENT))
	{
		STOP_SOUND(e, CHAN_STATIC, p->noise1);
		EMIT_SOUND(e, CHAN_STATIC, p->noise2, 1, ATTN_NORM);
	}
	p->m_toggle_state = TS_AT_TOP;
	if (p->spawnflags & SF_DOOR_NO_AUTO_RETURN)
	{
		if (!(p->spawnflags & SF_DOOR_USE_ONLY))
			SetTouch(e, FN_CBaseDoor_DoorTouch);
	}
	else
	{
		ThinkInLtime(e, p->m_flWait);
		SetThink(e, FN_CBaseDoor_DoorGoDown);
		if (p->m_flWait == -1)
			ent_think_at(e, -1);
	}
	if (p->netname != NAME_NONE && (p->spawnflags & SF_DOOR_START_OPEN))
		FireTargets(p->netname, act, e, USE_TOGGLE, 0);
	SUB_UseTargets(e, act, USE_TOGGLE, 0);
}

void CBaseDoor_DoorGoDown(int e)
{
	CBaseDoor *p = PRIV(CBaseDoor, e);

	if (!(p->spawnflags & SF_DOOR_SILENT) && p->m_toggle_state != TS_GOING_UP &&
	    p->m_toggle_state != TS_GOING_DOWN)
		EMIT_SOUND(e, CHAN_STATIC, p->noise1, 1, ATTN_NORM);
	p->m_toggle_state = TS_GOING_DOWN;
	p->m_pfnCallWhenMoveDone = FN_CBaseDoor_DoorHitBottom;
	if (is_rot_door(e))
		AngularMove(e, p->m_vecAngle1, p->speed);
	else
		LinearMove(e, p->m_vecPosition1, p->speed);
}

void CBaseDoor_DoorHitBottom(int e)
{
	CBaseDoor *p = PRIV(CBaseDoor, e);
	int act = EHGet(p->m_hActivator);

	if (!(p->spawnflags & SF_DOOR_SILENT))
	{
		STOP_SOUND(e, CHAN_STATIC, p->noise1);
		EMIT_SOUND(e, CHAN_STATIC, p->noise2, 1, ATTN_NORM);
	}
	p->m_toggle_state = TS_AT_BOTTOM;
	SetTouch(e, (p->spawnflags & SF_DOOR_USE_ONLY) ? FN_NONE : FN_CBaseDoor_DoorTouch);
	SUB_UseTargets(e, act, USE_TOGGLE, 0);
	if (p->netname != NAME_NONE && !(p->spawnflags & SF_DOOR_START_OPEN))
		FireTargets(p->netname, act, e, USE_TOGGLE, 0);
}

void CBaseDoor_Blocked(int e, int other)
{
	CBaseDoor *p = PRIV(CBaseDoor, e);
	name_t tn = ent_idc(e)->targetname;
	int it = 0, d;

	if (p->dmg)
		TakeDamage(other, e, e, p->dmg, DMG_CRUSH);
	if (p->m_flWait >= 0)
	{
		if (p->m_toggle_state == TS_GOING_DOWN)
			CBaseDoor_DoorGoUp(e);
		else
			CBaseDoor_DoorGoDown(e);
	}
	if (tn == NAME_NONE)
		return;
	while ((d = rec_next_named(tn, &it)) >= 0)
	{
		CBaseDoor *pd;
		cls_t c;

		if (d == e)
			continue;
		c = ent_idc(d)->cls;
		if (c != CLS_FUNC_DOOR && c != CLS_FUNC_DOOR_ROTATING)
			continue;
		pd = PRIV(CBaseDoor, d);
		if (pd->m_flWait < 0)
			continue;
		/* HLSDK compares the other's avelocity with this door's velocity */
		if (VecEq(ent_mot(d)->velocity, ent_mot(e)->velocity) &&
		    VecEq(ent_mot(d)->avelocity, ent_mot(e)->velocity))
		{
			if (c == CLS_FUNC_DOOR)
			{
				VecCopy(ent_cv(e)->origin, ent_cv(d)->origin);
				ent_set_velocity(d, g_vecZero);
				ent_relink(d, 0);
			}
			else
			{
				VecCopy(ent_cv(e)->angles, ent_cv(d)->angles);
				ent_set_avelocity(d, g_vecZero);
				ent_relink(d, 0);
			}
		}
		if (!(p->spawnflags & SF_DOOR_SILENT))
			STOP_SOUND(e, CHAN_STATIC, p->noise1);
		if (pd->m_toggle_state == TS_GOING_DOWN)
			CBaseDoor_DoorGoUp(d);
		else
			CBaseDoor_DoorGoDown(d);
	}
}

static int CBaseDoor_ObjectCaps(int e)
{
	return (CBase(e)->spawnflags & SF_ITEM_USE_ONLY) ? FCAP_IMPULSE_USE : 0;
}

static int CBaseDoor_GetToggleState(int e)
{
	return PRIV(CBaseToggle, e)->m_toggle_state;
}

static float CBaseDoor_GetDelay(int e)
{
	return PRIV(CBaseToggle, e)->m_flWait;
}

static void CBaseDoor_SetToggleState(int e, int state)
{
	CBaseDoor *p = PRIV(CBaseDoor, e);

	ent_set_origin(e, state == TS_AT_TOP ? p->m_vecPosition2 : p->m_vecPosition1);
}

static void CRotDoor_SetToggleState(int e, int state)
{
	CBaseDoor *p = PRIV(CBaseDoor, e);

	VecCopy(state == TS_AT_TOP ? p->m_vecAngle2 : p->m_vecAngle1, ent_cv(e)->angles);
	ent_set_origin(e, ent_cv(e)->origin);
}

static const game_vt_t CBaseDoor_vt = {
	.ObjectCaps = CBaseDoor_ObjectCaps,
	.GetToggleState = CBaseDoor_GetToggleState,
	.SetToggleState = CBaseDoor_SetToggleState,
	.GetDelay = CBaseDoor_GetDelay,
};

static const game_vt_t CRotDoor_vt = {
	.ObjectCaps = CBaseDoor_ObjectCaps,
	.GetToggleState = CBaseDoor_GetToggleState,
	.SetToggleState = CRotDoor_SetToggleState,
	.GetDelay = CBaseDoor_GetDelay,
};

static int CBaseDoor_Spawn(int e, gid_t16 gid)
{
	CBaseDoor *p = PRIV(CBaseDoor, e);
	float pos2[3];

	CBaseDoor_KeyValue(e, gid);
	CBaseDoor_Precache(e);
	SetMovedir(e);
	/* movetype first: SOLID_BSP requires MOVETYPE_PUSH when set */
	ent_set_movetype(e, MOVETYPE_PUSH);
	if (ent_cv(e)->skin == 0)
		ent_set_solid(e, (p->spawnflags & SF_DOOR_PASSABLE) ? SOLID_NOT : SOLID_BSP);
	else
	{
		ent_set_solid(e, SOLID_NOT);	/* special contents */
		p->spawnflags |= SF_DOOR_SILENT;	/* water is silent for now */
	}
	ent_set_origin(e, ent_cv(e)->origin);
	ent_set_model(e, p->model);
	if (p->speed == 0)
		p->speed = 100;
	VecCopy(ent_cv(e)->origin, p->m_vecPosition1);
	ToggleTravel(e, p->m_flLip, p->m_vecPosition2);
	if (p->spawnflags & SF_DOOR_START_OPEN)
	{
		VecCopy(p->m_vecPosition2, pos2);
		ent_set_origin(e, pos2);
		VecCopy(p->m_vecPosition1, p->m_vecPosition2);
		VecCopy(ent_cv(e)->origin, p->m_vecPosition1);
	}
	p->m_toggle_state = TS_AT_BOTTOM;
	SetTouch(e, (p->spawnflags & SF_DOOR_USE_ONLY) ? FN_NONE : FN_CBaseDoor_DoorTouch);
	SetUse(e, FN_CBaseDoor_Use);
	SetBlocked(e, FN_CBaseDoor_Blocked);
	return 0;
}

/* HLSDK CBaseDoor::m_SaveData (+ its locksound_t members) */
static const sv_field_t f_CBaseDoor[] = {
	SVF(CBaseDoor, m_ls.sLockedSound, SV_RES, SVK),
	SVF(CBaseDoor, m_ls.sLockedSentence, SV_RES, SVK),
	SVF(CBaseDoor, m_ls.sUnlockedSound, SV_RES, SVK),
	SVF(CBaseDoor, m_ls.sUnlockedSentence, SV_RES, SVK),
	SVF(CBaseDoor, m_ls.iLockedSentence, SV_I16, 0),
	SVF(CBaseDoor, m_ls.iUnlockedSentence, SV_I16, 0),
	SVF(CBaseDoor, m_ls.flwaitSound, SV_TIME, 0),
	SVF(CBaseDoor, m_ls.flwaitSentence, SV_TIME, 0),
	SVF(CBaseDoor, m_ls.bEOFLocked, SV_U8, 0),
	SVF(CBaseDoor, m_ls.bEOFUnlocked, SV_U8, 0),
	SVF(CBaseDoor, m_bHealthValue, SV_U8, SVK),
	SVF(CBaseDoor, m_bMoveSnd, SV_U8, SVK),
	SVF(CBaseDoor, m_bStopSnd, SV_U8, SVK),
	SVF(CBaseDoor, m_bLockedSound, SV_U8, SVK),
	SVF(CBaseDoor, m_bLockedSentence, SV_U8, SVK),
	SVF(CBaseDoor, m_bUnlockedSound, SV_U8, SVK),
	SVF(CBaseDoor, m_bUnlockedSentence, SV_U8, SVK),
};
static const sv_table_t sv_CBaseDoor = SV_TABLE(&sv_CBaseToggle, f_CBaseDoor);

const game_class_t gc_FUNC_DOOR = {
	.size = sizeof(CBaseDoor), .Spawn = CBaseDoor_Spawn, .vt = &CBaseDoor_vt,
	.save = &sv_CBaseDoor,
};
const game_class_t gc_FUNC_WATER = {
	.size = sizeof(CBaseDoor), .Spawn = CBaseDoor_Spawn, .vt = &CBaseDoor_vt,
	.save = &sv_CBaseDoor,
};

/* ---- func_door_rotating ----------------------------------------------------- */

static int CRotDoor_Spawn(int e, gid_t16 gid)
{
	CBaseDoor *p = PRIV(CBaseDoor, e);
	float sav[3];

	CBaseDoor_KeyValue(e, gid);
	CBaseDoor_Precache(e);
	AxisDir(e);
	if (p->spawnflags & SF_DOOR_ROTATE_BACKWARDS)
		VecScale(p->movedir, -1, p->movedir);
	VecCopy(ent_cv(e)->angles, p->m_vecAngle1);
	VecMA(ent_cv(e)->angles, p->m_flMoveDistance, p->movedir, p->m_vecAngle2);
	ent_set_movetype(e, MOVETYPE_PUSH);
	ent_set_solid(e, (p->spawnflags & SF_DOOR_PASSABLE) ? SOLID_NOT : SOLID_BSP);
	ent_set_origin(e, ent_cv(e)->origin);
	ent_set_model(e, p->model);
	if (p->speed == 0)
		p->speed = 100;
	if (p->spawnflags & SF_DOOR_START_OPEN)
	{
		VecCopy(p->m_vecAngle2, ent_cv(e)->angles);
		VecCopy(p->m_vecAngle1, sav);
		VecCopy(p->m_vecAngle1, p->m_vecAngle2);	/* sic (HLSDK) */
		VecCopy(sav, p->m_vecAngle1);
		VecScale(p->movedir, -1, p->movedir);
		ent_relink(e, 0);
	}
	p->m_toggle_state = TS_AT_BOTTOM;
	SetTouch(e, (p->spawnflags & SF_DOOR_USE_ONLY) ? FN_NONE : FN_CBaseDoor_DoorTouch);
	SetUse(e, FN_CBaseDoor_Use);
	SetBlocked(e, FN_CBaseDoor_Blocked);
	return 0;
}

const game_class_t gc_FUNC_DOOR_ROTATING = {
	.size = sizeof(CBaseDoor), .Spawn = CRotDoor_Spawn, .vt = &CRotDoor_vt,
	.save = &sv_CBaseDoor,
};

/* ---- momentary_door ----------------------------------------------------------- */

void CMomentaryDoor_DoorMoveDone(int e)
{
	CBaseDoor *p = PRIV(CBaseDoor, e);

	STOP_SOUND(e, CHAN_STATIC, p->noise1);
	EMIT_SOUND(e, CHAN_STATIC, p->noise2, 1, ATTN_NORM);
}

void CMomentaryDoor_Use(int e, int activator, int caller, int use_type, float value)
{
	CBaseDoor *p = PRIV(CBaseDoor, e);
	float move[3], delta[3], d[3], speed, nt = ent_tm(e)->nextthink;

	(void)activator;
	(void)caller;
	if (use_type != USE_SET)
		return;
	if (value > 1.0f)
		value = 1.0f;
	if (value < 0.0f)
		value = 0.0f;
	VecSub(p->m_vecPosition2, p->m_vecPosition1, d);
	VecMA(p->m_vecPosition1, value, d, move);
	VecSub(move, ent_cv(e)->origin, delta);
	speed = (float)(VecLen(delta) / 0.1);
	if (speed == 0)
		return;
	if (nt < ent_tm(e)->ltime || nt == 0)
		EMIT_SOUND(e, CHAN_STATIC, p->noise1, 1, ATTN_NORM);
	else if (VecEq(move, p->m_vecFinalDest))
		return;
	p->m_pfnCallWhenMoveDone = FN_CMomentaryDoor_DoorMoveDone;
	LinearMove(e, move, speed);
}

static int CMomentaryDoor_Spawn(int e, gid_t16 gid)
{
	CBaseDoor *p = PRIV(CBaseDoor, e);
	float pos2[3];

	CBaseToggle_KeyValue(e, gid);
	p->m_hActivator = EH_NONE;
	p->m_bMoveSnd = (uint8_t)KeyF(gid, K_MOVESND, 0);
	SetMovedir(e);
	ent_set_movetype(e, MOVETYPE_PUSH);
	ent_set_solid(e, SOLID_BSP);
	ent_set_origin(e, ent_cv(e)->origin);
	ent_set_model(e, p->model);
	if (p->speed == 0)
		p->speed = 100;
	if (p->dmg == 0)
		p->dmg = 2;
	VecCopy(ent_cv(e)->origin, p->m_vecPosition1);
	ToggleTravel(e, p->m_flLip, p->m_vecPosition2);
	if (p->spawnflags & SF_DOOR_START_OPEN)
	{
		VecCopy(p->m_vecPosition2, pos2);
		ent_set_origin(e, pos2);
		VecCopy(p->m_vecPosition1, p->m_vecPosition2);
		VecCopy(ent_cv(e)->origin, p->m_vecPosition1);
	}
	SetTouch(e, FN_NONE);
	SetUse(e, FN_CMomentaryDoor_Use);
	/* Precache: the move sound only; noiseArrived stays unset (HLSDK) */
	p->noise1 = p->m_bMoveSnd <= 8 ? DoorMoveSound(p->m_bMoveSnd) : RESID(SND_NULL);
	PRECACHE(p->noise1);
	return 0;
}

static int CMomentaryDoor_ObjectCaps(int e)
{
	(void)e;
	return 0;
}

static const game_vt_t CMomentaryDoor_vt = {
	.ObjectCaps = CMomentaryDoor_ObjectCaps,
	.GetToggleState = CBaseDoor_GetToggleState,
	.GetDelay = CBaseDoor_GetDelay,
};

const game_class_t gc_MOMENTARY_DOOR = {
	.size = sizeof(CBaseDoor), .Spawn = CMomentaryDoor_Spawn, .vt = &CMomentaryDoor_vt,
	.save = &sv_CBaseDoor,
};
