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
 * env.c -- HLSDK dlls/effects.cpp and explode.cpp effect entities on the
 * effects API (include/hl64/fx.h): env_sprite (CSprite), env_glow (CGlow),
 * env_explosion (CEnvExplosion, also created at run time by ExplosionCreate),
 * spark_shower (CShower), env_shake, env_fade, env_message, env_render
 * (triggers.cpp CRenderFxManager), env_spark (buttons.cpp CEnvSpark),
 * env_beverage, item_sodacan, gibshooter (CGibShooter), env_shooter
 * (CEnvShooter, gibs through GameMon's game/hl/gib.h CGib), and (M6
 * EnvFx) env_beam (CLightning), env_laser (CLaser, its end sprite a
 * temporary CSprite) and env_bubbles (CBubbling): the beam entities draw
 * as effects beams (engine/render/beam.c) keyed by the entity, their
 * strikes as TE_BEAM* effects beams, the bubbles as fx_fizz. env_shake /
 * fade / message / render / spark / explosion / shooter map records are
 * logic records (no entity): their state is the record's rec_logic_t and
 * their thinks record timers.
 * Retargets the old tree's hl_env.c 372-1166 (audit entry: HLSDK +
 * QUAKE-INTERFACE) and gsfx_sv.c env_sprite (HLSDK) by rewriting them
 * from effects.cpp / explode.cpp on the effects API; their own parts
 * (titles parser, render-tuple store, protocol writers) are replaced by
 * cooked titles (res_title) and fx.h. No line is imported.
 */
#include <math.h>
#include <string.h>

#include "engine/core/eng.h"
#include "game/hl/breakable.h"
#include "game/hl/doors.h"
#include "game/hl/gib.h"
#include "game/hl/savegame.h"

#define SF_SPRITE_STARTON		0x0001
#define SF_SPRITE_ONCE			0x0002
#define SF_SPRITE_TEMPORARY		0x8000

#define SF_ENVEXPLOSION_NODAMAGE	(1 << 0)
#define SF_ENVEXPLOSION_REPEATABLE	(1 << 1)
#define SF_ENVEXPLOSION_NOFIREBALL	(1 << 2)
#define SF_ENVEXPLOSION_NOSMOKE		(1 << 3)
#define SF_ENVEXPLOSION_NODECAL		(1 << 4)
#define SF_ENVEXPLOSION_NOSPARKS	(1 << 5)

#define SF_SHAKE_EVERYONE		0x0001
#define SF_FADE_IN			0x0001
#define SF_FADE_MODULATE		0x0002
#define SF_FADE_ONLYONE			0x0004
#define SF_MESSAGE_ONCE			0x0001
#define SF_MESSAGE_ALL			0x0002

#define SF_RENDER_MASKFX		(1 << 0)
#define SF_RENDER_MASKAMT		(1 << 1)
#define SF_RENDER_MASKMODE		(1 << 2)
#define SF_RENDER_MASKCOLOR		(1 << 3)

#define FFADE_IN			0x0000
#define FFADE_OUT			0x0001
#define FFADE_MODULATE			0x0002
#define FFADE_STAYOUT			0x0004

static uint32_t logic_spawnflags(gid_t16 gid)
{
	return (uint32_t)(int32_t)KeyF(gid, K_SPAWNFLAGS, 0);
}

static int NoTransition_ObjectCaps(int e)
{
	(void)e;
	return 0;
}

/* ---- env_sprite (CSprite) ------------------------------------------------- */

typedef struct {
	CBASEENTITY_FIELDS
	float		m_lastTime;
	float		m_maxFrame;
} CSprite;

static void CSprite_TurnOff(int e)
{
	ent_rnd(e)->effects = EF_NODRAW;
	ent_think_at(e, 0);
}

static void CSprite_TurnOn(int e)
{
	CSprite *p = PRIV(CSprite, e);

	ent_rnd(e)->effects = 0;
	if ((ent_rnd(e)->framerate && p->m_maxFrame > 1.0f) || (p->spawnflags & SF_SPRITE_ONCE))
	{
		SetThink(e, FN_CSprite_AnimateThink);
		ent_think_at(e, gtime());
		p->m_lastTime = gtime();
	}
	ent_rnd(e)->frame = 0;
}

static void CSprite_Animate(int e, float frames)
{
	CSprite *p = PRIV(CSprite, e);
	ent_rnd_t *r = ent_rnd(e);

	r->frame += frames;
	if (r->frame > p->m_maxFrame)
	{
		if (p->spawnflags & SF_SPRITE_ONCE)
			CSprite_TurnOff(e);
		else if (p->m_maxFrame > 0)
			r->frame = (float)fmod(r->frame, p->m_maxFrame);
	}
}

void CSprite_AnimateThink(int e)
{
	CSprite *p = PRIV(CSprite, e);
	float t = gtime();

	CSprite_Animate(e, ent_rnd(e)->framerate * (t - p->m_lastTime));
	ThinkIn(e, 0.1f);
	p->m_lastTime = t;
}

void CSprite_Use(int e, int activator, int caller, int use_type, float value)
{
	int on = ent_rnd(e)->effects != EF_NODRAW;

	(void)activator;
	(void)caller;
	(void)value;
	if (ShouldToggle(use_type, on))
	{
		if (on)
			CSprite_TurnOff(e);
		else
			CSprite_TurnOn(e);
	}
}

static int CSprite_Spawn(int e, gid_t16 gid)
{
	CSprite *p = PRIV(CSprite, e);
	float *ang = ent_cv(e)->angles;

	(void)gid;
	ent_set_solid(e, SOLID_NOT);
	ent_set_movetype(e, MOVETYPE_NONE);
	ent_rnd(e)->effects = 0;
	ent_rnd(e)->frame = 0;
	/* Precache: no attachment */
	PRECACHE(p->model);
	ent_cv(e)->skin = 0;
	ent_rnd(e)->body = 0;
	ent_set_model(e, p->model);
	p->m_maxFrame = (float)res_model_frames(p->model) - 1;
	SetUse(e, FN_CSprite_Use);
	if (ent_idc(e)->targetname != NAME_NONE && !(p->spawnflags & SF_SPRITE_STARTON))
		CSprite_TurnOff(e);
	else
		CSprite_TurnOn(e);
	/* Worldcraft only sets y rotation, copy to Z */
	if (ang[1] != 0 && ang[2] == 0)
	{
		ang[2] = ang[1];
		ang[1] = 0;
	}
	return 0;
}

static int CSprite_ObjectCaps(int e)
{
	if (PRIV(CSprite, e)->spawnflags & SF_SPRITE_TEMPORARY)
		return FCAP_DONT_SAVE;
	return 0;	/* CPointEntity: no level transitions */
}

static const game_vt_t CSprite_vt = {
	.ObjectCaps = CSprite_ObjectCaps,
};

/* SF_SPRITE_TEMPORARY sprites (FCAP_DONT_SAVE: an effect's helper, made
 * again by its owner) never persist */
static int CSprite_quiet(int e)
{
	return (PRIV(CSprite, e)->spawnflags & SF_SPRITE_TEMPORARY) ? -1 : 1;
}

/* HLSDK CSprite::m_SaveData */
static const sv_field_t f_CSprite[] = {
	SVF(CSprite, m_lastTime, SV_TIME, 0),
	SVF(CSprite, m_maxFrame, SV_F32, SVK),
};
static const sv_table_t sv_CSprite = SV_TABLE(&sv_CBaseEntity, f_CSprite,
	.quiet = CSprite_quiet);

const game_class_t gc_ENV_SPRITE = {
	.size = sizeof(CSprite), .Spawn = CSprite_Spawn, .vt = &CSprite_vt,
	.save = &sv_CSprite,
};

/* HLSDK CSprite::SpriteCreate(model, origin, animate) for an effect's
 * helper (CLaser's end sprite): a runtime env_sprite, SF_SPRITE_TEMPORARY
 * here (its owner keeps no saved pointer to it: it makes it again after a
 * restore); SpriteInit's model before Spawn's model steps. -1: no slot. */
static int CSprite_SpriteCreate(res_t model, const float org[3], int animate)
{
	int e = CBaseEntity_Create(CLS_ENV_SPRITE, org, g_vecZero, -1);
	CSprite *p;

	if (e < 0)
		return -1;
	p = PRIV(CSprite, e);
	p->spawnflags |= SF_SPRITE_TEMPORARY;
	p->model = model;
	PRECACHE(model);
	ent_set_model(e, model);
	p->m_maxFrame = (float)res_model_frames(model) - 1;
	ent_set_solid(e, SOLID_NOT);
	ent_set_movetype(e, MOVETYPE_NOCLIP);
	if (animate)
		CSprite_TurnOn(e);
	return e;
}

/* ---- env_glow (CGlow) ------------------------------------------------------ */

typedef struct {
	CBASEENTITY_FIELDS
	float		m_lastTime;
	float		m_maxFrame;
} CGlow;

static void CGlow_Animate(int e, float frames)
{
	CGlow *p = PRIV(CGlow, e);

	if (p->m_maxFrame > 0)
		ent_rnd(e)->frame = (float)fmod(ent_rnd(e)->frame + frames, p->m_maxFrame);
}

void CGlow_Think(int e)
{
	CGlow *p = PRIV(CGlow, e);

	CGlow_Animate(e, ent_rnd(e)->framerate * (gtime() - p->m_lastTime));
	ThinkIn(e, 0.1f);
	p->m_lastTime = gtime();
}

