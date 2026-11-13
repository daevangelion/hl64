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
 * breakable.c -- HLSDK dlls/func_break.cpp: func_breakable (CBreakable)
 * and func_pushable (CPushable), the material sound lists
 * (MaterialSoundPrecache / MaterialSoundRandom, also used by gibs and
 * env_shooter), and shards through fx_break_model. Retargets the old
 * tree's hl_breakable.c (audit entry: HLSDK + QUAKE-INTERFACE) by
 * rewriting it from func_break.cpp; no line is imported. CPushable::Move
 * uses the HLSDK sv_pushable_fixed_tick_fudge default (15).
 */
#include <math.h>
#include <stdlib.h>

#include "engine/phys/player.h"
#include "game/hl/breakable.h"

#define SF_BREAK_TRIGGER_ONLY		1
#define SF_BREAK_TOUCH			2
#define SF_BREAK_PRESSURE		4
#define SF_BREAK_CROWBAR		256
#define SF_PUSH_BREAKABLE		128

#define BREAK_GLASS			0x01
#define BREAK_METAL			0x02
#define BREAK_FLESH			0x04
#define BREAK_WOOD			0x08
#define BREAK_CONCRETE			0x40

#define PUSHABLE_FIXED_TICK_FUDGE	15.0f

/* ---- material sounds ------------------------------------------------------ */

static int MaterialSoundList(int material, res_t out[6])
{
	switch (material)
	{
	case matWood:
		out[0] = RESID(DEBRIS_WOOD1); out[1] = RESID(DEBRIS_WOOD2);
		out[2] = RESID(DEBRIS_WOOD3);
		return 3;
	case matFlesh:
		out[0] = RESID(DEBRIS_FLESH1); out[1] = RESID(DEBRIS_FLESH2);
		out[2] = RESID(DEBRIS_FLESH3); out[3] = RESID(DEBRIS_FLESH5);
		out[4] = RESID(DEBRIS_FLESH6); out[5] = RESID(DEBRIS_FLESH7);
		return 6;
	case matComputer:
	case matUnbreakableGlass:
	case matGlass:
		out[0] = RESID(DEBRIS_GLASS1); out[1] = RESID(DEBRIS_GLASS2);
		out[2] = RESID(DEBRIS_GLASS3);
		return 3;
	case matMetal:
		out[0] = RESID(DEBRIS_METAL1); out[1] = RESID(DEBRIS_METAL2);
		out[2] = RESID(DEBRIS_METAL3);
		return 3;
	case matCinderBlock:
	case matRocks:
		out[0] = RESID(DEBRIS_CONCRETE1); out[1] = RESID(DEBRIS_CONCRETE2);
		out[2] = RESID(DEBRIS_CONCRETE3);
		return 3;
	default:
		return 0;
	}
}

void MaterialSoundPrecache(int material)
{
	res_t s[6];
	int i, n = MaterialSoundList(material, s);

	for (i = 0; i < n; i++)
		PRECACHE(s[i]);
}

void MaterialSoundRandom(int e, int material, float volume)
{
	res_t s[6];
	int n = MaterialSoundList(material, s);

	if (n)
		EMIT_SOUND(e, CHAN_BODY, s[RANDOM_LONG(0, n - 1)], volume, 1.0f);
}

/* ---- func_breakable ------------------------------------------------------- */

/* pSpawnObjects (func_break.cpp), as classes.def ids; CLS_NONE for the
 * classes the game does not have */
