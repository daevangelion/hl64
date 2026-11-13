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
 * bmodels.c -- HLSDK dlls/bmodels.cpp: func_wall, func_wall_toggle,
 * func_conveyor, func_illusionary, func_rotating, func_pendulum.
 * (RampPitchVol / SpinUp's
 * abs() of floats are taken as fabsf: the float value, as HLSDK means.)
 */
#include <math.h>

#include "engine/phys/player.h"
#include "game/hl/doors.h"

#define SF_BRUSH_ROTATE_INSTANT		1
#define SF_BRUSH_ROTATE_BACKWARDS	2
#define SF_BRUSH_ROTATE_Z_AXIS		4
#define SF_BRUSH_ROTATE_X_AXIS		8
#define SF_PENDULUM_AUTO_RETURN		16
#define SF_BRUSH_ACCDCC			16
#define SF_BRUSH_HURT			32
#define SF_ROTATING_NOT_SOLID		64
#define SF_BRUSH_ROTATE_SMALLRADIUS	128
#define SF_BRUSH_ROTATE_MEDIUMRADIUS	256
#define SF_BRUSH_ROTATE_LARGERADIUS	512
#define SF_PENDULUM_SWING		2
#define SF_WALL_START_OFF		0x0001

static int NoTransition_ObjectCaps(int e)
{
	(void)e;
	return 0;
}

static const game_vt_t bmodel_vt = {
	.ObjectCaps = NoTransition_ObjectCaps,
};

/* ---- func_wall --------------------------------------------------------- */

void CFuncWall_Use(int e, int activator, int caller, int use_type, float value)
{
	ent_rnd_t *r = ent_rnd(e);

	(void)activator;
	(void)caller;
	(void)value;
	if (ShouldToggle(use_type, (int)r->frame))
		r->frame = 1 - r->frame;
}

static int CFuncWall_Spawn(int e, gid_t16 gid)
{
	(void)gid;
	VecSet(ent_cv(e)->angles, 0, 0, 0);
	ent_set_movetype(e, MOVETYPE_PUSH);	/* so it doesn't get pushed by anything */
	ent_set_solid(e, SOLID_BSP);
	ent_set_model(e, CBase(e)->model);
	EFlagsSet(e, FL_WORLDBRUSH);
	SetUse(e, FN_CFuncWall_Use);
	return 0;
}

const game_class_t gc_FUNC_WALL = {
	.size = sizeof(CBaseEntity), .Spawn = CFuncWall_Spawn, .vt = &bmodel_vt,
};

/* ---- func_wall_toggle ----------------------------------------------------- */

static void WallToggle_Set(int e, int on)
{
	ent_set_solid(e, on ? SOLID_BSP : SOLID_NOT);
	if (on)
		ent_rnd(e)->effects &= ~EF_NODRAW;
	else
		ent_rnd(e)->effects |= EF_NODRAW;
	ent_set_origin(e, ent_cv(e)->origin);
}

void CFuncWallToggle_Use(int e, int activator, int caller, int use_type, float value)
{
	int on = ent_cv(e)->solid != SOLID_NOT;

	(void)activator;
	(void)caller;
	(void)value;
	if (ShouldToggle(use_type, on))
		WallToggle_Set(e, !on);
}

static int CFuncWallToggle_Spawn(int e, gid_t16 gid)
{
	CFuncWall_Spawn(e, gid);
	SetUse(e, FN_CFuncWallToggle_Use);
	if (CBase(e)->spawnflags & SF_WALL_START_OFF)
		WallToggle_Set(e, 0);
	return 0;
}

const game_class_t gc_FUNC_WALL_TOGGLE = {
	.size = sizeof(CBaseEntity), .Spawn = CFuncWallToggle_Spawn, .vt = &bmodel_vt,
};

/* ---- func_conveyor ------------------------------------------------------------ */

#define SF_CONVEYOR_VISUAL	0x0001
#define SF_CONVEYOR_NOTSOLID	0x0002

/* HLSDK UpdateSpeed: the speed in rendercolor (4 fractional bits, sign in
 * x) for the scrolling texture; the engine's base velocity speed * movedir
 * for what rests on an FL_CONVEYOR brush (phy_set_conveyor). */
static void CFuncConveyor_UpdateSpeed(int e, float speed)
{
	ent_rnd_t *r = ent_rnd(e);
	int code = (int)(fabsf(speed) * 16.0f);
	float v[3];

	r->rendercolor[0] = speed < 0 ? 1 : 0;
	r->rendercolor[1] = (uint8_t)(code >> 8);
	r->rendercolor[2] = (uint8_t)(code & 0xFF);
	if (ent_cv(e)->flags & FL_CONVEYOR)
	{
		VecScale(CBase(e)->movedir, speed, v);
		phy_set_conveyor(e, v);
	}
}

