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
 * levelstate.c -- level state of the one-world campaign (docs/architecture.md
 * 6.4, 5.4): per-class dehydrate / rehydrate, seam twin handoff, transition
 * carry and cross-map reference cuts.
 *
 * What a delta keeps is what GoldSrc's save/restore keeps for a level the
 * player left: the entvars HLSDK's gEntvarsDescription saves (here the
 * engine components) and every class level's m_SaveData (here the class's
 * sv_table_t chain, game/hl/cbase.h; tables follow HLSDK's DEFINE_FIELD
 * lists on our private blocks). As in CSave::WriteFields only non-empty
 * fields are written (a presence bitmap per level), and as in
 * CRestore::ReadFields every listed field is cleared before the present ones
 * are read, so a rehydrated entity -- freshly spawned from its record, or
 * by class for a runtime entity -- ends with exactly the saved state; fields
 * no table lists keep their spawn value.
 *
 * Durable forms (nothing in a delta is a slot or a pointer):
 *   entity references   (CWG map, BSP lump) of a record entity, (CWG map,
 *                       runtime id) of a runtime one (rec_runtime_id), the
 *                       world, the player; resolved with rec_ref_entity
 *                       (absent: none, as HLSDK restores an EHANDLE whose
 *                       entity did not cross);
 *   names               the HLE1 name index of the entity's own map;
 *   brush submodels     the submodel number of the entity's own map;
 *   times               relative to the game clock at dehydration (HLSDK
 *                       FIELD_TIME): the entity's time stands still while
 *                       it is away, like a frozen level; pushers' nextthink
 *                       and ltime are their own clock and stay as they are.
 *
 * Delta: u8 version (1..255) | u16 class | components | each table level,
 * base first: bitmap + present fields. A record entity whose delta equals
 * the one it had right after its spawn is "pristine": the delta is the
 * single byte 0 and the record may simply go back to pending (rec.c).
 *
 * Quiescence, for the distance release only (game_dehydrate returns 0 = not
 * now; a closing map's release, rec_slot_closing, stores any state -- the
 * delta is then marked so that its references wake their records with it):
 *   - any live entity rides it (groundentity / aiment == it);
 *   - a pusher (MOVETYPE_PUSH) with velocity or avelocity (mid-travel),
 *     unless its class is a steady mover (func_rotating, func_pendulum:
 *     its motion is state, SVT_STEADY);
 *   - a toggle (door, button, plat, train) going up or down;
 *   - anything else moving off the ground (falling items, MOVETYPE_FOLLOW);
 *   - a monster in a script or in combat (state SCRIPT / COMBAT / PRONE,
 *     ideal COMBAT, a live m_pCine, a live living enemy) or dying;
 *   - a scripted_sequence with a bound monster;
 *   - class hooks (tripmine powering up, ...).
 * Transients (FCAP_DONT_SAVE classes, gibs, projectiles, effect entities):
 * the game frees them here and returns 0; they never persist.
 */
#include <string.h>

#include "engine/world/wld.h"
#include "game/hl/monster.h"
#include "game/hl/player.h"

#define SV_VERSION		1
#define SV_VERSION_CLOSED	2	/* written by a closing map: references wake */
#define SV_SCRATCH	1536		/* largest delta (a monster is < 400 B) */

/* ---- base tables (HLSDK cbase.cpp gEntvarsDescription remainder, subs.cpp,
 * animating.cpp, monsters.cpp, squadmonster.cpp, talkmonster.cpp) --------- */

static const sv_field_t f_CBaseEntity[] = {
	SVF(CBaseEntity, spawnflags, SV_U32, 0),	/* trains, paths, scripts change it */
	SVF(CBaseEntity, health, SV_F32, 0),
	SVF(CBaseEntity, max_health, SV_F32, SVK),
	SVF(CBaseEntity, dmg, SV_F32, SVK),
	SVF(CBaseEntity, dmg_take, SV_F32, 0),
	SVF(CBaseEntity, speed, SV_F32, 0),
	SVF(CBaseEntity, dmgtime, SV_TIME, 0),
	SVF(CBaseEntity, view_ofs, SV_VEC, SVK),
	SVF(CBaseEntity, movedir, SV_VEC, SVK),
	SVF(CBaseEntity, enemy, SV_EH, 0),
	SVF(CBaseEntity, dmg_inflictor, SV_EH, 0),
	SVF(CBaseEntity, m_pGoalEnt, SV_EH, 0),
	SVF(CBaseEntity, target, SV_NAME, 0),
	SVF(CBaseEntity, netname, SV_NAME, SVK),
	SVF(CBaseEntity, message, SV_U16, 0),	/* NAME when SVT_MSG_NAME (fire-once corners) */
	SVF(CBaseEntity, model, SV_RES, SVK),
	SVF(CBaseEntity, noise, SV_RES, SVK),
	SVF(CBaseEntity, noise1, SV_RES, SVK),
	SVF(CBaseEntity, noise2, SV_RES, SVK),
	SVF(CBaseEntity, noise3, SV_RES, SVK),
	SVF(CBaseEntity, takedamage, SV_U8, 0),
	SVF(CBaseEntity, deadflag, SV_U8, 0),
	SVF(CBaseEntity, m_fireState, SV_U8, 0),
};
const sv_table_t sv_CBaseEntity = SV_TABLE(NULL, f_CBaseEntity);

/* Gibs, projectiles, explosions, effect entities: never persist. */
static int transient_quiet(int e)
{
	(void)e;
	return -1;
}

const sv_table_t sv_Transient = { .base = &sv_CBaseEntity, .quiet = transient_quiet };

static const sv_field_t f_CBaseDelay[] = {
	SVF(CBaseDelay, m_flDelay, SV_F32, SVK),
	SVF(CBaseDelay, m_iszKillTarget, SV_NAME, 0),
};
const sv_table_t sv_CBaseDelay = SV_TABLE(&sv_CBaseEntity, f_CBaseDelay);

static const sv_field_t f_CBaseAnimating[] = {
	SVF(CBaseAnimating, anim.m_flFrameRate, SV_F32, 0),
	SVF(CBaseAnimating, anim.m_flGroundSpeed, SV_F32, 0),
	SVF(CBaseAnimating, anim.m_flLastEventCheck, SV_TIME, 0),
	SVF(CBaseAnimating, anim.m_fSequenceFinished, SV_U8, 0),
	SVF(CBaseAnimating, anim.m_fSequenceLoops, SV_U8, 0),
};
const sv_table_t sv_CBaseAnimating = SV_TABLE(&sv_CBaseDelay, f_CBaseAnimating);

static int CBaseToggle_quiet(int e)
{
	int s = PRIV(CBaseToggle, e)->m_toggle_state;

	return s != TS_GOING_UP && s != TS_GOING_DOWN;
}

