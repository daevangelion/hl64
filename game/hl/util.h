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
 * util.h -- HLSDK dlls/util.cpp, subs.cpp, cbase.cpp and combat.cpp helpers
 * on the hl64 engine API (architecture.md 7.1, 7.3 step 2): the game's id
 * enums (keys.def K_*, funcs.def FN_*, res.def R_*), the clock, vectors,
 * traces in the caller's map-local frame, searches, sounds, firing and
 * use-targets, CBaseToggle moves, masters, damage dispatch.
 *
 * Every position here is map-local in the frame of eng_frame_layer()
 * (architecture.md 8), as engine/coll takes and returns them.
 */
#ifndef HL64_GAME_UTIL_H
#define HL64_GAME_UTIL_H

#include <math.h>

#ifndef M_PI
#define M_PI		3.14159265358979323846
#endif
#include <stdint.h>

#include "engine/core/log.h"
#include "game/hl/cbase.h"
#include "hl64/fx.h"
#include "hl64/game.h"
#include "hl64/pmove.h"
#include "hl64/settings.h"
#include "hl64/snd.h"

/* ---- ids ------------------------------------------------------------------ */

/* keys.def: K_<IDENT> */
enum {
#define KEY(id, name)	K_##id,
#include "game/defs/keys.def"
#undef KEY
	K__COUNT
};

/* funcs.def: FN_<Ident> = line order + 1 (0 = none) */
enum {
	FN_NONE = 0,
#define THINK(f)	FN_##f,
#define TOUCH(f)	FN_##f,
#define USE(f)		FN_##f,
#define BLOCKED(f)	FN_##f,
#include "game/defs/funcs.def"
#undef THINK
#undef TOUCH
#undef USE
#undef BLOCKED
	FN__COUNT
};

/* Every funcs.def function, declared with its kind's signature. */
#define THINK(f)	void f(int e);
#define TOUCH(f)	void f(int e, int other);
#define USE(f)		void f(int e, int activator, int caller, int use_type, float value);
#define BLOCKED(f)	void f(int e, int other);
#include "game/defs/funcs.def"
#undef THINK
#undef TOUCH
#undef USE
#undef BLOCKED

/* res.def: R_<IDENT> = line order; RES(X) -> the cooked res_t */
enum {
#define RES(kind, id, name)	R_##id,
#include "game/defs/res.def"
#undef RES
	R__COUNT
};
#define RESID(id)		res_def(R_##id)

/* ---- clock, random ---------------------------------------------------------- */

/* gpGlobals->time: the game clock as HLSDK reads it (f32). */
static inline float	gtime(void) { return (float)eng_time(); }
static inline float	gframetime(void) { return (float)eng_frametime(); }
#define RANDOM_LONG(lo, hi)	hl64_rand_long((lo), (hi))
#define RANDOM_FLOAT(lo, hi)	hl64_rand_float((lo), (hi))

/* gpGlobals (the parts the game uses) */
typedef struct {
	float		v_forward[3], v_right[3], v_up[3];	/* UTIL_MakeVectors */
	float		found_secrets;
} game_globals_t;
extern game_globals_t	gpGlobals;
extern float		g_vecAttackDir[3];
extern const float	g_vecZero[3];

/* ---- vectors (HLSDK Vector, float arithmetic) --------------------------- */

