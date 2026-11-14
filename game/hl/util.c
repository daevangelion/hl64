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
 * util.c -- HLSDK dlls/util.cpp, subs.cpp, cbase.cpp and combat.cpp base
 * helpers written on the hl64 engine API (util.h). DelayedUse entities
 * become record-layer delayed fires (rec_fire_at); FIND_ENTITY_BY_TARGETNAME
 * walks the cooked name lists (rec_next_named); traces convert between
 * the caller's map-local frame and the collision module's global one.
 */
#include <string.h>

#include "engine/core/hlmath.h"
#include "game/hl/monster.h"
#include "game/hl/util.h"

game_globals_t	gpGlobals;
float		g_vecAttackDir[3];
const float	g_vecZero[3] = { 0, 0, 0 };

/* ---- vectors and angles ------------------------------------------------------ */

/* AngleVectors (six double sin / cos) of the last angles asked: a Look
 * makes the looker's vectors once per candidate (FInViewCone), a think
 * the same ones again and again. The vectors are a pure function of the
 * angles' bits, so a repeat copies them. */
void UTIL_MakeVectors(const float ang[3])
{
	static uint32_t	key[3];
	static float	v[9];
	static int	have;
	uint32_t	k[3];

	memcpy(k, ang, sizeof(k));
	if (!have || k[0] != key[0] || k[1] != key[1] || k[2] != key[2])
	{
		AngleVectors(ang, v, v + 3, v + 6);
		memcpy(key, k, sizeof(key));
		have = 1;
	}
	memcpy(gpGlobals.v_forward, v, sizeof(gpGlobals.v_forward));
	memcpy(gpGlobals.v_right, v + 3, sizeof(gpGlobals.v_right));
	memcpy(gpGlobals.v_up, v + 6, sizeof(gpGlobals.v_up));
}

void UTIL_MakeAimVectors(const float ang[3])
{
	float a[3];

	a[0] = -ang[0];
	a[1] = ang[1];
	a[2] = ang[2];
	UTIL_MakeVectors(a);
}

/* pfnVecToYaw */
float UTIL_VecToYaw(const float v[3])
{
	float yaw;

	if (v[1] == 0 && v[0] == 0)
		return 0;
	yaw = (float)(hl_atan2(v[1], v[0]) * 180 / M_PI);
	if (yaw < 0)
		yaw += 360;
	return yaw;
}

void UTIL_VecToAngles(const float v[3], float out[3])
{
	VectorAngles(v, out);
}

float UTIL_AngleMod(float a)
{
	if (a < 0)
		a = a + 360 * ((int)(a / 360) + 1);
	else if (a >= 360)
		a = a - 360 * ((int)(a / 360));
	return a;
}

float UTIL_AngleDiff(float destAngle, float srcAngle)
{
	float delta = destAngle - srcAngle;

	if (destAngle > srcAngle)
	{
		if (delta >= 180)
			delta -= 360;
	}
	else
	{
		if (delta <= -180)
			delta += 360;
	}
	return delta;
}

float UTIL_Approach(float target, float value, float speed)
{
	float delta = target - value;

	if (delta > speed)
		value += speed;
	else if (delta < -speed)
		value -= speed;
	else
		value = target;
	return value;
}

float UTIL_ApproachAngle(float target, float value, float speed)
{
	float delta;

	target = UTIL_AngleMod(target);
	value = UTIL_AngleMod(target);	/* sic (HLSDK) */
	delta = target - value;
	if (speed < 0)
		speed = -speed;
	if (delta < -180)
		delta += 360;
	else if (delta > 180)
		delta -= 360;
	if (delta > speed)
		value += speed;
	else if (delta < -speed)
		value -= speed;
	else
		value = target;
	return value;
}

float UTIL_AngleDistance(float next, float cur)
{
	float delta = next - cur;

	if (delta < -180)
		delta += 360;
	else if (delta > 180)
		delta -= 360;
	return delta;
}

float UTIL_WaterLevel(const float pos[3], float minz, float maxz)
{
	float midUp[3], diff;

	VecCopy(pos, midUp);
	midUp[2] = minz;
	if (UTIL_PointContents(midUp) != CONTENTS_WATER)
		return minz;
	midUp[2] = maxz;
	if (UTIL_PointContents(midUp) == CONTENTS_WATER)
		return maxz;
	diff = maxz - minz;
	while (diff > 1.0f)
	{
		midUp[2] = (float)(minz + diff / 2.0);
		if (UTIL_PointContents(midUp) == CONTENTS_WATER)
			minz = midUp[2];
		else
			maxz = midUp[2];
		diff = maxz - minz;
	}
	return midUp[2];
}

void UTIL_ClampVectorToBox(const float in[3], const float clamp[3], float out[3])
{
	float v[3];
	int i;

	for (i = 0; i < 3; i++)
	{
		v[i] = in[i];
		if (v[i] > clamp[i])
			v[i] -= clamp[i];
		else if (v[i] < -clamp[i])
			v[i] += clamp[i];
		else
			v[i] = 0;
	}
	VecNorm(v, out);
}

void SetMovedir(int e)
{
	float *ang = ent_cv(e)->angles;
	CBaseEntity *pev = CBase(e);

	if (ang[0] == 0 && ang[1] == -1 && ang[2] == 0)
		VecSet(pev->movedir, 0, 0, 1);
	else if (ang[0] == 0 && ang[1] == -2 && ang[2] == 0)
		VecSet(pev->movedir, 0, 0, -1);
	else
	{
		UTIL_MakeVectors(ang);
		VecCopy(gpGlobals.v_forward, pev->movedir);
	}
	VecSet(ang, 0, 0, 0);
}