cls_t Breakable_SpawnObject(int i)
{
	static const cls_t objs[] = {
		CLS_NONE, CLS_ITEM_BATTERY, CLS_ITEM_HEALTHKIT, CLS_WEAPON_9MMHANDGUN,
		CLS_AMMO_9MMCLIP, CLS_WEAPON_9MMAR, CLS_AMMO_9MMAR, CLS_AMMO_ARGRENADES,
		CLS_WEAPON_SHOTGUN, CLS_AMMO_BUCKSHOT, CLS_NONE /* weapon_crossbow */,
		CLS_NONE /* ammo_crossbow */, CLS_WEAPON_357, CLS_AMMO_357,
		CLS_NONE /* weapon_rpg */, CLS_NONE /* ammo_rpgclip */,
		CLS_NONE /* ammo_gaussclip */, CLS_WEAPON_HANDGRENADE,
		CLS_NONE /* weapon_tripmine */, CLS_NONE /* weapon_satchel */,
		CLS_NONE /* weapon_snark */, CLS_NONE /* weapon_hornetgun */
	};

	if (i <= 0 || i >= (int)(sizeof objs / sizeof objs[0]))
		return CLS_NONE;
	return objs[i];
}

static cls_t spawn_object(int i)
{
	cls_t c = Breakable_SpawnObject(i);

	if (i > 0 && c == CLS_NONE)
		log_printf("game: func_breakable spawnobject %d: class not in the game\n", i);
	return c;
}

static int IsBreakable(int e)
{
	return PRIV(CBreakable, e)->m_Material != matUnbreakableGlass;
}

static void CBreakable_KeyValue(int e, gid_t16 gid)
{
	CBreakable *p = PRIV(CBreakable, e);
	const char *s;

	CBaseDelay_KeyValue(e, gid);
	p->m_iszSpawnObject = CLS_NONE;
	if (gid == GID_NONE)
		return;
	s = rec_key_str(gid, K_EXPLOSION);
	p->m_Explosion = s && (s[0] == 'd' || s[0] == 'D') ? expDirected : expRandom;
	p->m_Material = Breakable_Material(gid);
	p->m_iszGibModel = rec_key_res(gid, K_GIBMODEL);
	p->m_iszSpawnObject = spawn_object((int)KeyF(gid, K_SPAWNOBJECT, 0));
	p->m_iExplodeMagnitude = (int)KeyF(gid, K_EXPLODEMAGNITUDE, 0);
}

res_t Breakable_Precache(int material, res_t gibmodel)
{
	res_t gib = RES_NONE;

	switch (material)
	{
	case matWood:
		gib = RESID(MDL_WOODGIBS);
		PRECACHE(RESID(DEBRIS_BUSTCRATE1));
		PRECACHE(RESID(DEBRIS_BUSTCRATE2));
		break;
	case matFlesh:
		gib = RESID(MDL_FLESHGIBS);
		PRECACHE(RESID(DEBRIS_BUSTFLESH1));
		PRECACHE(RESID(DEBRIS_BUSTFLESH2));
		break;
	case matComputer:
		PRECACHE(RESID(SPARK5));
		PRECACHE(RESID(SPARK6));
		gib = RESID(MDL_COMPUTERGIBS);
		PRECACHE(RESID(DEBRIS_BUSTMETAL1));
		PRECACHE(RESID(DEBRIS_BUSTMETAL2));
		break;
	case matUnbreakableGlass:
	case matGlass:
		gib = RESID(MDL_GLASSGIBS);
		PRECACHE(RESID(DEBRIS_BUSTGLASS1));
		PRECACHE(RESID(DEBRIS_BUSTGLASS2));
		break;
	case matMetal:
		gib = RESID(MDL_METALPLATEGIBS);
		PRECACHE(RESID(DEBRIS_BUSTMETAL1));
		PRECACHE(RESID(DEBRIS_BUSTMETAL2));
		break;
	case matCinderBlock:
		gib = RESID(MDL_CINDERGIBS);
		PRECACHE(RESID(DEBRIS_BUSTCONCRETE1));
		PRECACHE(RESID(DEBRIS_BUSTCONCRETE2));
		break;
	case matRocks:
		gib = RESID(MDL_ROCKGIBS);
		PRECACHE(RESID(DEBRIS_BUSTCONCRETE1));
		PRECACHE(RESID(DEBRIS_BUSTCONCRETE2));
		break;
	case matCeilingTile:
		gib = RESID(MDL_CEILINGGIBS);
		PRECACHE(RESID(DEBRIS_BUSTCEILING));
		break;
	}
	MaterialSoundPrecache(material);
	if (gibmodel)
		gib = gibmodel;
	PRECACHE(gib);
	return gib;
}

