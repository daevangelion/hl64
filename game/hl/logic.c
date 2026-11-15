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
 * logic.c -- the entity-less logic records (architecture.md 6.4): HLSDK
 * dlls/triggers.cpp CMultiManager, CTriggerRelay, CAutoTrigger and
 * dlls/buttons.cpp CMultiSource, CEnvGlobal. Their state lives in the
 * record's 16-byte rec_logic_t; their thinks are record timers that keep
 * HLSDK's f32 nextthink values (think-and-time.md 5: ManagerThink's float
 * comparison runs here unchanged). The cooked fire list of a multi_manager
 * is already in HLSDK's bubble-sort order; multisource inputs are the
 * cooked CMultiSource::Register list (records whose target names it, then
 * multi_managers firing it).
 */
#include "game/hl/util.h"
#include "game/hl/savegame.h"

/* SUB_UseTargets of a logic record (CBaseDelay semantics): delay -> a
 * delayed fire (the DelayedUse entity's work); else killtarget, target. */
static void LogicUseTargets(gid_t16 gid, name_t target, name_t kill, float delay, int activator,
	int use_type, float value)
{
	if (target == NAME_NONE && kill == NAME_NONE)
		return;
	if (delay != 0)
	{
		GameFireAt(gtime() + delay, target, kill, IsPlayer(activator) ? activator : -1, -1,
			use_type, 0);
		return;
	}
	if (kill != NAME_NONE)
		KillTargets(kill);
	if (target != NAME_NONE)
		GameFire(target, activator, -1, gid, use_type, value);
}

static name_t key_name(gid_t16 gid, int k)
{
	return rec_key_name(gid, (key_t16)k);
}

/* triggerstate: 0 off, 2 toggle, else on */
static int trigger_type(gid_t16 gid)
{
	switch ((int)KeyF(gid, K_TRIGGERSTATE, 1))
	{
	case 0:
		return USE_OFF;
	case 2:
		return USE_TOGGLE;
	default:
		return USE_ON;
	}
}

/* ---- multi_manager ----------------------------------------------------------- */

#define SF_MULTIMAN_THREAD	0x00000001
#define MAX_MULTI_TARGETS	16
#define MM_BUSY			1	/* use disabled until every target fired */
#define MM_CLONES		16

/* rec_logic_t: a16 = m_index, a = m_hActivator, f = m_startTime, b = the
 * base manager's nextthink bits. Thread clones (SF_MULTIMAN_THREAD) are
 * game-side; a record has one timer (rec_timer replaces), so it is kept at
 * the earliest nextthink of the base and its clones, and each due one
 * thinks when it fires (later ones already due this frame run in the same
 * pass: think-and-time.md 5). */
typedef struct {
	float		m_startTime;
	float		nextthink;	/* m_startTime + delay[m_index] (f32) */
	ent_h		m_hActivator;
	gid_t16		gid;		/* GID_NONE: free */
	uint16_t	m_index;
} mm_clone_t;
static mm_clone_t	mm_clones[MM_CLONES];

static int mm_count(gid_t16 gid)
{
	int n = rec_aux_count(gid);

	if (n > MAX_MULTI_TARGETS)
	{
		log_printf("game: multi_manager map %u lump %u: %d targets, HLSDK keeps %d\n",
			(unsigned)GID_SLOT(gid), (unsigned)GID_LUMP(gid), n, MAX_MULTI_TARGETS);
		n = MAX_MULTI_TARGETS;
	}
	return n;
}

static void CMultiManager_LogicSpawn(gid_t16 gid)
{
	rec_logic_t *l = rec_logic(gid);

	l->flags = 0;
	l->a16 = 0;
	l->a = (int32_t)EH_NONE;
	l->f = 0;
}

static float mm_base_next(rec_logic_t *l)
{
	union { int32_t i; float f; } u;

	u.i = l->b;
	return u.f;
}

static void mm_set_base_next(rec_logic_t *l, float t)
{
	union { int32_t i; float f; } u;

	u.f = t;
	l->b = u.i;
}

/* the record's timer at the earliest pending nextthink, or none */
static void mm_schedule(gid_t16 gid)
{
	rec_logic_t *l = rec_logic(gid);
	float best = 0;
	int i, have = 0, act = -1;

	if (l->flags & MM_BUSY)
	{
		best = mm_base_next(l);
		act = EHGet((ent_h)l->a);
		have = 1;
	}
	for (i = 0; i < MM_CLONES; i++)
	{
		if (mm_clones[i].gid != gid)
			continue;
		if (!have || mm_clones[i].nextthink < best)
		{
			best = mm_clones[i].nextthink;
			act = EHGet(mm_clones[i].m_hActivator);
		}
		have = 1;
	}
	if (have)
		rec_timer(gid, best, 0, act);
	else
		rec_timer_cancel(gid);
}