static inline void VecCopy(const float a[3], float o[3]) { o[0] = a[0]; o[1] = a[1]; o[2] = a[2]; }
static inline void VecSet(float o[3], float x, float y, float z) { o[0] = x; o[1] = y; o[2] = z; }
static inline void VecAdd(const float a[3], const float b[3], float o[3])
{ o[0] = a[0] + b[0]; o[1] = a[1] + b[1]; o[2] = a[2] + b[2]; }
static inline void VecSub(const float a[3], const float b[3], float o[3])
{ o[0] = a[0] - b[0]; o[1] = a[1] - b[1]; o[2] = a[2] - b[2]; }
static inline void VecScale(const float a[3], float s, float o[3])
{ o[0] = a[0] * s; o[1] = a[1] * s; o[2] = a[2] * s; }
static inline void VecMA(const float a[3], float s, const float b[3], float o[3])
{ o[0] = a[0] + s * b[0]; o[1] = a[1] + s * b[1]; o[2] = a[2] + s * b[2]; }
static inline float VecDot(const float a[3], const float b[3])
{ return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static inline int VecEq(const float a[3], const float b[3])
{ return a[0] == b[0] && a[1] == b[1] && a[2] == b[2]; }
static inline int VecIsZero(const float a[3]) { return a[0] == 0 && a[1] == 0 && a[2] == 0; }
/* Vector::Length: sqrt of the float sum */
static inline float VecLen(const float a[3])
{ return (float)sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]); }
static inline float VecLen2D(const float a[3]) { return (float)sqrt(a[0] * a[0] + a[1] * a[1]); }
/* Vector::Normalize: (0,0,0) stays; returns the normalised copy in o */
static inline void VecNorm(const float a[3], float o[3])
{
	float l = VecLen(a);
	if (l == 0) { o[0] = o[1] = 0; o[2] = 0; return; }
	l = 1 / l;
	o[0] = a[0] * l; o[1] = a[1] * l; o[2] = a[2] * l;
}
static inline void VecCross(const float a[3], const float b[3], float o[3])
{
	o[0] = a[1] * b[2] - a[2] * b[1];
	o[1] = a[2] * b[0] - a[0] * b[2];
	o[2] = a[0] * b[1] - a[1] * b[0];
}

/* game/pm/pm_math.c (HLSDK) */
void		AngleVectors(const float *angles, float *forward, float *right, float *up);
void		VectorAngles(const float *forward, float *angles);

void		UTIL_MakeVectors(const float ang[3]);
void		UTIL_MakeAimVectors(const float ang[3]);
float		UTIL_VecToYaw(const float v[3]);
void		UTIL_VecToAngles(const float v[3], float out[3]);
float		UTIL_AngleMod(float a);
float		UTIL_AngleDiff(float dest, float src);
float		UTIL_Approach(float target, float value, float speed);
float		UTIL_ApproachAngle(float target, float value, float speed);
float		UTIL_AngleDistance(float next, float cur);
float		UTIL_WaterLevel(const float pos[3], float minz, float maxz);
void		UTIL_ClampVectorToBox(const float in[3], const float clamp[3], float out[3]);
/* QuakeEd angles -> movedir (subs.cpp SetMovedir); angles := 0 */
void		SetMovedir(int e);
/* VecBModelOrigin: centre of a brush model's box */
void		VecBModelOrigin(int e, float out[3]);
/* CBaseEntity::Center */
void		Center(int e, float out[3]);
int		Intersects(int a, int b);
int		IsInWorld(int e);

/* ---- entity helpers --------------------------------------------------------- */

static inline void	SetThink(int e, fn_t f) { ent_tm(e)->think = f; }
static inline void	SetTouch(int e, fn_t f) { ent_tm(e)->touch = f; }
static inline void	SetUse(int e, fn_t f) { ent_tm(e)->use = f; }
static inline void	SetBlocked(int e, fn_t f) { ent_tm(e)->blocked = f; }
/* pev->nextthink = gpGlobals->time + dt */
static inline void	ThinkIn(int e, float dt) { ent_think_at(e, gtime() + dt); }
/* pushers: pev->nextthink = pev->ltime + dt */
static inline void	ThinkInLtime(int e, float dt) { ent_think_at(e, ent_tm(e)->ltime + dt); }
static inline uint32_t	EFlags(int e) { return ent_cv(e)->flags; }
static inline void	EFlagsSet(int e, uint32_t bits) { ent_set_flags(e, ent_cv(e)->flags | bits); }
static inline void	EFlagsClear(int e, uint32_t bits) { ent_set_flags(e, ent_cv(e)->flags & ~bits); }
static inline CBaseEntity *CBase(int e) { return PRIV(CBaseEntity, e); }
static inline int	IsPlayer(int e) { return e == ENT_PLAYER; }