static void CBreakable_Precache(int e)
{
	CBreakable *p = PRIV(CBreakable, e);

	p->m_idShard = Breakable_Precache(p->m_Material, p->m_iszGibModel);
}

int Breakable_Material(gid_t16 gid)
{
	int i = (int)KeyF(gid, K_MATERIAL, 0);

	return (i < 0 || i >= matLastMaterial) ? matWood : i;
}

static void DamageSound(int e)
{
	CBreakable *p = PRIV(CBreakable, e);
	int pitch, n = 0, material = p->m_Material;
	float vol;
	res_t s[6];

	if (RANDOM_LONG(0, 2))
		pitch = PITCH_NORM;
	else
		pitch = 95 + RANDOM_LONG(0, 34);
	vol = RANDOM_FLOAT(0.75f, 1.0f);
	if (material == matComputer && RANDOM_LONG(0, 1))
		material = matMetal;
	switch (material)
	{
	case matComputer:
	case matGlass:
	case matUnbreakableGlass:
		s[0] = RESID(DEBRIS_GLASS1); s[1] = RESID(DEBRIS_GLASS2); s[2] = RESID(DEBRIS_GLASS3);
		n = 3;
		break;
	case matWood:
		s[0] = RESID(DEBRIS_WOOD1); s[1] = RESID(DEBRIS_WOOD2); s[2] = RESID(DEBRIS_WOOD3);
		n = 3;
		break;
	case matMetal:
		s[0] = RESID(DEBRIS_METAL1); s[1] = RESID(DEBRIS_METAL3); s[2] = RESID(DEBRIS_METAL2);
		n = 2;
		break;
	case matFlesh:
		MaterialSoundList(matFlesh, s);
		n = 6;
		break;
	case matRocks:
	case matCinderBlock:
		MaterialSoundList(matRocks, s);
		n = 3;
		break;
	default:
		break;
	}
	if (n)
		EMIT_SOUND_DYN(e, CHAN_VOICE, s[RANDOM_LONG(0, n - 1)], vol, ATTN_NORM, 0, pitch);
}

