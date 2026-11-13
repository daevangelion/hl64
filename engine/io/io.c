/* SPDX-License-Identifier: MIT */
/*
 * io.c -- cart read scheduler (io.h), imported from the old tree's
 * q64_io.c (audit entry n64/port/q64_io.c CLEAN); the cart / host
 * backends moved behind sys.h's sys_pi_* (plat/n64/cart.c,
 * plat/host/io_host.c).
 */
#include <string.h>

#include "engine/core/log.h"
#include "engine/core/sys.h"
#include "engine/io/io.h"

static io_req_t	*q_head[IO_NPRIO], *q_tail[IO_NPRIO];
static int		q_len;
static io_req_t	*act;		/* request owning the piece in flight */
static uint32_t		act_len;	/* bytes of that piece */
static uint32_t		budget = IO_BUDGET;
static uint32_t		fr_async, fr_blocked_us, fr_us;
static uint32_t		last_async, last_blocked;
static io_stats_t	st;
static int		loading;	/* io_set_load depth */
static int		booting;	/* boot: until the first io_frame_begin */
static uint32_t		q_bytes;	/* remaining bytes of queued + active requests */
static uint32_t		fr_pi_us;	/* modelled PI busy time this frame (host) */
/* this frame's sync reads, audio reads and in-flight waits (the > 1 ms
 * report) */
static uint32_t		fr_sync_calls, fr_sync_bytes, fr_sync_max, fr_sync_max_rom;
static uint32_t		fr_audio_calls, fr_drain_us;
/* since boot, never reset by io_stats */
static uint32_t		life_frames, life_blocked_max, life_blocked_1ms, life_budget_over;
static uint32_t		life_blocked_1ms_tele;	/* > 1 ms frames inside io_tele_window */
static uint32_t		tele_until;	/* io_tele_window: teleport frames before this one */

#define LOADING()	(loading > 0 || booting)

#ifdef HL64_TEST
/*
 * Test builds (K10, architecture.md 15): I/O completes in frames. At
 * io_frame_begin the scheduler plans the frame's pieces from the queue
 * alone (priority order, up to the byte budget and IO_TEST_PIECES); they
 * move during the frame as in play (one in flight, pumped), but nothing
 * is published (request done / state, callbacks) until the next
 * io_frame_begin, which first finishes a plan the frame did not move
 * (late: blocked I/O, logged, counted) and then publishes it in plan
 * order. A planned request is IO_ACTIVE for the whole frame and its
 * `done` only advances there, so nothing the game sees depends on PI
 * timing, pump counts or audio reads. io_finish / io_cancel / sync reads
 * keep their immediate semantics.
 */
typedef struct
{
	io_req_t	*r;	/* NULL: published, finished or cancelled */
	uint32_t	n;	/* bytes planned from r->done */
	uint32_t	moved;	/* bytes landed */
} plan_t;

static plan_t		plan[IO_TEST_PIECES];
static int		nplan, plan_cur;	/* entries; first one with bytes to launch */
static plan_t		*act_p;			/* the entry owning the piece in flight */
static uint32_t		life_late_frames, life_late_pieces;
static uint32_t		fr_pumps, fr_pumps_busy;	/* this frame's pumps; with a piece in flight */

static plan_t *PlanOf(const io_req_t *r)
{
	int i;

	for (i = 0; i < nplan; i++)
		if (plan[i].r == r)
			return &plan[i];
	return NULL;
}
#endif

/* ---- backend (sys_pi_*) ------------------------------------------------ */

static void HwLaunch(uint8_t *dst, uint32_t rom, uint32_t len)
{
	sys_pi_launch(dst, rom, len);
	fr_pi_us += sys_pi_cost_us(len);
}

static void HwWaitIdle(int audio)
{
	uint32_t w = sys_pi_wait(audio);

	fr_blocked_us += w;
	if (!LOADING())
		fr_drain_us += w;
}

/* A blocking PI read; audio reads are counted apart from sync reads. */
static void HwSync(uint8_t *dst, uint32_t rom, uint32_t len, int audio)
{
	fr_blocked_us += sys_pi_read(dst, rom, len);
	if (LOADING())
		return;		/* layer-open reads are not a playing frame's PI time */
	fr_pi_us += sys_pi_cost_us(len);
	if (audio)
	{
		fr_audio_calls++;
		return;
	}
	fr_sync_calls++;
	fr_sync_bytes += len;
	if (len > fr_sync_max)
	{
		fr_sync_max = len;
		fr_sync_max_rom = rom;
	}
}

