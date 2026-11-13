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
 * turret.c -- monster_miniturret (HLSDK dlls/turret.cpp CBaseTurret,
 * CMiniTurret; h_ai.cpp FBoxVisible). The mini turret has no eye glow
 * (m_pEyeGlow NULL: EyeOn / EyeOff only keep the brightness).
 */
#include <math.h>

#include "hl64/fx.h"
#include "engine/phys/player.h"
#include "game/hl/monster.h"

#define TURRET_RANGE		(100 * 12)
#define TURRET_TURNRATE		30	/* angles per 0.1 second */
#define TURRET_MAXWAIT		15	/* seconds turret will stay active w/o a target */
#define TURRET_MACHINE_VOLUME	0.5f

enum {
	TURRET_ANIM_NONE = 0,
	TURRET_ANIM_FIRE,
	TURRET_ANIM_SPIN,
	TURRET_ANIM_DEPLOY,
	TURRET_ANIM_RETIRE,
	TURRET_ANIM_DIE,
};

static const float sk_miniturretHealth[3] = { 40, 40, 50 };
static const float TURRET_SPREAD[3] = { 0, 0, 0 };

typedef struct {
	CBASEENTITY_FIELDS
	CBASEDELAY_FIELDS
	CBASEANIMATING_FIELDS
	CBASETOGGLE_FIELDS
	CBASEMONSTER_FIELDS
	float		m_flMaxSpin;	/* Max time to spin the barrel w/o a target */
	int		m_iSpin;
	int		m_eyeBrightness;
	int		m_iDeployHeight;
	int		m_iRetractHeight;
	int		m_iMinPitch;
	int		m_iBaseTurnRate;	/* angles per second */
	float		m_fTurnRate;		/* actual turn rate */
	int		m_iOrientation;		/* 0 = floor, 1 = ceiling */
	int		m_iOn;
	int		m_fBeserk;		/* Sometimes this bitch will just freak out */
	int		m_iAutoStart;		/* true if the turret auto deploys when a target enters its range */
	float		m_vecLastSight[3];
	float		m_flLastSight;		/* Last time we saw a target */
	float		m_flMaxWait;		/* Max time to seach w/o a target */
	int		m_iSearchSpeed;		/* Not Used! */
	float		m_flStartYaw;
	float		m_vecCurAngles[3];
	float		m_vecGoalAngles[3];
	float		m_flPingTime;		/* Time until the next ping, used when searching */
	float		m_flSpinUpTime;		/* Amount of time until the barrel should spin down when searching */
} CBaseTurret;

#define TUR(e)	((CBaseTurret *)ent_priv(e))

static const int16_t hks[] = { R_TU_HKS1, R_TU_HKS2, R_TU_HKS3 };

static int Tur_Classify(int e)
{
	if (TUR(e)->m_iOn || TUR(e)->m_iAutoStart)
		return CLASS_MACHINE;
	return CLASS_NONE;
}

static int Tur_BloodColor(int e) { (void)e; return DONT_BLEED; }
static void Tur_GibMonster(int e) { (void)e; }

static int waterlevel_of(int e)
{
	return IsPlayer(e) ? phy_player()->waterlevel : ent_phys(e)->waterlevel;
}

/* FBoxVisible: a more accurate (and slower) version of FVisible */
static int FBoxVisible(int looker, int target, float out[3])
{
	const col_ent_t	*t = ent_cv(target);
	col_trace_t	tr;
	float		eye[3], tgt[3];
	int		i;

	/* don't look through water */
	if ((waterlevel_of(looker) != 3 && waterlevel_of(target) == 3) ||
		(waterlevel_of(looker) == 3 && waterlevel_of(target) == 0))
		return 0;
	VecAdd(ent_cv(looker)->origin, CBase(looker)->view_ofs, eye);
	for (i = 0; i < 5; i++)
	{
		tgt[0] = t->origin[0] + RANDOM_FLOAT(t->mins[0], t->maxs[0]);
		tgt[1] = t->origin[1] + RANDOM_FLOAT(t->mins[1], t->maxs[1]);
		tgt[2] = t->origin[2] + RANDOM_FLOAT(t->mins[2], t->maxs[2]);
		UTIL_TraceLineGlass(eye, tgt, ignore_monsters, ignore_glass, looker, &tr);
		if (tr.frac == 1.0f)
		{
			VecCopy(tgt, out);
			return 1;	/* line of sight is valid */
		}
	}
	return 0;
}