static const sv_field_t f_CBaseToggle[] = {
	SVF(CBaseToggle, m_toggle_state, SV_U8, 0),
	SVF(CBaseToggle, m_flActivateFinished, SV_TIME, 0),
	SVF(CBaseToggle, m_flMoveDistance, SV_F32, SVK),
	SVF(CBaseToggle, m_flWait, SV_F32, SVK),
	SVF(CBaseToggle, m_flLip, SV_F32, SVK),
	SVF(CBaseToggle, m_flTWidth, SV_F32, SVK),
	SVF(CBaseToggle, m_flTLength, SV_F32, SVK),
	SVF(CBaseToggle, m_vecPosition1, SV_POS, SVK),
	SVF(CBaseToggle, m_vecPosition2, SV_POS, SVK),
	SVF(CBaseToggle, m_vecAngle1, SV_VEC, SVK),
	SVF(CBaseToggle, m_vecAngle2, SV_VEC, SVK),
	SVF(CBaseToggle, m_cTriggersLeft, SV_I32, 0),
	SVF(CBaseToggle, m_flHeight, SV_F32, SVK),
	SVF(CBaseToggle, m_hActivator, SV_EH, 0),
	SVF(CBaseToggle, m_pfnCallWhenMoveDone, SV_FN, 0),
	SVF(CBaseToggle, m_vecFinalDest, SV_POS, 0),
	SVF(CBaseToggle, m_vecFinalAngle, SV_VEC, 0),
	SVF(CBaseToggle, m_sMaster, SV_NAME, SVK),
	SVF(CBaseToggle, m_bitsDamageInflict, SV_I32, SVK),
};
const sv_table_t sv_CBaseToggle = SV_TABLE(&sv_CBaseAnimating, f_CBaseToggle,
	.quiet = CBaseToggle_quiet);

/* CBaseMonster: HLSDK does not save the schedule, the route, m_Activity or
 * m_iTaskStatus; Restore clears the route and the schedule, resets the
 * activity and task status, and clears the conditions without an enemy. */
static int CBaseMonster_quiet(int e)
{
	CBaseMonster *m = MON(e);
	int en = EHGet(m->m_hEnemy);

	if (m->m_MonsterState == MONSTERSTATE_SCRIPT || m->m_MonsterState == MONSTERSTATE_COMBAT ||
	    m->m_MonsterState == MONSTERSTATE_PRONE ||
	    m->m_IdealMonsterState == MONSTERSTATE_COMBAT ||
	    m->m_IdealMonsterState == MONSTERSTATE_SCRIPT)
		return 0;
	if (EHGet(m->m_pCine) >= 0 || m->deadflag == DEAD_DYING)
		return 0;
	if (en >= 0 && en != e && IsAlive(en))
		return 0;
	return 1;
}

static void CBaseMonster_restored(int e)
{
	CBaseMonster *m = MON(e);

	Mon_RouteClear(e);
	m->m_pSchedule = NULL;
	m->m_iTaskStatus = TASKSTATUS_NEW;
	m->m_Activity = ACT_RESET;
	if (EHGet(m->m_hEnemy) < 0)
		m->m_afConditions = 0;
}

static void CBaseMonster_twin(int from, int to, const float d[3])
{
	const CBaseMonster *a = MON(from);
	CBaseMonster *b = MON(to);
	int i;

	b->m_pSchedule = a->m_pSchedule;
	b->m_iTaskStatus = a->m_iTaskStatus;
	b->m_Activity = a->m_Activity;
	b->m_movementGoal = a->m_movementGoal;
	b->m_iRouteIndex = a->m_iRouteIndex;
	b->m_moveWaitTime = a->m_moveWaitTime;
	for (i = 0; i < ROUTE_SIZE; i++)
	{
		b->m_Route[i] = a->m_Route[i];
		VecAdd(a->m_Route[i].vecLocation, d, b->m_Route[i].vecLocation);
	}
}

/* A script whose scripted_sequence leaves: end it the way HLSDK's
 * CineCleanup does while the sequence still exists. */
static void CBaseMonster_cut(int e, int slot)
{
	int cine = EHGet(MON(e)->m_pCine);

	if (cine >= 0 && ent_idc(cine)->layer == slot)
		Mon_CineCleanup(e);
}

static const sv_field_t f_CBaseMonster[] = {
	SVF(CBaseMonster, m_hEnemy, SV_EH, 0),
	SVF(CBaseMonster, m_hTargetEnt, SV_EH, 0),
	SVF(CBaseMonster, m_hOldEnemy, SV_EH, 0),
	SVF(CBaseMonster, m_vecOldEnemy, SV_POS, 0),
	SVF(CBaseMonster, m_flFieldOfView, SV_F32, 0),
	SVF(CBaseMonster, m_flWaitFinished, SV_TIME, 0),
	SVF(CBaseMonster, m_flMoveWaitFinished, SV_TIME, 0),
	SVF(CBaseMonster, m_IdealActivity, SV_I32, 0),
	SVF(CBaseMonster, m_LastHitGroup, SV_I32, 0),
	SVF(CBaseMonster, m_MonsterState, SV_I32, 0),
	SVF(CBaseMonster, m_IdealMonsterState, SV_I32, 0),
	SVF(CBaseMonster, m_iScheduleIndex, SV_I32, 0),
	SVF(CBaseMonster, m_afConditions, SV_I32, SVR),
	SVF(CBaseMonster, m_vecMoveGoal, SV_POS, 0),
	SVF(CBaseMonster, m_movementActivity, SV_I32, 0),
	SVF(CBaseMonster, m_vecLastPosition, SV_POS, 0),
	SVF(CBaseMonster, m_iHintNode, SV_I32, 0),
	SVF(CBaseMonster, m_afMemory, SV_I32, 0),
	SVF(CBaseMonster, m_iMaxHealth, SV_I32, 0),
	SVF(CBaseMonster, m_vecEnemyLKP, SV_POS, 0),
	SVF(CBaseMonster, m_cAmmoLoaded, SV_I32, 0),
	SVF(CBaseMonster, m_afCapability, SV_I32, 0),
	SVF(CBaseMonster, m_flNextAttack, SV_TIME, 0),
	SVF(CBaseMonster, m_bitsDamageType, SV_I32, 0),
	SVF(CBaseMonster, m_rgbTimeBasedDamage, SV_U8, 0),
	SVF(CBaseMonster, m_bloodColor, SV_I32, SVK),
	SVF(CBaseMonster, m_failSchedule, SV_I32, 0),
	SVF(CBaseMonster, m_flHungryTime, SV_TIME, 0),
	SVF(CBaseMonster, m_flDistTooFar, SV_F32, SVK),
	SVF(CBaseMonster, m_flDistLook, SV_F32, SVK),
	SVF(CBaseMonster, m_iTriggerCondition, SV_I32, 0),
	SVF(CBaseMonster, m_iszTriggerTarget, SV_NAME, 0),
	SVF(CBaseMonster, m_HackedGunPos, SV_VEC, SVK),
	SVF(CBaseMonster, m_scriptState, SV_I32, 0),
	SVF(CBaseMonster, m_pCine, SV_EH, 0),
	SVF(CBaseMonster, m_flLastYawTime, SV_TIME, 0),
	SVF(CBaseMonster, m_makerGid, SV_GID, SVK),
};
const sv_table_t sv_CBaseMonster = SV_TABLE(&sv_CBaseToggle, f_CBaseMonster,
	.quiet = CBaseMonster_quiet, .restored = CBaseMonster_restored,
	.twin = CBaseMonster_twin, .cut = CBaseMonster_cut);