void CFuncConveyor_Use(int e, int activator, int caller, int use_type, float value)
{
	(void)activator;
	(void)caller;
	(void)use_type;
	(void)value;
	CBase(e)->speed = -CBase(e)->speed;
	CFuncConveyor_UpdateSpeed(e, CBase(e)->speed);
}

static int CFuncConveyor_Spawn(int e, gid_t16 gid)
{
	CBaseEntity *p = CBase(e);

	SetMovedir(e);
	CFuncWall_Spawn(e, gid);
	SetUse(e, FN_CFuncConveyor_Use);
	if (!(p->spawnflags & SF_CONVEYOR_VISUAL))
		EFlagsSet(e, FL_CONVEYOR);
	/* HACKHACK - This is to allow for some special effects */
	if (p->spawnflags & SF_CONVEYOR_NOTSOLID)
	{
		ent_set_solid(e, SOLID_NOT);
		ent_cv(e)->skin = 0;	/* no special contents on this brush */
	}
	if (p->speed == 0)
		p->speed = 100;
	CFuncConveyor_UpdateSpeed(e, p->speed);
	return 0;
}

const game_class_t gc_FUNC_CONVEYOR = {
	.size = sizeof(CBaseEntity), .Spawn = CFuncConveyor_Spawn, .vt = &bmodel_vt,
};

/* ---- func_illusionary --------------------------------------------------- */

static int CFuncIllusionary_Spawn(int e, gid_t16 gid)
{
	CBaseToggle_KeyValue(e, gid);
	VecSet(ent_cv(e)->angles, 0, 0, 0);
	ent_set_movetype(e, MOVETYPE_NONE);
	ent_set_solid(e, SOLID_NOT);
	ent_set_model(e, CBase(e)->model);
	return 0;
}

const game_class_t gc_FUNC_ILLUSIONARY = {
	.size = sizeof(CBaseToggle), .Spawn = CFuncIllusionary_Spawn, .vt = &bmodel_vt,
};

/* ---- func_rotating ---------------------------------------------------------- */

#define FANPITCHMIN	30
#define FANPITCHMAX	100

typedef struct {
	CBASEENTITY_FIELDS
	float		m_flFanFriction;
	float		m_flAttenuation;
	float		m_flVolume;
	float		m_pitch;
	int32_t		m_sounds;
} CFuncRotating;

static void RampPitchVol(int e)
{
	CFuncRotating *p = PRIV(CFuncRotating, e);
	const float *av = ent_mot(e)->avelocity, *md = p->movedir;
	float cur, fin, pct, vol, fpitch;
	int pitch;

	cur = fabsf(av[0] != 0 ? av[0] : (av[1] != 0 ? av[1] : av[2]));
	fin = md[0] != 0 ? md[0] : (md[1] != 0 ? md[1] : md[2]);
	fin *= p->speed;
	fin = fabsf(fin);
	pct = cur / fin;
	vol = p->m_flVolume * pct;
	fpitch = FANPITCHMIN + (FANPITCHMAX - FANPITCHMIN) * pct;
	pitch = (int)fpitch;
	if (pitch == PITCH_NORM)
		pitch = PITCH_NORM - 1;
	EMIT_SOUND_DYN(e, CHAN_STATIC, p->noise3, vol, p->m_flAttenuation,
		SND_CHANGE_PITCH | SND_CHANGE_VOL, pitch);
}

void CFuncRotating_Rotate(int e)
{
	ThinkInLtime(e, 10);
}

void CFuncRotating_SpinUp(int e)
{
	CFuncRotating *p = PRIV(CFuncRotating, e);
	float av[3];

	ThinkInLtime(e, 0.1f);
	VecMA(ent_mot(e)->avelocity, p->speed * p->m_flFanFriction, p->movedir, av);
	ent_set_avelocity(e, av);
	if (fabsf(av[0]) >= fabsf(p->movedir[0] * p->speed) &&
	    fabsf(av[1]) >= fabsf(p->movedir[1] * p->speed) &&
	    fabsf(av[2]) >= fabsf(p->movedir[2] * p->speed))
	{
		VecScale(p->movedir, p->speed, av);
		ent_set_avelocity(e, av);
		EMIT_SOUND_DYN(e, CHAN_STATIC, p->noise3, p->m_flVolume, p->m_flAttenuation,
			SND_CHANGE_PITCH | SND_CHANGE_VOL, FANPITCHMAX);
		SetThink(e, FN_CFuncRotating_Rotate);
		CFuncRotating_Rotate(e);
	}
	else
		RampPitchVol(e);
}