void VecBModelOrigin(int e, float out[3])
{
	const col_ent_t *c = ent_cv(e);
	int i;

	for (i = 0; i < 3; i++)
		out[i] = c->absmin[i] + (c->absmax[i] - c->absmin[i]) * 0.5f;
}

void Center(int e, float out[3])
{
	const col_ent_t *c = ent_cv(e);
	int i;

	for (i = 0; i < 3; i++)
		out[i] = (c->absmax[i] + c->absmin[i]) * 0.5f;
}

int Intersects(int a, int b)
{
	const col_ent_t *p = ent_cv(a), *o = ent_cv(b);

	if (o->absmin[0] > p->absmax[0] || o->absmin[1] > p->absmax[1] ||
	    o->absmin[2] > p->absmax[2] || o->absmax[0] < p->absmin[0] ||
	    o->absmax[1] < p->absmin[1] || o->absmax[2] < p->absmin[2])
		return 0;
	return 1;
}

int IsInWorld(int e)
{
	const float *o = ent_cv(e)->origin, *v = ent_mot(e)->velocity;
	int i;

	for (i = 0; i < 3; i++)
	{
		if (o[i] >= 4096 || o[i] <= -4096)
			return 0;
		if (v[i] >= 2000 || v[i] <= -2000)
			return 0;
	}
	return 1;
}

/* ---- lifetime ------------------------------------------------------------------ */

void UTIL_Remove(int e)
{
	ent_id_t *id;

	if (e <= ENT_PLAYER || !ent_is_live(e))
		return;
	id = ent_idc(e);
	if (id->gid != GID_NONE)
		rec_kill(id->gid);
	ent_remove(e);
	id->targetname = NAME_NONE;
}

void SUB_Remove(int e)
{
	CBaseEntity *pev = CBase(e);

	if (pev && pev->health > 0)
		pev->health = 0;
	UTIL_Remove(e);
}

void SUB_DoNothing(int e)
{
	(void)e;
}

void SUB_CallUseToggle(int e)
{
	UseEntity(e, e, e, USE_TOGGLE, 0);
}

/* combat.cpp SUB_StartFadeOut / SUB_FadeOut */
void SUB_StartFadeOut(int e)
{
	ent_rnd_t *r = ent_rnd(e);
	float zero[3] = { 0, 0, 0 };

	if (ent_cv(e)->rendermode == kRenderNormal)
	{
		r->renderamt = 255;
		ent_cv(e)->rendermode = kRenderTransTexture;
	}
	ent_set_solid(e, SOLID_NOT);
	ent_set_avelocity(e, zero);
	SetThink(e, FN_SUB_FadeOut);
	ThinkIn(e, 0.1f);
}

void SUB_FadeOut(int e)
{
	ent_rnd_t *r = ent_rnd(e);

	if (r->renderamt > 7)
	{
		r->renderamt -= 7;
		ThinkIn(e, 0.1f);
	}
	else
	{
		r->renderamt = 0;
		ThinkIn(e, 0.2f);
		SetThink(e, FN_SUB_Remove);
	}
}

int game_spawn_class(cls_t cls, int e);

int CBaseEntity_Create(cls_t cls, const float org[3], const float ang[3], int owner)
{
	int e = ent_create(ENT_ALLOC_GAME);

	if (e < 0)
	{
		log_printf("game: create %u: no free slot\n", (unsigned)cls);
		return -1;
	}
	ent_set_origin(e, org);
	VecCopy(ang, ent_cv(e)->angles);
	ent_cv(e)->owner = owner >= 0 ? (uint16_t)owner : COL_NONE;
	if (game_spawn_class(cls, e) < 0)
		return -1;
	return e;
}

void MakeDormant(int e)
{
	EFlagsSet(e, FL_DORMANT);
	ent_set_solid(e, SOLID_NOT);
	ent_set_movetype(e, MOVETYPE_NONE);
	ent_rnd(e)->effects |= EF_NODRAW;
	ent_think_at(e, 0);
	ent_set_origin(e, ent_cv(e)->origin);
}

/* ---- keys ----------------------------------------------------------------- */

float KeyF(gid_t16 gid, int k, float def)
{
	float v;

	if (gid == GID_NONE || !rec_key_f(gid, (key_t16)k, &v))
		return def;
	return v;
}

static int key_u8(gid_t16 gid, int k, uint8_t *out)
{
	float v;

	if (!rec_key_f(gid, (key_t16)k, &v))
		return 0;
	*out = (uint8_t)(int)v;
	return 1;
}