static void Tur_EyeOn(int e)
{
	TUR(e)->m_eyeBrightness = 255;
}

static void Tur_EyeOff(int e)
{
	CBaseTurret *t = TUR(e);

	if (t->m_eyeBrightness > 0)
		t->m_eyeBrightness = t->m_eyeBrightness - 30 > 0 ? t->m_eyeBrightness - 30 : 0;
}

static void Tur_Ping(int e)
{
	CBaseTurret *t = TUR(e);

	/* make the pinging noise every second while searching */
	if (t->m_flPingTime == 0)
		t->m_flPingTime = gtime() + 1;
	else if (t->m_flPingTime <= gtime())
	{
		t->m_flPingTime = gtime() + 1;
		EMIT_SOUND(e, CHAN_ITEM, RESID(TU_PING), 1, ATTN_NORM);
		Tur_EyeOn(e);
	}
	else if (t->m_eyeBrightness > 0)
		Tur_EyeOff(e);
}

static void Tur_SetTurretAnim(int e, int anim)
{
	ent_rnd_t *rn = ent_rnd(e);

	if (rn->sequence == anim)
		return;
	switch (anim)
	{
	case TURRET_ANIM_FIRE:
	case TURRET_ANIM_SPIN:
		if (rn->sequence != TURRET_ANIM_FIRE && rn->sequence != TURRET_ANIM_SPIN)
			rn->frame = 0;
		break;
	default:
		rn->frame = 0;
		break;
	}
	rn->sequence = (uint16_t)anim;
	Mon_ResetSequenceInfo(e);
	switch (anim)
	{
	case TURRET_ANIM_RETIRE:
		rn->frame = 255;
		rn->framerate = -1.0f;
		break;
	case TURRET_ANIM_DIE:
		rn->framerate = 1.0f;
		break;
	}
}

static int Tur_MoveTurret(int e)
{
	CBaseTurret	*t = TUR(e);
	float		flDir, flDist;
	int		state = 0;

	/* any x movement? */
	if (t->m_vecCurAngles[0] != t->m_vecGoalAngles[0])
	{
		flDir = t->m_vecGoalAngles[0] > t->m_vecCurAngles[0] ? 1 : -1;
		t->m_vecCurAngles[0] += 0.1f * t->m_fTurnRate * flDir;
		/* if we started below the goal, and now we're past, peg to goal */
		if (flDir == 1)
		{
			if (t->m_vecCurAngles[0] > t->m_vecGoalAngles[0])
				t->m_vecCurAngles[0] = t->m_vecGoalAngles[0];
		}
		else if (t->m_vecCurAngles[0] < t->m_vecGoalAngles[0])
			t->m_vecCurAngles[0] = t->m_vecGoalAngles[0];
		Mon_SetBoneController(e, 1, t->m_iOrientation == 0 ? -t->m_vecCurAngles[0] :
			t->m_vecCurAngles[0]);
		state = 1;
	}
	if (t->m_vecCurAngles[1] != t->m_vecGoalAngles[1])
	{
		flDir = t->m_vecGoalAngles[1] > t->m_vecCurAngles[1] ? 1 : -1;
		flDist = fabsf(t->m_vecGoalAngles[1] - t->m_vecCurAngles[1]);
		if (flDist > 180)
		{
			flDist = 360 - flDist;
			flDir = -flDir;
		}
		if (flDist > 30)
		{
			if (t->m_fTurnRate < t->m_iBaseTurnRate * 10)
				t->m_fTurnRate += t->m_iBaseTurnRate;
		}
		else if (t->m_fTurnRate > 45)
			t->m_fTurnRate -= t->m_iBaseTurnRate;
		else
			t->m_fTurnRate += t->m_iBaseTurnRate;
		t->m_vecCurAngles[1] += 0.1f * t->m_fTurnRate * flDir;
		if (t->m_vecCurAngles[1] < 0)
			t->m_vecCurAngles[1] += 360;
		else if (t->m_vecCurAngles[1] >= 360)
			t->m_vecCurAngles[1] -= 360;
		if (flDist < 0.05f * t->m_iBaseTurnRate)
			t->m_vecCurAngles[1] = t->m_vecGoalAngles[1];
		if (t->m_iOrientation == 0)
			Mon_SetBoneController(e, 0, t->m_vecCurAngles[1] - ent_cv(e)->angles[1]);
		else
			Mon_SetBoneController(e, 0, ent_cv(e)->angles[1] - 180 - t->m_vecCurAngles[1]);
		state = 1;
	}
	if (!state)
		t->m_fTurnRate = (float)t->m_iBaseTurnRate;
	return state;
}