void CBreakable_Die(int e)
{
	CBreakable *p = PRIV(CBreakable, e);
	const col_ent_t *c = ent_cv(e);
	float vol, vel[3], spot[3], size[3], mn[3], mx[3], ctr[3];
	uint16_t list[256];
	int pitch, flag = 0, i, n;
	res_t s = RES_NONE;

	pitch = 95 + RANDOM_LONG(0, 29);
	if (pitch > 97 && pitch < 103)
		pitch = 100;
	vol = (float)(RANDOM_FLOAT(0.85f, 1.0f) + (abs((int)p->health) / 100.0));
	if (vol > 1.0f)
		vol = 1.0f;
	switch (p->m_Material)
	{
	case matGlass:
		s = RANDOM_LONG(0, 1) ? RESID(DEBRIS_BUSTGLASS2) : RESID(DEBRIS_BUSTGLASS1);
		flag = BREAK_GLASS;
		break;
	case matWood:
		s = RANDOM_LONG(0, 1) ? RESID(DEBRIS_BUSTCRATE2) : RESID(DEBRIS_BUSTCRATE1);
		flag = BREAK_WOOD;
		break;
	case matComputer:
	case matMetal:
		s = RANDOM_LONG(0, 1) ? RESID(DEBRIS_BUSTMETAL2) : RESID(DEBRIS_BUSTMETAL1);
		flag = BREAK_METAL;
		break;
	case matFlesh:
		s = RANDOM_LONG(0, 1) ? RESID(DEBRIS_BUSTFLESH2) : RESID(DEBRIS_BUSTFLESH1);
		flag = BREAK_FLESH;
		break;
	case matRocks:
	case matCinderBlock:
		s = RANDOM_LONG(0, 1) ? RESID(DEBRIS_BUSTCONCRETE2) : RESID(DEBRIS_BUSTCONCRETE1);
		flag = BREAK_CONCRETE;
		break;
	case matCeilingTile:
		s = RESID(DEBRIS_BUSTCEILING);
		break;
	}
	if (s)
		EMIT_SOUND_DYN(e, CHAN_VOICE, s, vol, ATTN_NORM, 0, pitch);
	if (p->m_Explosion == expDirected)
		VecScale(g_vecAttackDir, 200, vel);
	else
		VecSet(vel, 0, 0, 0);
	for (i = 0; i < 3; i++)
	{
		spot[i] = c->origin[i] + (c->mins[i] + c->maxs[i]) * 0.5f;
		size[i] = c->maxs[i] - c->mins[i];
	}
	fx_break_model(spot, size, vel, 10, p->m_idShard, 0, 25, flag);
	/* drop what stands on it */
	VecCopy(c->absmin, mn);
	VecCopy(c->absmax, mx);
	mn[2] = c->absmax[2];
	mx[2] += 8;
	n = UTIL_EntitiesInBox(list, 256, mn, mx, FL_ONGROUND);
	for (i = 0; i < n; i++)
	{
		EFlagsClear(list[i], FL_ONGROUND);
		ent_set_ground(list[i], ENT_NONE);
	}
	ent_idc(e)->targetname = NAME_NONE;
	ent_set_solid(e, SOLID_NOT);
	SUB_UseTargets(e, -1, USE_TOGGLE, 0);
	SetThink(e, FN_SUB_Remove);
	ThinkInLtime(e, 0.1f);
	if (p->m_iszSpawnObject != CLS_NONE)
	{
		VecBModelOrigin(e, ctr);
		CBaseEntity_Create(p->m_iszSpawnObject, ctr, c->angles, e);
	}
	if (p->m_iExplodeMagnitude > 0)
	{
		Center(e, ctr);
		ExplosionCreate(ctr, c->angles, e, p->m_iExplodeMagnitude, 1);
	}
}

void CBreakable_BreakTouch(int e, int other)
{
	CBreakable *p = PRIV(CBreakable, e);
	float dmg;

	if (!IsPlayer(other) || !IsBreakable(e))
		return;
	if (p->spawnflags & SF_BREAK_TOUCH)
	{
		dmg = VecLen(phy_player()->velocity) * 0.01f;
		if (dmg >= p->health)
		{
			SetTouch(e, FN_NONE);
			TakeDamage(e, other, other, dmg, DMG_CRUSH);
			TakeDamage(other, e, e, dmg / 4, DMG_SLASH);
		}
	}
	if ((p->spawnflags & SF_BREAK_PRESSURE) &&
	    ent_cv(other)->absmin[2] >= ent_cv(e)->maxs[2] - 2)
	{
		DamageSound(e);
		SetThink(e, FN_CBreakable_Die);
		SetTouch(e, FN_NONE);
		if (p->m_flDelay == 0)
			p->m_flDelay = 0.1f;
		ThinkInLtime(e, p->m_flDelay);
	}
}

void CBreakable_Use(int e, int activator, int caller, int use_type, float value)
{
	CBreakable *p = PRIV(CBreakable, e);

	(void)activator;
	(void)caller;
	(void)use_type;
	(void)value;
	if (IsBreakable(e))
	{
		ent_cv(e)->angles[1] = p->m_angle;
		UTIL_MakeVectors(ent_cv(e)->angles);
		VecCopy(gpGlobals.v_forward, g_vecAttackDir);
		CBreakable_Die(e);
	}
}

