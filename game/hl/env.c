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
 * effects API (include/hl64/fx.h): env_sprite (CSprite), env_explosion
 * (CEnvExplosion, also created at run time by ExplosionCreate),
 * spark_shower (CShower), env_shake, env_fade, env_message, env_render
 * (triggers.cpp CRenderFxManager), env_spark (buttons.cpp CEnvSpark),
 * env_beverage and item_sodacan. env_shake / fade / message / render /
 * spark / explosion map records are logic records (no entity): their
 * state is the record's rec_logic_t and their thinks record timers.
 * Retargets the old tree's hl_env.c 372-1166 (audit entry: HLSDK +
 * QUAKE-INTERFACE) and gsfx_sv.c env_sprite (HLSDK) by rewriting them
 * from effects.cpp / explode.cpp on the effects API; their own parts
 * (titles parser, render-tuple store, protocol writers) are replaced by
 * cooked titles (res_title) and fx.h. No line is imported.
 */
#include <math.h>

#include "game/hl/breakable.h"
#include "game/hl/doors.h"

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

static const game_vt_t CSprite_vt = {
	.ObjectCaps = NoTransition_ObjectCaps,
};

const game_class_t gc_ENV_SPRITE = {
	.size = sizeof(CSprite), .Spawn = CSprite_Spawn, .vt = &CSprite_vt,
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

const game_class_t gc_ENV_EXPLOSION = {
	.size = sizeof(CEnvExplosion), .Spawn = CEnvExplosion_Spawn,
	.LogicSpawn = CEnvExplosion_LogicSpawn,
	.LogicUse = CEnvExplosion_LogicUse,
	.LogicTimer = CEnvExplosion_LogicTimer,
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

const game_class_t gc_ENV_BEVERAGE = {
	.size = sizeof(CEnvBeverage), .Spawn = CEnvBeverage_Spawn,
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