/* CMiniTurret::Shoot */
static void Tur_Shoot(int e, const float src[3], const float dir[3])
{
	FireBullets(e, 1, src, dir, TURRET_SPREAD, TURRET_RANGE, BULLET_MONSTER_9MM, 1, 0, -1);
	EMIT_SOUND(e, CHAN_WEAPON, RANDOM_SOUND_ARRAY(hks), 1, ATTN_NORM);
	ent_rnd(e)->effects |= EF_MUZZLEFLASH;
}

/* CBaseTurret SpinUpCall / SpinDownCall (the mini turret's) */
static void Tur_SpinUpCall(int e) { TUR(e)->m_iSpin = 1; }
static void Tur_SpinDownCall(int e) { TUR(e)->m_iSpin = 0; }

void CBaseTurret_ActiveThink(int e)
{
	CBaseTurret	*t = TUR(e);
	int		en = EHGet(t->m_hEnemy), fAttack, fEnemyVisible;
	float		mid[3], midEnemy[3], dir[3], los[3], vec[3], src[3], flDist;

	ThinkIn(e, 0.1f);
	Mon_StudioFrameAdvance(e, 0);
	if (!t->m_iOn || en < 0)
	{
		t->m_hEnemy = EH_NONE;
		t->m_flLastSight = gtime() + t->m_flMaxWait;
		SetThink(e, FN_CBaseTurret_SearchThink);
		return;
	}
	/* if it's dead, look for something new */
	if (!IsAlive(en))
	{
		if (!t->m_flLastSight)
			t->m_flLastSight = gtime() + 0.5f;
		else if (gtime() > t->m_flLastSight)
		{
			/* should we look for a new target? */
			t->m_hEnemy = EH_NONE;
			t->m_flLastSight = gtime() + t->m_flMaxWait;
			SetThink(e, FN_CBaseTurret_SearchThink);
			return;
		}
	}
	VecAdd(ent_cv(e)->origin, t->view_ofs, mid);
	BodyTarget(en, mid, midEnemy);
	/* look for a new target? */
	fEnemyVisible = FBoxVisible(e, en, midEnemy);
	VecSub(midEnemy, mid, dir);
	flDist = VecLen(dir);
	UTIL_VecToAngles(dir, vec);
	/* lost sight, or too far */
	if (!fEnemyVisible || flDist > TURRET_RANGE)
	{
		if (!t->m_flLastSight)
			t->m_flLastSight = gtime() + 0.5f;
		else if (gtime() > t->m_flLastSight)
		{
			/* should we look for a new target? */
			t->m_hEnemy = EH_NONE;
			t->m_flLastSight = gtime() + t->m_flMaxWait;
			SetThink(e, FN_CBaseTurret_SearchThink);
			return;
		}
		fEnemyVisible = 0;
	}
	else
		VecCopy(midEnemy, t->m_vecLastSight);
	UTIL_MakeAimVectors(t->m_vecCurAngles);
	VecNorm(dir, los);
	/* is the gun looking at the target? within 30 degrees */
	fAttack = VecDot(los, gpGlobals.v_forward) > 0.866f;
	/* fire the gun */
	if (t->m_iSpin && (fAttack || t->m_fBeserk))
	{
		gs_attachment(e, 0, src);
		Tur_SetTurretAnim(e, TURRET_ANIM_FIRE);
		Tur_Shoot(e, src, gpGlobals.v_forward);
	}
	else
		Tur_SetTurretAnim(e, TURRET_ANIM_SPIN);
	/* move the gun */
	if (t->m_fBeserk)
	{
		if (RANDOM_LONG(0, 9) == 0)
		{
			t->m_vecGoalAngles[1] = RANDOM_FLOAT(0, 360);
			t->m_vecGoalAngles[0] = RANDOM_FLOAT(0, 90) - 90 * t->m_iOrientation;
			TakeDamage(e, e, e, 1, DMG_GENERIC);	/* don't beserk forever */
			return;
		}
	}
	else if (fEnemyVisible)
	{
		if (vec[1] > 360)
			vec[1] -= 360;
		if (vec[1] < 0)
			vec[1] += 360;
		if (vec[0] < -180)
			vec[0] += 360;
		if (vec[0] > 180)
			vec[0] -= 360;
		/* now all numbers should be in [1...360]; pin to turret limitations */
		if (t->m_iOrientation == 0)
		{
			if (vec[0] > 90)
				vec[0] = 90;
			else if (vec[0] < t->m_iMinPitch)
				vec[0] = (float)t->m_iMinPitch;
		}
		else
		{
			if (vec[0] < -90)
				vec[0] = -90;
			else if (vec[0] > -t->m_iMinPitch)
				vec[0] = (float)-t->m_iMinPitch;
		}
		t->m_vecGoalAngles[1] = vec[1];
		t->m_vecGoalAngles[0] = vec[0];
	}
	Tur_SpinUpCall(e);
	Tur_MoveTurret(e);
}