static int CGlow_Spawn(int e, gid_t16 gid)
{
	CGlow *p = PRIV(CGlow, e);

	(void)gid;
	ent_set_solid(e, SOLID_NOT);
	ent_set_movetype(e, MOVETYPE_NONE);
	ent_rnd(e)->effects = 0;
	ent_rnd(e)->frame = 0;
	PRECACHE(p->model);
	ent_set_model(e, p->model);
	p->m_maxFrame = (float)res_model_frames(p->model) - 1;
	if (p->m_maxFrame > 1.0f && ent_rnd(e)->framerate != 0)
	{
		SetThink(e, FN_CGlow_Think);
		ThinkIn(e, 0.1f);
	}
	p->m_lastTime = gtime();
	return 0;
}

/* CPointEntity: no level transitions */
static const game_vt_t CGlow_vt = {
	.ObjectCaps = NoTransition_ObjectCaps,
};

/* HLSDK CGlow::m_SaveData */
static const sv_field_t f_CGlow[] = {
	SVF(CGlow, m_lastTime, SV_TIME, 0),
	SVF(CGlow, m_maxFrame, SV_F32, SVK),
};
static const sv_table_t sv_CGlow = SV_TABLE(&sv_CBaseEntity, f_CGlow);

const game_class_t gc_ENV_GLOW = {
	.size = sizeof(CGlow), .Spawn = CGlow_Spawn, .vt = &CGlow_vt,
	.save = &sv_CGlow,
};

/* ---- spark_shower (CShower) --------------------------------------------- */

void CShower_Think(int e)
{
	CBaseEntity *pev = CBase(e);

	UTIL_Sparks(ent_cv(e)->origin);
	pev->speed -= 0.1f;
	if (pev->speed > 0)
		ThinkIn(e, 0.1f);
	else
		UTIL_Remove(e);
	EFlagsClear(e, FL_ONGROUND);
}

void CShower_Touch(int e, int other)
{
	CBaseEntity *pev = CBase(e);
	float v[3];

	(void)other;
	VecScale(ent_mot(e)->velocity, (ent_cv(e)->flags & FL_ONGROUND) ? 0.1f : 0.6f, v);
	ent_set_velocity(e, v);
	if (v[0] * v[0] + v[1] * v[1] < 10.0f)
		pev->speed = 0;
}

static int CShower_Spawn(int e, gid_t16 gid)
{
	CBaseEntity *pev = CBase(e);
	float v[3], *ang = ent_cv(e)->angles;

	(void)gid;
	VecScale(ang, RANDOM_FLOAT(200, 300), v);
	v[0] += RANDOM_FLOAT(-100.f, 100.f);
	v[1] += RANDOM_FLOAT(-100.f, 100.f);
	if (v[2] >= 0)
		v[2] += 200;
	else
		v[2] -= 200;
	ent_set_velocity(e, v);
	ent_set_movetype(e, MOVETYPE_BOUNCE);
	ent_phys(e)->gravity = 0.5f;
	ThinkIn(e, 0.1f);
	ent_set_solid(e, SOLID_NOT);
	ent_set_model(e, RESID(MDL_GRENADE));	/* a model; never drawn */
	UTIL_SetSize(e, g_vecZero, g_vecZero);
	ent_rnd(e)->effects |= EF_NODRAW;
	pev->speed = RANDOM_FLOAT(0.5f, 1.5f);
	VecSet(ang, 0, 0, 0);
	SetThink(e, FN_CShower_Think);
	SetTouch(e, FN_CShower_Touch);
	return 0;
}

static int CShower_ObjectCaps(int e)
{
	(void)e;
	return FCAP_DONT_SAVE;
}

static const game_vt_t CShower_vt = {
	.ObjectCaps = CShower_ObjectCaps,
};

const game_class_t gc_SPARK_SHOWER = {
	.size = sizeof(CBaseEntity), .Spawn = CShower_Spawn, .vt = &CShower_vt,
	.save = &sv_Transient,
};

/* ---- env_explosion (CEnvExplosion) ---------------------------------------- */

static int explosion_sprite_scale(int magnitude)
{
	float s = (magnitude - 50) * 0.6f;

	if (s < 10)
		s = 10;
	return (int)s;
}

/* CEnvExplosion::Use on an explosion at org (moved off the surface below
 * it, as HLSDK moves pev->origin); inflictor = the explosion entity (-1
 * for a logic record). */
static void explosion_use(float org[3], int magnitude, uint32_t sf, int self)
{
	col_trace_t tr;
	float spot[3], end[3];
	int i, sparks, scale = explosion_sprite_scale(magnitude);

	VecCopy(org, spot);
	spot[2] += 8;
	VecCopy(spot, end);
	end[2] -= 40;
	UTIL_TraceLine(spot, end, ignore_monsters, self, &tr);
	if (tr.frac != 1.0f)
		VecMA(tr.end, (magnitude - 24) * 0.6f, tr.normal, org);
	if (!(sf & SF_ENVEXPLOSION_NODECAL))
		UTIL_DecalTrace(&tr, RANDOM_FLOAT(0, 1) < 0.5f ? DECAL_SCORCH1 : DECAL_SCORCH2);
	fx_explosion(org, RESID(SPR_FIREBALL),
		(sf & SF_ENVEXPLOSION_NOFIREBALL) ? 0 : (uint8_t)scale, 15, 0);
	if (!(sf & SF_ENVEXPLOSION_NODAMAGE))
		RadiusDamage(org, self, self, (float)magnitude, magnitude * 2.5f, CLASS_NONE, DMG_BLAST);
	if (!(sf & SF_ENVEXPLOSION_NOSPARKS))
	{
		sparks = RANDOM_LONG(0, 3);
		for (i = 0; i < sparks; i++)
			CBaseEntity_Create(CLS_SPARK_SHOWER, org, tr.normal, -1);
	}
}

static void explosion_smoke(const float org[3], int magnitude, uint32_t sf)
{
	if (!(sf & SF_ENVEXPLOSION_NOSMOKE))
		fx_smoke(org, RESID(SPR_SMOKE), (uint8_t)explosion_sprite_scale(magnitude), 12);
}

/* entity form (ExplosionCreate): m_iMagnitude in dmg */
typedef struct {
	CBASEENTITY_FIELDS
	int32_t		m_iMagnitude;
} CEnvExplosion;

void CEnvExplosion_Smoke(int e)
{
	CEnvExplosion *p = PRIV(CEnvExplosion, e);

	explosion_smoke(ent_cv(e)->origin, p->m_iMagnitude, p->spawnflags);
	if (!(p->spawnflags & SF_ENVEXPLOSION_REPEATABLE))
		UTIL_Remove(e);
}

void CEnvExplosion_Use(int e, int activator, int caller, int use_type, float value)
{
	CEnvExplosion *p = PRIV(CEnvExplosion, e);
	float org[3];

	(void)activator;
	(void)caller;
	(void)use_type;
	(void)value;
	p->model = RES_NONE;
	ent_set_solid(e, SOLID_NOT);
	VecCopy(ent_cv(e)->origin, org);
	explosion_use(org, p->m_iMagnitude, p->spawnflags, e);
	ent_set_origin(e, org);
	SetThink(e, FN_CEnvExplosion_Smoke);
	ThinkIn(e, 0.3f);
}

static int CEnvExplosion_Spawn(int e, gid_t16 gid)
{
	CEnvExplosion *p = PRIV(CEnvExplosion, e);

	p->m_iMagnitude = (int)KeyF(gid, K_IMAGNITUDE, 0);
	ent_set_solid(e, SOLID_NOT);
	ent_rnd(e)->effects = EF_NODRAW;
	ent_set_movetype(e, MOVETYPE_NONE);
	SetUse(e, FN_CEnvExplosion_Use);
	return 0;
}

void ExplosionCreate(const float center[3], const float angles[3], int owner, int magnitude,
	int do_damage)
{
	int e = CBaseEntity_Create(CLS_ENV_EXPLOSION, center, angles, owner);
	CEnvExplosion *p;

	if (e < 0)
		return;
	p = PRIV(CEnvExplosion, e);
	p->m_iMagnitude = magnitude;
	if (!do_damage)
		p->spawnflags |= SF_ENVEXPLOSION_NODAMAGE;
	CEnvExplosion_Use(e, -1, -1, USE_TOGGLE, 0);
}

/* logic form: a = m_iMagnitude, flags = 1 while the smoke timer runs */
static void CEnvExplosion_LogicSpawn(gid_t16 gid)
{
	rec_logic(gid)->a = (int)KeyF(gid, K_IMAGNITUDE, 0);
}

static void CEnvExplosion_LogicUse(gid_t16 gid, int activator, int caller, int use_type,
	float value)
{
	rec_logic_t *l = rec_logic(gid);
	float org[3];

	(void)activator;
	(void)caller;
	(void)use_type;
	(void)value;
	rec_origin(gid, org);
	explosion_use(org, l->a, logic_spawnflags(gid), -1);
	rec_timer(gid, gtime() + 0.3f, 0, -1);
}

static void CEnvExplosion_LogicTimer(gid_t16 gid, uint16_t arg, int activator)
{
	rec_logic_t *l = rec_logic(gid);
	uint32_t sf = logic_spawnflags(gid);
	col_trace_t tr;
	float org[3], spot[3], end[3];

	(void)arg;
	(void)activator;
	/* the origin Use moved to (the same trace) */
	rec_origin(gid, org);
	VecCopy(org, spot);
	spot[2] += 8;
	VecCopy(spot, end);
	end[2] -= 40;
	UTIL_TraceLine(spot, end, ignore_monsters, -1, &tr);
	if (tr.frac != 1.0f)
		VecMA(tr.end, (l->a - 24) * 0.6f, tr.normal, org);
	explosion_smoke(org, l->a, sf);
	if (!(sf & SF_ENVEXPLOSION_REPEATABLE))
		rec_kill(gid);
}