/* UTIL_Remove: FL_KILLME (the engine frees it), the record is gone for
 * good, the targetname cleared (HLSDK UpdateOnRemove). */
void		UTIL_Remove(int e);
/* CBaseEntity::SUB_Remove (think) and SUB_DoNothing: funcs.def */
/* CBaseEntity::Create: a fresh entity of class cls at org / ang (map-local
 * in the current frame), owner; -1 when refused or no slot. */
int		CBaseEntity_Create(cls_t cls, const float org[3], const float ang[3], int owner);
/* UTIL_SetSize / UTIL_SetOrigin */
static inline void	UTIL_SetOrigin(int e, const float o[3]) { ent_set_origin(e, o); }
static inline void	UTIL_SetSize(int e, const float mn[3], const float mx[3]) { ent_set_size(e, mn, mx); }
/* MakeDormant / IsDormant */
void		MakeDormant(int e);

/* ---- keys (KeyValue at spawn) -------------------------------------------- */

/* Number key, default when absent. */
float		KeyF(gid_t16 gid, int k, float def);
/* EntvarsKeyvalue part the engine does not own: spawnflags, target,
 * netname, health, speed, dmg, takedamage, model, render fields, skin,
 * body, scale, frame, framerate, sequence, gravity, friction, effects,
 * max_health. (game_spawn_record calls it before the class spawn.) */
void		EntvarsKeyvalue(int e, gid_t16 gid);
/* CBaseDelay::KeyValue (delay, killtarget) */
void		CBaseDelay_KeyValue(int e, gid_t16 gid);
/* CBaseToggle::KeyValue (lip, wait, master, distance + delay keys) */
void		CBaseToggle_KeyValue(int e, gid_t16 gid);

/* ---- firing ------------------------------------------------------------------ */

/* FireTargets(targetName, activator, caller, useType, value) */
void		FireTargets(name_t target, int activator, int caller, int use_type, float value);
/* CBaseEntity::SUB_UseTargets (target only) */
void		CBaseEntity_SUB_UseTargets(int e, int activator, int use_type, float value);
/* CBaseDelay::SUB_UseTargets: delay -> rec_fire_at (the DelayedUse
 * entity's work), else killtarget removal, then the targets. */
void		SUB_UseTargets(int e, int activator, int use_type, float value);
/* Remove every entity (and logic record) named n. */
void		KillTargets(name_t n);
/* ShouldToggle(useType, currentState) */
int		ShouldToggle(int use_type, int current);
/* UTIL_IsMasterTriggered */
int		UTIL_IsMasterTriggered(name_t master, int activator);
/* Use entity e (CBaseEntity::Use through its use function). */
void		UseEntity(int e, int activator, int caller, int use_type, float value);

/* ---- CBaseToggle moves (subs.cpp) ------------------------------------------- */

void		LinearMove(int e, const float dest[3], float speed);
void		AngularMove(int e, const float dest[3], float speed);
int		IsLockedByMaster(int e);
float		AxisValue(int flags, const float ang[3]);
void		AxisDir(int e);
float		AxisDelta(int flags, const float a1[3], const float a2[3]);

/* ---- traces (map-local; tr->ent: slot, 0 world, -1 none) -------------- */

#define ignore_monsters		1
#define dont_ignore_monsters	0
#define missile			2
#define ignore_glass		1
#define dont_ignore_glass	0
#define point_hull		0
#define human_hull		1
#define large_hull		2
#define head_hull		3

void		UTIL_TraceLine(const float start[3], const float end[3], int igmon, int ignore,
			col_trace_t *tr);
void		UTIL_TraceLineGlass(const float start[3], const float end[3], int igmon,
			int ignglass, int ignore, col_trace_t *tr);