void CFuncRotating_SpinDown(int e)
{
	CFuncRotating *p = PRIV(CFuncRotating, e);
	const float *md = p->movedir;
	float av[3], dir;

	ThinkInLtime(e, 0.1f);
	VecMA(ent_mot(e)->avelocity, -(p->speed * p->m_flFanFriction), md, av);
	ent_set_avelocity(e, av);
	dir = md[0] != 0 ? md[0] : (md[1] != 0 ? md[1] : md[2]);
	if ((dir > 0 && av[0] <= 0 && av[1] <= 0 && av[2] <= 0) ||
	    (dir < 0 && av[0] >= 0 && av[1] >= 0 && av[2] >= 0))
	{
		ent_set_avelocity(e, g_vecZero);
		EMIT_SOUND_DYN(e, CHAN_STATIC, p->noise3, 0, 0, SND_STOP, (int)p->m_pitch);
		SetThink(e, FN_CFuncRotating_Rotate);
		CFuncRotating_Rotate(e);
	}
	else
		RampPitchVol(e);
}

void CFuncRotating_HurtTouch(int e, int other)
{
	CFuncRotating *p = PRIV(CFuncRotating, e);
	CBaseEntity *o = CBase(other);
	float c[3], d[3], v[3];

	if (!o || !o->takedamage)
		return;
	p->dmg = VecLen(ent_mot(e)->avelocity) / 10;
	TakeDamage(other, e, e, p->dmg, DMG_CRUSH);
	VecBModelOrigin(e, c);
	VecSub(ent_cv(other)->origin, c, d);
	VecNorm(d, v);
	VecScale(v, p->dmg, v);
	ent_set_velocity(other, v);
}

void CFuncRotating_RotatingUse(int e, int activator, int caller, int use_type, float value)
{
	CFuncRotating *p = PRIV(CFuncRotating, e);
	float av[3];

	(void)activator;
	(void)caller;
	(void)use_type;
	(void)value;
	if (!VecIsZero(ent_mot(e)->avelocity))
	{
		SetThink(e, FN_CFuncRotating_SpinDown);
		ThinkInLtime(e, 0.1f);
	}
	else if (p->spawnflags & SF_BRUSH_ACCDCC)
	{
		SetThink(e, FN_CFuncRotating_SpinUp);
		EMIT_SOUND_DYN(e, CHAN_STATIC, p->noise3, 0.01f, p->m_flAttenuation, 0,
			FANPITCHMIN);
		ThinkInLtime(e, 0.1f);
	}
	else
	{
		EMIT_SOUND_DYN(e, CHAN_STATIC, p->noise3, p->m_flVolume, p->m_flAttenuation, 0,
			FANPITCHMAX);
		VecScale(p->movedir, p->speed, av);
		ent_set_avelocity(e, av);
		SetThink(e, FN_CFuncRotating_Rotate);
		CFuncRotating_Rotate(e);
	}
}

void CFuncRotating_Blocked(int e, int other)
{
	TakeDamage(other, e, e, CBase(e)->dmg, DMG_CRUSH);
}

static res_t fan_sound(int n)
{
	switch (n)
	{
	case 1: return RESID(FAN1);
	case 2: return RESID(FAN2);
	case 3: return RESID(FAN3);
	case 4: return RESID(FAN4);
	case 5: return RESID(FAN5);
	default: return RES_NONE;
	}
}