static const sv_field_t f_CSquadMonster[] = {
	SVF(CSquadMonster, m_hSquadLeader, SV_EH, 0),
	SVF(CSquadMonster, m_hSquadMember, SV_EH, 0),
	SVF(CSquadMonster, m_afSquadSlots, SV_I32, 0),
	SVF(CSquadMonster, m_flLastEnemySightTime, SV_TIME, 0),
	SVF(CSquadMonster, m_fEnemyEluded, SV_I32, 0),
	SVF(CSquadMonster, m_iMySlot, SV_I32, 0),
};
const sv_table_t sv_CSquadMonster = SV_TABLE(&sv_CBaseMonster, f_CSquadMonster);

static const sv_field_t f_CTalkMonster[] = {
	SVF(CTalkMonster, m_bitsSaid, SV_I32, 0),
	SVF(CTalkMonster, m_nSpeak, SV_I32, 0),
	SVF(CTalkMonster, m_voicePitch, SV_I32, 0),	/* random at spawn */
	SVF(CTalkMonster, m_szGrp, SV_RES, SVK),
	SVF(CTalkMonster, m_useTime, SV_TIME, 0),
	SVF(CTalkMonster, m_iszUse, SV_RES, SVK),
	SVF(CTalkMonster, m_iszUnUse, SV_RES, SVK),
	SVF(CTalkMonster, m_flLastSaidSmelled, SV_TIME, 0),
	SVF(CTalkMonster, m_flStopTalkTime, SV_TIME, 0),
	SVF(CTalkMonster, m_hTalkTarget, SV_EH, 0),
};
const sv_table_t sv_CTalkMonster = SV_TABLE(&sv_CBaseMonster, f_CTalkMonster);

/* ---- components (the entvars the engine interprets) ---------------------- */

enum { C_CV, C_MOT, C_PHYS, C_TM, C_RND, C_ID };

typedef struct {
	uint8_t		comp, type, n;
	uint16_t	ofs;
} comp_field_t;

#define CF(c, T, m, ty)	{ c, ty, (uint8_t)(sizeof(((T *)0)->m) / SVSZ_##ty), \
			  (uint16_t)offsetof(T, m) }

/* order matters: movetype precedes nextthink (a pusher's clock) */
static const comp_field_t comp_fields[] = {
	CF(C_CV, col_ent_t, origin, SV_POS),
	CF(C_CV, col_ent_t, angles, SV_VEC),
	CF(C_CV, col_ent_t, mins, SV_VEC),
	CF(C_CV, col_ent_t, maxs, SV_VEC),
	CF(C_CV, col_ent_t, flags, SV_U32),
	CF(C_CV, col_ent_t, owner, SV_IX16),
	CF(C_CV, col_ent_t, skin, SV_I16),
	CF(C_CV, col_ent_t, solid, SV_U8),
	CF(C_CV, col_ent_t, rendermode, SV_U8),
	CF(C_CV, col_ent_t, movetype, SV_U8),
	CF(C_MOT, ent_mot_t, velocity, SV_VEC),
	CF(C_MOT, ent_mot_t, avelocity, SV_VEC),
	CF(C_MOT, ent_mot_t, basevelocity, SV_VEC),
	CF(C_PHYS, ent_phys_t, gravity, SV_F32),
	CF(C_PHYS, ent_phys_t, friction, SV_F32),
	CF(C_PHYS, ent_phys_t, ideal_yaw, SV_F32),
	CF(C_PHYS, ent_phys_t, yaw_speed, SV_F32),
	CF(C_PHYS, ent_phys_t, groundentity, SV_IX16),
	CF(C_PHYS, ent_phys_t, aiment, SV_IX16),
	CF(C_PHYS, ent_phys_t, waterlevel, SV_U8),
	CF(C_PHYS, ent_phys_t, watertype, SV_U8),
	CF(C_PHYS, ent_phys_t, view_ofs_z, SV_I16),
	CF(C_TM, ent_tm_t, nextthink, SV_TIME),
	CF(C_TM, ent_tm_t, ltime, SV_F32),
	CF(C_TM, ent_tm_t, think, SV_FN),
	CF(C_TM, ent_tm_t, touch, SV_FN),
	CF(C_TM, ent_tm_t, use, SV_FN),
	CF(C_TM, ent_tm_t, blocked, SV_FN),
	CF(C_RND, ent_rnd_t, model, SV_RES),
	CF(C_RND, ent_rnd_t, sequence, SV_U16),
	CF(C_RND, ent_rnd_t, frame, SV_F32),
	CF(C_RND, ent_rnd_t, animtime, SV_TIME),
	CF(C_RND, ent_rnd_t, framerate, SV_F32),
	CF(C_RND, ent_rnd_t, scale, SV_F32),
	CF(C_RND, ent_rnd_t, effects, SV_U16),
	CF(C_RND, ent_rnd_t, body, SV_U8),
	CF(C_RND, ent_rnd_t, renderfx, SV_U8),
	CF(C_RND, ent_rnd_t, controller, SV_U8),
	CF(C_RND, ent_rnd_t, blending, SV_U8),
	CF(C_RND, ent_rnd_t, renderamt, SV_U8),
	CF(C_RND, ent_rnd_t, rendercolor, SV_U8),
	CF(C_ID, ent_id_t, targetname, SV_NAME),
};
#define N_COMP		((int)(sizeof(comp_fields) / sizeof(comp_fields[0])))
#define COMP_NEXTTHINK	22

/* A component snapshot: the entity's components, decoded into and then
 * applied through the engine setters. */
typedef struct {
	col_ent_t	cv;
	ent_mot_t	mot;
	ent_phys_t	phys;
	ent_tm_t	tm;
	ent_rnd_t	rnd;
	ent_id_t	id;
} comps_t;

static uint8_t *comp_ptr(comps_t *s, const comp_field_t *f)
{
	static const uint16_t base[] = {
		offsetof(comps_t, cv), offsetof(comps_t, mot), offsetof(comps_t, phys),
		offsetof(comps_t, tm), offsetof(comps_t, rnd), offsetof(comps_t, id),
	};

	return (uint8_t *)s + base[f->comp] + f->ofs;
}

static void comps_get(int e, comps_t *s)
{
	s->cv = *ent_cv(e);
	s->mot = *ent_mot(e);
	s->phys = *ent_phys(e);
	s->tm = *ent_tm(e);
	s->rnd = *ent_rnd(e);
	s->id = *ent_idc(e);
}

/* ---- the class's table ------------------------------------------------------- */

static const sv_table_t *table_of_cls(cls_t cls)
{
	const game_class_t *c = game_class(cls);

	if (!c || !c->Spawn)
		return NULL;
	if (c->save)
		return c->save;
	return c->size <= sizeof(CBaseEntity) ? &sv_CBaseEntity : NULL;
}

const sv_table_t *sv_table_of(int e)
{
	return ent_is_live(e) ? table_of_cls(ent_idc(e)->cls) : NULL;
}

static uint16_t chain_flags(const sv_table_t *t)
{
	uint16_t f = 0;

	for (; t; t = t->base)
		f |= t->flags;
	return f;
}