/* a runtime explosion (ExplosionCreate) is a transient; map env_explosions
 * are logic records */
const game_class_t gc_ENV_EXPLOSION = {
	.size = sizeof(CEnvExplosion), .Spawn = CEnvExplosion_Spawn,
	.LogicSpawn = CEnvExplosion_LogicSpawn,
	.LogicUse = CEnvExplosion_LogicUse,
	.LogicTimer = CEnvExplosion_LogicTimer,
	.save = &sv_Transient,
};

/* ---- env_shake (CShake) ---------------------------------------------------- */

static void CShake_LogicUse(gid_t16 gid, int activator, int caller, int use_type, float value)
{
	float org[3], radius = KeyF(gid, K_RADIUS, 0);

	(void)activator;
	(void)caller;
	(void)use_type;
	(void)value;
	if (logic_spawnflags(gid) & SF_SHAKE_EVERYONE)
		radius = 0;
	rec_origin(gid, org);
	UTIL_ScreenShake(org, KeyF(gid, K_AMPLITUDE, 0), KeyF(gid, K_FREQUENCY, 0),
		KeyF(gid, K_DURATION, 0), radius);
}

const game_class_t gc_ENV_SHAKE = { .LogicUse = CShake_LogicUse };

/* ---- env_fade (CFade) ------------------------------------------------------- */

static void CFade_LogicUse(gid_t16 gid, int activator, int caller, int use_type, float value)
{
	uint32_t sf = logic_spawnflags(gid);
	int flags = 0;
	float c[3] = { 0, 0, 0 };
	name_t target;

	(void)caller;
	(void)use_type;
	(void)value;
	if (!(sf & SF_FADE_IN))
		flags |= FFADE_OUT;
	if (sf & SF_FADE_MODULATE)
		flags |= FFADE_MODULATE;
	rec_key_v(gid, K_RENDERCOLOR, c);
	/* ONLYONE fades the activator if it is the client; else every client */
	if (!(sf & SF_FADE_ONLYONE) || IsPlayer(activator))
		UTIL_ScreenFade((int)c[0], (int)c[1], (int)c[2], (int)KeyF(gid, K_RENDERAMT, 0),
			KeyF(gid, K_DURATION, 0), KeyF(gid, K_HOLDTIME, 0), flags);
	/* CBaseEntity::SUB_UseTargets(this, USE_TOGGLE, 0) */
	target = rec_key_name(gid, K_TARGET);
	if (target != NAME_NONE)
		GameFire(target, -1, -1, gid, USE_TOGGLE, 0);
}

const game_class_t gc_ENV_FADE = { .LogicUse = CFade_LogicUse };

/* ---- env_message (CMessage) ------------------------------------------------- */

/* rec_logic_t: a = title id (-1 none), a16 = messagesound res,
 * f = volume, b = attenuation index */
static void CMessage_LogicSpawn(gid_t16 gid)
{
	rec_logic_t *l = rec_logic(gid);
	float vol;

	l->a = rec_key_title(gid, K_MESSAGE);
	l->a16 = rec_key_res(gid, K_MESSAGESOUND);
	PRECACHE(l->a16);
	vol = KeyF(gid, K_MESSAGEVOLUME, 0) * 0.1f;
	l->f = vol <= 0 ? 1.0f : vol;
	l->b = (int)KeyF(gid, K_MESSAGEATTENUATION, 0);
}

static float message_attn(int i)
{
	switch (i)
	{
	case 1: return ATTN_STATIC;
	case 2: return ATTN_NORM;
	case 3: return ATTN_NONE;
	default: return ATTN_IDLE;
	}
}

/* UTIL_ShowMessage(title): the HUD draws the cooked title */
static void ShowMessage(int title, gid_t16 gid)
{
	const char *s;

	if (title < 0)
	{
		log_printf("message: map %u lump %u: no title\n", (unsigned)GID_SLOT(gid),
			(unsigned)GID_LUMP(gid));
		return;
	}
	s = res_title_text((uint16_t)title);
	log_printf("message: title %d %.40s\n", title, s ? s : "");
	fx_text((uint16_t)title);
}

static void CMessage_LogicUse(gid_t16 gid, int activator, int caller, int use_type, float value)
{
	rec_logic_t *l = rec_logic(gid);
	uint32_t sf = logic_spawnflags(gid);
	float org[3];
	name_t target;

	(void)activator;
	(void)caller;
	(void)use_type;
	(void)value;
	ShowMessage(l->a, gid);		/* SF_MESSAGE_ALL or the player: one client */
	if (l->a16)
	{
		rec_origin(gid, org);
		snd_ambient(org, l->a16, l->f, message_attn(l->b), 0, PITCH_NORM);
	}
	target = rec_key_name(gid, K_TARGET);
	if (sf & SF_MESSAGE_ONCE)
		rec_kill(gid);
	if (target != NAME_NONE)
		GameFire(target, -1, -1, gid, USE_TOGGLE, 0);
}

const game_class_t gc_ENV_MESSAGE = {
	.LogicSpawn = CMessage_LogicSpawn,
	.LogicUse = CMessage_LogicUse,
};

/* worldspawn's chapter title (CWorld::Precache creates a one-shot
 * env_message used 0.3 s later) */
void ShowChapterTitle(int title)
{
	if (title >= 0)
		ShowMessage(title, GID_NONE);
}

/* ---- env_render (CRenderFxManager) ------------------------------------------ */

static void CRenderFxManager_LogicUse(gid_t16 gid, int activator, int caller, int use_type,
	float value)
{
	uint32_t sf = logic_spawnflags(gid);
	name_t target = rec_key_name(gid, K_TARGET);
	float c[3] = { 0, 0, 0 };
	int it = 0, t, fx, amt, mode;

	(void)activator;
	(void)caller;
	(void)use_type;
	(void)value;
	if (target == NAME_NONE)
		return;
	fx = (int)KeyF(gid, K_RENDERFX, 0);
	amt = (int)KeyF(gid, K_RENDERAMT, 0);
	mode = (int)KeyF(gid, K_RENDERMODE, 0);
	rec_key_v(gid, K_RENDERCOLOR, c);
	while ((t = rec_next_named(target, &it)) >= 0)
	{
		ent_rnd_t *r = ent_rnd(t);

		if (!(sf & SF_RENDER_MASKFX))
			r->renderfx = (uint8_t)fx;
		if (!(sf & SF_RENDER_MASKAMT))
			r->renderamt = (uint8_t)amt;
		if (!(sf & SF_RENDER_MASKMODE))
			ent_cv(t)->rendermode = (uint8_t)mode;
		if (!(sf & SF_RENDER_MASKCOLOR))
		{
			r->rendercolor[0] = (uint8_t)c[0];
			r->rendercolor[1] = (uint8_t)c[1];
			r->rendercolor[2] = (uint8_t)c[2];
		}
	}
}

const game_class_t gc_ENV_RENDER = { .LogicUse = CRenderFxManager_LogicUse };

/* ---- env_spark (CEnvSpark) ---------------------------------------------- */

/* rec_logic_t: f = m_flDelay, flags = 1 while sparking (think set),
 * a16 = 1 when +use toggles (SF 32) */
static void spark_schedule(gid_t16 gid, float t)
{
	rec_logic(gid)->flags = 1;
	rec_timer(gid, t, 0, -1);
}

static void CEnvSpark_LogicSpawn(gid_t16 gid)
{
	rec_logic_t *l = rec_logic(gid);
	uint32_t sf = logic_spawnflags(gid);
	float t = gtime();

	l->f = KeyF(gid, K_MAXDELAY, 0);
	l->flags = 0;
	l->a16 = 0;
	if (sf & 32)
	{
		l->a16 = 1;
		if (sf & 64)
			spark_schedule(gid, t + (0.1f + RANDOM_FLOAT(0, 1.5f)));	/* start on */
	}
	else
		spark_schedule(gid, t + (0.1f + RANDOM_FLOAT(0, 1.5f)));
	if (l->f <= 0)
		l->f = 1.5f;
	DoSparkPrecache();
}

static void CEnvSpark_LogicTimer(gid_t16 gid, uint16_t arg, int activator)
{
	rec_logic_t *l = rec_logic(gid);
	float org[3];

	(void)arg;
	(void)activator;
	if (!l->flags)
		return;
	rec_timer(gid, gtime() + 0.1f + RANDOM_FLOAT(0, l->f), 0, -1);
	rec_origin(gid, org);
	DoSpark(-1, org, g_vecZero);
}

static void CEnvSpark_LogicUse(gid_t16 gid, int activator, int caller, int use_type,
	float value)
{
	rec_logic_t *l = rec_logic(gid);

	(void)activator;
	(void)caller;
	(void)use_type;
	(void)value;
	if (!l->a16)
		return;		/* no use function */
	if (l->flags)
	{
		l->flags = 0;	/* SparkStop */
		rec_timer_cancel(gid);
	}
	else
		spark_schedule(gid, gtime() + (0.1f + RANDOM_FLOAT(0, l->f)));	/* SparkStart */
}

const game_class_t gc_ENV_SPARK = {
	.LogicSpawn = CEnvSpark_LogicSpawn,
	.LogicUse = CEnvSpark_LogicUse,
	.LogicTimer = CEnvSpark_LogicTimer,
};

/* ---- env_beverage / item_sodacan ------------------------------------------ */