void CBaseTurret_SearchThink(int e)
{
	CBaseTurret	*t = TUR(e);
	int		en;

	/* ensure rethink */
	Tur_SetTurretAnim(e, TURRET_ANIM_SPIN);
	Mon_StudioFrameAdvance(e, 0);
	ThinkIn(e, 0.1f);
	if (t->m_flSpinUpTime == 0 && t->m_flMaxSpin)
		t->m_flSpinUpTime = gtime() + t->m_flMaxSpin;
	Tur_Ping(e);
	/* if we have a target and we're still healthy */
	en = EHGet(t->m_hEnemy);
	if (en >= 0 && !IsAlive(en))
		t->m_hEnemy = EH_NONE;	/* dead enemy, forget it */
	/* acquire target */
	if (EHGet(t->m_hEnemy) < 0)
	{
		Mon_Look(e, TURRET_RANGE);
		en = Mon_BestVisibleEnemy(e);
		t->m_hEnemy = en >= 0 ? EHANDLE(en) : EH_NONE;
	}
	if (EHGet(t->m_hEnemy) >= 0)
	{
		/* if we've found a target, spin up the barrel and start firing */
		t->m_flLastSight = 0;
		t->m_flSpinUpTime = 0;
		SetThink(e, FN_CBaseTurret_ActiveThink);
		return;
	}
	/* are we out of time, do we need to retract? */
	if (gtime() > t->m_flLastSight)
	{
		/* before we retrace, make sure that we are spun down */
		t->m_flLastSight = 0;
		t->m_flSpinUpTime = 0;
		SetThink(e, FN_CBaseTurret_Retire);
	}
	/* should we stop the spin? */
	else if (t->m_flSpinUpTime && gtime() > t->m_flSpinUpTime)
		Tur_SpinDownCall(e);
	/* generic hunt for new victims */
	t->m_vecGoalAngles[1] = t->m_vecGoalAngles[1] + 0.1f * t->m_fTurnRate;
	if (t->m_vecGoalAngles[1] >= 360)
		t->m_vecGoalAngles[1] -= 360;
	Tur_MoveTurret(e);
}