/* the chain base first */
static int chain(const sv_table_t *t, const sv_table_t **out)
{
	const sv_table_t *tmp[8];
	int n = 0, i;

	for (; t && n < 8; t = t->base)
		tmp[n++] = t;
	for (i = 0; i < n; i++)
		out[i] = tmp[n - 1 - i];
	return n;
}

static int field_type(const sv_table_t *t, const sv_field_t *f, uint16_t cflags)
{
	if (t == &sv_CBaseEntity && f->ofs == offsetof(CBaseEntity, message) &&
	    (cflags & SVT_MSG_NAME))
		return SV_NAME;
	return f->type;
}

static const uint8_t sv_size[SV_NTYPES] = {
	SVSZ_SV_F32, SVSZ_SV_TIME, SVSZ_SV_VEC, SVSZ_SV_POS, SVSZ_SV_I32, SVSZ_SV_U32,
	SVSZ_SV_I16, SVSZ_SV_U16, SVSZ_SV_U8, SVSZ_SV_FN, SVSZ_SV_RES, SVSZ_SV_NAME,
	SVSZ_SV_GID, SVSZ_SV_EH, SVSZ_SV_IX16, SVSZ_SV_IX32,
};

int sv_elem_size(int type)
{
	return type >= 0 && type < SV_NTYPES ? sv_size[type] : 0;
}

/* The entity a reference element names: the world, a live entity, or -1
 * (none, or freed since: a stale handle is an empty field, as an EHANDLE
 * whose entity is gone reads NULL in HLSDK). */
static int ref_target(int ty, const uint8_t *v)
{
	uint32_t w;
	uint16_t s;
	int32_t i;

	if (ty == SV_EH)
	{
		memcpy(&w, v, 4);
		if (w == EH_NONE)
			return -1;
		return w == ent_handle_of(ENT_WORLD) ? ENT_WORLD : ent_of(w);
	}
	if (ty == SV_IX16)
	{
		memcpy(&s, v, 2);
		i = s == 0xFFFF ? -1 : s;
	}
	else
		memcpy(&i, v, 4);
	return i == ENT_WORLD || (i > 0 && i < HL64_MAX_ENTITIES && ent_is_live(i)) ? i : -1;
}

static int elem_empty(int ty, const uint8_t *v)
{
	uint16_t u;
	int i;

	switch (ty)
	{
	case SV_EH: case SV_IX16: case SV_IX32:
		return ref_target(ty, v) < 0;
	case SV_NAME: case SV_GID:
		memcpy(&u, v, 2);
		return u == 0xFFFF;
	default:
		for (i = 0; i < sv_size[ty]; i++)
			if (v[i])
				return 0;
		return 1;
	}
}

static void elem_clear(int ty, uint8_t *v)
{
	switch (ty)
	{
	case SV_EH: case SV_IX32:
		memset(v, 0xFF, 4);
		break;
	case SV_NAME: case SV_GID: case SV_IX16:
		memset(v, 0xFF, 2);
		break;
	default:
		memset(v, 0, sv_size[ty]);
		break;
	}
}

static int field_empty(int ty, int n, const uint8_t *v)
{
	int i;

	for (i = 0; i < n; i++)
		if (!elem_empty(ty, v + i * sv_size[ty]))
			return 0;
	return 1;
}

/* ---- byte streams ---------------------------------------------------------------- */

typedef struct {
	uint8_t		*p, *end;
	int		bad;
} wbuf_t;

typedef struct {
	const uint8_t	*p, *end;
	int		bad;
} rbuf_t;

static void put(wbuf_t *w, const void *v, int n)
{
	if (w->bad || w->end - w->p < n)
	{
		w->bad = 1;
		return;
	}
	memcpy(w->p, v, (size_t)n);
	w->p += n;
}

static void put_var(wbuf_t *w, uint32_t v)
{
	uint8_t b;

	do
	{
		b = (uint8_t)(v & 0x7F);
		v >>= 7;
		if (v)
			b |= 0x80;
		put(w, &b, 1);
	} while (v);
}

static void get(rbuf_t *r, void *v, int n)
{
	if (r->bad || r->end - r->p < n)
	{
		r->bad = 1;
		memset(v, 0, (size_t)n);
		return;
	}
	memcpy(v, r->p, (size_t)n);
	r->p += n;
}

static uint32_t get_var(rbuf_t *r)
{
	uint32_t v = 0;
	uint8_t b;
	int s = 0;

	do
	{
		get(r, &b, 1);
		v |= (uint32_t)(b & 0x7F) << s;
		s += 7;
	} while ((b & 0x80) && !r->bad && s < 35);
	return v;
}

/* ---- durable forms ------------------------------------------------------------- */

#define REF_SPECIAL	0xFF
#define REF_RUNTIME	0x80

static int slot_map(int slot)
{
	wld_layer_t *L = slot >= 0 ? wld_layer(slot) : NULL;

	return L ? L->map : -1;
}

static void ref_put(wbuf_t *w, int r)
{
	uint8_t m = REF_SPECIAL;
	uint16_t id = 0xFFFF;
	int map;

	if (r == ENT_WORLD)
		id = 0;
	else if (r == ENT_PLAYER && ent_is_live(r))
		id = 1;
	else if (r > ENT_PLAYER && ent_is_live(r))
	{
		gid_t16 g = ent_idc(r)->gid;

		if (g != GID_NONE && (map = slot_map(GID_SLOT(g))) >= 0)
		{
			m = (uint8_t)map;
			id = GID_LUMP(g);
		}
		else if (g == GID_NONE && (map = slot_map(ent_idc(r)->layer)) >= 0 &&
			 (id = rec_runtime_id(r)) != 0)
			m = (uint8_t)(REF_RUNTIME | map);
		else
			id = 0xFFFF;
	}
	put(w, &m, 1);
	put(w, &id, 2);
}

/* wake: the delta was written when its map closed (the whole level was
 * saved, as GoldSrc saves a level it leaves): a referenced record of an
 * open map that is not live comes back now with it, as HLSDK restores the
 * whole level before the references are used (a monster's scripted_sequence,
 * a sequence's monster). */
static int ref_get(rbuf_t *r, int wake)
{
	uint8_t m;
	uint16_t id;
	int e;
	wld_layer_t *L;

	get(r, &m, 1);
	get(r, &id, 2);
	if (m == REF_SPECIAL)
		return id == 0 ? ENT_WORLD : id == 1 && ent_is_live(ENT_PLAYER) ? ENT_PLAYER : -1;
	e = rec_ref_entity(m & 0x7F, id, (m & REF_RUNTIME) != 0);
	if (e < 0 && wake && !(m & REF_RUNTIME) && id < 1024 &&
	    (L = wld_layer_of_map(m & 0x7F)) != NULL && rec_kind(GID_MAKE(L->slot, id)) == REC_KIND_ENTITY)
		e = rec_wake(GID_MAKE(L->slot, id));
	return e;
}

/* A name of another map held by an entity of slot `slot`: the same name in
 * its own map, or none. */
static name_t own_name(name_t n, int slot)
{
	if (n == NAME_NONE || (int)NAME_SLOT(n) == slot)
		return n;
	return rec_name_in(n, slot);
}

/* ---- field codec ---------------------------------------------------------------------- */

