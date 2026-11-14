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
 *     think function, schedule, task or monster state is changed by
 *     something else, or at a map open / close, a dehydrate or twin copy,
 *     New Game.
 *   - Age: entity slots are not reused within 0.5 s of their free
 *     (engine/ent/ent.c), so an entity index a paused think holds stays
 *     valid while the think is younger than AI_MAX_AGE; an older think
 *     stops pausing and runs to its end. Thinks before AI_MIN_START (the
 *     boot window where freed slots are reused at once) run whole.
 *   - More paused thinks than contexts: a new think waits a frame.
 *   - ai_spread 2 (test): pause at every point after the slice's first and
 *     continue at once after scribbling the shared scratch: decides
 *     exactly as ai_spread 0 (a frozen world).
 *
 * The engine dispatches the monster's think while it is paused (its
 * nextthink came due): that call is absorbed and the think re-armed 0.1 s
 * on (HLSDK MonsterThink's own first statement).
 */
#include <string.h>

#include "hl64/co.h"
#include "hl64/settings.h"
#include "game/hl/monster.h"

#define AI_MAX_AGE	0.45f
#define AI_MIN_START	2.5f

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
	mon_shared_t	sh;
	mon_think_acc_t	acc;
} ai_ctx_t;

/* counts since boot (`stats N ai:`: work units, host = cart) */
static struct {
	uint32_t	spread, pauses, resumes, drops, defers, overruns, absorbed;
	uint32_t	max_paused, max_frames;
} ac;

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
		m->deadflag == c->dead;
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

int Ai_Think(int e)
{
	ai_ctx_t	*c;
	int		i, live, mode = settings.ai_spread;

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
	for (i = 0, c = NULL; i < HL64_AI_CONTEXTS; i++)
		if (ctx[i].state == AC_FREE)
		{
			c = &ctx[i];
			break;
		}
	if (!c)
	{
		/* every context holds a paused think: this one waits a frame */
		ac.defers++;
		ent_think_at(e, gtime());
		return 1;
	}
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
}

void Ai_PrintCounts(void)
{
	log_printf(" spread=%lu pauses=%lu resumes=%lu drops=%lu defers=%lu overruns=%lu absorbed=%lu "
		"max_paused=%lu max_frames=%lu", (unsigned long)ac.spread, (unsigned long)ac.pauses,
		(unsigned long)ac.resumes, (unsigned long)ac.drops, (unsigned long)ac.defers,
		(unsigned long)ac.overruns, (unsigned long)ac.absorbed, (unsigned long)ac.max_paused,
		(unsigned long)ac.max_frames);
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