/* ManagerThink on the base record (clone < 0) or a clone: fires the due
 * targets, stores the next f32 nextthink or ends the run */
static void ManagerThink(gid_t16 gid, int clone)
{
	rec_logic_t *l = rec_logic(gid);
	mm_clone_t *c = clone >= 0 ? &mm_clones[clone] : 0;
	int count = mm_count(gid), index;
	float time, start, delay;
	ent_h act;
	name_t name;

	start = c ? c->m_startTime : l->f;
	act = c ? c->m_hActivator : (ent_h)l->a;
	index = c ? c->m_index : l->a16;
	time = gtime() - start;
	for (;;)
	{
		if (index >= count)
			break;
		rec_fire_entry(gid, index, &name, &delay);
		if (!(delay <= time))
			break;
		index++;
		if (c)
			c->m_index = (uint16_t)index;
		else
			l->a16 = (uint16_t)index;
		GameFire(name, EHGet(act), -1, gid, USE_TOGGLE, 0);
		if (rec_state(gid) == REC_KILLED)
			return;
	}
	if (index >= count)
	{
		if (c)
			c->gid = GID_NONE;	/* UTIL_Remove(clone) */
		else
			l->flags &= ~MM_BUSY;	/* SetUse(ManagerUse) */
		return;
	}
	rec_fire_entry(gid, index, &name, &delay);
	if (c)
		c->nextthink = start + delay;
	else
		mm_set_base_next(l, start + delay);
}

static void CMultiManager_LogicTimer(gid_t16 gid, uint16_t arg, int activator)
{
	rec_logic_t *l = rec_logic(gid);
	float t = gtime();
	int i;

	(void)arg;
	(void)activator;
	/* every think of this record due now, in slot order (base first) */
	if ((l->flags & MM_BUSY) && mm_base_next(l) <= t)
		ManagerThink(gid, -1);
	for (i = 0; i < MM_CLONES && rec_state(gid) != REC_KILLED; i++)
		if (mm_clones[i].gid == gid && mm_clones[i].nextthink <= t)
			ManagerThink(gid, i);
	if (rec_state(gid) != REC_KILLED)
		mm_schedule(gid);
}

static void CMultiManager_LogicUse(gid_t16 gid, int activator, int caller, int use_type,
	float value)
{
	rec_logic_t *l = rec_logic(gid);
	float t = gtime();
	int i;

	(void)caller;
	(void)use_type;
	(void)value;
	if ((uint32_t)KeyF(gid, K_SPAWNFLAGS, 0) & SF_MULTIMAN_THREAD)
	{
		/* Clone()->ManagerUse */
		for (i = 0; i < MM_CLONES; i++)
			if (mm_clones[i].gid == GID_NONE)
				break;
		if (i == MM_CLONES)
		{
			log_printf("game: multi_manager map %u lump %u: no free thread\n",
				(unsigned)GID_SLOT(gid), (unsigned)GID_LUMP(gid));
			return;
		}
		mm_clones[i].gid = gid;
		mm_clones[i].m_hActivator = EHANDLE(activator);
		mm_clones[i].m_index = 0;
		mm_clones[i].m_startTime = t;
		mm_clones[i].nextthink = t;
		mm_schedule(gid);
		return;
	}
	if (l->flags & MM_BUSY)
		return;		/* SetUse(NULL) while firing */
	l->a = (int32_t)EHANDLE(activator);
	l->a16 = 0;
	l->f = t;
	l->flags |= MM_BUSY;
	mm_set_base_next(l, t);
	mm_schedule(gid);
}

const game_class_t gc_MULTI_MANAGER = {
	.LogicSpawn = CMultiManager_LogicSpawn,
	.LogicUse = CMultiManager_LogicUse,
	.LogicTimer = CMultiManager_LogicTimer,
};

void game_logic_reset(void)
{
	int i;

	for (i = 0; i < MM_CLONES; i++)
		mm_clones[i].gid = GID_NONE;
}