static void CBreakable_TraceAttack(int e, int attacker, float damage, const float dir[3],
	const col_trace_t *tr, int bits)
{
	CBreakable *p = PRIV(CBreakable, e);

	if (RANDOM_LONG(0, 1))
	{
		switch (p->m_Material)
		{
		case matComputer:
			UTIL_Sparks(tr->end);
			EMIT_SOUND(e, CHAN_VOICE, RANDOM_LONG(0, 1) ? RESID(SPARK6) : RESID(SPARK5),
				RANDOM_FLOAT(0.7f, 1.0f), ATTN_NORM);
			break;
		case matUnbreakableGlass:
			UTIL_Ricochet(tr->end, RANDOM_FLOAT(0.5f, 1.5f));
			break;
		}
	}
	CBaseEntity_TraceAttack(e, attacker, damage, dir, tr, bits);
}

int CBreakable_TakeDamage(int e, int inflictor, int attacker, float damage, int bits)
{
	CBreakable *p = PRIV(CBreakable, e);
	const col_ent_t *c = ent_cv(e);
	float tmp[3];
	int i;

	for (i = 0; i < 3; i++)
		tmp[i] = (inflictor >= 0 ? ent_cv(inflictor)->origin[i] : 0) -
			(c->absmin[i] + (c->absmax[i] - c->absmin[i]) * 0.5f);
	if (attacker == inflictor && attacker >= 0 &&
	    (IsPlayer(attacker) || (ent_cv(attacker)->flags & FL_CLIENT)) &&
	    (p->spawnflags & SF_BREAK_CROWBAR) && (bits & DMG_CLUB))
		damage = p->health;
	if (!IsBreakable(e))
		return 0;
	if (bits & DMG_CLUB)
		damage *= 2;
	if (bits & DMG_POISON)
		damage *= 0.1f;
	VecNorm(tmp, g_vecAttackDir);
	p->health -= damage;
	if (p->health <= 0)
	{
		Killed(e, attacker, GIB_NORMAL);	/* CBaseEntity::Killed: UTIL_Remove */
		CBreakable_Die(e);
		return 0;
	}
	DamageSound(e);
	return 1;
}

static int CBreakable_DamageDecal(int e, int bits)
{
	CBreakable *p = PRIV(CBreakable, e);

	if (p->m_Material == matGlass)
		return DECAL_GLASSBREAK1 + RANDOM_LONG(0, 2);
	if (p->m_Material == matUnbreakableGlass)
		return DECAL_BPROOF1;
	return CBaseEntity_DamageDecal(e, bits);
}

static int CBreakable_ObjectCaps(int e)
{
	(void)e;
	return 0;
}

static const game_vt_t CBreakable_vt = {
	.ObjectCaps = CBreakable_ObjectCaps,
	.TraceAttack = CBreakable_TraceAttack,
	.TakeDamage = CBreakable_TakeDamage,
	.DamageDecal = CBreakable_DamageDecal,
};

static int CBreakable_Spawn(int e, gid_t16 gid)
{
	CBreakable *p = PRIV(CBreakable, e);

	CBreakable_KeyValue(e, gid);
	CBreakable_Precache(e);
	p->takedamage = (p->spawnflags & SF_BREAK_TRIGGER_ONLY) ? DAMAGE_NO : DAMAGE_YES;
	ent_set_movetype(e, MOVETYPE_PUSH);
	ent_set_solid(e, SOLID_BSP);
	p->m_angle = ent_cv(e)->angles[1];
	ent_cv(e)->angles[1] = 0;
	ent_set_model(e, p->model);
	SetTouch(e, (p->spawnflags & SF_BREAK_TRIGGER_ONLY) ? FN_NONE : FN_CBreakable_BreakTouch);
	SetUse(e, FN_CBreakable_Use);
	if (!IsBreakable(e) && ent_cv(e)->rendermode != kRenderNormal)
		EFlagsSet(e, FL_WORLDBRUSH);
	return 0;
}