typedef struct {
	int		slot;		/* the entity's HLE slot */
	float		now;
	int		pusher;		/* nextthink in ltime units */
	int		wake;		/* a closed map's delta: wake referenced records */
} ctx_t;

static void put_elem(wbuf_t *w, const ctx_t *c, int ty, const uint8_t *v, int raw_time)
{
	float f;
	uint32_t u;
	int32_t i;
	uint16_t s;

	switch (ty)
	{
	case SV_TIME:
		memcpy(&f, v, 4);
		if (f != 0 && !raw_time)
		{
			f -= c->now;
			if (f == 0)
				f = -0.0f;	/* set, due now: not "unset" */
		}
		put(w, &f, 4);
		break;
	case SV_I32:
		memcpy(&i, v, 4);
		put_var(w, ((uint32_t)i << 1) ^ (uint32_t)(i >> 31));
		break;
	case SV_U32:
		memcpy(&u, v, 4);
		put_var(w, u);
		break;
	case SV_RES:
		memcpy(&s, v, 2);
		if (RES_KIND(s) == RES_K_SUBMODEL)
			s = RES_MAKE(RES_K_SUBMODEL, RES_SUB_NUM(s));
		put(w, &s, 2);
		break;
	case SV_NAME:
		memcpy(&s, v, 2);
		s = own_name(s, c->slot);
		if (s != NAME_NONE)
			s = NAME_INDEX(s);
		put(w, &s, 2);
		break;
	case SV_GID:
		memcpy(&s, v, 2);
		s = s != GID_NONE && (int)GID_SLOT(s) == c->slot ? GID_LUMP(s) : 0xFFFF;
		put(w, &s, 2);
		break;
	case SV_EH: case SV_IX16: case SV_IX32:
		ref_put(w, ref_target(ty, v));
		break;
	default:
		put(w, v, sv_size[ty]);
		break;
	}
}

static void get_elem(rbuf_t *r, const ctx_t *c, int ty, uint8_t *v, int raw_time)
{
	float f;
	uint32_t u;
	int32_t i;
	uint16_t s;
	int e;

	switch (ty)
	{
	case SV_TIME:
		get(r, &f, 4);
		memcpy(&u, &f, 4);
		if (u != 0 && !raw_time)
			f += c->now;
		memcpy(v, &f, 4);
		break;
	case SV_I32:
		u = get_var(r);
		i = (int32_t)(u >> 1) ^ -(int32_t)(u & 1);
		memcpy(v, &i, 4);
		break;
	case SV_U32:
		u = get_var(r);
		memcpy(v, &u, 4);
		break;
	case SV_RES:
		get(r, &s, 2);
		if (RES_KIND(s) == RES_K_SUBMODEL)
			s = RES_SUBMODEL(c->slot, RES_SUB_NUM(s));
		memcpy(v, &s, 2);
		break;
	case SV_NAME:
		get(r, &s, 2);
		if (s != 0xFFFF)
			s = NAME_MAKE(c->slot, s & 0xFFF);
		memcpy(v, &s, 2);
		break;
	case SV_GID:
		get(r, &s, 2);
		if (s != 0xFFFF)
			s = GID_MAKE(c->slot, s & 1023);
		memcpy(v, &s, 2);
		break;
	case SV_EH:
		e = ref_get(r, c->wake);
		u = EHANDLE(e);
		memcpy(v, &u, 4);
		break;
	case SV_IX16:
		e = ref_get(r, c->wake);
		s = e < 0 ? 0xFFFF : (uint16_t)e;
		memcpy(v, &s, 2);
		break;
	case SV_IX32:
		i = ref_get(r, c->wake);
		memcpy(v, &i, 4);
		break;
	default:
		get(r, v, sv_size[ty]);
		break;
	}
}

/* One level: bitmap, then the present fields. */
static void put_level(wbuf_t *w, const ctx_t *c, int nf, const uint8_t *const *ptr,
	const uint8_t *type, const uint8_t *count, int pusher_field)
{
	uint8_t *map = w->p;
	int i, k, nb = (nf + 7) / 8;
	static const uint8_t zero[8];

	put(w, zero, nb);
	if (w->bad)
		return;
	memset(map, 0, (size_t)nb);
	for (i = 0; i < nf; i++)
	{
		if (field_empty(type[i], count[i], ptr[i]))
			continue;
		map[i >> 3] |= (uint8_t)(1 << (i & 7));
		for (k = 0; k < count[i]; k++)
			put_elem(w, c, type[i], ptr[i] + k * sv_size[type[i]],
				i == pusher_field && c->pusher);
	}
}

/* mt: where the level's movetype is decoded (components), NULL otherwise:
 * the pusher field is raw when it says MOVETYPE_PUSH (it precedes it). */
static void get_level(rbuf_t *r, const ctx_t *c, int nf, uint8_t *const *ptr,
	const uint8_t *type, const uint8_t *count, int pusher_field, const uint8_t *mt)
{
	uint8_t map[32];
	int i, k, nb = (nf + 7) / 8;

	get(r, map, nb);
	for (i = 0; i < nf; i++)
	{
		for (k = 0; k < count[i]; k++)
			elem_clear(type[i], ptr[i] + k * sv_size[type[i]]);
		if (!(map[i >> 3] & (1 << (i & 7))))
			continue;
		for (k = 0; k < count[i]; k++)
			get_elem(r, c, type[i], ptr[i] + k * sv_size[type[i]],
				i == pusher_field && mt && *mt == MOVETYPE_PUSH);
	}
}

/* The fields of one table level of e, flattened. A record entity is
 * respawned from its record before it rehydrates, so its spawn constants
 * (SVK) are already right and are not written (runtime entities write
 * them: their spawn has no keys). */
#define MAXF	64
typedef struct {
	int		nf;
	uint8_t		*ptr[MAXF];
	uint8_t		type[MAXF], count[MAXF];
} flat_t;

static void flatten(const sv_table_t *t, uint8_t *priv, uint16_t cflags, int record, flat_t *o)
{
	int i;

	o->nf = 0;
	for (i = 0; i < t->nf && o->nf < MAXF; i++)
	{
		if (record && (t->f[i].flags & SVK))
			continue;
		o->ptr[o->nf] = priv + t->f[i].ofs;
		o->type[o->nf] = (uint8_t)field_type(t, &t->f[i], cflags);
		o->count[o->nf] = t->f[i].n;
		o->nf++;
	}
}

static void flatten_comps(comps_t *s, flat_t *o)
{
	int i;

	o->nf = N_COMP;
	for (i = 0; i < N_COMP; i++)
	{
		o->ptr[i] = comp_ptr(s, &comp_fields[i]);
		o->type[i] = comp_fields[i].type;
		o->count[i] = comp_fields[i].n;
	}
}