void EntvarsKeyvalue(int e, gid_t16 gid)
{
	CBaseEntity *pev = CBase(e);
	col_ent_t *cv = ent_cv(e);
	ent_rnd_t *r = ent_rnd(e);
	float v, c[3];

	if (gid == GID_NONE)
		return;
	ent_idc(e)->targetname = rec_targetname(gid);
	pev->spawnflags = (uint32_t)(int32_t)KeyF(gid, K_SPAWNFLAGS, 0);
	pev->target = rec_key_name(gid, K_TARGET);
	pev->netname = rec_key_name(gid, K_NETNAME);
	pev->model = rec_key_res(gid, K_MODEL);
	pev->health = KeyF(gid, K_HEALTH, 0);
	pev->max_health = KeyF(gid, K_MAX_HEALTH, 0);
	pev->speed = KeyF(gid, K_SPEED, 0);
	pev->dmg = KeyF(gid, K_DMG, 0);
	pev->takedamage = (uint8_t)(int)KeyF(gid, K_TAKEDAMAGE, 0);
	if (rec_key_f(gid, K_RENDERMODE, &v))
		cv->rendermode = (uint8_t)(int)v;
	key_u8(gid, K_RENDERAMT, &r->renderamt);
	key_u8(gid, K_RENDERFX, &r->renderfx);
	if (rec_key_v(gid, K_RENDERCOLOR, c))
	{
		r->rendercolor[0] = (uint8_t)(int)c[0];
		r->rendercolor[1] = (uint8_t)(int)c[1];
		r->rendercolor[2] = (uint8_t)(int)c[2];
	}
	if (rec_key_f(gid, K_SKIN, &v))
		cv->skin = (int16_t)(int)v;
	key_u8(gid, K_BODY, &r->body);
	if (rec_key_f(gid, K_SCALE, &v))
		r->scale = v;
	if (rec_key_f(gid, K_FRAME, &v))
		r->frame = v;
	if (rec_key_f(gid, K_FRAMERATE, &v))
		r->framerate = v;
	if (rec_key_f(gid, K_SEQUENCE, &v))
		r->sequence = (uint16_t)(int)v;
	if (rec_key_f(gid, K_EFFECTS, &v))
		r->effects = (uint16_t)(int)v;
	if (rec_key_f(gid, K_GRAVITY, &v))
		ent_phys(e)->gravity = v;
	if (rec_key_f(gid, K_FRICTION, &v))
		ent_phys(e)->friction = v;
}

void CBaseDelay_KeyValue(int e, gid_t16 gid)
{
	CBaseDelay *p = PRIV(CBaseDelay, e);

	p->m_iszKillTarget = NAME_NONE;
	if (gid == GID_NONE)
		return;
	p->m_flDelay = KeyF(gid, K_DELAY, 0);
	p->m_iszKillTarget = rec_key_name(gid, K_KILLTARGET);
}

void CBaseToggle_KeyValue(int e, gid_t16 gid)
{
	CBaseToggle *p = PRIV(CBaseToggle, e);

	CBaseDelay_KeyValue(e, gid);
	p->m_sMaster = NAME_NONE;
	p->m_pfnCallWhenMoveDone = FN_NONE;
	if (gid == GID_NONE)
		return;
	p->m_flLip = KeyF(gid, K_LIP, 0);
	p->m_flWait = KeyF(gid, K_WAIT, 0);
	p->m_sMaster = rec_key_name(gid, K_MASTER);
	p->m_flMoveDistance = KeyF(gid, K_DISTANCE, 0);
}

/* ---- firing ------------------------------------------------------------------ */

void UseEntity(int e, int activator, int caller, int use_type, float value)
{
	game_use(e, activator, caller, use_type, value);
}

void FireTargets(name_t target, int activator, int caller, int use_type, float value)
{
	if (target == NAME_NONE)
		return;
	GameFire(target, activator, caller, GID_NONE, use_type, value);
}

void CBaseEntity_SUB_UseTargets(int e, int activator, int use_type, float value)
{
	CBaseEntity *pev = CBase(e);

	if (pev->target != NAME_NONE)
		FireTargets(pev->target, activator, e, use_type, value);
}

void KillTargets(name_t n)
{
	int i, cnt, e, it;
	gid_t16 g;

	if (n == NAME_NONE)
		return;
	cnt = rec_name_count(n);
	for (i = 0; i < cnt; i++)
	{
		g = rec_name_gid(n, i);
		switch (rec_kind(g))
		{
		case REC_KIND_LOGIC:
			rec_timer_cancel(g);
			rec_kill(g);
			break;
		case REC_KIND_ENTITY:
			e = rec_entity(g);
			if (e >= 0)
				UTIL_Remove(e);
			else if (rec_state(g) != REC_KILLED)
				rec_kill(g);
			break;
		default:
			break;
		}
	}
	/* runtime holders of the name (monstermaker children) */
	it = 0;
	while ((e = rec_next_named(n, &it)) >= 0)
		if (!(ent_cv(e)->flags & FL_KILLME))
			UTIL_Remove(e);
}

void SUB_UseTargets(int e, int activator, int use_type, float value)
{
	CBaseDelay *p = PRIV(CBaseDelay, e);

	if (p->target == NAME_NONE && p->m_iszKillTarget == NAME_NONE)
		return;
	if (p->m_flDelay != 0)
	{
		/* DelayedUse: owner = the activator only when it is the player;
		 * DelayThink fires with value 0 */
		GameFireAt(gtime() + p->m_flDelay, p->target, p->m_iszKillTarget,
			IsPlayer(activator) ? activator : -1, e, use_type, 0);
		return;
	}
	if (p->m_iszKillTarget != NAME_NONE)
		KillTargets(p->m_iszKillTarget);
	if (p->target != NAME_NONE)
		FireTargets(p->target, activator, e, use_type, value);
}