/* this think is called when the turret is retracted and waiting for a target */
void CBaseTurret_AutoSearchThink(int e)
{
	CBaseTurret	*t = TUR(e);
	int		en;

	/* ensure rethink */
	Mon_StudioFrameAdvance(e, 0);
	ThinkIn(e, 0.3f);
	/* if we have a target and we're still healthy */
	en = EHGet(t->m_hEnemy);
	if (en >= 0 && !IsAlive(en))
		t->m_hEnemy = EH_NONE;	/* dead enemy, forget it */
	/* acquire target */
	if (EHGet(t->m_hEnemy) < 0)
	{
		Mon_Look(e, TURRET_RANGE);
		en = Mon_BestVisibleEnemy(e);
		t->m_hEnemy = en >= 0 ? EHANDLE(en) : EH_NONE;
	}
	if (EHGet(t->m_hEnemy) >= 0)
	{
		SetThink(e, FN_CBaseTurret_Deploy);
		EMIT_SOUND(e, CHAN_BODY, RESID(TU_ALERT), TURRET_MACHINE_VOLUME, ATTN_NORM);
	}
}

static void tur_set_height(int e, int h)
{
	float mn[3], mx[3];

	VecCopy(ent_cv(e)->mins, mn);
	VecCopy(ent_cv(e)->maxs, mx);
	mx[2] = (float)h;
	mn[2] = (float)-h;
	ent_set_size(e, mn, mx);
}

void CBaseTurret_Deploy(int e)
{
	CBaseTurret	*t = TUR(e);
	ent_rnd_t	*rn = ent_rnd(e);

	ThinkIn(e, 0.1f);
	Mon_StudioFrameAdvance(e, 0);
	if (rn->sequence != TURRET_ANIM_DEPLOY)
	{
		t->m_iOn = 1;
		Tur_SetTurretAnim(e, TURRET_ANIM_DEPLOY);
		EMIT_SOUND(e, CHAN_BODY, RESID(TU_DEPLOY), TURRET_MACHINE_VOLUME, ATTN_NORM);
		CBaseEntity_SUB_UseTargets(e, e, USE_ON, 0);
	}
	if (t->anim.m_fSequenceFinished)
	{
		tur_set_height(e, t->m_iDeployHeight);
		t->m_vecCurAngles[0] = 0;
		if (t->m_iOrientation == 1)
			t->m_vecCurAngles[1] = UTIL_AngleMod(ent_cv(e)->angles[1] + 180);
		else
			t->m_vecCurAngles[1] = UTIL_AngleMod(ent_cv(e)->angles[1]);
		Tur_SetTurretAnim(e, TURRET_ANIM_SPIN);
		rn->framerate = 0;
		SetThink(e, FN_CBaseTurret_SearchThink);
	}
	t->m_flLastSight = gtime() + t->m_flMaxWait;
}

