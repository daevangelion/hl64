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
 * aithink.c -- monster thinks spread over frames (M5: AI cost bounded per
 * frame). HLSDK's MonsterThink runs whole; a grunt's schedule selection
 * can walk a thousand CheckLocalMove steps (route triangulation, cover
 * search). Here every monster think (settings ai_spread 1) runs on a
 * cooperative context (include/hl64/co.h); at a pause point (each
 * CheckLocalMove, each cover candidate) it yields once the frame's think
 * work has reached the budget (settings ai_budget, work units: walk steps
 * and traces -- never time, so the host and the cart pause at the same
 * points), and continues at the start of the next frame. The decisions
 * are HLSDK's; only the world may have moved while a think was paused.
 *
 *   - Fairness: paused thinks continue at the frame start (game step 4) in
 *     FIFO order, oldest first; each slice runs at least one check (the one
 *     it paused before), so every paused think progresses every frame.
 *   - Shared scratch a pause would expose (CheckLocalMove memo scope, sight
 *     list, gpGlobals vectors) is saved per think (Mon_SharedSave / Load;
 *     the memo restarts on continuing); the cover search keeps its path
 *     lengths in the context's own tables (Ai_CoverTables). Pause points
 *     are outside FindNearestNode and the Dijkstra searches.
 *   - A paused think ends (dropped; a live monster's half-built route is
 *     cleared) when its monster is freed or changes layer, is killed, its
 *     think function, schedule, task, monster state, enemy or target
 *     entity is changed by something else or its enemy / target entity is
 *     freed, or at a map open / close, a dehydrate or twin copy, New Game.
 *   - Entity indices a paused think holds come from its enemy / target
 *     handles (above: still the same live entities) or are read fresh
 *     after the pause point (the check's blocker); entity slots are not
 *     reused within 0.5 s of their free (engine/ent/ent.c). A think older
 *     than AI_MAX_AGE stops pausing and runs to its end (a bound on how
 *     old its picture of the world gets). Thinks before AI_MIN_START (the
 *     boot window where freed slots are reused at once) run whole.
 *   - More paused thinks than contexts: a new think waits a frame.
 *   - Held thinks: a new think dispatched once the frame's think work has
 *     reached the budget, or while older thinks wait, does not start: it
 *     joins a FIFO queue and its nextthink stays due (its due time, so a
 *     dehydrate, twin copy or New Game sees an ordinary due think). At
 *     the next frame start, after the paused thinks' slices, queued
 *     thinks start oldest first (as the engine dispatches a think:
 *     nextthink := 0, the frame's time) while the frame's work is under
 *     the budget; one queued for settings ai_hold (HL64_AI_HOLD 0.2 s)
 *     of game time starts regardless (lateness bound; without a free
 *     context it runs whole at its next dispatch). A queued think leaves
 *     the queue unstarted when its entity is freed, its think function
 *     or nextthink is changed by something else, at a dehydrate / twin
 *     copy (Ai_Drop),
 *     with ai_spread != 1, or when the queue (AI_QUEUE) is full (it runs
 *     at once).
 *   - ai_spread 2 (test): pause at every point after the slice's first and
 *     continue at once after scribbling the shared scratch: decides
 *     exactly as ai_spread 0 (a frozen world); nothing is held.
 *
 * The engine dispatches the monster's think while it is paused or queued
 * (its nextthink came due): that call is absorbed and the think re-armed
 * (paused: 0.1 s on, HLSDK MonsterThink's own first statement; queued:
 * its due time).
 */
#include <string.h>

#include "hl64/co.h"
#include "hl64/settings.h"
#include "game/hl/monster.h"

#define AI_MAX_AGE	2.0f
#define AI_MIN_START	2.5f
#define AI_MAX_HOLD	(settings.ai_hold)	/* s: a queued think's lateness bound */
#define AI_QUEUE	64

enum { AC_FREE, AC_RUN, AC_PAUSED, AC_DONE };

typedef struct {
	int		e, state, drop;
	ent_h		h;
	int		layer;
	CBaseMonster	*m;
	fn_t		think;
	float		t_start;
	uint32_t	order, slice_points, frame_seen;
	uint8_t		overrun;
	/* the monster at the pause: anything else changing these ends it */
	const Schedule_t *sched;
	int		sidx, tstat, mstate, dead;
	ent_h		enemy, target;
	mon_shared_t	sh;
	mon_think_acc_t	acc;
} ai_ctx_t;

/* counts since boot (`stats N ai:`: work units, host = cart) */
static struct {
	uint32_t	spread, pauses, resumes, drops, defers, overruns, absorbed;
	uint32_t	max_paused, max_frames;
	uint32_t	holds, held_runs, held_aged, held_whole, held_drops, max_held, max_late_ms;
} ac;

/* queued (held) thinks, oldest first */
typedef struct {
	int		e;
	ent_h		h;
	fn_t		think;
	float		due, t_held;
} ai_held_t;

static ai_held_t	held[AI_QUEUE];
static int		nheld;

static ai_ctx_t		ctx[HL64_AI_CONTEXTS];
static ai_ctx_t		*cur;		/* the running context */
static mon_shared_t	main_sh;
static uint32_t		order_seq, frame_no;

static int ai_index(const ai_ctx_t *c) { return (int)(c - ctx); }

static void ai_entry(void)
{
	for (;;)
	{
		Mon_ThinkRun(cur->e);
		cur->state = AC_DONE;
		co_yield();
	}
}

float *Ai_CoverTables(void)
{
	return cur ? (float *)co_scratch(ai_index(cur)) : NULL;
}

/* a handle the pause kept: still the same handle and, unless none, a live
 * entity */
static int ai_handle_ok(ent_h now, ent_h then)
{
	return now == then && (now == EH_NONE || ent_of(now) >= 0);
}

/* 1 while nothing but the think itself changed the monster since its
 * pause; 0 when its think must end (freed: also !*live) */
static int ai_valid(const ai_ctx_t *c, int *live)
{
	const CBaseMonster *m;
	int e = c->e;

	*live = ent_of(c->h) == e && ent_is_live(e) && MON(e) == c->m;
	if (!*live)
		return 0;
	m = c->m;
	return ent_idc(e)->layer == c->layer && ent_tm(e)->think == c->think &&
		m->m_pSchedule == c->sched && m->m_iScheduleIndex == c->sidx &&
		m->m_iTaskStatus == c->tstat && m->m_MonsterState == c->mstate &&
		m->deadflag == c->dead && ai_handle_ok(m->m_hEnemy, c->enemy) &&
		ai_handle_ok(m->m_hTargetEnt, c->target);
}

static void ai_free(ai_ctx_t *c)
{
	c->state = AC_FREE;
	c->e = -1;
	c->drop = 0;
}

/* end a paused think here: its stack is abandoned (it holds nothing but
 * its own frames); a live monster loses the route it was building */
static void ai_drop(ai_ctx_t *c)
{
	int live = ent_of(c->h) == c->e && ent_is_live(c->e) && MON(c->e) == c->m;

	if (live)
		Mon_RouteClear(c->e);
	ac.drops++;
	ai_free(c);
}

static void ai_snapshot(ai_ctx_t *c)
{
	const CBaseMonster *m = c->m;

	c->sched = m->m_pSchedule;
	c->sidx = m->m_iScheduleIndex;
	c->tstat = m->m_iTaskStatus;
	c->mstate = m->m_MonsterState;
	c->dead = m->deadflag;
	c->enemy = m->m_hEnemy;
	c->target = m->m_hTargetEnt;
}

/* One slice of c's think: from its start or pause to its next pause or
 * end. Main stack only. */
static void ai_slice(ai_ctx_t *c)
{
	int resumed = c->state == AC_PAUSED;

	Mon_SharedSave(&main_sh);
	if (resumed)
	{
		Mon_SharedLoad(&c->sh, 1);
		ac.resumes++;
	}
	if (c->frame_seen != frame_no)
	{
		c->frame_seen = frame_no;
		c->acc.frames++;
	}
	c->state = AC_RUN;
	c->slice_points = resumed;	/* the check it paused before is this slice's progress */
	cur = c;
	Mon_SliceBegin(&c->acc);
	co_resume(ai_index(c));
	Mon_SliceEnd(&c->acc);
	cur = NULL;
	if (c->state == AC_PAUSED)
	{
		Mon_SharedSave(&c->sh);
		Mon_SharedLoad(&main_sh, 0);
		ai_snapshot(c);
		ac.pauses++;
		if (c->drop)
			ai_drop(c);
		return;
	}
	/* ended: the shared scratch stays as the think left it (as a whole
	 * think leaves it) */
	if (c->acc.frames > ac.max_frames)
		ac.max_frames = c->acc.frames;
	Mon_ThinkEnd(c->e, &c->acc);
	ai_free(c);
}

static ai_ctx_t *ai_of(int e)
{
	int i;

	for (i = 0; i < HL64_AI_CONTEXTS; i++)
		if (ctx[i].state == AC_PAUSED && ctx[i].e == e)
			return &ctx[i];
	return NULL;
}

static ai_ctx_t *ai_free_ctx(void)
{
	int i;

	for (i = 0; i < HL64_AI_CONTEXTS; i++)
		if (ctx[i].state == AC_FREE)
			return &ctx[i];
	return NULL;
}

/* start e's think on context c (its first slice) */
static void ai_begin(ai_ctx_t *c, int e, int mode)
{
	memset(c, 0, sizeof(*c));
	c->e = e;
	c->h = ent_handle_of(e);
	c->layer = ent_idc(e)->layer;
	c->m = MON(e);
	c->think = ent_tm(e)->think;
	c->t_start = gtime();
	c->order = ++order_seq;
	c->frame_seen = frame_no - 1;
	ac.spread++;
	Mon_ThinkBegin(e, &c->acc);
	co_start(ai_index(c), ai_entry);
	ai_slice(c);
	while (mode == 2 && c->state == AC_PAUSED && c->e == e)
	{
		/* frozen world: what another think would leave, then continue */
		Mon_SharedScribble();
		Graph_ScribbleScratch();
		c->frame_seen = frame_no;
		ai_slice(c);
	}
}

static int held_find(int e)
{
	int i;

	for (i = 0; i < nheld; i++)
		if (held[i].e == e)
			return i;
	return -1;
}

static void held_remove(int i)
{
	nheld--;
	memmove(&held[i], &held[i + 1], (size_t)(nheld - i) * sizeof(held[0]));
}

/* the queued think is still the one the engine would dispatch: the same
 * live entity, think function and due nextthink */
static int held_valid(const ai_held_t *h)
{
	int e = h->e;

	return ent_of(h->h) == e && ent_is_live(e) && ent_tm(e)->think == h->think &&
		ent_tm(e)->nextthink == h->due;
}

static void held_late(const ai_held_t *h)
{
	uint32_t ms = (uint32_t)((gtime() - h->t_held) * 1000.0f + 0.5f);

	if (ms > ac.max_late_ms)
		ac.max_late_ms = ms;
}

int Ai_Think(int e)
{
	ai_ctx_t	*c;
	int		live, q, mode = settings.ai_spread;

	if (mode == 0 || co_running() >= 0)
		return 0;	/* whole; a think nested in a think runs inline */
	c = ai_of(e);
	if (c)
	{
		if (ai_valid(c, &live))
		{
			/* still thinking: this dispatch is absorbed */
			ac.absorbed++;
			if (!(ent_tm(e)->nextthink > 0))
				ThinkIn(e, 0.1f);
			return 1;
		}
		ai_drop(c);
	}
	if (gtime() < AI_MIN_START)
		return 0;
	q = held_find(e);
	if (q >= 0 && ent_of(held[q].h) != e)
	{
		/* a stale entry (its entity was freed) */
		held_remove(q);
		ac.held_drops++;
		q = -1;
	}
	if (q >= 0)
	{
		/* queued: the frame start starts it; this dispatch is absorbed
		 * until it is AI_MAX_HOLD late, then it starts here */
		if (mode == 1 && gtime() - held[q].t_held < AI_MAX_HOLD)
		{
			ac.absorbed++;
			ent_think_at(e, held[q].due);
			return 1;
		}
		held_late(&held[q]);
		held_remove(q);
		if (mode == 1)
		{
			ac.held_aged++;
			c = ai_free_ctx();
			if (!c)
			{
				ac.held_whole++;
				return 0;
			}
			ai_begin(c, e, mode);
			return 1;
		}
	}
	else if (mode == 1 && (nheld > 0 || Mon_FrameUnits(NULL) >= (uint32_t)settings.ai_budget) &&
		nheld < AI_QUEUE && (ent_tm(e)->think == FN_CBaseMonster_CallMonsterThink ||
		ent_tm(e)->think == FN_CBaseMonster_MonsterThink))
	{
		/* held: queued behind the older ones, nextthink due (only a think
		 * that is MonsterThink itself: the frame start runs its body) */
		held[nheld].e = e;
		held[nheld].h = ent_handle_of(e);
		held[nheld].think = ent_tm(e)->think;
		held[nheld].due = gtime();
		held[nheld].t_held = gtime();
		nheld++;
		if ((uint32_t)nheld > ac.max_held)
			ac.max_held = (uint32_t)nheld;
		ac.holds++;
		ent_think_at(e, gtime());
		return 1;
	}
	c = ai_free_ctx();
	if (!c)
	{
		/* every context holds a paused think: this one waits a frame */
		ac.defers++;
		ent_think_at(e, gtime());
		return 1;
	}
	ai_begin(c, e, mode);
	return 1;
}

void Ai_PausePoint(int e)
{
	ai_ctx_t *c = cur;

	if (!c || c->e != e || co_running() != ai_index(c))
		return;
	if (c->slice_points++ == 0 || c->overrun)
		return;
	if (settings.ai_spread == 1)
	{
		if (Mon_FrameUnits(&c->acc) < (uint32_t)settings.ai_budget)
			return;
		if (gtime() - c->t_start >= AI_MAX_AGE)
		{
			c->overrun = 1;
			ac.overruns++;
			return;
		}
	}
	c->state = AC_PAUSED;
	co_yield();
}

void Ai_FrameStart(void)
{
	ai_ctx_t	*c, *order[HL64_AI_CONTEXTS];
	int		i, j, n = 0, live, layer;

	frame_no++;
	if (settings.ai_spread != 1)
	{
		Ai_DropAll();
		return;
	}
	for (i = 0; i < HL64_AI_CONTEXTS; i++)
	{
		c = &ctx[i];
		if (c->state != AC_PAUSED)
			continue;
		if (!ai_valid(c, &live))
		{
			ai_drop(c);
			continue;
		}
		/* oldest first */
		for (j = n; j > 0 && order[j - 1]->order > c->order; j--)
			order[j] = order[j - 1];
		order[j] = c;
		n++;
	}
	if ((uint32_t)n > ac.max_paused)
		ac.max_paused = (uint32_t)n;
	layer = eng_frame_layer();
	for (i = 0; i < n; i++)
	{
		c = order[i];
		/* an earlier slice may have changed this one's monster */
		if (c->state != AC_PAUSED || !ai_valid(c, &live))
		{
			if (c->state == AC_PAUSED)
				ai_drop(c);
			continue;
		}
		eng_set_frame_layer(c->layer);
		ai_slice(c);
	}
	/* queued thinks, oldest first, while the frame's work is under the
	 * budget; one AI_MAX_HOLD late starts regardless (the queue is in
	 * hold order: the late ones lead) */
	while (nheld > 0)
	{
		ai_held_t h = held[0];
		int aged;

		if (!held_valid(&h))
		{
			held_remove(0);
			ac.held_drops++;
			continue;
		}
		aged = gtime() - h.t_held >= AI_MAX_HOLD;
		if (!aged && Mon_FrameUnits(NULL) >= (uint32_t)settings.ai_budget)
			break;
		c = ai_free_ctx();
		if (!c)
			break;
		held_remove(0);
		held_late(&h);
		ac.held_runs++;
		ac.held_aged += (uint32_t)aged;
		/* as the engine dispatches a think: nextthink := 0 */
		ent_think_at(h.e, 0);
		eng_set_frame_layer(ent_idc(h.e)->layer);
		ai_begin(c, h.e, 1);
	}
	eng_set_frame_layer(layer);
}

void Ai_Drop(int e)
{
	ai_ctx_t *c;
	int i;

	for (i = 0; i < HL64_AI_CONTEXTS; i++)
	{
		c = &ctx[i];
		if (c->state == AC_FREE || c->e != e)
			continue;
		if (c == cur)
			c->drop = 1;	/* the running think: at its pause */
		else if (c->state == AC_PAUSED)
			ai_drop(c);
	}
	/* a queued think leaves the queue; its nextthink stays due */
	i = held_find(e);
	if (i >= 0)
	{
		held_remove(i);
		ac.held_drops++;
	}
}

void Ai_DropAll(void)
{
	int i;

	for (i = 0; i < HL64_AI_CONTEXTS; i++)
	{
		if (&ctx[i] == cur)
			ctx[i].drop = 1;
		else if (ctx[i].state == AC_PAUSED)
			ai_drop(&ctx[i]);
	}
	ac.held_drops += (uint32_t)nheld;
	nheld = 0;
}

void Ai_PrintCounts(void)
{
	log_printf(" spread=%lu pauses=%lu resumes=%lu drops=%lu defers=%lu overruns=%lu absorbed=%lu "
		"max_paused=%lu max_frames=%lu holds=%lu held_runs=%lu held_aged=%lu held_whole=%lu "
		"held_drops=%lu max_held=%lu max_late_ms=%lu", (unsigned long)ac.spread,
		(unsigned long)ac.pauses, (unsigned long)ac.resumes, (unsigned long)ac.drops,
		(unsigned long)ac.defers, (unsigned long)ac.overruns, (unsigned long)ac.absorbed,
		(unsigned long)ac.max_paused, (unsigned long)ac.max_frames, (unsigned long)ac.holds,
		(unsigned long)ac.held_runs, (unsigned long)ac.held_aged, (unsigned long)ac.held_whole,
		(unsigned long)ac.held_drops, (unsigned long)ac.max_held, (unsigned long)ac.max_late_ms);
}

/* `prof ai_ctx` (after `prof ai`): the counts and each context's deepest
 * stack use (bytes of HL64_AI_STACK_BYTES; machine-dependent) */
void Ai_ProfLine(void)
{
	int i;

	log_printf("prof ai_ctx");
	Ai_PrintCounts();
	log_printf(" stack=");
	for (i = 0; i < HL64_AI_CONTEXTS; i++)
		log_printf("%s%lu", i ? "," : "", (unsigned long)co_stack_peak(i));
	log_printf("/%d\n", HL64_AI_STACK_BYTES);
}