int ShouldToggle(int use_type, int current)
{
	if (use_type != USE_TOGGLE && use_type != USE_SET)
	{
		if ((current && use_type == USE_ON) || (!current && use_type == USE_OFF))
			return 0;
	}
	return 1;
}

int UTIL_IsMasterTriggered(name_t master, int activator)
{
	int s;

	if (master == NAME_NONE)
		return 1;
	s = game_master_state(master, activator);
	if (s < 0)
	{
		log_printf("game: Master was null or not a master!\n");
		return 1;
	}
	return s;
}

/* ---- CBaseToggle moves ---------------------------------------------------- */

void LinearMove(int e, const float dest[3], float speed)
{
	CBaseToggle *p = PRIV(CBaseToggle, e);
	float delta[3], v[3], travel;

	VecCopy(dest, p->m_vecFinalDest);
	if (VecEq(dest, ent_cv(e)->origin))
	{
		CBaseToggle_LinearMoveDone(e);
		return;
	}
	VecSub(dest, ent_cv(e)->origin, delta);
	travel = VecLen(delta) / speed;
	ent_think_at(e, ent_tm(e)->ltime + travel);
	SetThink(e, FN_CBaseToggle_LinearMoveDone);
	v[0] = delta[0] / travel;
	v[1] = delta[1] / travel;
	v[2] = delta[2] / travel;
	ent_set_velocity(e, v);
}

void CBaseToggle_LinearMoveDone(int e)
{
	CBaseToggle *p = PRIV(CBaseToggle, e);
	float delta[3];

	VecSub(p->m_vecFinalDest, ent_cv(e)->origin, delta);
	if (VecLen(delta) > 0.03125f)
	{
		LinearMove(e, p->m_vecFinalDest, 100);
		return;
	}
	ent_set_origin(e, p->m_vecFinalDest);
	ent_set_velocity(e, g_vecZero);
	ent_think_at(e, -1);
	if (p->m_pfnCallWhenMoveDone)
		game_call_think(e, p->m_pfnCallWhenMoveDone);
}

void AngularMove(int e, const float dest[3], float speed)
{
	CBaseToggle *p = PRIV(CBaseToggle, e);
	float delta[3], v[3], travel;

	VecCopy(dest, p->m_vecFinalAngle);
	if (VecEq(dest, ent_cv(e)->angles))
	{
		CBaseToggle_AngularMoveDone(e);
		return;
	}
	VecSub(dest, ent_cv(e)->angles, delta);
	travel = VecLen(delta) / speed;
	ent_think_at(e, ent_tm(e)->ltime + travel);
	SetThink(e, FN_CBaseToggle_AngularMoveDone);
	v[0] = delta[0] / travel;
	v[1] = delta[1] / travel;
	v[2] = delta[2] / travel;
	ent_set_avelocity(e, v);
}

void CBaseToggle_AngularMoveDone(int e)
{
	CBaseToggle *p = PRIV(CBaseToggle, e);

	VecCopy(p->m_vecFinalAngle, ent_cv(e)->angles);
	ent_set_avelocity(e, g_vecZero);
	ent_think_at(e, -1);
	ent_relink(e, 0);
	if (p->m_pfnCallWhenMoveDone)
		game_call_think(e, p->m_pfnCallWhenMoveDone);
}

int IsLockedByMaster(int e)
{
	CBaseToggle *p = PRIV(CBaseToggle, e);

	return p->m_sMaster != NAME_NONE &&
		!UTIL_IsMasterTriggered(p->m_sMaster, EHGet(p->m_hActivator));
}

#define SF_DOOR_ROTATE_Z	64
#define SF_DOOR_ROTATE_X	128

float AxisValue(int flags, const float ang[3])
{
	if (flags & SF_DOOR_ROTATE_Z)
		return ang[2];
	if (flags & SF_DOOR_ROTATE_X)
		return ang[0];
	return ang[1];
}

void AxisDir(int e)
{
	CBaseEntity *pev = CBase(e);

	if (pev->spawnflags & SF_DOOR_ROTATE_Z)
		VecSet(pev->movedir, 0, 0, 1);
	else if (pev->spawnflags & SF_DOOR_ROTATE_X)
		VecSet(pev->movedir, 1, 0, 0);
	else
		VecSet(pev->movedir, 0, 1, 0);
}

float AxisDelta(int flags, const float a1[3], const float a2[3])
{
	if (flags & SF_DOOR_ROTATE_Z)
		return a1[2] - a2[2];
	if (flags & SF_DOOR_ROTATE_X)
		return a1[0] - a2[0];
	return a1[1] - a2[1];
}

/* ---- traces --------------------------------------------------------------- */

/* engine/coll takes and returns the frame of eng_frame_layer() (coll.h):
 * the game's map-local coordinates pass straight through. */

void UTIL_TraceLineGlass(const float start[3], const float end[3], int igmon, int ignglass,
	int ignore, col_trace_t *tr)
{
	int type = igmon == ignore_monsters ? COL_MOVE_NOMONSTERS :
		igmon == missile ? COL_MOVE_MISSILE : COL_MOVE_NORMAL;

	if (ignglass)
		type |= COL_MOVE_NOGLASS;
	col_trace_line(start, end, type, ignore >= 0 ? ent_cv(ignore) : 0, tr);
}

void UTIL_TraceLine(const float start[3], const float end[3], int igmon, int ignore,
	col_trace_t *tr)
{
	UTIL_TraceLineGlass(start, end, igmon, dont_ignore_glass, ignore, tr);
}