typedef struct {
	CBASEENTITY_FIELDS
	CBASEDELAY_FIELDS
	float		frags;		/* a can is waiting in the dispenser */
} CEnvBeverage;

void CEnvBeverage_Use(int e, int activator, int caller, int use_type, float value)
{
	CEnvBeverage *p = PRIV(CEnvBeverage, e);
	int can;

	(void)activator;
	(void)caller;
	(void)use_type;
	(void)value;
	if (p->frags != 0 || p->health <= 0)
		return;
	can = CBaseEntity_Create(CLS_ITEM_SODACAN, ent_cv(e)->origin, ent_cv(e)->angles, e);
	if (can >= 0)
		ent_cv(can)->skin = ent_cv(e)->skin == 6 ? (int16_t)RANDOM_LONG(0, 5) :
			ent_cv(e)->skin;
	p->frags = 1;
	p->health--;
}

static int CEnvBeverage_Spawn(int e, gid_t16 gid)
{
	CEnvBeverage *p = PRIV(CEnvBeverage, e);

	CBaseDelay_KeyValue(e, gid);
	PRECACHE(RESID(MDL_CAN));
	PRECACHE(RESID(G_BOUNCE3));
	ent_set_solid(e, SOLID_NOT);
	ent_rnd(e)->effects = EF_NODRAW;
	p->frags = 0;
	if (p->health == 0)
		p->health = 10;
	SetUse(e, FN_CEnvBeverage_Use);
	return 0;
}

/* HLSDK CEnvBeverage has no m_SaveData (CBaseDelay's; cans left in
 * health, a can in the chute in frags) */
static const sv_field_t f_CEnvBeverage[] = {
	SVF(CEnvBeverage, frags, SV_F32, 0),
};
static const sv_table_t sv_CEnvBeverage = SV_TABLE(&sv_CBaseDelay, f_CEnvBeverage);

const game_class_t gc_ENV_BEVERAGE = {
	.size = sizeof(CEnvBeverage), .Spawn = CEnvBeverage_Spawn,
	.save = &sv_CEnvBeverage,
};

void CItemSoda_CanThink(int e)
{
	static const float mn[3] = { -8, -8, 0 }, mx[3] = { 8, 8, 8 };

	EMIT_SOUND(e, CHAN_WEAPON, RESID(G_BOUNCE3), 1, ATTN_NORM);
	ent_set_solid(e, SOLID_TRIGGER);
	UTIL_SetSize(e, mn, mx);
	SetThink(e, FN_NONE);
	SetTouch(e, FN_CItemSoda_CanTouch);
}

void CItemSoda_CanTouch(int e, int other)
{
	int owner = ent_cv(e)->owner;

	if (!IsPlayer(other))
		return;
	TakeHealth(other, 1, DMG_GENERIC);
	if (owner != COL_NONE && ent_is_live(owner) && ent_idc(owner)->cls == CLS_ENV_BEVERAGE)
		PRIV(CEnvBeverage, owner)->frags = 0;	/* tell the machine the can was taken */
	ent_set_solid(e, SOLID_NOT);
	ent_set_movetype(e, MOVETYPE_NONE);
	ent_rnd(e)->effects = EF_NODRAW;
	SetTouch(e, FN_NONE);
	SetThink(e, FN_SUB_Remove);
	ent_think_at(e, gtime());
}

static int CItemSoda_Spawn(int e, gid_t16 gid)
{
	(void)gid;
	ent_set_solid(e, SOLID_NOT);
	ent_set_movetype(e, MOVETYPE_TOSS);
	ent_set_model(e, RESID(MDL_CAN));
	UTIL_SetSize(e, g_vecZero, g_vecZero);
	SetThink(e, FN_CItemSoda_CanThink);
	ThinkIn(e, 0.5f);
	return 0;
}

const game_class_t gc_ITEM_SODACAN = {
	.size = sizeof(CBaseEntity), .Spawn = CItemSoda_Spawn,
};

/* ---- gibshooter / env_shooter (CGibShooter / CEnvShooter) ------------------- */

#define SF_GIBSHOOTER_REPEATABLE	1	/* allows a gibshooter to be refired */

/* Logic records. rec_logic_t: a = m_iGibs, b = m_iGibCapacity, f = m_flDelay;
 * m_flGibVelocity / m_flVariance / m_flGibLife, the model, the material and
 * the gib render keys are read from the record when a gib is thrown. */
static res_t shooter_model(gid_t16 gid)
{
	return rec_class(gid) == CLS_ENV_SHOOTER ? rec_key_res(gid, K_SHOOTMODEL)
		: RESID(MON_HGIBS);
}

/* CEnvShooter::KeyValue "shootsounds" -> m_iGibMaterial */
static int shooter_material(gid_t16 gid)
{
	switch ((int)KeyF(gid, K_SHOOTSOUNDS, -1))
	{
	case 0: return matGlass;
	case 1: return matWood;
	case 2: return matMetal;
	case 3: return matFlesh;
	case 4: return matRocks;
	default: return matNone;
	}
}

static void CGibShooter_LogicSpawn(gid_t16 gid)
{
	rec_logic_t *l = rec_logic(gid);
	res_t model = shooter_model(gid);

	/* Precache */
	PRECACHE(model);
	if (rec_class(gid) == CLS_ENV_SHOOTER)
		MaterialSoundPrecache(shooter_material(gid));
	/* KeyValue m_iGibs; Spawn: m_flDelay 0 -> 0.1 (m_flGibLife 0 -> 25 at
	 * the throw) */
	l->a = l->b = (int32_t)KeyF(gid, K_M_IGIBS, 0);
	l->f = KeyF(gid, K_DELAY, 0);
	if (l->f == 0)
		l->f = 0.1f;
	l->flags = 0;
}

static void CGibShooter_LogicUse(gid_t16 gid, int activator, int caller, int use_type,
	float value)
{
	(void)activator;
	(void)caller;
	(void)use_type;
	(void)value;
	/* SetThink(ShootThink); pev->nextthink = gpGlobals->time */
	rec_logic(gid)->flags = 1;
	rec_timer(gid, gtime(), 0, -1);
}

/* CreateGib: hgibs body 1..body-1, red blood (gibshooter); the shooter's
 * model, render keys and material, no blood (env_shooter). */
static int CGibShooter_CreateGib(gid_t16 gid, res_t model)
{
	int body = res_model_frames(model), e;
	CGib *g;

	e = CGib_Create();
	if (e < 0)
		return -1;
	CGib_Spawn(e, model);
	g = GIB(e);
	if (rec_class(gid) == CLS_ENV_SHOOTER)
	{
		col_ent_t *c = ent_cv(e);
		ent_rnd_t *r = ent_rnd(e);
		float color[3] = { 0, 0, 0 };

		ent_rnd(e)->body = (uint8_t)(body > 1 ? RANDOM_LONG(0, body - 1) : 0);
		g->m_bloodColor = DONT_BLEED;
		g->m_material = shooter_material(gid);
		c->rendermode = (uint8_t)KeyF(gid, K_RENDERMODE, 0);
		r->renderamt = (uint8_t)KeyF(gid, K_RENDERAMT, 0);
		rec_key_v(gid, K_RENDERCOLOR, color);
		r->rendercolor[0] = (uint8_t)color[0];
		r->rendercolor[1] = (uint8_t)color[1];
		r->rendercolor[2] = (uint8_t)color[2];
		r->renderfx = (uint8_t)KeyF(gid, K_RENDERFX, 0);
		r->scale = KeyF(gid, K_SCALE, 0);
		c->skin = (int16_t)KeyF(gid, K_SKIN, 0);
	}
	else
	{
		g->m_bloodColor = BLOOD_COLOR_RED;
		if (body <= 1)
			log_printf("GibShooter Body is <= 1!\n");
		/* avoid throwing random amounts of the 0th gib (skull) */
		ent_rnd(e)->body = (uint8_t)RANDOM_LONG(1, body - 1);
	}
	return e;
}