void		UTIL_TraceHull(const float start[3], const float end[3], int igmon, int hull,
			int ignore, col_trace_t *tr);
/* TRACE_MONSTER_HULL: e's own box */
void		UTIL_TraceMonsterHull(int e, const float start[3], const float end[3], int igmon,
			int ignore, col_trace_t *tr);
void		UTIL_TraceModel(const float start[3], const float end[3], int hull, int model_e,
			col_trace_t *tr);
int		UTIL_PointContents(const float p[3]);
/* TRACE_TEXTURE: material character of the surface hit by a trace of
 * entity hit (0 world) along start -> end; 0 when none */
int		UTIL_TraceTexture(int hit, const float start[3], const float end[3]);
/* FVisible (entity eyes / point) */
int		FVisible(int e, int other);
int		FVisiblePoint(int e, const float p[3]);
void		EyePosition(int e, float out[3]);
void		BodyTarget(int e, const float src[3], float out[3]);

/* ---- searches -------------------------------------------------------------- */

static inline int	UTIL_FindEntityInSphere(int after, const float org[3], float r)
{ return ent_next_in_sphere(after, org, r); }
/* next live entity named n; *it starts 0 */
static inline int	UTIL_FindEntityByTargetname(name_t n, int *it)
{ return n == NAME_NONE ? -1 : rec_next_named(n, it); }
int		UTIL_FindByTargetname1(name_t n);	/* first, -1 none */
int		UTIL_EntitiesInBox(uint16_t *list, int max, const float mins[3],
			const float maxs[3], uint32_t flagmask);
int		UTIL_MonstersInSphere(uint16_t *list, int max, const float center[3], float r);
/* GetNextTarget: first live entity named by e's target */
int		GetNextTarget(int e);
/* point record (info_target / landmark / teleport destination) or entity
 * named n: its origin / angles; 0 when none */
int		UTIL_NamedOrigin(name_t n, float org[3], float ang[3]);

/* ---- sound ------------------------------------------------------------------ */

static inline void	EMIT_SOUND(int e, int chan, res_t s, float vol, float attn)
{ snd_emit(e, chan, s, vol, attn, 0, PITCH_NORM); }
static inline void	EMIT_SOUND_DYN(int e, int chan, res_t s, float vol, float attn, int flags,
				int pitch)
{ snd_emit(e, chan, s, vol, attn, flags, pitch); }
static inline void	STOP_SOUND(int e, int chan, res_t s) { (void)s; snd_stop(e, chan); }
/* UTIL_EmitAmbientSound: a SENTENCE res plays as a sentence on e */
void		UTIL_EmitAmbientSound(int e, const float org[3], res_t s, float vol, float attn,
			int flags, int pitch);
/* SENTENCEG_PlayRndSz / EMIT_SOUND of a SENTENCE or GROUP res */
void		EMIT_SENTENCE(int e, int chan, res_t s, float vol, float attn, int flags, int pitch);
void		PRECACHE(res_t r);

/* ---- damage (cbase.cpp / combat.cpp) ------------------------------------------ */

int		ObjectCaps(int e);
int		Classify(int e);
int		BloodColor(int e);
int		TakeDamage(int e, int inflictor, int attacker, float damage, int bits);
int		TakeHealth(int e, float health, int bits);
void		Killed(int e, int attacker, int gib);
void		TraceAttack(int e, int attacker, float damage, const float dir[3],
			const col_trace_t *tr, int bits);
int		IsTriggered(int e, int activator);
int		GetToggleState(int e);
float		GetDelay(int e);
int		IsMoving(int e);
int		IsAlive(int e);
int		IsBSPModel(int e);
int		DamageDecal(int e, int bits);
/* defaults (callable from overrides as the base class) */
int		CBaseEntity_TakeDamage(int e, int inflictor, int attacker, float damage, int bits);
int		CBaseEntity_TakeHealth(int e, float health, int bits);
void		CBaseEntity_Killed(int e, int attacker, int gib);
void		CBaseEntity_TraceAttack(int e, int attacker, float damage, const float dir[3],
			const col_trace_t *tr, int bits);