static void trace_box(const float start[3], const float mins[3], const float maxs[3],
	const float end[3], int igmon, int ignore, col_trace_t *tr)
{
	int type = igmon == ignore_monsters ? COL_MOVE_NOMONSTERS :
		igmon == missile ? COL_MOVE_MISSILE : COL_MOVE_NORMAL;

	col_move(start, mins, maxs, end, type, ignore >= 0 ? ent_cv(ignore) : 0, tr);
}

void UTIL_TraceHull(const float start[3], const float end[3], int igmon, int hull, int ignore,
	col_trace_t *tr)
{
	if (hull < 0 || hull >= COL_HULLS)
		hull = 0;
	trace_box(start, col_hull_mins[hull], col_hull_maxs[hull], end, igmon, ignore, tr);
}

void UTIL_TraceMonsterHull(int e, const float start[3], const float end[3], int igmon,
	int ignore, col_trace_t *tr)
{
	trace_box(start, ent_cv(e)->mins, ent_cv(e)->maxs, end, igmon, ignore, tr);
}

void UTIL_TraceModel(const float start[3], const float end[3], int hull, int model_e,
	col_trace_t *tr)
{
	if (hull < 0 || hull >= COL_HULLS)
		hull = 0;
	col_clip(ent_cv(model_e), start, col_hull_mins[hull], col_hull_maxs[hull], end, tr);
}

int UTIL_PointContents(const float p[3])
{
	return col_contents(p);
}

int UTIL_TraceTexture(int hit, const float start[3], const float end[3])
{
	static const float zero[3] = { 0, 0, 0 };
	const col_ent_t *c;
	float org[3], d[3];

	if (hit <= 0)
		return col_material(COL_MODEL(eng_frame_layer(), 0), zero, zero, start, end);
	c = ent_cv(hit);
	if (c->model == COL_NONE)
		return 0;
	ent_frame_delta(hit, d);
	VecAdd(c->origin, d, org);
	return col_material(c->model, org, c->angles, start, end);
}

void EyePosition(int e, float out[3])
{
	const game_vt_t *vt = ent_vt(e);

	if (vt && vt->EyePosition)
	{
		vt->EyePosition(e, out);
		return;
	}
	VecAdd(ent_cv(e)->origin, CBase(e)->view_ofs, out);
}

void BodyTarget(int e, const float src[3], float out[3])
{
	const game_vt_t *vt = ent_vt(e);

	if (vt && vt->BodyTarget)
	{
		vt->BodyTarget(e, src, out);
		return;
	}
	Center(e, out);
}

int FVisible(int e, int other)
{
	col_trace_t tr;
	float a[3], b[3];
	int wl = ent_phys(e)->waterlevel, owl = ent_phys(other)->waterlevel;

	if (ent_cv(other)->flags & FL_NOTARGET)
		return 0;
	if ((wl != 3 && owl == 3) || (wl == 3 && owl == 0))
		return 0;
	VecAdd(ent_cv(e)->origin, CBase(e)->view_ofs, a);
	EyePosition(other, b);
	UTIL_TraceLineGlass(a, b, ignore_monsters, ignore_glass, e, &tr);
	return tr.frac == 1.0f;
}

int FVisiblePoint(int e, const float p[3])
{
	col_trace_t tr;
	float a[3];

	EyePosition(e, a);
	UTIL_TraceLineGlass(a, p, ignore_monsters, ignore_glass, e, &tr);
	return tr.frac == 1.0f;
}

/* ---- searches -------------------------------------------------------------- */

int UTIL_FindByTargetname1(name_t n)
{
	int it = 0;

	return UTIL_FindEntityByTargetname(n, &it);
}

int UTIL_EntitiesInBox(uint16_t *list, int max, const float mins[3], const float maxs[3],
	uint32_t flagmask)
{
	return ent_in_box(mins, maxs, flagmask, list, max);
}

int UTIL_MonstersInSphere(uint16_t *list, int max, const float center[3], float r)
{
	float mn[3], mx[3], d, delta, r2 = r * r;
	uint16_t tmp[64];
	int i, j, n, cnt = 0;

	for (i = 0; i < 3; i++)
	{
		mn[i] = center[i] - r;
		mx[i] = center[i] + r;
	}
	n = ent_in_box(mn, mx, FL_CLIENT | FL_MONSTER, tmp, 64);
	for (i = 0; i < n && cnt < max; i++)
	{
		const col_ent_t *c = ent_cv(tmp[i]);
		if (!(c->flags & (FL_CLIENT | FL_MONSTER)))
			continue;
		d = 0;
		for (j = 0; j < 3; j++)
		{
			delta = center[j] - (c->absmin[j] + c->absmax[j]) * 0.5f;
			d += delta * delta;
		}
		if (d > r2)
			continue;
		list[cnt++] = tmp[i];
	}
	return cnt;
}

int GetNextTarget(int e)
{
	return UTIL_FindByTargetname1(CBase(e)->target);
}

int UTIL_NamedOrigin(name_t n, float org[3], float ang[3])
{
	int i, cnt, e;
	gid_t16 g;

	if (n == NAME_NONE)
		return 0;
	cnt = rec_name_count(n);
	for (i = 0; i < cnt; i++)
	{
		g = rec_name_gid(n, i);
		if (rec_kind(g) == REC_KIND_POINT)
		{
			rec_origin(g, org);
			if (ang)
				rec_angles(g, ang);
			return 1;
		}
	}
	e = UTIL_FindByTargetname1(n);
	if (e < 0)
		return 0;
	VecCopy(ent_cv(e)->origin, org);
	if (ang)
		VecCopy(ent_cv(e)->angles, ang);
	return 1;
}