static int encode(int e, uint8_t *buf, uint32_t max)
{
	const sv_table_t *t = sv_table_of(e), *ch[8];
	wbuf_t w = { buf, buf + max, 0 };
	uint8_t ver = SV_VERSION;
	uint16_t cflags = chain_flags(t);
	comps_t s;
	flat_t fl;
	ctx_t c;
	int n, i;

	c.slot = ent_idc(e)->layer;
	c.now = gtime();
	c.pusher = ent_cv(e)->movetype == MOVETYPE_PUSH;
	c.wake = 0;
	put(&w, &ver, 1);
	put(&w, &ent_idc(e)->cls, 2);
	comps_get(e, &s);
	flatten_comps(&s, &fl);
	put_level(&w, &c, fl.nf, (const uint8_t *const *)fl.ptr, fl.type, fl.count,
		COMP_NEXTTHINK);
	n = chain(t, ch);
	for (i = 0; i < n; i++)
	{
		flatten(ch[i], (uint8_t *)ent_priv(e), cflags, ent_idc(e)->gid != GID_NONE, &fl);
		put_level(&w, &c, fl.nf, (const uint8_t *const *)fl.ptr, fl.type, fl.count, -1);
	}
	return w.bad ? -1 : (int)(w.p - buf);
}

/* ---- stats -------------------------------------------------------------------------- */

static sv_class_stats_t st[CLS__COUNT];
static uint32_t spawn_sum[HL64_MAX_ENTITIES];	/* 0 = no pristine form */
static uint8_t scratch[SV_SCRATCH];
static int restore_hooks = 1;		/* test builds may switch them off */

const sv_class_stats_t *sv_stats(cls_t cls)
{
	return cls < CLS__COUNT ? &st[cls] : NULL;
}

void sv_stats_reset(void)
{
	memset(st, 0, sizeof st);
}

static uint32_t fnv(const uint8_t *p, int n)
{
	uint32_t h = 2166136261u;
	int i;

	for (i = 0; i < n; i++)
		h = (h ^ p[i]) * 16777619u;
	return h ? h : 1;
}

void sv_spawned(int e)
{
	int n;

	spawn_sum[e] = 0;
	if (ent_idc(e)->gid == GID_NONE || !sv_table_of(e))
		return;
	n = encode(e, scratch, sizeof scratch);
	if (n > 0)
		spawn_sum[e] = fnv(scratch, n);
}

/* ---- quiescence ---------------------------------------------------------------------- */

static int has_rider(int e)
{
	int r;

	for (r = ENT_PLAYER; r < HL64_MAX_ENTITIES; r++)
		if (r != e && ent_is_live(r) &&
		    (ent_phys(r)->groundentity == e || ent_phys(r)->aiment == e))
			return 1;
	return 0;
}

static int moving(const float v[3])
{
	return v[0] != 0 || v[1] != 0 || v[2] != 0;
}

int sv_quiet_default(int e)
{
	const col_ent_t *cv = ent_cv(e);
	const ent_mot_t *m = ent_mot(e);

	if (ObjectCaps(e) & FCAP_DONT_SAVE)
		return -1;
	if (cv->flags & FL_KILLME)
		return -1;
	if (has_rider(e))
		return 0;
	if (cv->movetype == MOVETYPE_PUSH)
	{
		if ((moving(m->velocity) || moving(m->avelocity)) &&
		    !(chain_flags(sv_table_of(e)) & SVT_STEADY))
			return 0;
	}
	else if (cv->movetype == MOVETYPE_FOLLOW)
		return 0;
	else if (moving(m->velocity) && !(cv->flags & FL_ONGROUND) && cv->movetype != MOVETYPE_NONE)
		return 0;
	return 1;
}

/* Class hooks first (a refusal wins over a transient: a plat's trigger
 * stays while its plat lives), then the default rules. A closing map
 * (rec_slot_closing) refuses nothing: GoldSrc saves any state of a level it
 * leaves (a monster mid-script, a door mid-travel); only transients go. */
static int quiet(int e, const sv_table_t *t, int closing)
{
	const sv_table_t *ch[8];
	int n, i, r, transient = 0, refuse = 0;

	n = chain(t, ch);
	for (i = 0; i < n; i++)
		if (ch[i]->quiet && (r = ch[i]->quiet(e)) <= 0)
		{
			if (r == 0)
				refuse = 1;
			else
				transient = 1;
		}
	if (closing)
		return transient || sv_quiet_default(e) < 0 ? -1 : 1;
	if (refuse)
		return 0;
	return transient ? -1 : sv_quiet_default(e);
}

/* ---- game.h ------------------------------------------------------------------------- */

uint32_t game_dehydrate(int e, void *buf, uint32_t max)
{
	const sv_table_t *t, *ch[8];
	sv_class_stats_t *s;
	int n, i, q, nc;

	if (e <= ENT_PLAYER || !ent_is_live(e))
		return 0;
	s = &st[ent_idc(e)->cls < CLS__COUNT ? ent_idc(e)->cls : 0];
	t = sv_table_of(e);
	if (!t)
	{
		log_printf("levelstate: %s (%d) has no save table: not dehydrated\n",
			game_class_name(ent_idc(e)->cls), e);
		s->refused++;
		return 0;
	}
	q = quiet(e, t, rec_slot_closing(ent_idc(e)->layer));
	if (q < 0)
	{
		s->transient++;
		ent_destroy(e);
		return 0;
	}
	if (q == 0)
	{
		s->refused++;
		return 0;
	}
	n = encode(e, scratch, sizeof scratch);
	if (n < 0)
	{
		log_printf("levelstate: %s (%d) delta over %d B\n", game_class_name(ent_idc(e)->cls),
			e, SV_SCRATCH);
		s->refused++;
		return 0;
	}
	if (spawn_sum[e] && fnv(scratch, n) == spawn_sum[e])
	{
		n = 1;
		scratch[0] = 0;
		s->pristine++;
	}
	else if (rec_slot_closing(ent_idc(e)->layer))
		scratch[0] = SV_VERSION_CLOSED;	/* references wake their records */
	if ((uint32_t)n > max)
	{
		s->refused++;
		return 0;
	}
	memcpy(buf, scratch, (size_t)n);
	s->dehydrated++;
	s->bytes += (uint32_t)n;
	if ((uint32_t)n > s->bytes_max)
		s->bytes_max = (uint32_t)n;
	nc = chain(t, ch);
	for (i = 0; i < nc; i++)
		if (ch[i]->released)
			ch[i]->released(e);
	return (uint32_t)n;
}