void CBaseTurret_Retire(int e)
{
	CBaseTurret	*t = TUR(e);
	ent_rnd_t	*rn = ent_rnd(e);

	/* make the turret level */
	t->m_vecGoalAngles[0] = 0;
	t->m_vecGoalAngles[1] = t->m_flStartYaw;
	ThinkIn(e, 0.1f);
	Mon_StudioFrameAdvance(e, 0);
	Tur_EyeOff(e);
	if (Tur_MoveTurret(e))
	{
		Tur_SetTurretAnim(e, TURRET_ANIM_SPIN);
		return;
	}
	if (t->m_iSpin)
		Tur_SpinDownCall(e);
	else if (rn->sequence != TURRET_ANIM_RETIRE)
	{
		Tur_SetTurretAnim(e, TURRET_ANIM_RETIRE);
		EMIT_SOUND_DYN(e, CHAN_BODY, RESID(TU_DEPLOY), TURRET_MACHINE_VOLUME, ATTN_NORM, 0, 120);
		CBaseEntity_SUB_UseTargets(e, e, USE_OFF, 0);
	}
	else if (t->anim.m_fSequenceFinished)
	{
		t->m_iOn = 0;
		t->m_flLastSight = 0;
		Tur_SetTurretAnim(e, TURRET_ANIM_NONE);
		tur_set_height(e, t->m_iRetractHeight);
		if (t->m_iAutoStart)
		{
			SetThink(e, FN_CBaseTurret_AutoSearchThink);
			ThinkIn(e, 0.1f);
		}
		else
			SetThink(e, FN_SUB_DoNothing);
	}
}

void CBaseTurret_Initialize(int e)
{
	CBaseTurret	*t = TUR(e);
	float		*ang = ent_cv(e)->angles;

	t->m_iOn = 0;
	t->m_fBeserk = 0;
	t->m_iSpin = 0;
	Mon_SetBoneController(e, 0, 0);
	Mon_SetBoneController(e, 1, 0);
	if (t->m_iBaseTurnRate == 0)
		t->m_iBaseTurnRate = TURRET_TURNRATE;
	if (t->m_flMaxWait == 0)
		t->m_flMaxWait = TURRET_MAXWAIT;
	t->m_flStartYaw = ang[1];
	if (t->m_iOrientation == 1)
	{
		/* pev->idealpitch = 180 (only ChangePitch reads it) */
		ang[0] = 180;
		t->view_ofs[2] = -t->view_ofs[2];
		ent_phys(e)->view_ofs_z = (int16_t)-ent_phys(e)->view_ofs_z;
		ent_rnd(e)->effects |= EF_INVLIGHT;
		ang[1] = ang[1] + 180;
		if (ang[1] > 360)
			ang[1] = ang[1] - 360;
		ent_motion_changed(e);
	}
	t->m_vecGoalAngles[0] = 0;
	if (t->m_iAutoStart)
	{
		t->m_flLastSight = gtime() + t->m_flMaxWait;
		SetThink(e, FN_CBaseTurret_AutoSearchThink);
		ThinkIn(e, 0.1f);
	}
	else
		SetThink(e, FN_SUB_DoNothing);
}

void CBaseTurret_TurretUse(int e, int activator, int caller, int useType, float value)
{
	CBaseTurret *t = TUR(e);

	(void)activator; (void)caller; (void)value;
	if (!ShouldToggle(useType, t->m_iOn))
		return;
	if (t->m_iOn)
	{
		t->m_hEnemy = EH_NONE;
		ThinkIn(e, 0.1f);
		t->m_iAutoStart = 0;	/* switching off a turret disables autostart */
		SetThink(e, FN_CBaseTurret_Retire);
	}
	else
	{
		ThinkIn(e, 0.1f);	/* turn on delay */
		/* if the turret is flagged as an autoactivate turret, re-enable its ability open self */
		if (t->spawnflags & SF_MONSTER_TURRET_AUTOACTIVATE)
			t->m_iAutoStart = 1;
		SetThink(e, FN_CBaseTurret_Deploy);
	}
}