/* ShootThink */
static void CGibShooter_LogicTimer(gid_t16 gid, uint16_t arg, int activator)
{
	rec_logic_t *l = rec_logic(gid);
	float ang[3], movedir[3], dir[3], org[3], v[3], save[3][3];
	float velocity = KeyF(gid, K_M_FLVELOCITY, 0), variance = KeyF(gid, K_M_FLVARIANCE, 0);
	float life = KeyF(gid, K_M_FLGIBLIFE, 0);
	int e;

	(void)arg;
	(void)activator;
	if (!l->flags)
		return;
	rec_timer(gid, gtime() + l->f, 0, -1);
	if (life == 0)
		life = 25;
	/* Spawn's SetMovedir of the record's angles (it leaves gpGlobals' vectors
	 * as they were: ShootThink reads whatever MakeVectors ran last) */
	rec_angles(gid, ang);
	if (ang[0] == 0 && ang[1] == -1 && ang[2] == 0)
		VecSet(movedir, 0, 0, 1);
	else if (ang[0] == 0 && ang[1] == -2 && ang[2] == 0)
		VecSet(movedir, 0, 0, -1);
	else
	{
		VecCopy(gpGlobals.v_forward, save[0]);
		VecCopy(gpGlobals.v_right, save[1]);
		VecCopy(gpGlobals.v_up, save[2]);
		UTIL_MakeVectors(ang);
		VecCopy(gpGlobals.v_forward, movedir);
		VecCopy(save[0], gpGlobals.v_forward);
		VecCopy(save[1], gpGlobals.v_right);
		VecCopy(save[2], gpGlobals.v_up);
	}
	VecMA(movedir, RANDOM_FLOAT(-1, 1) * variance, gpGlobals.v_right, dir);
	VecMA(dir, RANDOM_FLOAT(-1, 1) * variance, gpGlobals.v_forward, dir);
	VecMA(dir, RANDOM_FLOAT(-1, 1) * variance, gpGlobals.v_up, dir);
	VecNorm(dir, dir);
	e = CGibShooter_CreateGib(gid, shooter_model(gid));
	if (e >= 0)
	{
		CGib *g = GIB(e);
		float think = ent_tm(e)->nextthink - gtime();

		rec_origin(gid, org);
		ent_set_origin(e, org);
		VecScale(dir, velocity, v);
		ent_set_velocity(e, v);
		/* draws in source order (argument order is unspecified, K10) */
		v[0] = RANDOM_FLOAT(100, 200);
		v[1] = RANDOM_FLOAT(100, 300);
		v[2] = 0;
		ent_set_avelocity(e, v);
		g->m_lifeTime = life * RANDOM_FLOAT(0.95f, 1.05f);	/* +/- 5% */
		if (g->m_lifeTime < think)
		{
			ent_think_at(e, gtime() + g->m_lifeTime);
			g->m_lifeTime = 0;
		}
	}
	if (--l->a <= 0)
	{
		if (logic_spawnflags(gid) & SF_GIBSHOOTER_REPEATABLE)
		{
			l->a = l->b;
			l->flags = 0;	/* SetThink(NULL) */
			rec_timer_cancel(gid);
		}
		else
		{
			/* SetThink(SUB_Remove) */
			rec_timer_cancel(gid);
			rec_kill(gid);
		}
	}
}

const game_class_t gc_GIBSHOOTER = {
	.LogicSpawn = CGibShooter_LogicSpawn,
	.LogicUse = CGibShooter_LogicUse,
	.LogicTimer = CGibShooter_LogicTimer,
};

const game_class_t gc_ENV_SHOOTER = {
	.LogicSpawn = CGibShooter_LogicSpawn,
	.LogicUse = CGibShooter_LogicUse,
	.LogicTimer = CGibShooter_LogicTimer,
};

/* ---- beams: env_beam (CLightning), env_laser (CLaser) --------------------------- */

#define SF_BEAM_STARTON		0x0001
#define SF_BEAM_TOGGLE		0x0002
#define SF_BEAM_RANDOM		0x0004
#define SF_BEAM_RING		0x0008
#define SF_BEAM_SPARKSTART	0x0010
#define SF_BEAM_SPARKEND	0x0020
#define SF_BEAM_DECALS		0x0040
#define SF_BEAM_SHADEIN		0x0080
#define SF_BEAM_SHADEOUT	0x0100

/* CBeam types (HLSDK BEAM_*); BEAM_NONE: the ends did not resolve */
#define BEAM_POINTS		0
#define BEAM_ENTPOINT		1
#define BEAM_ENTS		2
#define BEAM_NONE		0xFF

/* CBeam: HLSDK keeps a beam's state in its entvars (rendermode type and
 * flags, origin / angles the points, sequence / skin the end entities,
 * modelindex, scale, body, frame, animtime); here the members, map-local
 * in the beam's frame. The beam itself is an effects beam keyed
 * FX_BEAM_KEY(e, 0) (unsaved: the restore hooks issue it again). */
#define CBEAM_FIELDS \
	float		m_vecStart[3];	/* pev->origin: start point */ \
	float		m_vecEnd[3];	/* pev->angles: end point */ \
	ent_h		m_hStart, m_hEnd; /* start / end entities (EH_NONE) */ \
	res_t		m_texture;	/* SetTexture */ \
	uint8_t		m_type;		/* BEAM_* */ \
	uint8_t		m_flags;	/* FX_BEAM_SHADE* (SetFlags) */ \
	uint8_t		m_width, m_noise, m_frame, m_scroll; \
	uint8_t		m_decalSet;	/* m_decalAt holds a decal [own] */ \
	float		m_decalAt[3];	/* the beam's last decal point [own] */

typedef struct {
	CBASEENTITY_FIELDS
	CBEAM_FIELDS
} CBeam;

#define BEAM(e)		PRIV(CBeam, e)

static const sv_field_t f_CBeam[] = {
	SVF(CBeam, m_vecStart, SV_POS, 0),
	SVF(CBeam, m_vecEnd, SV_POS, 0),
	SVF(CBeam, m_hStart, SV_EH, 0),
	SVF(CBeam, m_hEnd, SV_EH, 0),
	SVF(CBeam, m_texture, SV_RES, SVK),
	SVF(CBeam, m_type, SV_U8, 0),
	SVF(CBeam, m_flags, SV_U8, SVK),
	SVF(CBeam, m_width, SV_U8, SVK),
	SVF(CBeam, m_noise, SV_U8, SVK),
	SVF(CBeam, m_frame, SV_U8, SVK),
	SVF(CBeam, m_scroll, SV_U8, SVK),
	SVF(CBeam, m_decalSet, SV_U8, 0),
	SVF(CBeam, m_decalAt, SV_POS, 0),
};

/* GetStartPos / GetEndPos (they ignore attachments, as HLSDK's) */
static void BeamStartPos(int e, float out[3])
{
	CBeam *b = BEAM(e);
	int s = b->m_type == BEAM_ENTS ? EHGet(b->m_hStart) : -1;

	VecCopy(s >= 0 ? ent_cv(s)->origin : b->m_vecStart, out);
}

static void BeamEndPos(int e, float out[3])
{
	CBeam *b = BEAM(e);
	int t = b->m_type == BEAM_POINTS ? -1 : EHGet(b->m_hEnd);

	VecCopy(t >= 0 ? ent_cv(t)->origin : b->m_vecEnd, out);
}

/* The client's view of the beam entity: drawn unless EF_NODRAW, as long
 * as its entities exist. */
static void BeamShow(int e)
{
	CBeam *b = BEAM(e);
	const ent_rnd_t *r = ent_rnd(e);
	int s = -1, t = -1;

	if (b->m_type != BEAM_NONE && !(r->effects & EF_NODRAW))
	{
		if (b->m_type == BEAM_ENTS)
			s = EHGet(b->m_hStart);
		if (b->m_type != BEAM_POINTS)
			t = EHGet(b->m_hEnd);
		if ((b->m_type != BEAM_ENTS || s >= 0) && (b->m_type == BEAM_POINTS || t >= 0))
		{
			fx_beam(FX_BEAM_KEY(e, 0), s, s < 0 ? b->m_vecStart : NULL, t,
				t < 0 ? b->m_vecEnd : NULL, b->m_texture, b->m_frame,
				(int)r->framerate, b->m_width, b->m_noise, r->rendercolor[0],
				r->rendercolor[1], r->rendercolor[2], r->renderamt, b->m_scroll, 0,
				b->m_flags);
			return;
		}
	}
	fx_beam_kill(FX_BEAM_KEY(e, 0));
}

/* released: the effects beam goes with the entity */
static void CBeam_released(int e)
{
	fx_beam_kill(FX_BEAM_KEY(e, 0));
}

static const sv_table_t sv_CBeam = SV_TABLE(&sv_CBaseEntity, f_CBeam,
	.released = CBeam_released);

/* An entity a beam names, as HLSDK's searches see it: point records
 * (info_target, landmarks) and logic records are points at their record
 * origin (IsPointEntity: no model); physical records wake. *ent -1 for a
 * point. RandomTargetname (rnd 1): a uniform pick over every match (the
 * game random stream, one draw per match), else the first match
 * (FIND_ENTITY_BY_TARGETNAME). 0: no match. */
static int BeamFind(name_t n, int rnd, int *ent, float org[3])
{
	int i, cnt, total = 0;

	*ent = -1;
	if (n == NAME_NONE)
		return 0;
	cnt = rec_name_count(n);
	for (i = 0; i < cnt; i++)
	{
		gid_t16 g = rec_name_gid(n, i);
		int k = rec_kind(g), e = -1;

		if (k < 0)
			continue;
		if (k != REC_KIND_POINT && k != REC_KIND_LOGIC && (e = rec_wake(g)) < 0)
			continue;
		total++;
		if (rnd ? RANDOM_LONG(0, total - 1) < 1 : total == 1)
		{
			*ent = e;
			if (e >= 0)
				VecCopy(ent_cv(e)->origin, org);
			else
				rec_origin(g, org);
		}
		if (!rnd)
			break;
	}
	return total > 0;
}

/* HLSDK IsPointEntity: no model, or a target / landmark / corner */
static int IsPointEntity(int e)
{
	cls_t c;

	if (e < 0 || !ent_rnd(e)->model)
		return 1;
	c = ent_idc(e)->cls;
	return c == CLS_INFO_TARGET || c == CLS_INFO_LANDMARK || c == CLS_PATH_CORNER;
}

static void DoSparks(int e, const float start[3], const float end[3])
{
	uint32_t sf = CBase(e)->spawnflags;

	if (sf & SF_BEAM_SPARKSTART)
		UTIL_Sparks(start);
	if (sf & SF_BEAM_SPARKEND)
		UTIL_Sparks(end);
}

/* Beam decals closer than this to the beam's last one are skipped [own]:
 * a beam held on a surface (env_laser at 10 thinks a second, the welder
 * moving 25 u/s) lays a 16 u decal every 2.5 u; half a decal apart keeps
 * the trail unbroken with a third of the decals (pool and draw cost). */
#define BEAM_DECAL_SPACING	8.0f