/* ---- sound ------------------------------------------------------------------- */

void UTIL_EmitAmbientSound(int e, const float org[3], res_t s, float vol, float attn, int flags,
	int pitch)
{
	if (RES_KIND(s) == RES_K_SENTENCE)
	{
		snd_sentence(e, CHAN_STATIC, (uint16_t)RES_INDEX(s), vol, attn, flags, pitch);
		return;
	}
	snd_ambient(org, s, vol, attn, flags, pitch);
}

void EMIT_SENTENCE(int e, int chan, res_t s, float vol, float attn, int flags, int pitch)
{
	uint16_t first;
	int n;

	switch (RES_KIND(s))
	{
	case RES_K_SENTENCE:
		snd_sentence(e, chan, (uint16_t)RES_INDEX(s), vol, attn, flags, pitch);
		break;
	case RES_K_GROUP:
		/* SENTENCEG_PlayRndSz: a random sentence of the group */
		n = res_group((uint16_t)RES_INDEX(s), &first);
		if (n > 0)
			snd_sentence(e, chan, (uint16_t)(first + RANDOM_LONG(0, n - 1)), vol, attn,
				flags, pitch);
		break;
	case RES_K_SOUND:
		snd_emit(e, chan, s, vol, attn, flags, pitch);
		break;
	default:
		break;
	}
}

void PRECACHE(res_t r)
{
	if (r != RES_NONE)
		res_precache(r);
}

/* ---- damage --------------------------------------------------------------- */

int ObjectCaps(int e)
{
	const game_vt_t *vt = ent_vt(e);

	return vt && vt->ObjectCaps ? vt->ObjectCaps(e) : FCAP_ACROSS_TRANSITION;
}

int Classify(int e)
{
	const game_vt_t *vt = ent_vt(e);

	return vt && vt->Classify ? vt->Classify(e) : CLASS_NONE;
}

int BloodColor(int e)
{
	const game_vt_t *vt = ent_vt(e);

	return vt && vt->BloodColor ? vt->BloodColor(e) : DONT_BLEED;
}

int IsTriggered(int e, int activator)
{
	const game_vt_t *vt = ent_vt(e);

	return vt && vt->IsTriggered ? vt->IsTriggered(e, activator) : 1;
}

int GetToggleState(int e)
{
	const game_vt_t *vt = ent_vt(e);

	return vt && vt->GetToggleState ? vt->GetToggleState(e) : TS_AT_TOP;
}

float GetDelay(int e)
{
	const game_vt_t *vt = ent_vt(e);

	return vt && vt->GetDelay ? vt->GetDelay(e) : 0;
}

int IsMoving(int e)
{
	const game_vt_t *vt = ent_vt(e);

	if (vt && vt->IsMoving)
		return vt->IsMoving(e);
	return !VecIsZero(ent_mot(e)->velocity);
}

int IsAlive(int e)
{
	const game_vt_t *vt = ent_vt(e);
	CBaseEntity *pev;

	if (vt && vt->IsAlive)
		return vt->IsAlive(e);
	pev = CBase(e);
	return pev && pev->deadflag == DEAD_NO && pev->health > 0;
}

int IsBSPModel(int e)
{
	return ent_cv(e)->solid == SOLID_BSP || ent_cv(e)->movetype == MOVETYPE_PUSHSTEP;
}

int CBaseEntity_DamageDecal(int e, int bits)
{
	(void)bits;
	if (ent_cv(e)->rendermode == kRenderTransAlpha)
		return -1;
	if (ent_cv(e)->rendermode != kRenderNormal)
		return DECAL_BPROOF1;
	return DECAL_GUNSHOT1 + RANDOM_LONG(0, 4);
}

int DamageDecal(int e, int bits)
{
	const game_vt_t *vt = ent_vt(e);

	return vt && vt->DamageDecal ? vt->DamageDecal(e, bits) : CBaseEntity_DamageDecal(e, bits);
}

int CBaseEntity_TakeHealth(int e, float health, int bits)
{
	CBaseEntity *pev = CBase(e);

	(void)bits;
	if (!pev->takedamage)
		return 0;
	if (pev->health >= pev->max_health)
		return 0;
	pev->health += health;
	if (pev->health > pev->max_health)
		pev->health = pev->max_health;
	return 1;
}

int TakeHealth(int e, float health, int bits)
{
	const game_vt_t *vt = ent_vt(e);

	return vt && vt->TakeHealth ? vt->TakeHealth(e, health, bits) :
		CBaseEntity_TakeHealth(e, health, bits);
}