static int CFuncRotating_Spawn(int e, gid_t16 gid)
{
	CFuncRotating *p = PRIV(CFuncRotating, e);
	uint32_t sf = p->spawnflags;
	float v, so[3];

	/* KeyValue */
	p->m_flFanFriction = KeyF(gid, K_FANFRICTION, 0) / 100;
	if (gid != GID_NONE && rec_key_f(gid, K_VOLUME, &v))
	{
		v = v / 10.0f;
		p->m_flVolume = v > 1.0f ? 1.0f : v < 0.0f ? 0.0f : v;
	}
	if (gid != GID_NONE && rec_key_v(gid, K_SPAWNORIGIN, so) && !VecIsZero(so))
		VecCopy(so, ent_cv(e)->origin);
	p->m_sounds = (int)KeyF(gid, K_SOUNDS, 0);
	p->message = gid == GID_NONE ? RES_NONE : rec_key_res(gid, K_MESSAGE);
	/* Spawn */
	p->m_pitch = PITCH_NORM - 1;
	if (p->m_flVolume == 0.0f)
		p->m_flVolume = 1.0f;
	p->m_flAttenuation = ATTN_NORM;
	if (sf & SF_BRUSH_ROTATE_SMALLRADIUS)
		p->m_flAttenuation = ATTN_IDLE;
	else if (sf & SF_BRUSH_ROTATE_MEDIUMRADIUS)
		p->m_flAttenuation = ATTN_STATIC;
	else if (sf & SF_BRUSH_ROTATE_LARGERADIUS)
		p->m_flAttenuation = ATTN_NORM;
	if (p->m_flFanFriction == 0)
		p->m_flFanFriction = 1;
	if (sf & SF_BRUSH_ROTATE_Z_AXIS)
		VecSet(p->movedir, 0, 0, 1);
	else if (sf & SF_BRUSH_ROTATE_X_AXIS)
		VecSet(p->movedir, 1, 0, 0);
	else
		VecSet(p->movedir, 0, 1, 0);
	if (sf & SF_BRUSH_ROTATE_BACKWARDS)
		VecScale(p->movedir, -1, p->movedir);
	ent_set_movetype(e, MOVETYPE_PUSH);
	if (sf & SF_ROTATING_NOT_SOLID)
	{
		ent_set_solid(e, SOLID_NOT);
		ent_cv(e)->skin = CONTENTS_EMPTY;
	}
	else
		ent_set_solid(e, SOLID_BSP);
	ent_set_origin(e, ent_cv(e)->origin);
	ent_set_model(e, p->model);
	SetUse(e, FN_CFuncRotating_RotatingUse);
	SetBlocked(e, FN_CFuncRotating_Blocked);
	if (p->speed <= 0)
		p->speed = 0;
	if (sf & SF_BRUSH_ROTATE_INSTANT)
	{
		SetThink(e, FN_SUB_CallUseToggle);
		ThinkInLtime(e, 1.5f);
	}
	if (sf & SF_BRUSH_HURT)
		SetTouch(e, FN_CFuncRotating_HurtTouch);
	/* Precache */
	if (RES_KIND(p->message) == RES_K_SOUND)
		p->noise3 = p->message;
	else
		p->noise3 = fan_sound(p->m_sounds);
	if (!p->noise3)
		p->noise3 = RESID(SND_NULL);
	PRECACHE(p->noise3);
	if (!VecIsZero(ent_mot(e)->avelocity))
	{
		SetThink(e, FN_CFuncRotating_SpinUp);
		ThinkInLtime(e, 1.5f);
	}
	return 0;
}

const game_class_t gc_FUNC_ROTATING = {
	.size = sizeof(CFuncRotating), .Spawn = CFuncRotating_Spawn, .vt = &bmodel_vt,
};

/* ---- func_pendulum ------------------------------------------------------------ */

typedef struct {
	CBASEENTITY_FIELDS
	float		m_accel;
	float		m_distance;
	float		m_time;
	float		m_damp;
	float		m_maxSpeed;
	float		m_dampSpeed;
	float		m_center[3];
	float		m_start[3];
} CPendulum;

void CPendulum_Stop(int e)
{
	CPendulum *p = PRIV(CPendulum, e);

	VecCopy(p->m_start, ent_cv(e)->angles);
	ent_relink(e, 0);
	p->speed = 0;
	SetThink(e, FN_NONE);
	ent_set_avelocity(e, g_vecZero);
}

void CPendulum_Swing(int e)
{
	CPendulum *p = PRIV(CPendulum, e);
	float delta, dt, t = gtime(), av[3];

	delta = AxisDelta(p->spawnflags, ent_cv(e)->angles, p->m_center);
	dt = t - p->m_time;
	p->m_time = t;
	if (delta > 0 && p->m_accel > 0)
		p->speed -= p->m_accel * dt;
	else
		p->speed += p->m_accel * dt;
	if (p->speed > p->m_maxSpeed)
		p->speed = p->m_maxSpeed;
	else if (p->speed < -p->m_maxSpeed)
		p->speed = -p->m_maxSpeed;
	VecScale(p->movedir, p->speed, av);
	ent_set_avelocity(e, av);
	ThinkInLtime(e, 0.1f);
	if (p->m_damp)
	{
		p->m_dampSpeed -= p->m_damp * p->m_dampSpeed * dt;
		if (p->m_dampSpeed < 30.0f)
		{
			VecCopy(p->m_center, ent_cv(e)->angles);
			ent_relink(e, 0);
			p->speed = 0;
			SetThink(e, FN_NONE);
			ent_set_avelocity(e, g_vecZero);
		}
		else if (p->speed > p->m_dampSpeed)
			p->speed = p->m_dampSpeed;
		else if (p->speed < -p->m_dampSpeed)
			p->speed = -p->m_dampSpeed;
	}
}