/* CBeam::BeamDamage: dmg x the time since the last damage to what the
 * trace hit (the world takes none), decals on brushes with SF_BEAM_DECALS */
static void BeamDamage(int e, const col_trace_t *tr)
{
	CBaseEntity *pev = CBase(e);
	int hit = tr->ent;

	if (tr->frac != 1.0f && hit >= 0)
	{
		if (hit > 0 && ent_is_live(hit))
		{
			float start[3], dir[3];

			BeamStartPos(e, start);
			VecSub(tr->end, start, dir);
			VecNorm(dir, dir);
			ClearMultiDamage();
			TraceAttack(hit, e, pev->dmg * (gtime() - pev->dmgtime), dir, tr,
				DMG_ENERGYBEAM);
			ApplyMultiDamage(e, e);
		}
		if ((pev->spawnflags & SF_BEAM_DECALS) && (hit == 0 || IsBSPModel(hit)))
		{
			CBeam *b = BEAM(e);
			int d = DECAL_BIGSHOT1 + RANDOM_LONG(0, 4);	/* HLSDK's draw, always */
			float gap[3];

			VecSub(tr->end, b->m_decalAt, gap);
			if (!b->m_decalSet || VecLen(gap) >= BEAM_DECAL_SPACING)
			{
				UTIL_DecalTrace(tr, d);
				VecCopy(tr->end, b->m_decalAt);
				b->m_decalSet = 1;
			}
		}
	}
	pev->dmgtime = gtime();
}

typedef struct {
	CBASEENTITY_FIELDS
	CBEAM_FIELDS
	int32_t		m_active;
	name_t		m_iszStartEntity;
	name_t		m_iszEndEntity;
	float		m_life;
	int32_t		m_boltWidth;
	int32_t		m_noiseAmplitude;
	int32_t		m_speed;
	float		m_restrike;
	int32_t		m_frameStart;
	float		m_radius;
} CLightning;

#define LIGHTNING(e)	PRIV(CLightning, e)

static int ServerSide(int e)
{
	return LIGHTNING(e)->m_life == 0 && !(CBase(e)->spawnflags & SF_BEAM_RING);
}

/* BeamUpdateVars: the beam entity's type, points and entities from the
 * first entities named (HLSDK at Activate; here at spawn and restore) */
static void BeamUpdateVars(int e)
{
	CLightning *p = LIGHTNING(e);
	float so[3] = { 0, 0, 0 }, eo[3] = { 0, 0, 0 };
	int s, t, ps, pe, have;

	have = BeamFind(p->m_iszStartEntity, 0, &s, so);
	have &= BeamFind(p->m_iszEndEntity, 0, &t, eo);
	p->m_hStart = p->m_hEnd = EH_NONE;
	if (!have)
	{
		log_printf("env_beam %d: unknown start or end entity\n", e);
		p->m_type = BEAM_NONE;
		BeamShow(e);
		return;
	}
	ps = IsPointEntity(s);
	pe = IsPointEntity(t);
	p->m_type = BEAM_ENTS;
	if (ps || pe)
	{
		if (!ps)
		{
			/* one point entity must be the start */
			int ts = s, tp = ps;
			float to[3];

			s = t;
			t = ts;
			ps = pe;
			pe = tp;
			VecCopy(so, to);
			VecCopy(eo, so);
			VecCopy(to, eo);
		}
		p->m_type = pe ? BEAM_POINTS : BEAM_ENTPOINT;
	}
	if (p->m_type == BEAM_ENTS)
		p->m_hStart = EHANDLE(s);
	else
		VecCopy(so, p->m_vecStart);
	if (p->m_type == BEAM_ENTPOINT)
		p->m_hEnd = EHANDLE(t);
	else if (p->m_type == BEAM_POINTS)
		VecCopy(eo, p->m_vecEnd);
	else
		p->m_hEnd = EHANDLE(t);
	p->m_width = (uint8_t)p->m_boltWidth;
	p->m_noise = (uint8_t)p->m_noiseAmplitude;
	p->m_frame = (uint8_t)p->m_frameStart;
	p->m_scroll = (uint8_t)p->m_speed;
	p->m_flags = (p->spawnflags & SF_BEAM_SHADEIN) ? FX_BEAM_SHADEIN
		: (p->spawnflags & SF_BEAM_SHADEOUT) ? FX_BEAM_SHADEOUT : 0;
	BeamShow(e);
}

/* A TE_BEAM* strike (HLSDK StrikeThink / Zap message): life m_life */
static void Strike(int e, int s, const float a[3], int t, const float b[3])
{
	CLightning *p = LIGHTNING(e);
	const ent_rnd_t *r = ent_rnd(e);

	fx_beam(-1, s, a, t, b, p->m_texture, p->m_frameStart, (int)r->framerate,
		p->m_boltWidth, p->m_noiseAmplitude, r->rendercolor[0], r->rendercolor[1],
		r->rendercolor[2], r->renderamt, p->m_speed, (int)(p->m_life * 10.0f), 0);
}

static void Zap(int e, const float src[3], const float dest[3])
{
	Strike(e, -1, src, -1, dest);
	DoSparks(e, src, dest);
}

static void RandomDir(float d[3])
{
	d[0] = RANDOM_FLOAT(-1.0f, 1.0f);
	d[1] = RANDOM_FLOAT(-1.0f, 1.0f);
	d[2] = RANDOM_FLOAT(-1.0f, 1.0f);
}

static void RandomArea(int e)
{
	CLightning *p = LIGHTNING(e);
	const float *src = ent_cv(e)->origin;
	int i;

	for (i = 0; i < 10; i++)
	{
		float d1[3], d2[3], to[3], gap[3];
		col_trace_t tr1, tr2;

		RandomDir(d1);
		VecNorm(d1, d1);
		VecMA(src, p->m_radius, d1, to);
		UTIL_TraceLine(src, to, ignore_monsters, e, &tr1);
		if (tr1.frac == 1.0f)
			continue;
		do
			RandomDir(d2);
		while (VecDot(d1, d2) > 0);
		VecNorm(d2, d2);
		VecMA(src, p->m_radius, d2, to);
		UTIL_TraceLine(src, to, ignore_monsters, e, &tr2);
		if (tr2.frac == 1.0f)
			continue;
		VecSub(tr1.end, tr2.end, gap);
		if (VecLen(gap) < p->m_radius * 0.1f)
			continue;
		VecCopy(tr2.end, to);
		UTIL_TraceLine(tr1.end, to, ignore_monsters, e, &tr2);
		if (tr2.frac != 1.0f)
			continue;
		Zap(e, tr1.end, tr2.end);
		break;
	}
}

static void RandomPoint(int e, const float src[3])
{
	CLightning *p = LIGHTNING(e);
	int i;

	for (i = 0; i < 10; i++)
	{
		float d1[3], to[3], gap[3];
		col_trace_t tr1;

		RandomDir(d1);
		VecNorm(d1, d1);
		VecMA(src, p->m_radius, d1, to);
		UTIL_TraceLine(src, to, ignore_monsters, e, &tr1);
		VecSub(tr1.end, src, gap);
		if (VecLen(gap) < p->m_radius * 0.1f)
			continue;
		if (tr1.frac == 1.0f)
			continue;
		Zap(e, src, tr1.end);
		break;
	}
}

void CLightning_StrikeThink(int e)
{
	CLightning *p = LIGHTNING(e);
	float so[3], eo[3];
	int s, t;

	if (p->m_life != 0)
	{
		if (p->spawnflags & SF_BEAM_RANDOM)
			ent_think_at(e, gtime() + p->m_life + RANDOM_FLOAT(0, p->m_restrike));
		else
			ent_think_at(e, gtime() + p->m_life + p->m_restrike);
	}
	p->m_active = 1;
	if (p->m_iszEndEntity == NAME_NONE)
	{
		if (p->m_iszStartEntity == NAME_NONE)
			RandomArea(e);
		else if (BeamFind(p->m_iszStartEntity, 1, &s, so))
			RandomPoint(e, so);
		else
			log_printf("env_beam %d: unknown start entity\n", e);
		return;
	}
	if (!BeamFind(p->m_iszStartEntity, 1, &s, so) || !BeamFind(p->m_iszEndEntity, 1, &t, eo))
		return;
	if (IsPointEntity(s) || IsPointEntity(t))
	{
		if (p->spawnflags & SF_BEAM_RING)
			return;		/* "don't work" */
		if (!IsPointEntity(t))
		{
			/* one point entity must be in pEnd */
			int ts = s;
			float to[3];

			s = t;
			t = ts;
			VecCopy(so, to);
			VecCopy(eo, so);
			VecCopy(to, eo);
		}
		if (!IsPointEntity(s))
			Strike(e, s, NULL, -1, eo);		/* TE_BEAMENTPOINT */
		else
			Strike(e, -1, so, -1, eo);		/* TE_BEAMPOINTS */
	}
	else if (p->spawnflags & SF_BEAM_RING)
	{
		/* TE_BEAMRING: a ring whose diameter is the line between the
		 * two entities; drawn as the effects' ring at its centre, its
		 * radius half the line (the effects ring grows to it over the
		 * life) */
		const ent_rnd_t *r = ent_rnd(e);
		float mid[3], d[3];

		VecAdd(so, eo, mid);
		VecScale(mid, 0.5f, mid);
		VecSub(eo, so, d);
		fx_beam_cylinder(mid, 0.5f * VecLen(d), p->m_texture, (int)(p->m_life * 10.0f),
			p->m_boltWidth, p->m_noiseAmplitude, r->rendercolor[0], r->rendercolor[1],
			r->rendercolor[2], r->renderamt);
	}
	else
		Strike(e, s, NULL, t, NULL);			/* TE_BEAMENTS */
	DoSparks(e, so, eo);
	if (p->dmg > 0)
	{
		col_trace_t tr;

		UTIL_TraceLine(so, eo, dont_ignore_monsters, -1, &tr);
		/* BeamDamageInstant */
		p->dmgtime = gtime() - 1.0f;
		BeamDamage(e, &tr);
	}
}