int CBaseEntity_TakeDamage(int e, int inflictor, int attacker, float damage, int bits)
{
	CBaseEntity *pev = CBase(e);
	const col_ent_t *cv = ent_cv(e);
	float tmp[3], org[3], dir[3], force, size[3];

	(void)bits;
	if (!pev->takedamage)
		return 0;
	if (inflictor >= 0)
	{
		VecBModelOrigin(e, org);
		VecSub(ent_cv(inflictor)->origin, org, tmp);
		VecNorm(tmp, g_vecAttackDir);
	}
	if (inflictor >= 0 && (cv->movetype == MOVETYPE_WALK || cv->movetype == MOVETYPE_STEP) &&
	    (attacker < 0 || ent_cv(attacker)->solid != SOLID_TRIGGER))
	{
		const col_ent_t *ic = ent_cv(inflictor);
		int i;

		for (i = 0; i < 3; i++)
			tmp[i] = cv->origin[i] - (ic->absmin[i] + ic->absmax[i]) * 0.5f;
		VecNorm(tmp, dir);
		VecSub(cv->maxs, cv->mins, size);
		force = (float)(damage * ((32 * 32 * 72.0) / (size[0] * size[1] * size[2])) * 5);
		if (force > 1000.0f)
			force = 1000.0f;
		VecMA(ent_mot(e)->velocity, force, dir, tmp);
		ent_set_velocity(e, tmp);
	}
	pev->health -= damage;
	if (pev->health <= 0)
	{
		Killed(e, attacker, GIB_NORMAL);
		return 0;
	}
	return 1;
}

int TakeDamage(int e, int inflictor, int attacker, float damage, int bits)
{
	const game_vt_t *vt = ent_vt(e);

	if (e < 0 || !ent_is_live(e) || !CBase(e))
		return 0;
	return vt && vt->TakeDamage ? vt->TakeDamage(e, inflictor, attacker, damage, bits) :
		CBaseEntity_TakeDamage(e, inflictor, attacker, damage, bits);
}

void CBaseEntity_Killed(int e, int attacker, int gib)
{
	CBaseEntity *pev = CBase(e);

	(void)attacker;
	(void)gib;
	pev->takedamage = DAMAGE_NO;
	pev->deadflag = DEAD_DEAD;
	UTIL_Remove(e);
}

void Killed(int e, int attacker, int gib)
{
	const game_vt_t *vt = ent_vt(e);

	if (vt && vt->Killed)
		vt->Killed(e, attacker, gib);
	else
		CBaseEntity_Killed(e, attacker, gib);
}

/* CBaseEntity::TraceBleed */
void TraceBleed(int e, float damage, const float dir[3], const col_trace_t *ptr, int bits)
{
	col_trace_t btr;
	float tdir[3], to[3], noise;
	int count, i, color = BloodColor(e);

	if (color == DONT_BLEED || damage == 0)
		return;
	if (!(bits & (DMG_CRUSH | DMG_BULLET | DMG_SLASH | DMG_BLAST | DMG_CLUB | DMG_MORTAR)))
		return;
	if (damage < 10)
	{
		noise = 0.1f;
		count = 1;
	}
	else if (damage < 25)
	{
		noise = 0.2f;
		count = 2;
	}
	else
	{
		noise = 0.3f;
		count = 4;
	}
	for (i = 0; i < count; i++)
	{
		VecScale(dir, -1, tdir);
		tdir[0] += RANDOM_FLOAT(-noise, noise);
		tdir[1] += RANDOM_FLOAT(-noise, noise);
		tdir[2] += RANDOM_FLOAT(-noise, noise);
		VecMA(ptr->end, -172, tdir, to);
		UTIL_TraceLine(ptr->end, to, ignore_monsters, e, &btr);
		if (btr.frac != 1.0f)
			UTIL_BloodDecalTrace(&btr, color);
	}
}

/* combat.cpp SpawnBlood */
void SpawnBlood(const float org[3], int color, float damage)
{
	UTIL_BloodDrips(org, g_vecAttackDir, color, (int)damage);
}

void CBaseEntity_TraceAttack(int e, int attacker, float damage, const float dir[3],
	const col_trace_t *tr, int bits)
{
	float org[3];
	int blood;

	VecMA(tr->end, -4, dir, org);
	if (CBase(e)->takedamage)
	{
		AddMultiDamage(attacker, e, damage, bits);
		blood = BloodColor(e);
		if (blood != DONT_BLEED)
		{
			SpawnBlood(org, blood, damage);	/* a little surface blood */
			TraceBleed(e, damage, dir, tr, bits);
		}
	}
}

void TraceAttack(int e, int attacker, float damage, const float dir[3], const col_trace_t *tr,
	int bits)
{
	const game_vt_t *vt = ent_vt(e);

	if (e < 0 || !CBase(e))
		return;
	if (vt && vt->TraceAttack)
		vt->TraceAttack(e, attacker, damage, dir, tr, bits);
	else
		CBaseEntity_TraceAttack(e, attacker, damage, dir, tr, bits);
}

static struct {
	ent_h	ent;
	int	valid;
	float	amount;
	int	type;
} gMultiDamage;

void ClearMultiDamage(void)
{
	gMultiDamage.valid = 0;
	gMultiDamage.amount = 0;
	gMultiDamage.type = 0;
}

void ApplyMultiDamage(int inflictor, int attacker)
{
	int e;

	if (!gMultiDamage.valid)
		return;
	e = ent_of(gMultiDamage.ent);
	if (e < 0)
		return;
	TakeDamage(e, inflictor, attacker, gMultiDamage.amount, gMultiDamage.type);
}

void AddMultiDamage(int inflictor, int e, float damage, int bits)
{
	if (e < 0)
		return;
	gMultiDamage.type |= bits;
	if (!gMultiDamage.valid || ent_of(gMultiDamage.ent) != e)
	{
		ApplyMultiDamage(inflictor, inflictor);
		gMultiDamage.ent = ent_handle_of(e);
		gMultiDamage.valid = 1;
		gMultiDamage.amount = 0;
	}
	gMultiDamage.amount += damage;
}