int		CBaseEntity_DamageDecal(int e, int bits);
/* CBaseEntity::TraceBleed; combat.cpp SpawnBlood */
void		TraceBleed(int e, float damage, const float dir[3], const col_trace_t *tr, int bits);
void		SpawnBlood(const float org[3], int color, float damage);
/* multi-damage (combat.cpp) */
void		ClearMultiDamage(void);
void		ApplyMultiDamage(int inflictor, int attacker);
void		AddMultiDamage(int inflictor, int e, float damage, int bits);
/* ::RadiusDamage(vecSrc, inflictor, attacker, damage, radius, iClassIgnore,
 * bitsDamageType) */
void		RadiusDamage(const float src[3], int inflictor, int attacker, float damage,
			float radius, int class_ignore, int bits);

/* ---- global states (cbase.cpp CGlobalState) ------------------------------- */

#define GLOBAL_OFF		0
#define GLOBAL_ON		1
#define GLOBAL_DEAD		2
/* ids: rec_key_global (-1 none). EntityGetState: GLOBAL_OFF when the id is
 * not in the table; EntitySetState of an absent id does nothing;
 * EntityAdd adds or overwrites. */
int		GlobalState(int id);
int		GlobalInTable(int id);
void		GlobalSetState(int id, int state);
void		GlobalAdd(int id, int state);
void		GlobalReset(void);

/* ---- decals (HLSDK decals.h order; FX owns the cooked sheet) --------------- */

enum {
	DECAL_GUNSHOT1, DECAL_GUNSHOT2, DECAL_GUNSHOT3, DECAL_GUNSHOT4, DECAL_GUNSHOT5,
	DECAL_LAMBDA1, DECAL_LAMBDA2, DECAL_LAMBDA3, DECAL_LAMBDA4, DECAL_LAMBDA5, DECAL_LAMBDA6,
	DECAL_SCORCH1, DECAL_SCORCH2,
	DECAL_BLOOD1, DECAL_BLOOD2, DECAL_BLOOD3, DECAL_BLOOD4, DECAL_BLOOD5, DECAL_BLOOD6,
	DECAL_YBLOOD1, DECAL_YBLOOD2, DECAL_YBLOOD3, DECAL_YBLOOD4, DECAL_YBLOOD5, DECAL_YBLOOD6,
	DECAL_GLASSBREAK1, DECAL_GLASSBREAK2, DECAL_GLASSBREAK3,
	DECAL_BIGSHOT1, DECAL_BIGSHOT2, DECAL_BIGSHOT3, DECAL_BIGSHOT4, DECAL_BIGSHOT5,
	DECAL_SPIT1, DECAL_SPIT2,
	DECAL_BPROOF1,
	DECAL_GARGSTOMP1,
	DECAL_SMALLSCORCH1, DECAL_SMALLSCORCH2, DECAL_SMALLSCORCH3,
	DECAL_MOMMABIRTH, DECAL_MOMMASPLAT
};
void		UTIL_DecalTrace(const col_trace_t *tr, int decal);
void		UTIL_BloodDecalTrace(const col_trace_t *tr, int color);
int		UTIL_ShouldShowBlood(int color);
void		UTIL_BloodDrips(const float org[3], const float dir[3], int color, int amount);
void		UTIL_BloodStream(const float org[3], const float dir[3], int color, int amount);
void		UTIL_Sparks(const float org[3]);
void		UTIL_Ricochet(const float org[3], float scale);
void		UTIL_ScreenShake(const float center[3], float amplitude, float frequency,
			float duration, float radius);
void		UTIL_ScreenFade(int r, int g, int b, int alpha, float fade_time, float hold, int flags);

/* ---- game-side lifetime hooks (dispatch.c) ------------------------------- */

/* Run think-kind function f on e (m_pfnCallWhenMoveDone and the like). */
void		game_call_think(int e, fn_t f);

#endif