void CPendulum_PendulumUse(int e, int activator, int caller, int use_type, float value)
{
	CPendulum *p = PRIV(CPendulum, e);
	float delta, av[3];

	(void)activator;
	(void)caller;
	(void)use_type;
	(void)value;
	if (p->speed)
	{
		if (p->spawnflags & SF_PENDULUM_AUTO_RETURN)
		{
			delta = AxisDelta(p->spawnflags, ent_cv(e)->angles, p->m_start);
			VecScale(p->movedir, p->m_maxSpeed, av);
			ent_set_avelocity(e, av);
			ThinkInLtime(e, delta / p->m_maxSpeed);
			SetThink(e, FN_CPendulum_Stop);
		}
		else
		{
			p->speed = 0;
			SetThink(e, FN_NONE);
			ent_set_avelocity(e, g_vecZero);
		}
	}
	else
	{
		ThinkInLtime(e, 0.1f);
		p->m_time = gtime();
		SetThink(e, FN_CPendulum_Swing);
		p->m_dampSpeed = p->m_maxSpeed;
	}
}

void CPendulum_Blocked(int e, int other)
{
	(void)other;
	PRIV(CPendulum, e)->m_time = gtime();
}

void CPendulum_Touch(int e, int other)
{
	CPendulum *p = PRIV(CPendulum, e);
	CBaseEntity *o = CBase(other);
	float damage, c[3], d[3], v[3];

	if (p->dmg <= 0 || !o || !o->takedamage)
		return;
	damage = p->dmg * p->speed * 0.01f;
	if (damage < 0)
		damage = -damage;
	TakeDamage(other, e, e, damage, DMG_CRUSH);
	VecBModelOrigin(e, c);
	VecSub(ent_cv(other)->origin, c, d);
	VecNorm(d, v);
	VecScale(v, damage, v);
	ent_set_velocity(other, v);
}

void CPendulum_RopeTouch(int e, int other)
{
	CPendulum *p = PRIV(CPendulum, e);

	if (!IsPlayer(other))
	{
		log_printf("game: Not a client\n");
		return;
	}
	if (EHGet(p->enemy) == other)
		return;
	p->enemy = EHANDLE(other);
	VecSet(phy_player()->velocity, 0, 0, 0);
	phy_player()->movetype = PL_MOVE_NONE;
}

static int CPendulum_Spawn(int e, gid_t16 gid)
{
	CPendulum *p = PRIV(CPendulum, e);

	p->m_distance = KeyF(gid, K_DISTANCE, 0);
	p->m_damp = KeyF(gid, K_DAMP, 0) * 0.001f;
	AxisDir(e);
	ent_set_movetype(e, MOVETYPE_PUSH);
	ent_set_solid(e, (p->spawnflags & SF_DOOR_PASSABLE) ? SOLID_NOT : SOLID_BSP);
	ent_set_origin(e, ent_cv(e)->origin);
	ent_set_model(e, p->model);
	SetTouch(e, FN_CPendulum_Touch);
	SetBlocked(e, FN_CPendulum_Blocked);
	if (p->m_distance == 0)
		return 0;
	if (p->speed == 0)
		p->speed = 100;
	p->m_accel = (float)((p->speed * p->speed) / (2 * fabs(p->m_distance)));
	p->m_maxSpeed = p->speed;
	VecCopy(ent_cv(e)->angles, p->m_start);
	VecMA(ent_cv(e)->angles, p->m_distance * 0.5f, p->movedir, p->m_center);
	if (p->spawnflags & SF_BRUSH_ROTATE_INSTANT)
	{
		SetThink(e, FN_SUB_CallUseToggle);
		ThinkIn(e, 0.1f);
	}
	p->speed = 0;
	SetUse(e, FN_CPendulum_PendulumUse);
	if (p->spawnflags & SF_PENDULUM_SWING)
		SetTouch(e, FN_CPendulum_RopeTouch);
	return 0;
}

const game_class_t gc_FUNC_PENDULUM = {
	.size = sizeof(CPendulum), .Spawn = CPendulum_Spawn, .vt = &bmodel_vt,
};