uint32_t MM_SaveState(int op)
{
	uint32_t n = sg_io(op, mm_clones, sizeof(mm_clones));
	int i;

	if (op == SG_GET)
		for (i = 0; i < MM_CLONES; i++)
			if (mm_clones[i].gid != GID_NONE && mm_clones[i].m_index > MAX_MULTI_TARGETS)
			{
				sv_fail("game: multi_manager clone index out of range");
				game_logic_reset();
				break;
			}
	return n;
}

/* ---- trigger_relay -------------------------------------------------------- */

#define SF_RELAY_FIREONCE	0x0001

/* rec_logic_t: flags = triggerType | FIREONCE << 7, a16 = target,
 * a = m_iszKillTarget, f = m_flDelay */
static void CTriggerRelay_LogicSpawn(gid_t16 gid)
{
	rec_logic_t *l = rec_logic(gid);

	l->flags = (uint8_t)trigger_type(gid);
	if ((uint32_t)KeyF(gid, K_SPAWNFLAGS, 0) & SF_RELAY_FIREONCE)
		l->flags |= 0x80;
	l->a16 = key_name(gid, K_TARGET);
	l->a = key_name(gid, K_KILLTARGET);
	l->f = KeyF(gid, K_DELAY, 0);
}

static void CTriggerRelay_LogicUse(gid_t16 gid, int activator, int caller, int use_type,
	float value)
{
	rec_logic_t *l = rec_logic(gid);
	int once = l->flags & 0x80;

	(void)activator;
	(void)caller;
	(void)use_type;
	(void)value;
	/* SUB_UseTargets(this, triggerType, 0): the relay is the activator */
	LogicUseTargets(gid, l->a16, (name_t)l->a, l->f, -1, l->flags & 3, 0);
	if (once)
	{
		rec_timer_cancel(gid);
		rec_kill(gid);
	}
}

const game_class_t gc_TRIGGER_RELAY = {
	.LogicSpawn = CTriggerRelay_LogicSpawn,
	.LogicUse = CTriggerRelay_LogicUse,
};

/* ---- trigger_auto --------------------------------------------------------- */

#define SF_AUTO_FIREONCE	0x0001

/* rec_logic_t: as trigger_relay, b = m_globalstate (-1 none) */
static void CAutoTrigger_LogicSpawn(gid_t16 gid)
{
	rec_logic_t *l = rec_logic(gid);

	CTriggerRelay_LogicSpawn(gid);
	l->flags &= 3;
	if ((uint32_t)KeyF(gid, K_SPAWNFLAGS, 0) & SF_AUTO_FIREONCE)
		l->flags |= 0x80;
	l->b = rec_key_global(gid, K_GLOBALSTATE);
	rec_timer(gid, gtime() + 0.1f, 0, -1);		/* Precache: nextthink */
}

static void CAutoTrigger_LogicTimer(gid_t16 gid, uint16_t arg, int activator)
{
	rec_logic_t *l = rec_logic(gid);

	(void)arg;
	(void)activator;
	if (l->b < 0 || GlobalState(l->b) == GLOBAL_ON)
	{
		LogicUseTargets(gid, l->a16, (name_t)l->a, l->f, -1, l->flags & 3, 0);
		if (l->flags & 0x80)
			rec_kill(gid);
	}
}

/* DispatchRestore: Restore, then Precache: nextthink = time + 0.1 again
 * (a level revisited re-tests the global, e.g. hldemo1's dish-state
 * trigger_autos once hldemo3 set hldemo_global_dish_ms) */
static void CAutoTrigger_LogicRestored(gid_t16 gid)
{
	rec_timer_cancel(gid);
	rec_timer(gid, gtime() + 0.1f, 0, -1);
}

const game_class_t gc_TRIGGER_AUTO = {
	.LogicSpawn = CAutoTrigger_LogicSpawn,
	.LogicTimer = CAutoTrigger_LogicTimer,
	.LogicRestored = CAutoTrigger_LogicRestored,
};

/* ---- env_global ------------------------------------------------------------- */

#define SF_GLOBAL_SET		1

/* rec_logic_t: a = m_globalstate, a16 = m_triggermode */
static void CEnvGlobal_LogicSpawn(gid_t16 gid)
{
	rec_logic_t *l = rec_logic(gid);

	l->a = rec_key_global(gid, K_GLOBALSTATE);
	l->a16 = (uint16_t)(int)KeyF(gid, K_TRIGGERMODE, 0);
	if (l->a < 0)
	{
		game_refuse_log(rec_class(gid), gid, -1,
			"CEnvGlobal::Spawn: no globalstate (REMOVE_ENTITY)");
		rec_kill(gid);
		return;
	}
	if (((uint32_t)KeyF(gid, K_SPAWNFLAGS, 0) & SF_GLOBAL_SET) && !GlobalInTable(l->a))
		GlobalAdd(l->a, (int)KeyF(gid, K_INITIALSTATE, 0));
}