/* HLSDK CBreakable::m_SaveData */
static const sv_field_t f_CBreakable[] = {
	SVF(CBreakable, m_Material, SV_I32, SVK),
	SVF(CBreakable, m_Explosion, SV_I32, SVK),
	SVF(CBreakable, m_idShard, SV_RES, SVK),
	SVF(CBreakable, m_iszGibModel, SV_RES, SVK),
	SVF(CBreakable, m_angle, SV_F32, SVK),
	SVF(CBreakable, m_iszSpawnObject, SV_U16, SVK),
	SVF(CBreakable, m_iExplodeMagnitude, SV_I32, SVK),
};
static const sv_table_t sv_CBreakable = SV_TABLE(&sv_CBaseDelay, f_CBreakable);

const game_class_t gc_FUNC_BREAKABLE = {
	.size = sizeof(CBreakable), .Spawn = CBreakable_Spawn, .vt = &CBreakable_vt,
	.save = &sv_CBreakable,
};

/* ---- func_pushable ------------------------------------------------------ */

typedef struct {
	CBREAKABLE_FIELDS
	int32_t		m_lastSound;
	float		m_maxSpeed;
	float		m_soundTime;
} CPushable;

static res_t push_sound(int i)
{
	return i == 2 ? RESID(DEBRIS_PUSHBOX3) : i == 1 ? RESID(DEBRIS_PUSHBOX2) :
		RESID(DEBRIS_PUSHBOX1);
}

static void CPushable_Move(int e, int other, int push)
{
	CPushable *p = PRIV(CPushable, e);
	const col_ent_t *oc = ent_cv(other);
	float factor, length, *v, ov[3], t = gtime();
	uint32_t oflags;
	int oground, player = 0;

	if (IsPlayer(other))
	{
		ent_player_t *pl = phy_player();

		VecCopy(pl->velocity, ov);
		oflags = pl->flags;
		oground = pl->groundentity;
	}
	else
	{
		VecCopy(ent_mot(other)->velocity, ov);
		oflags = oc->flags;
		oground = ent_phys(other)->groundentity == ENT_NONE ? -1 :
			ent_phys(other)->groundentity;
	}
	v = ent_mot(e)->velocity;
	if ((oflags & FL_ONGROUND) && oground == e)
	{
		if (ent_phys(e)->waterlevel > 0)
		{
			float nv[3];

			VecCopy(v, nv);
			nv[2] += ov[2] * 0.1f;
			ent_set_velocity(e, nv);
		}
		return;
	}
	if (IsPlayer(other))
	{
		if (push && (phy_player()->buttons & IN_BTN_USE))
			return;
		player = 1;
	}
	if (player)
	{
		if (!(oflags & FL_ONGROUND))
		{
			if (ent_phys(e)->waterlevel < 1)
				return;
			factor = 0.1f;
		}
		else
			factor = 1;
	}
	else
		factor = 0.25f;
	if (!push)
		factor *= gframetime() * PUSHABLE_FIXED_TICK_FUDGE;
	{
		float nv[3];

		VecCopy(v, nv);
		if (push || fabsf(nv[0]) < fabsf(ov[0] - ov[0] * factor))
			nv[0] += ov[0] * factor;
		if (push || fabsf(nv[1]) < fabsf(ov[1] - ov[1] * factor))
			nv[1] += ov[1] * factor;
		length = (float)sqrt(nv[0] * nv[0] + nv[1] * nv[1]);
		if (length > p->m_maxSpeed)
		{
			nv[0] = nv[0] * p->m_maxSpeed / length;
			nv[1] = nv[1] * p->m_maxSpeed / length;
		}
		ent_set_velocity(e, nv);
		if (player)
		{
			if (push)
			{
				phy_player()->velocity[0] = nv[0];
				phy_player()->velocity[1] = nv[1];
			}
			if (t - p->m_soundTime > 0.7f)
			{
				p->m_soundTime = t;
				if (length > 0 && (ent_cv(e)->flags & FL_ONGROUND))
				{
					p->m_lastSound = RANDOM_LONG(0, 2);
					EMIT_SOUND(e, CHAN_WEAPON, push_sound(p->m_lastSound), 0.5f, ATTN_NORM);
				}
				else
					STOP_SOUND(e, CHAN_WEAPON, push_sound(p->m_lastSound));
			}
		}
	}
}