void CBaseTurret_TurretDeath(int e)
{
	CBaseTurret	*t = TUR(e);
	const col_ent_t	*c = ent_cv(e);
	float		r, src[3];

	Mon_StudioFrameAdvance(e, 0);
	ThinkIn(e, 0.1f);
	if (t->deadflag != DEAD_DEAD)
	{
		t->deadflag = DEAD_DEAD;
		r = RANDOM_FLOAT(0, 1);
		if (r <= 0.33f)
			EMIT_SOUND(e, CHAN_BODY, RESID(TU_DIE), 1.0f, ATTN_NORM);
		else if (r <= 0.66f)
			EMIT_SOUND(e, CHAN_BODY, RESID(TU_DIE2), 1.0f, ATTN_NORM);
		else
			EMIT_SOUND(e, CHAN_BODY, RESID(TU_DIE3), 1.0f, ATTN_NORM);
		EMIT_SOUND_DYN(e, CHAN_STATIC, RESID(TU_ACTIVE2), 0, 0, SND_STOP, 100);
		t->m_vecGoalAngles[0] = t->m_iOrientation == 0 ? -15 : -90;
		Tur_SetTurretAnim(e, TURRET_ANIM_DIE);
		Tur_EyeOn(e);
	}
	Tur_EyeOff(e);
	if (t->dmgtime + RANDOM_FLOAT(0, 2) > gtime())
	{
		/* lots of smoke */
		src[0] = RANDOM_FLOAT(c->absmin[0], c->absmax[0]);
		src[1] = RANDOM_FLOAT(c->absmin[1], c->absmax[1]);
		src[2] = c->origin[2] - t->m_iOrientation * 64;
		fx_smoke(src, RESID(SPR_SMOKE), 25, 10 - t->m_iOrientation * 5);
	}
	if (t->dmgtime + RANDOM_FLOAT(0, 5) > gtime())
	{
		src[0] = RANDOM_FLOAT(c->absmin[0], c->absmax[0]);
		src[1] = RANDOM_FLOAT(c->absmin[1], c->absmax[1]);
		if (t->m_iOrientation == 0)
			src[2] = RANDOM_FLOAT(c->origin[2], c->absmax[2]);
		else
			src[2] = RANDOM_FLOAT(c->absmin[2], c->origin[2]);
		UTIL_Sparks(src);
	}
	if (t->anim.m_fSequenceFinished && !Tur_MoveTurret(e) && t->dmgtime + 5 < gtime())
	{
		ent_rnd(e)->framerate = 0;
		SetThink(e, FN_NONE);
	}
}

static void Tur_TraceAttack(int e, int attacker, float dmg, const float dir[3],
	const col_trace_t *tr, int bits)
{
	CBaseTurret *t = TUR(e);

	(void)dir;
	if (tr->hitgroup == 10)
	{
		/* hit armor */
		if (t->dmgtime != gtime() || RANDOM_LONG(0, 10) < 1)
		{
			UTIL_Ricochet(tr->end, RANDOM_FLOAT(1, 2));
			t->dmgtime = gtime();
		}
		dmg = 0.1f;	/* don't hurt the monster much, but allow bits_COND_LIGHT_DAMAGE to be generated */
	}
	if (!t->takedamage)
		return;
	AddMultiDamage(attacker, e, dmg, bits);
}

/* take damage. bitsDamageType indicates type of damage sustained */
static int Tur_TakeDamage(int e, int inflictor, int attacker, float flDamage, int bits)
{
	CBaseTurret *t = TUR(e);

	(void)inflictor; (void)attacker; (void)bits;
	if (!t->takedamage)
		return 0;
	if (!t->m_iOn)
		flDamage /= 10.0f;
	t->health -= flDamage;
	if (t->health <= 0)
	{
		t->health = 0;
		t->takedamage = DAMAGE_NO;
		t->dmgtime = gtime();
		EFlagsClear(e, FL_MONSTER);	/* why are they set in the first place??? */
		SetUse(e, FN_NONE);
		SetThink(e, FN_CBaseTurret_TurretDeath);
		CBaseEntity_SUB_UseTargets(e, e, USE_ON, 0);	/* wake up others */
		ThinkIn(e, 0.1f);
		return 0;
	}
	if (t->health <= 10 && t->m_iOn)
	{
		t->m_fBeserk = 1;
		SetThink(e, FN_CBaseTurret_SearchThink);
	}
	return 1;
}