static void apply_comps(int e, const comps_t *s)
{
	col_ent_t *cv = ent_cv(e);
	ent_rnd_t *r = ent_rnd(e);
	ent_phys_t *p = ent_phys(e);
	ent_tm_t *tm = ent_tm(e);

	if (s->rnd.model != r->model)
	{
		if (s->rnd.model != RES_NONE)
			ent_set_model(e, s->rnd.model);
		else
			r->model = RES_NONE;
	}
	r->sequence = s->rnd.sequence;
	r->frame = s->rnd.frame;
	r->animtime = s->rnd.animtime;
	r->framerate = s->rnd.framerate;
	r->scale = s->rnd.scale;
	r->effects = s->rnd.effects;
	r->body = s->rnd.body;
	r->renderfx = s->rnd.renderfx;
	memcpy(r->controller, s->rnd.controller, sizeof r->controller);
	memcpy(r->blending, s->rnd.blending, sizeof r->blending);
	r->renderamt = s->rnd.renderamt;
	memcpy(r->rendercolor, s->rnd.rendercolor, sizeof r->rendercolor);
	ent_set_size(e, s->cv.mins, s->cv.maxs);
	VecCopy(s->cv.angles, cv->angles);
	ent_set_origin(e, s->cv.origin);
	cv->owner = s->cv.owner;
	cv->skin = s->cv.skin;
	cv->rendermode = s->cv.rendermode;
	if (s->cv.solid == SOLID_BSP)
	{
		ent_set_movetype(e, s->cv.movetype);
		ent_set_solid(e, s->cv.solid);
	}
	else
	{
		ent_set_solid(e, s->cv.solid);
		ent_set_movetype(e, s->cv.movetype);
	}
	ent_set_flags(e, s->cv.flags);
	ent_set_velocity(e, s->mot.velocity);
	ent_set_avelocity(e, s->mot.avelocity);
	ent_set_basevelocity(e, s->mot.basevelocity);
	p->gravity = s->phys.gravity;
	p->friction = s->phys.friction;
	p->ideal_yaw = s->phys.ideal_yaw;
	p->yaw_speed = s->phys.yaw_speed;
	p->waterlevel = s->phys.waterlevel;
	p->watertype = s->phys.watertype;
	p->view_ofs_z = s->phys.view_ofs_z;
	ent_set_ground(e, s->phys.groundentity);
	ent_set_aiment(e, s->phys.aiment);
	tm->ltime = s->tm.ltime;
	tm->think = s->tm.think;
	tm->touch = s->tm.touch;
	tm->use = s->tm.use;
	tm->blocked = s->tm.blocked;
	ent_think_at(e, s->tm.nextthink);
	ent_idc(e)->targetname = s->id.targetname;
	if (ent_idc(e)->gid == GID_NONE && s->id.targetname != NAME_NONE)
		rec_name_hold(e, s->id.targetname);
}

void game_rehydrate(int e, const void *buf, uint32_t n)
{
	const sv_table_t *t, *ch[8];
	rbuf_t r = { buf, (const uint8_t *)buf + n, 0 };
	uint16_t cflags, cls;
	uint8_t ver;
	comps_t s;
	flat_t fl;
	ctx_t c;
	int i, nc;

	if (!ent_is_live(e) || n == 0)
		return;
	get(&r, &ver, 1);
	if (ver == 0 && n == 1)
		return;		/* pristine: the spawn is the state */
	get(&r, &cls, 2);
	t = sv_table_of(e);
	if ((ver != SV_VERSION && ver != SV_VERSION_CLOSED) || cls != ent_idc(e)->cls || !t)
	{
		log_printf("levelstate: delta of %s (v%u) does not fit %s (%d)\n",
			game_class_name(cls), ver, game_class_name(ent_idc(e)->cls), e);
		return;
	}
	cflags = chain_flags(t);
	c.slot = ent_idc(e)->layer;
	c.now = gtime();
	c.pusher = 0;
	c.wake = ver == SV_VERSION_CLOSED;
	comps_get(e, &s);
	flatten_comps(&s, &fl);
	get_level(&r, &c, fl.nf, fl.ptr, fl.type, fl.count, COMP_NEXTTHINK, &s.cv.movetype);
	if (r.bad)
	{
		log_printf("levelstate: truncated delta of %s (%d)\n", game_class_name(cls), e);
		return;
	}
	apply_comps(e, &s);
	nc = chain(t, ch);
	for (i = 0; i < nc; i++)
	{
		flatten(ch[i], (uint8_t *)ent_priv(e), cflags, ent_idc(e)->gid != GID_NONE, &fl);
		get_level(&r, &c, fl.nf, fl.ptr, fl.type, fl.count, -1, NULL);
	}
	if (r.bad)
		log_printf("levelstate: truncated delta of %s (%d)\n", game_class_name(cls), e);
	if (restore_hooks)
		sv_restored(e);
	ent_motion_changed(e);
	ent_relink(e, 0);
	st[cls].rehydrated++;
}

void sv_restored(int e)
{
	const sv_table_t *ch[8];
	int i, nc = chain(sv_table_of(e), ch);

	for (i = 0; i < nc; i++)
		if (ch[i]->restored)
			ch[i]->restored(e);
}

#ifdef HL64_TEST
void sv_test_restore_hooks(int on)
{
	restore_hooks = on;
}

int sv_test_encode(int e, void *buf, uint32_t max)
{
	return sv_table_of(e) ? encode(e, buf, max) : -1;
}

/* bytes of one encoded element */
static int elem_len(const uint8_t *p, const uint8_t *end, int ty)
{
	int n = 0;

	switch (ty)
	{
	case SV_I32: case SV_U32:
		while (p + n < end && (p[n] & 0x80))
			n++;
		return n + 1;
	case SV_EH: case SV_IX16: case SV_IX32:
		return 3;
	default:
		return sv_size[ty];
	}
}

/* The first field where two deltas of e's class differ: level (0 =
 * components), field index, its offset in the level's struct; -1 none. */
int sv_test_diff(int e, const uint8_t *a, int na, const uint8_t *b, int nb, int *level,
	int *ofs)
{
	const sv_table_t *ch[8];
	const uint8_t *pa = a + 3, *pb = b + 3;
	int nc = chain(sv_table_of(e), ch), l, i, k, la, lb;
	comps_t s;
	flat_t fl;

	comps_get(e, &s);
	for (l = 0; l <= nc; l++)
	{
		const uint8_t *ma, *mb;
		int nbm;

		if (l == 0)
			flatten_comps(&s, &fl);
		else
			flatten(ch[l - 1], (uint8_t *)ent_priv(e), chain_flags(ch[nc - 1]),
				ent_idc(e)->gid != GID_NONE, &fl);
		nbm = (fl.nf + 7) / 8;
		ma = pa;
		mb = pb;
		pa += nbm;
		pb += nbm;
		for (i = 0; i < fl.nf; i++)
		{
			int ina = (ma[i >> 3] >> (i & 7)) & 1, inb = (mb[i >> 3] >> (i & 7)) & 1;

			*level = l;
			*ofs = l == 0 ? i : (int)(fl.ptr[i] - (uint8_t *)ent_priv(e));
			if (ina != inb)
				return i;
			if (!ina)
				continue;
			for (k = 0; k < fl.count[i]; k++)
			{
				la = elem_len(pa, a + na, fl.type[i]);
				lb = elem_len(pb, b + nb, fl.type[i]);
				if (la != lb || memcmp(pa, pb, (size_t)la))
					return i;
				pa += la;
				pb += lb;
			}
		}
	}
	return -1;
}
#endif

/* ---- seam twins (architecture.md 5.4 req. 3) ------------------------------------- */

/* A reference held by A's twin: an entity of A's map that has a twin of its
 * own maps to that twin (a monster's scripted_sequence); anything else (the
 * player, B's entities, entities without a twin) stays. */
static int twin_ref(int r, int slot_a)
{
	gid_t16 g;
	int b;

	if (r <= ENT_PLAYER || !ent_is_live(r) || ent_idc(r)->layer != slot_a)
		return r;
	g = ent_idc(r)->gid;
	if (g == GID_NONE || (g = rec_twin_of(g)) == GID_NONE)
		return r;
	b = rec_entity(g);
	return b >= 0 ? b : r;
}