void RadiusDamage(const float src_in[3], int inflictor, int attacker, float damage, float radius,
	int class_ignore, int bits)
{
	col_trace_t tr;
	float src[3], spot[3], d[3], dir[3], falloff, adj;
	int e = -1, in_water;

	falloff = radius ? damage / radius : 1.0f;
	in_water = UTIL_PointContents(src_in) == CONTENTS_WATER;
	VecCopy(src_in, src);
	src[2] += 1;
	if (attacker < 0)
		attacker = inflictor;
	while ((e = UTIL_FindEntityInSphere(e, src, radius)) >= 0)
	{
		CBaseEntity *pev = CBase(e);
		if (!pev || pev->takedamage == DAMAGE_NO)
			continue;
		if (class_ignore != CLASS_NONE && Classify(e) == class_ignore)
			continue;
		if (in_water && ent_phys(e)->waterlevel == 0)
			continue;
		if (!in_water && ent_phys(e)->waterlevel == 3)
			continue;
		BodyTarget(e, src, spot);
		UTIL_TraceLine(src, spot, dont_ignore_monsters, inflictor, &tr);
		if (tr.frac == 1.0f || tr.ent == e)
		{
			if (tr.startsolid)
			{
				VecCopy(src, tr.end);
				tr.frac = 0;
			}
			VecSub(src, tr.end, d);
			adj = damage - VecLen(d) * falloff;
			if (adj < 0)
				adj = 0;
			if (tr.frac != 1.0f)
			{
				ClearMultiDamage();
				VecSub(tr.end, src, d);
				VecNorm(d, dir);
				TraceAttack(e, inflictor, adj, dir, &tr, bits);
				ApplyMultiDamage(inflictor, attacker);
			}
			else
				TakeDamage(e, inflictor, attacker, adj, bits);
		}
	}
}

/* ---- global states ------------------------------------------------------------ */

#define GAME_MAX_GLOBALS	64
/* 0: not in the table, else GLOBAL_* + 1 */
static uint8_t	global_state[GAME_MAX_GLOBALS];

static int global_ok(int id)
{
	if (id >= 0 && id < GAME_MAX_GLOBALS && id < res_globals())
		return 1;
	if (id >= 0)
		log_printf("game: global state id %d out of range\n", id);
	return 0;
}

int GlobalState(int id)
{
	if (!global_ok(id) || !global_state[id])
		return GLOBAL_OFF;
	return global_state[id] - 1;
}

int GlobalInTable(int id)
{
	return global_ok(id) && global_state[id] != 0;
}

void GlobalSetState(int id, int state)
{
	if (GlobalInTable(id))
		global_state[id] = (uint8_t)(state + 1);
}

void GlobalAdd(int id, int state)
{
	if (global_ok(id))
		global_state[id] = (uint8_t)(state + 1);
}

void GlobalReset(void)
{
	memset(global_state, 0, sizeof global_state);
}

int game_global_state(int id)
{
	return GlobalState(id);
}

/* ---- effects helpers (util.cpp) ------------------------------------------------ */

int UTIL_ShouldShowBlood(int color)
{
	return color != DONT_BLEED;
}

void UTIL_DecalTrace(const col_trace_t *tr, int decal)
{
	if (decal < 0 || tr->frac == 1.0f)
		return;
	if (tr->ent > 0 && !IsBSPModel(tr->ent))
		return;
	mon_aiw.decals++;
	fx_decal_trace(tr, decal);
}

void UTIL_BloodDecalTrace(const col_trace_t *tr, int color)
{
	if (!UTIL_ShouldShowBlood(color))
		return;
	if (color == BLOOD_COLOR_RED)
		UTIL_DecalTrace(tr, DECAL_BLOOD1 + RANDOM_LONG(0, 5));
	else
		UTIL_DecalTrace(tr, DECAL_YBLOOD1 + RANDOM_LONG(0, 5));
}

void UTIL_BloodDrips(const float org[3], const float dir[3], int color, int amount)
{
	int size;

	(void)dir;
	if (!UTIL_ShouldShowBlood(color) || amount == 0)
		return;
	if (amount > 255)
		amount = 255;
	size = amount / 10;
	if (size < 3)
		size = 3;
	if (size > 16)
		size = 16;
	fx_blood_sprite(org, RESID(SPR_BLOODSPRAY), RESID(SPR_BLOOD), color, size);
}

void UTIL_BloodStream(const float org[3], const float dir[3], int color, int amount)
{
	if (!UTIL_ShouldShowBlood(color))
		return;
	fx_blood_stream(org, dir, color, amount > 255 ? 255 : amount);
}

void UTIL_Sparks(const float org[3])
{
	fx_sparks(org);
}

void UTIL_Ricochet(const float org[3], float scale)
{
	fx_ricochet(org, scale);
}

void UTIL_ScreenShake(const float center[3], float amplitude, float frequency, float duration,
	float radius)
{
	fx_shake(center, amplitude, frequency, duration, radius);
}

void UTIL_ScreenFade(int r, int g, int b, int alpha, float fade_time, float hold, int flags)
{
	fx_fade(r, g, b, alpha, fade_time, hold, flags);
}