static void CEnvGlobal_LogicUse(gid_t16 gid, int activator, int caller, int use_type,
	float value)
{
	rec_logic_t *l = rec_logic(gid);
	int old = GlobalState(l->a), state;

	(void)activator;
	(void)caller;
	(void)use_type;
	(void)value;
	switch (l->a16)
	{
	case 0:
		state = GLOBAL_OFF;
		break;
	case 1:
		state = GLOBAL_ON;
		break;
	case 2:
		state = GLOBAL_DEAD;
		break;
	default:
		state = old == GLOBAL_ON ? GLOBAL_OFF : old == GLOBAL_OFF ? GLOBAL_ON : old;
		break;
	}
	if (GlobalInTable(l->a))
		GlobalSetState(l->a, state);
	else
		GlobalAdd(l->a, state);
}

const game_class_t gc_ENV_GLOBAL = {
	.LogicSpawn = CEnvGlobal_LogicSpawn,
	.LogicUse = CEnvGlobal_LogicUse,
};

/* ---- multisource -------------------------------------------------------------- */

#define SF_MULTI_INIT		1
#define MS_MAX_TARGETS		32

/* rec_logic_t: flags = SF_MULTI_INIT until Register, a = m_rgTriggered
 * bits, a16 = m_globalstate + 1, b = target */
static int ms_total(gid_t16 gid)
{
	int n = rec_aux_count(gid);

	return n > MS_MAX_TARGETS ? MS_MAX_TARGETS : n;
}

static void CMultiSource_LogicSpawn(gid_t16 gid)
{
	rec_logic_t *l = rec_logic(gid);

	l->flags = SF_MULTI_INIT;
	l->a = 0;
	l->a16 = (uint16_t)(rec_key_global(gid, K_GLOBALSTATE) + 1);
	l->b = key_name(gid, K_TARGET);
	rec_timer(gid, gtime() + 0.1f, 0, -1);		/* SetThink(Register) */
}

/* Register: the input list is cooked */
static void CMultiSource_LogicTimer(gid_t16 gid, uint16_t arg, int activator)
{
	(void)arg;
	(void)activator;
	rec_logic(gid)->flags &= ~SF_MULTI_INIT;
}

static int CMultiSource_IsTriggered(gid_t16 gid, int activator)
{
	rec_logic_t *l = rec_logic(gid);
	int i, total = ms_total(gid);

	(void)activator;
	if (l->flags & SF_MULTI_INIT)
		return 0;
	for (i = 0; i < total; i++)
		if (!(l->a & (1 << i)))
			break;
	if (i == total)
	{
		if (!l->a16 || GlobalState(l->a16 - 1) == GLOBAL_ON)
			return 1;
	}
	return 0;
}

static void CMultiSource_LogicUse(gid_t16 gid, int activator, int caller, int use_type,
	float value)
{
	rec_logic_t *l = rec_logic(gid);
	gid_t16 from = caller >= 0 ? ent_idc(caller)->gid : rec_caller_gid();
	int i = 0, total = ms_total(gid);

	(void)use_type;
	(void)value;
	/* HLSDK: the search leaves i == m_iTotal for a non-member, whose use
	 * then toggles the last input (the "used by non member" message is
	 * unreachable) */
	while (i < total)
		if (rec_aux_gid(gid, i++) == from)
			break;
	if (i == 0)
		return;
	l->a ^= 1 << (i - 1);
	if (CMultiSource_IsTriggered(gid, activator))
	{
		log_printf("multisource: map %u lump %u enabled (%d inputs)\n",
			(unsigned)GID_SLOT(gid), (unsigned)GID_LUMP(gid), total);
		/* CBaseEntity::SUB_UseTargets(NULL, ...) */
		if ((name_t)l->b != NAME_NONE)
			GameFire((name_t)l->b, -1, -1, gid, l->a16 ? USE_ON : USE_TOGGLE, 0);
	}
}

const game_class_t gc_MULTISOURCE = {
	.flags = GC_MASTER,
	.LogicSpawn = CMultiSource_LogicSpawn,
	.LogicUse = CMultiSource_LogicUse,
	.LogicTimer = CMultiSource_LogicTimer,
	.LogicIsTriggered = CMultiSource_IsTriggered,
};