void CLightning_DamageThink(int e)
{
	float a[3], b[3];
	col_trace_t tr;

	ThinkIn(e, 0.1f);
	BeamStartPos(e, a);
	BeamEndPos(e, b);
	UTIL_TraceLine(a, b, dont_ignore_monsters, -1, &tr);
	BeamDamage(e, &tr);
}

void CLightning_ToggleUse(int e, int activator, int caller, int use_type, float value)
{
	CLightning *p = LIGHTNING(e);
	ent_rnd_t *r = ent_rnd(e);

	(void)activator;
	(void)caller;
	(void)value;
	if (!ShouldToggle(use_type, p->m_active))
		return;
	if (p->m_active)
	{
		p->m_active = 0;
		r->effects |= EF_NODRAW;
		ent_think_at(e, 0);
	}
	else
	{
		float a[3], b[3];

		p->m_active = 1;
		r->effects &= (uint16_t)~EF_NODRAW;
		/* the end entities again by name: one released since Activate
		 * (out of its spawn radius) left a stale handle; HLSDK's entity
		 * pointers never go stale, and the first named entity is the
		 * same entity */
		BeamUpdateVars(e);
		BeamStartPos(e, a);
		BeamEndPos(e, b);
		DoSparks(e, a, b);
		if (p->dmg > 0)
		{
			ent_think_at(e, gtime());
			p->dmgtime = gtime();
		}
		return;
	}
	BeamShow(e);
}

void CLightning_StrikeUse(int e, int activator, int caller, int use_type, float value)
{
	CLightning *p = LIGHTNING(e);

	(void)activator;
	(void)caller;
	(void)value;
	if (!ShouldToggle(use_type, p->m_active))
		return;
	if (p->m_active)
	{
		p->m_active = 0;
		SetThink(e, 0);
	}
	else
	{
		SetThink(e, FN_CLightning_StrikeThink);
		ThinkIn(e, 0.1f);
	}
	if (!(p->spawnflags & SF_BEAM_TOGGLE))
		SetUse(e, 0);
}

static int CLightning_Spawn(int e, gid_t16 gid)
{
	CLightning *p = LIGHTNING(e);

	/* KeyValue */
	p->m_iszStartEntity = rec_key_name(gid, K_LIGHTNINGSTART);
	p->m_iszEndEntity = rec_key_name(gid, K_LIGHTNINGEND);
	p->m_life = KeyF(gid, K_LIFE, 0);
	p->m_boltWidth = (int32_t)KeyF(gid, K_BOLTWIDTH, 0);
	p->m_noiseAmplitude = (int32_t)KeyF(gid, K_NOISEAMPLITUDE, 0);
	p->m_speed = (int32_t)KeyF(gid, K_TEXTURESCROLL, 0);
	p->m_restrike = KeyF(gid, K_STRIKETIME, 0);
	p->m_texture = rec_key_res(gid, K_TEXTURE);	/* m_iszSpriteName, m_spriteTexture */
	p->m_frameStart = (int32_t)KeyF(gid, K_FRAMESTART, 0);
	p->m_radius = KeyF(gid, K_RADIUS_BEAM, 0);
	p->dmg = KeyF(gid, K_DAMAGE, 0);
	p->m_type = BEAM_NONE;
	p->m_hStart = p->m_hEnd = EH_NONE;
	if (!p->m_texture)
		return SpawnRefuse("env_beam: no texture (HLSDK CLightning::Spawn removes it)");
	ent_set_solid(e, SOLID_NOT);
	PRECACHE(p->m_texture);
	p->dmgtime = gtime();
	if (ServerSide(e))
	{
		SetThink(e, 0);
		if (p->dmg > 0)
		{
			SetThink(e, FN_CLightning_DamageThink);
			ThinkIn(e, 0.1f);
		}
		if (ent_idc(e)->targetname != NAME_NONE)
		{
			if (!(p->spawnflags & SF_BEAM_STARTON))
			{
				ent_rnd(e)->effects = EF_NODRAW;
				p->m_active = 0;
				ent_think_at(e, 0);
			}
			else
				p->m_active = 1;
			SetUse(e, FN_CLightning_ToggleUse);
		}
	}
	else
	{
		p->m_active = 0;
		if (ent_idc(e)->targetname != NAME_NONE)
			SetUse(e, FN_CLightning_StrikeUse);
		if (ent_idc(e)->targetname == NAME_NONE || (p->spawnflags & SF_BEAM_STARTON))
		{
			SetThink(e, FN_CLightning_StrikeThink);
			ThinkIn(e, 1.0f);
		}
	}
	return 0;
}

static void CLightning_Activate(int e)
{
	if (ServerSide(e))
		BeamUpdateVars(e);
}

/* restored (a rehydration: a fresh spawn's Activate saw the record, the
 * saved fields came back after it) and after a saved game's load: the
 * beam entity is drawn again as the restored fields say */
static void CLightning_restored(int e)
{
	if (ServerSide(e))
		BeamShow(e);
}

void CLightning_LoadDone(int e)
{
	CLightning_restored(e);
}

static const game_vt_t CLightning_vt = {
	.Activate = CLightning_Activate,
};

/* HLSDK CLightning::m_SaveData (m_spriteTexture, m_iszSpriteName: the
 * cooked texture in CBeam's m_texture; m_brightness: never set) */
static const sv_field_t f_CLightning[] = {
	SVF(CLightning, m_active, SV_I32, 0),
	SVF(CLightning, m_iszStartEntity, SV_NAME, SVK),
	SVF(CLightning, m_iszEndEntity, SV_NAME, SVK),
	SVF(CLightning, m_life, SV_F32, SVK),
	SVF(CLightning, m_boltWidth, SV_I32, SVK),
	SVF(CLightning, m_noiseAmplitude, SV_I32, SVK),
	SVF(CLightning, m_speed, SV_I32, SVK),
	SVF(CLightning, m_restrike, SV_F32, SVK),
	SVF(CLightning, m_frameStart, SV_I32, SVK),
	SVF(CLightning, m_radius, SV_F32, SVK),
};
static const sv_table_t sv_CLightning = SV_TABLE(&sv_CBeam, f_CLightning,
	.restored = CLightning_restored);

const game_class_t gc_ENV_BEAM = {
	.size = sizeof(CLightning), .Spawn = CLightning_Spawn, .vt = &CLightning_vt,
	.save = &sv_CLightning,
};

typedef struct {
	CBASEENTITY_FIELDS
	CBEAM_FIELDS
	ent_h		m_pSprite;	/* the end sprite (SF_SPRITE_TEMPORARY), EH_NONE */
	res_t		m_iszSpriteName;
	uint16_t	cl_pad;
	float		m_firePosition[3];
} CLaser;

#define LASER(e)	PRIV(CLaser, e)

static int CLaser_IsOn(int e)
{
	return !(ent_rnd(e)->effects & EF_NODRAW);
}

/* the end sprite: kRenderGlow in the laser's colour and brightness */
static void CLaser_MakeSprite(int e, const float org[3])
{
	CLaser *p = LASER(e);
	const ent_rnd_t *r = ent_rnd(e);
	int s;

	p->m_pSprite = EH_NONE;
	if (!p->m_iszSpriteName || (s = CSprite_SpriteCreate(p->m_iszSpriteName, org, 1)) < 0)
		return;
	/* SetTransparency(kRenderGlow, rendercolor, renderamt, renderfx) */
	ent_cv(s)->rendermode = kRenderGlow;
	memcpy(ent_rnd(s)->rendercolor, r->rendercolor, 3);
	ent_rnd(s)->renderamt = r->renderamt;
	ent_rnd(s)->renderfx = r->renderfx;
	p->m_pSprite = EHANDLE(s);
}

static void CLaser_TurnOff(int e)
{
	int s = EHGet(LASER(e)->m_pSprite);

	ent_rnd(e)->effects |= EF_NODRAW;
	ent_think_at(e, 0);
	if (s >= 0)
		CSprite_TurnOff(s);
	BeamShow(e);
}

static void CLaser_TurnOn(int e)
{
	int s = EHGet(LASER(e)->m_pSprite);

	ent_rnd(e)->effects &= (uint16_t)~EF_NODRAW;
	if (s >= 0)
		CSprite_TurnOn(s);
	CBase(e)->dmgtime = gtime();
	ent_think_at(e, gtime());
	BeamShow(e);
}

void CLaser_Use(int e, int activator, int caller, int use_type, float value)
{
	int active = CLaser_IsOn(e);

	(void)activator;
	(void)caller;
	(void)value;
	if (!ShouldToggle(use_type, active))
		return;
	if (active)
		CLaser_TurnOff(e);
	else
		CLaser_TurnOn(e);
}

static void CLaser_FireAtPoint(int e, const col_trace_t *tr)
{
	CLaser *p = LASER(e);
	int s = EHGet(p->m_pSprite);

	VecCopy(tr->end, p->m_vecEnd);		/* SetEndPos */
	if (s >= 0)
		UTIL_SetOrigin(s, tr->end);
	BeamShow(e);
	BeamDamage(e, tr);
	DoSparks(e, p->m_vecStart, tr->end);
}