/* ---- queue -------------------------------------------------------------- */

static void Unlink(io_req_t *r)
{
	io_req_t **pp = &q_head[r->prio], *prev = NULL;

	while (*pp && *pp != r)
	{
		prev = *pp;
		pp = &(*pp)->next;
	}
	if (!*pp)
		return;
	*pp = r->next;
	if (q_tail[r->prio] == r)
		q_tail[r->prio] = prev;
	r->next = NULL;
	q_len--;
	q_bytes -= r->len - r->done;
}

static void Link(io_req_t *r)
{
	io_req_t **pp = &q_head[r->prio];

	q_bytes += r->len - r->done;
	if (!r->front)
		pp = q_tail[r->prio] ? &q_tail[r->prio]->next : pp;
	else
		/* after the class's leading front requests */
		while (*pp && (*pp)->front)
			pp = &(*pp)->next;
	r->next = *pp;
	*pp = r;
	if (!r->next)
		q_tail[r->prio] = r;
	if (++q_len > (int)st.queue_max)
		st.queue_max = (uint32_t)q_len;
}

static void Complete(io_req_t *r)
{
	Unlink(r);
	r->state = IO_DONE;
	if (r->cb)
		r->cb(r);
}

/* Retire the piece in flight once the PI is idle (test builds: it landed
 * in its plan entry; the request learns at the next io_frame_begin). */
static void Retire(void)
{
	io_req_t *r = act;

	if (!r || sys_pi_busy())
		return;
	act = NULL;
#ifdef HL64_TEST
	act_p->moved += act_len;
	act_p = NULL;
#else
	r->done += act_len;
	q_bytes -= act_len;
	if (r->done >= r->len)
		Complete(r);
	else
		r->state = IO_QUEUED;
#endif
	act_len = 0;
}

#ifdef HL64_TEST
static uint32_t		plan_budget;	/* the budget the frame planned with */
static void Drain(int audio);

/* Frame boundary, first: the closing frame's plan moved in full. A piece
 * still in flight is waited for, pieces never launched are read now:
 * late (blocked I/O of the closing frame, logged and counted). */
static void PlanFinish(void)
{
	uint32_t pieces = 0, bytes = 0, total = 0;
	int i;

	for (i = 0; i < nplan; i++)
		total += plan[i].r ? plan[i].n : 0;

	Retire();
	for (i = 0; i < nplan; i++)
	{
		plan_t *e = &plan[i];
		uint32_t at, n;

		if (!e->r)
			continue;
		if (act_p == e)
		{
			pieces++;
			bytes += act_len;
			Drain(0);
		}
		if (e->moved >= e->n)
			continue;
		at = e->r->done + e->moved;
		n = e->n - e->moved;
		pieces += (n + IO_PIECE - 1) / IO_PIECE;
		bytes += n;
		HwSync(e->r->dst + at, e->r->rom + at, n, 0);
		e->moved = e->n;
	}
	if (!pieces)
		return;
	life_late_frames++;
	life_late_pieces += pieces;
	log_printf("io: frame %lu late: %lu pieces %lu B of its plan (%d requests, %lu B) finished "
		"at the frame boundary; %lu pumps, %lu found a piece in flight\n",
		(unsigned long)life_frames, (unsigned long)pieces, (unsigned long)bytes, nplan,
		(unsigned long)total, (unsigned long)fr_pumps, (unsigned long)fr_pumps_busy);
}

/* Then the plan's completions, in plan order: the requests' done / state
 * and callbacks (which may finish, cancel or submit requests). */
static void PlanPublish(void)
{
	int i;

	for (i = 0; i < nplan; i++)
	{
		plan_t *e = &plan[i];
		io_req_t *r = e->r;

		if (!r)
			continue;
		e->r = NULL;
		r->done += e->n;
		q_bytes -= e->n;
		if (r->done >= r->len)
			Complete(r);
		else
			r->state = IO_QUEUED;
	}
	nplan = plan_cur = 0;
}