static void twin_elem(int ty, const uint8_t *a, uint8_t *b, const float d[3], int sa, int sb)
{
	float v[3];
	uint32_t h;
	uint16_t s;
	int32_t i;
	name_t n;

	switch (ty)
	{
	case SV_POS:
		memcpy(v, a, 12);
		VecAdd(v, d, v);
		memcpy(b, v, 12);
		break;
	case SV_NAME:
		memcpy(&n, a, 2);
		if (n != NAME_NONE && (n = own_name(n, sb)) == NAME_NONE)
			break;		/* not a name of B's map: B keeps its own */
		memcpy(b, &n, 2);
		break;
	case SV_RES:
		memcpy(&s, a, 2);
		if (RES_KIND(s) != RES_K_SUBMODEL)
			memcpy(b, &s, 2);
		break;
	case SV_GID:
		break;		/* B's own records */
	case SV_EH:
		memcpy(&h, a, 4);
		if (h != EH_NONE)
			h = EHANDLE(twin_ref(ent_of(h), sa));
		memcpy(b, &h, 4);
		break;
	case SV_IX16:
		memcpy(&s, a, 2);
		if (s != 0xFFFF)
		{
			i = twin_ref(s, sa);
			s = i < 0 ? 0xFFFF : (uint16_t)i;
		}
		memcpy(b, &s, 2);
		break;
	case SV_IX32:
		memcpy(&i, a, 4);
		if (i >= 0)
			i = twin_ref(i, sa);
		memcpy(b, &i, 4);
		break;
	default:
		memcpy(b, a, sv_size[ty]);
		break;
	}
}

void game_twin_copy(int from, int to)
{
	const sv_table_t *t, *ch[8];
	const uint8_t *pa;
	uint8_t *pb;
	uint16_t cflags;
	float d[3];
	int sa, sb, n, i, j, k, ty;

	if (!ent_is_live(from) || !ent_is_live(to))
		return;
	t = sv_table_of(from);
	if (!t || ent_idc(from)->cls != ent_idc(to)->cls)
	{
		log_printf("levelstate: twin %s (%d) -> %s (%d) not copied\n",
			game_class_name(ent_idc(from)->cls), from, game_class_name(ent_idc(to)->cls),
			to);
		return;
	}
	sa = ent_idc(from)->layer;
	sb = ent_idc(to)->layer;
	VecSub(ent_cv(to)->origin, ent_cv(from)->origin, d);
	cflags = chain_flags(t);
	pa = (const uint8_t *)ent_priv(from);
	pb = (uint8_t *)ent_priv(to);
	n = chain(t, ch);
	for (i = 0; i < n; i++)
		for (j = 0; j < ch[i]->nf; j++)
		{
			const sv_field_t *f = &ch[i]->f[j];

			if (f->flags & SVK)
				continue;
			ty = field_type(ch[i], f, cflags);
			for (k = 0; k < f->n; k++)
				twin_elem(ty, pa + f->ofs + k * sv_size[ty], pb + f->ofs + k * sv_size[ty],
					d, sa, sb);
		}
	ent_tm(to)->think = ent_tm(from)->think;
	ent_tm(to)->touch = ent_tm(from)->touch;
	ent_tm(to)->use = ent_tm(from)->use;
	ent_tm(to)->blocked = ent_tm(from)->blocked;
	ent_rnd(to)->effects = ent_rnd(from)->effects;
	if (ent_cv(to)->solid != ent_cv(from)->solid)
		ent_set_solid(to, ent_cv(from)->solid);
	for (i = 0; i < n; i++)
		if (ch[i]->twin)
			ch[i]->twin(from, to, d);
	ent_motion_changed(to);
}

/* ---- transitions (HLSDK CChangeLevel::ChangeList) -------------------------------- */

/* HLSDK marks an entity in the landmark's PVS movable when its ObjectCaps
 * has FCAP_ACROSS_TRANSITION and not FCAP_DONT_SAVE (FCAP_FORCE_TRANSITION
 * only skips the trigger_transition screen: CFireAndDie, which hl64's seam
 * replaces); the player crosses by itself. */
int game_carry(int e)
{
	int caps;

	if (e <= ENT_PLAYER || !ent_is_live(e) || ent_idc(e)->cls == CLS_WORLDSPAWN)
		return 0;	/* the world is in no PVS list */
	caps = ObjectCaps(e);
	return !(caps & FCAP_DONT_SAVE) && (caps & FCAP_ACROSS_TRANSITION) != 0;
}

/* ---- slot close ---------------------------------------------------------------------- */

static int in_slot(int r, int slot)
{
	return r > ENT_PLAYER && r < HL64_MAX_ENTITIES && ent_is_live(r) &&
		ent_idc(r)->layer == slot;
}

/* Clear e's references (save-table handles and slots, its owner) to
 * entities of the closing slot, as if they had gone out of range (freed:
 * every holder already copes with that). Ground and aiment links are the
 * engine's. */
static int cut_entity(int e, int slot)
{
	const sv_table_t *t = sv_table_of(e), *ch[8];
	uint8_t *p = (uint8_t *)ent_priv(e);
	int n, i, j, k, cut = 0;

	if (in_slot(ent_cv(e)->owner, slot))
	{
		ent_cv(e)->owner = COL_NONE;
		cut++;
	}
	if (!t || !p)
		return cut;
	n = chain(t, ch);
	for (i = 0; i < n; i++)
		if (ch[i]->cut)
			ch[i]->cut(e, slot);
	for (i = 0; i < n; i++)
		for (j = 0; j < ch[i]->nf; j++)
		{
			const sv_field_t *f = &ch[i]->f[j];
			uint8_t *v = p + f->ofs;
			uint32_t h;
			uint16_t s;
			int32_t x;

			for (k = 0; k < f->n; k++, v += sv_size[f->type])
				switch (f->type)
				{
				case SV_EH:
					memcpy(&h, v, 4);
					if (h != EH_NONE && in_slot(ent_of(h), slot))
					{
						elem_clear(SV_EH, v);
						cut++;
					}
					break;
				case SV_IX16:
					memcpy(&s, v, 2);
					if (s != 0xFFFF && in_slot(s, slot))
					{
						elem_clear(SV_IX16, v);
						cut++;
					}
					break;
				case SV_IX32:
					memcpy(&x, v, 4);
					if (x >= 0 && in_slot(x, slot))
					{
						elem_clear(SV_IX32, v);
						cut++;
					}
					break;
				default:
					break;
				}
		}
	return cut;
}

void game_cut_refs(int slot)
{
	CBasePlayer *pl = Player();
	int e, cut = 0;

	for (e = ENT_PLAYER; e < HL64_MAX_ENTITIES; e++)
		if (ent_is_live(e) && (e == ENT_PLAYER || ent_idc(e)->layer != slot))
			cut += cut_entity(e, slot);
	if (pl && pl->dmg_inflictor != EH_NONE && in_slot(ent_of(pl->dmg_inflictor), slot))
	{
		pl->dmg_inflictor = EH_NONE;
		cut++;
	}
	if (cut)
		log_printf("levelstate: slot %d closing: %d references cut\n", slot, cut);
}