void CPushable_Touch(int e, int other)
{
	if (other == ENT_WORLD)
		return;
	CPushable_Move(e, other, 1);
}

void CPushable_Use(int e, int activator, int caller, int use_type, float value)
{
	if (activator < 0 || !IsPlayer(activator))
	{
		if (CBase(e)->spawnflags & SF_PUSH_BREAKABLE)
			CBreakable_Use(e, activator, caller, use_type, value);
		return;
	}
	if (!VecIsZero(phy_player()->velocity))
		CPushable_Move(e, activator, 0);
}

static int CPushable_TakeDamage(int e, int inflictor, int attacker, float damage, int bits)
{
	if (CBase(e)->spawnflags & SF_PUSH_BREAKABLE)
		return CBreakable_TakeDamage(e, inflictor, attacker, damage, bits);
	return 1;
}

static int CPushable_ObjectCaps(int e)
{
	(void)e;
	return FCAP_CONTINUOUS_USE;
}

static const game_vt_t CPushable_vt = {
	.ObjectCaps = CPushable_ObjectCaps,
	.TraceAttack = CBreakable_TraceAttack,
	.TakeDamage = CPushable_TakeDamage,
	.DamageDecal = CBreakable_DamageDecal,
};

static int CPushable_Spawn(int e, gid_t16 gid)
{
	CPushable *p = PRIV(CPushable, e);
	col_ent_t *c = ent_cv(e);
	ent_phys_t *ph = ent_phys(e);
	float o[3], buoy;
	int i;

	if (p->spawnflags & SF_PUSH_BREAKABLE)
		CBreakable_Spawn(e, gid);
	else
		CBreakable_KeyValue(e, gid);
	/* KeyValue: buoyancy is kept in skin */
	if (gid != GID_NONE && rec_key_f(gid, K_BUOYANCY, &buoy))
		c->skin = (int16_t)(int)buoy;
	/* Precache */
	for (i = 0; i < 3; i++)
		PRECACHE(push_sound(i));
	if (p->spawnflags & SF_PUSH_BREAKABLE)
		CBreakable_Precache(e);
	ent_set_solid(e, SOLID_BBOX);	/* first: a breakable spawned SOLID_BSP */
	ent_set_movetype(e, MOVETYPE_PUSHSTEP);
	ent_set_model(e, p->model);
	if (ph->friction > 399)
		ph->friction = 399;
	p->m_maxSpeed = 400 - ph->friction;
	EFlagsSet(e, FL_FLOAT);
	ph->friction = 0;
	VecCopy(c->origin, o);
	o[2] += 1;	/* pick up off of the floor */
	ent_set_origin(e, o);
	c->skin = (int16_t)(int)((c->skin * (c->maxs[0] - c->mins[0]) * (c->maxs[1] - c->mins[1])) *
		0.0005f);
	p->m_soundTime = 0;
	SetTouch(e, FN_CPushable_Touch);
	SetUse(e, FN_CPushable_Use);
	return 0;
}

/* HLSDK CPushable::m_SaveData */
static const sv_field_t f_CPushable[] = {
	SVF(CPushable, m_lastSound, SV_I32, 0),
	SVF(CPushable, m_maxSpeed, SV_F32, SVK),
	SVF(CPushable, m_soundTime, SV_TIME, 0),
};
static const sv_table_t sv_CPushable = SV_TABLE(&sv_CBreakable, f_CPushable);

const game_class_t gc_FUNC_PUSHABLE = {
	.size = sizeof(CPushable), .Spawn = CPushable_Spawn, .vt = &CPushable_vt,
	.save = &sv_CPushable,
};