/* Then the new frame's plan, from the queue alone: requests in priority
 * order, as much of each as the byte budget and IO_TEST_PIECES allow
 * (a partial one stays 16-aligned and ends the plan). */
static void PlanFrame(void)
{
	uint32_t left = budget, pieces = 0;
	io_req_t *r;
	int p;

	plan_budget = budget;
	for (p = 0; p < IO_NPRIO; p++)
		for (r = q_head[p]; r; r = r->next)
		{
			uint32_t rem = r->len - r->done, n = rem;

			if (nplan == IO_TEST_PIECES)
				return;
			if (n > left)
				n = left & ~15u;
			if (n > (IO_TEST_PIECES - pieces) * IO_PIECE)
				n = (IO_TEST_PIECES - pieces) * IO_PIECE;
			if (!n && rem)
				return;
			plan[nplan].r = r;
			plan[nplan].n = n;
			plan[nplan].moved = 0;
			nplan++;
			r->state = IO_ACTIVE;
			left -= n;
			pieces += (n + IO_PIECE - 1) / IO_PIECE;
			if (n < rem || pieces >= IO_TEST_PIECES)
				return;
		}
}
#endif

void io_init(void)
{
	memset(q_head, 0, sizeof(q_head));
	memset(q_tail, 0, sizeof(q_tail));
	q_len = 0;
	q_bytes = 0;
	loading = 0;
	booting = 1;
	act = NULL;
	act_len = 0;
	budget = IO_BUDGET;
	fr_async = fr_blocked_us = fr_pi_us = 0;
	fr_sync_calls = fr_sync_bytes = fr_sync_max = fr_sync_max_rom = 0;
	fr_audio_calls = fr_drain_us = 0;
	life_frames = life_blocked_max = life_blocked_1ms = life_budget_over = 0;
	life_blocked_1ms_tele = 0;
	tele_until = 0;
	memset(&st, 0, sizeof(st));
#ifdef HL64_TEST
	nplan = plan_cur = 0;
	act_p = NULL;
	plan_budget = IO_BUDGET;
	life_late_frames = life_late_pieces = 0;
#endif
}

void io_frame_begin(uint32_t frame_us)
{
	uint32_t cap = budget;

#ifdef HL64_TEST
	/* the closing frame's plan lands before the frame closes (a late
	 * piece is its blocked time); its budget is the one it planned with */
	PlanFinish();
	cap = plan_budget;
#endif
	/* close the previous frame (the boot interval is load I/O: its
	 * blocked time already went to the load counters) */
	if (!booting && life_frames)
	{
		if (fr_async > st.frame_async_max)
			st.frame_async_max = fr_async;
		st.hist[!fr_async ? 0 : fr_async <= 8192 ? 1 : fr_async <= 16384 ? 2
			: fr_async <= 32768 ? 3 : fr_async <= 65536 ? 4 : 5]++;
		{
			uint32_t kb = (fr_async + 1023) / 1024;

			st.kb_hist[kb < IO_KB_BINS - 1 ? kb : IO_KB_BINS - 1]++;
		}
		if (fr_async > cap)
		{
			st.frames_budget_over++;
			life_budget_over++;
		}
		if (fr_blocked_us > st.frame_blocked_max_us)
			st.frame_blocked_max_us = fr_blocked_us;
		if (fr_blocked_us > 1000)
		{
			/* a test teleport's frames (route SETPOS: the destination
			 * hydrates) are counted apart, never dropped */
			int tele = life_frames < tele_until;

			st.frames_blocked_1ms++;
			if (tele)
				life_blocked_1ms_tele++;
			else
				life_blocked_1ms++;
			log_printf("io: frame %lu blocked %lu us: %lu sync reads %lu B, largest %lu B "
				"at 0x%08lx, %lu audio reads, drain %lu us%s\n", (unsigned long)life_frames,
				(unsigned long)fr_blocked_us, (unsigned long)fr_sync_calls,
				(unsigned long)fr_sync_bytes, (unsigned long)fr_sync_max,
				(unsigned long)fr_sync_max_rom, (unsigned long)fr_audio_calls,
				(unsigned long)fr_drain_us, tele ? " (teleport window)" : "");
		}
		if (fr_blocked_us > life_blocked_max && life_frames >= tele_until)
			life_blocked_max = fr_blocked_us;
		st.blocked_us += fr_blocked_us;
		if (fr_us && fr_pi_us > fr_us)
			st.frames_pi_over++;
	}
	booting = 0;
	last_async = fr_async;
	last_blocked = fr_blocked_us;
	fr_async = 0;
	fr_blocked_us = 0;
	fr_us = frame_us;
	fr_pi_us = 0;
	fr_sync_calls = fr_sync_bytes = fr_sync_max = fr_sync_max_rom = 0;
	fr_audio_calls = fr_drain_us = 0;
	st.frames++;
	life_frames++;
#ifdef HL64_TEST
	/* the closing frame's completions first, then the new frame's plan */
	PlanPublish();
	PlanFrame();
	fr_pumps = fr_pumps_busy = 0;
#endif
}