void CLaser_StrikeThink(int e)
{
	CLaser *p = LASER(e);
	float org[3];
	col_trace_t tr;
	int t;

	if (BeamFind(p->message, 1, &t, org))
		VecCopy(org, p->m_firePosition);
	UTIL_TraceLine(ent_cv(e)->origin, p->m_firePosition, dont_ignore_monsters, -1, &tr);
	CLaser_FireAtPoint(e, &tr);
	ThinkIn(e, 0.1f);
}

static int CLaser_Spawn(int e, gid_t16 gid)
{
	CLaser *p = LASER(e);

	/* KeyValue: LaserTarget -> pev->message (a name), width, noise,
	 * scroll, texture -> pev->model, EndSprite, framestart, damage */
	p->message = rec_key_name(gid, K_LASERTARGET);
	p->m_width = (uint8_t)(int)KeyF(gid, K_WIDTH, 0);
	p->m_noise = (uint8_t)(int)KeyF(gid, K_NOISEAMPLITUDE, 0);
	p->m_scroll = (uint8_t)(int)KeyF(gid, K_TEXTURESCROLL, 0);
	p->m_texture = rec_key_res(gid, K_TEXTURE);
	p->m_iszSpriteName = rec_key_res(gid, K_ENDSPRITE);
	p->m_frame = (uint8_t)(int)KeyF(gid, K_FRAMESTART, 0);
	p->dmg = KeyF(gid, K_DAMAGE, 0);
	p->m_hStart = p->m_hEnd = EH_NONE;
	p->m_pSprite = EH_NONE;
	if (!p->m_texture)
		return SpawnRefuse("env_laser: no texture (HLSDK CLaser::Spawn removes it)");
	ent_set_solid(e, SOLID_NOT);
	PRECACHE(p->m_texture);
	PRECACHE(p->m_iszSpriteName);
	SetThink(e, FN_CLaser_StrikeThink);
	SetUse(e, FN_CLaser_Use);
	/* PointsInit(origin, origin) */
	p->m_type = BEAM_POINTS;
	VecCopy(ent_cv(e)->origin, p->m_vecStart);
	VecCopy(ent_cv(e)->origin, p->m_vecEnd);
	VecCopy(ent_cv(e)->origin, p->m_firePosition);
	CLaser_MakeSprite(e, ent_cv(e)->origin);
	if (ent_idc(e)->targetname != NAME_NONE && !(p->spawnflags & SF_BEAM_STARTON))
		CLaser_TurnOff(e);
	else
		CLaser_TurnOn(e);
	return 0;
}

/* released: the beam and the end sprite go with the laser */
static void CLaser_released(int e)
{
	int s = EHGet(LASER(e)->m_pSprite);

	if (s >= 0)
		UTIL_Remove(s);
	LASER(e)->m_pSprite = EH_NONE;
}

/* restored / after a saved game's load: the end sprite at the last end
 * point (in the laser's state), the beam as the fields say */
static void CLaser_restored(int e)
{
	CLaser *p = LASER(e);
	int s = EHGet(p->m_pSprite);

	if (s < 0)
		CLaser_MakeSprite(e, p->m_vecEnd);
	s = EHGet(p->m_pSprite);
	if (s >= 0 && !CLaser_IsOn(e))
		CSprite_TurnOff(s);
	BeamShow(e);
}

void CLaser_LoadDone(int e)
{
	CLaser_restored(e);
}

/* HLSDK CLaser::m_SaveData (m_pSprite: made again on restore) */
static const sv_field_t f_CLaser[] = {
	SVF(CLaser, m_iszSpriteName, SV_RES, SVK),
	SVF(CLaser, m_firePosition, SV_POS, 0),
};
static const sv_table_t sv_CLaser = SV_TABLE(&sv_CBeam, f_CLaser,
	.flags = SVT_MSG_NAME, .released = CLaser_released, .restored = CLaser_restored);

const game_class_t gc_ENV_LASER = {
	.size = sizeof(CLaser), .Spawn = CLaser_Spawn, .save = &sv_CLaser,
};

/* ---- env_bubbles (CBubbling) ---------------------------------------------------- */

#define SF_BUBBLES_STARTOFF	0x0001

typedef struct {
	CBASEENTITY_FIELDS
	int32_t		m_density;
	int32_t		m_frequency;
	int32_t		m_state;
} CBubbling;

void CBubbling_FizzThink(int e)
{
	CBubbling *p = PRIV(CBubbling, e);

	fx_fizz(e, RESID(SPR_BUBBLE), p->m_density);
	if (p->m_frequency > 19)
		ThinkIn(e, 0.5f);
	else
		ThinkIn(e, 2.5f - 0.1f * (float)p->m_frequency);
}

void CBubbling_Use(int e, int activator, int caller, int use_type, float value)
{
	CBubbling *p = PRIV(CBubbling, e);

	(void)activator;
	(void)caller;
	(void)value;
	if (ShouldToggle(use_type, p->m_state))
		p->m_state = !p->m_state;
	if (p->m_state)
	{
		SetThink(e, FN_CBubbling_FizzThink);
		ThinkIn(e, 0.1f);
	}
	else
	{
		SetThink(e, 0);
		ent_think_at(e, 0);
	}
}

static int CBubbling_Spawn(int e, gid_t16 gid)
{
	CBubbling *p = PRIV(CBubbling, e);
	ent_rnd_t *r = ent_rnd(e);
	int speed;

	/* KeyValue (atoi) */
	p->m_density = (int32_t)KeyF(gid, K_DENSITY, 0);
	p->m_frequency = (int32_t)KeyF(gid, K_FREQUENCY, 0);
	p->speed = (float)(int)KeyF(gid, K_CURRENT, 0);
	PRECACHE(RESID(SPR_BUBBLE));
	ent_set_model(e, p->model);		/* set size */
	ent_set_solid(e, SOLID_NOT);		/* remove model & collisions */
	r->renderamt = 0;			/* not drawn: blending at 0 */
	ent_cv(e)->rendermode = kRenderTransTexture;
	/* HACKHACK: the speed in rendercolor (fx_fizz reads it) */
	speed = p->speed > 0 ? (int)p->speed : -(int)p->speed;
	r->rendercolor[0] = (uint8_t)(speed >> 8);
	r->rendercolor[1] = (uint8_t)(speed & 255);
	r->rendercolor[2] = p->speed < 0 ? 1 : 0;
	SetUse(e, FN_CBubbling_Use);
	if (!(p->spawnflags & SF_BUBBLES_STARTOFF))
	{
		SetThink(e, FN_CBubbling_FizzThink);
		ThinkIn(e, 2.0f);
		p->m_state = 1;
	}
	else
		p->m_state = 0;
	return 0;
}

/* CBaseEntity::ObjectCaps() & ~FCAP_ACROSS_TRANSITION */
static const game_vt_t CBubbling_vt = {
	.ObjectCaps = NoTransition_ObjectCaps,
};

/* HLSDK CBubbling::m_SaveData */
static const sv_field_t f_CBubbling[] = {
	SVF(CBubbling, m_density, SV_I32, SVK),
	SVF(CBubbling, m_frequency, SV_I32, SVK),
	SVF(CBubbling, m_state, SV_I32, 0),
};
static const sv_table_t sv_CBubbling = SV_TABLE(&sv_CBaseEntity, f_CBubbling);

const game_class_t gc_ENV_BUBBLES = {
	.size = sizeof(CBubbling), .Spawn = CBubbling_Spawn, .vt = &CBubbling_vt,
	.save = &sv_CBubbling,
};

/* ---- player_loadsaved (player.cpp CRevertSaved) --------------------------------- */

/* A logic record: the fade (UTIL_ScreenFadeAll: rendercolor, duration,
 * holdtime, renderamt, FFADE_OUT) when used, the title (UTIL_ShowMessageAll
 * of message) messagetime later, the reload (SERVER_COMMAND "reload":
 * eng_reload, the newest saved game) loadtime after the use. Timer arg 1
 * MessageThink, 2 LoadThink. */
static void CRevertSaved_LogicUse(gid_t16 gid, int activator, int caller, int use_type,
	float value)
{
	float c[3] = { 0, 0, 0 };

	(void)activator;
	(void)caller;
	(void)use_type;
	(void)value;
	rec_key_v(gid, K_RENDERCOLOR, c);
	UTIL_ScreenFade((int)c[0], (int)c[1], (int)c[2], (int)KeyF(gid, K_RENDERAMT, 0),
		KeyF(gid, K_DURATION, 0), KeyF(gid, K_HOLDTIME, 0), FFADE_OUT);
	rec_timer(gid, gtime() + KeyF(gid, K_MESSAGETIME, 0), 1, -1);
}

static void CRevertSaved_LogicTimer(gid_t16 gid, uint16_t arg, int activator)
{
	float next;

	(void)activator;
	if (arg == 1)
	{
		ShowMessage(rec_key_title(gid, K_MESSAGE), gid);
		next = KeyF(gid, K_LOADTIME, 0) - KeyF(gid, K_MESSAGETIME, 0);
		if (next > 0)
		{
			rec_timer(gid, gtime() + next, 2, -1);
			return;
		}
	}
	/* LoadThink: single player */
	log_printf("player_loadsaved: map %u lump %u: reload\n", (unsigned)GID_SLOT(gid),
		(unsigned)GID_LUMP(gid));
	eng_reload();
}

const game_class_t gc_PLAYER_LOADSAVED = {
	.LogicUse = CRevertSaved_LogicUse,
	.LogicTimer = CRevertSaved_LogicTimer,
};