static int CMiniTurret_Spawn(int e, gid_t16 gid)
{
	CBaseMonster	*m = Mon_SpawnKeys(e, gid);
	CBaseTurret	*t = (CBaseTurret *)m;
	float		mn[3], mx[3];
	static const int16_t pre[] = {
		R_TU_FIRE1, R_TU_PING, R_TU_ACTIVE2, R_TU_DIE, R_TU_DIE2, R_TU_DIE3, R_TU_DEPLOY,
		R_TU_SPINUP, R_TU_SPINDOWN, R_TU_SEARCH, R_TU_ALERT, R_TU_HKS1, R_TU_HKS2, R_TU_HKS3,
		R_SPR_SMOKE,
	};

	/* CBaseTurret::KeyValue */
	t->m_flMaxWait = KeyF(gid, K_MAXSLEEP, 0);
	t->m_iOrientation = (int)KeyF(gid, K_ORIENTATION, 0);
	t->m_iSearchSpeed = (int)KeyF(gid, K_SEARCHSPEED, 0);
	t->m_iBaseTurnRate = (int)KeyF(gid, K_TURNRATE, 0);
	/* CMiniTurret::Precache */
	PRECACHE(RESID(MON_MINITURRET_MDL));
	PRECACHE_SOUND_ARRAY(pre);
	ent_set_model(e, RESID(MON_MINITURRET_MDL));
	m->health = mon_skill(sk_miniturretHealth);
	VecSet(m->m_HackedGunPos, 0, 0, 12.75f);
	t->m_flMaxSpin = 0;
	m->view_ofs[2] = 12.75f;
	ent_phys(e)->view_ofs_z = 13;
	/* CBaseTurret::Spawn */
	ThinkIn(e, 1);
	ent_set_movetype(e, MOVETYPE_FLY);
	ent_rnd(e)->sequence = 0;
	ent_rnd(e)->frame = 0;
	ent_set_solid(e, SOLID_SLIDEBOX);
	m->takedamage = DAMAGE_AIM;
	EFlagsSet(e, FL_MONSTER);
	SetUse(e, FN_CBaseTurret_TurretUse);
	if ((t->spawnflags & SF_MONSTER_TURRET_AUTOACTIVATE) &&
		!(t->spawnflags & SF_MONSTER_TURRET_STARTINACTIVE))
		t->m_iAutoStart = 1;
	Mon_ResetSequenceInfo(e);
	Mon_SetBoneController(e, 0, 0);
	Mon_SetBoneController(e, 1, 0);
	m->m_flFieldOfView = VIEW_FIELD_FULL;
	/* CMiniTurret::Spawn, after the base */
	t->m_iRetractHeight = 16;
	t->m_iDeployHeight = 32;
	t->m_iMinPitch = -15;
	VecSet(mn, -16, -16, (float)-t->m_iRetractHeight);
	VecSet(mx, 16, 16, (float)t->m_iRetractHeight);
	ent_set_size(e, mn, mx);
	SetThink(e, FN_CBaseTurret_Initialize);
	ThinkIn(e, 0.3f);
	return 0;
}

static const game_vt_t tur_vt = {
	.TakeHealth = CBaseMonster_TakeHealth,
	.IsAlive = CBaseMonster_IsAlive,
	.IsMoving = CBaseMonster_IsMoving,
	.BodyTarget = Mon_BodyTarget,
	.BloodColor = Tur_BloodColor,
	.TraceAttack = Tur_TraceAttack,
	.TakeDamage = Tur_TakeDamage,
	.Killed = CBaseMonster_Killed,
	.IRelationship = CBaseMonster_IRelationship,
	.Classify = Tur_Classify,
};

static const mon_vt_t tur_mvt = {
	.GibMonster = Tur_GibMonster,
};

const game_class_t gc_MONSTER_MINITURRET = {
	.size = sizeof(CBaseTurret),
	.Spawn = CMiniTurret_Spawn,
	.vt = &tur_vt,
	.ext = &tur_mvt,
};