void io_set_seam(int on)
{
	budget = on ? IO_BUDGET_SEAM : IO_BUDGET;
}

uint32_t io_budget_left(void)
{
	return fr_async >= budget ? 0 : budget - fr_async;
}

uint32_t io_budget(void)
{
	return budget;
}

void io_submit(io_req_t *r)
{
	if (r->state == IO_QUEUED || r->state == IO_ACTIVE)
		return;
	if (r->prio >= IO_NPRIO)
		r->prio = IO_PRIO_PREFETCH;
	r->done = 0;
	r->state = IO_QUEUED;
	Link(r);
}

void io_promote(io_req_t *r, int prio)
{
	if ((r->state != IO_QUEUED && r->state != IO_ACTIVE) || prio >= r->prio)
		return;
	Unlink(r);
	r->prio = (uint8_t)prio;
	Link(r);
	st.promoted++;
}

#ifdef HL64_TEST
static void Pump(uint32_t max);

void io_pump(void)
{
	Pump(IO_PIECE);
}

void io_pump_short(void)
{
	Pump(IO_PIECE_SHORT);
}

static void Pump(uint32_t max)
{
	plan_t *e;
	uint32_t at, n;

	Retire();
	fr_pumps++;
	if (act)
	{
		fr_pumps_busy++;
		return;
	}
	while (plan_cur < nplan && (!plan[plan_cur].r || plan[plan_cur].moved >= plan[plan_cur].n))
		plan_cur++;
	if (plan_cur == nplan)
		return;
	e = &plan[plan_cur];
	n = e->n - e->moved;
	if (n > max)
		n = max;
	at = e->r->done + e->moved;
	act = e->r;
	act_p = e;
#else
void io_pump(void)
{
	io_req_t *r = NULL;
	uint32_t left, n, at;
	int p;

	Retire();
	if (act)
		return;
	for (p = 0; p < IO_NPRIO && !r; p++)
		r = q_head[p];
	if (!r)
		return;
	left = io_budget_left();
	n = r->len - r->done;
	if (n > IO_PIECE)
		n = IO_PIECE;
	if (n > left)
	{
		/* a partial piece must keep the next one 16-aligned */
		n = left & ~15u;
		if (!n)
			return;
	}
	r->state = IO_ACTIVE;
	at = r->done;
	act = r;
#endif
	act_len = n;
	fr_async += n;
	st.async_bytes += n;
	st.async_pieces++;
	HwLaunch(act->dst + at, act->rom + at, n);
}

/* Wait for the piece in flight (someone else's or r's) and retire it;
 * audio: the waiter is an AUDIO read (sys_pi_wait). */
static void Drain(int audio)
{
	if (!act)
		return;
	HwWaitIdle(audio);
	Retire();
}

void io_cancel(io_req_t *r)
{
#ifdef HL64_TEST
	plan_t *e = PlanOf(r);

	if (e)
	{
		if (act_p == e)
			Drain(0);
		e->r = NULL;	/* its landed bytes are dropped */
	}
	if (r->state == IO_QUEUED || r->state == IO_ACTIVE)
		Unlink(r);
#else
	if (r->state == IO_ACTIVE)
		Drain(0);
	if (r->state == IO_QUEUED)
		Unlink(r);
#endif
	if (r->state != IO_DONE)
		r->state = IO_IDLE;
}

/* Map-load reads: move this call's blocked time to the load counters. */
static void LoadAccount(uint32_t blocked_before, uint32_t n)
{
	if (!LOADING())
		return;
	st.load_us += fr_blocked_us - blocked_before;
	st.load_bytes += n;
	fr_blocked_us = blocked_before;
}

void io_set_load(int on)
{
	loading += on ? 1 : -1;
	if (loading < 0)
		sys_error("io: io_set_load(0) without io_set_load(1)");
}

void io_tele_window(uint32_t frames)
{
	tele_until = life_frames + frames;
}

int io_loading(void)
{
	return LOADING();
}

uint32_t io_queued(void)
{
	return q_bytes;
}

/* A sync read in play: counted, and named in test builds. */
static void SyncCount(const char *what, uint32_t rom, uint32_t n)
{
	if (LOADING())
		return;
	st.sync_calls++;
	st.sync_bytes += n;
#ifdef HL64_TEST
	log_printf("io: sync %s %lu B at 0x%08lx\n", what, (unsigned long)n, (unsigned long)rom);
#else
	(void)what;
	(void)rom;
#endif
}

void io_finish(io_req_t *r, const char *what)
{
	uint32_t n, at, b0 = fr_blocked_us;
#ifdef HL64_TEST
	plan_t *e = PlanOf(r);

	if (r->state != IO_QUEUED && r->state != IO_ACTIVE)
		return;
	/* planned: its landed bytes publish now, the rest is read */
	if (e && act_p == e)
		Drain(0);
	at = r->done + (e ? e->moved : 0);
	if (e)
		e->r = NULL;
#else
	if (r->state == IO_ACTIVE)
		Drain(0);
	if (r->state != IO_QUEUED)
		return;
	at = r->done;
#endif
	n = r->len - at;
	if (n)
	{
		Drain(0);
		SyncCount(what, r->rom + at, n);
		HwSync(r->dst + at, r->rom + at, n, 0);
	}
	q_bytes -= r->len - r->done;
	r->done = r->len;
	Complete(r);
	LoadAccount(b0, n);
}

void io_read_sync(void *dst, uint32_t rom, uint32_t len, const char *what)
{
	uint32_t b0 = fr_blocked_us;

	Drain(0);
	SyncCount(what, rom, len);
	HwSync(dst, rom, len, 0);
	LoadAccount(b0, len);
}

void io_read_audio(void *dst, uint32_t rom, uint32_t len)
{
	uint32_t b0;

	Drain(1);
	b0 = fr_blocked_us;
	HwSync(dst, rom, len, 1);
	st.audio_calls++;
	st.audio_bytes += len;
	st.audio_us += fr_blocked_us - b0;
	fr_blocked_us = b0;
}

int io_busy(void)
{
	return act != NULL || q_len > 0;
}

int io_in_flight(void)
{
	Retire();
	return act != NULL;
}

void io_stats(io_stats_t *out, int reset)
{
	*out = st;
	out->life_frames = life_frames;
	out->life_blocked_max_us = life_blocked_max;
	out->life_frames_blocked_1ms = life_blocked_1ms;
	out->life_frames_budget_over = life_budget_over;
	out->life_frames_blocked_1ms_tele = life_blocked_1ms_tele;
#ifdef HL64_TEST
	out->life_late_frames = life_late_frames;
	out->life_late_pieces = life_late_pieces;
#endif
	if (reset)
		memset(&st, 0, sizeof(st));
}

uint32_t io_frame_kb_percentile(const io_stats_t *s, int pct)
{
	uint32_t n = 0, acc = 0, want;
	int i;

	for (i = 0; i < IO_KB_BINS; i++)
		n += s->kb_hist[i];
	if (!n)
		return 0;
	want = (n * (uint32_t)pct + 99) / 100;
	for (i = 0; i < IO_KB_BINS; i++)
	{
		acc += s->kb_hist[i];
		if (acc >= want)
			return (uint32_t)i;
	}
	return IO_KB_BINS - 1;
}

void io_last_frame(uint32_t *async_bytes, uint32_t *blocked_us, uint32_t *queued)
{
	*async_bytes = last_async;
	*blocked_us = last_blocked;
	*queued = (uint32_t)q_len;
}

uint32_t io_frame_blocked_us(void)
{
	return fr_blocked_us;
}
