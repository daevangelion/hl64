/* SPDX-License-Identifier: MIT */
/*
 * io.c -- cart read scheduler (io.h), imported from the old tree's
 * q64_io.c (audit entry n64/port/q64_io.c CLEAN); the cart / host
 * backends moved behind sys.h's sys_pi_* (plat/n64/cart.c,
 * plat/host/io_host.c). The plan array and its interrupt-driven DMA
 * chain are ours (K10).
 */
#include <string.h>

#include "engine/core/log.h"
#include "engine/core/sys.h"
#include "engine/io/io.h"

static io_req_t	*q_head[IO_NPRIO], *q_tail[IO_NPRIO];
static int		q_len;
static uint32_t		budget = IO_BUDGET;
static uint32_t		fr_async, fr_blocked_us, fr_us;
static uint32_t		fr_planned;	/* bytes planned this frame (playable: + the carried rest) */
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

/*
 * The plan and its chain. A plan entry is a request and the bytes planned
 * from its done at planning time; its pieces (at most IO_PIECE each, in
 * order) are pre-built in pieces[]. The chain step lands the piece in
 * flight once the PI is idle and, unless held, launches the next piece of
 * a live entry: the PI completion interrupt runs it on the cart
 * (ChainStep is the handler; any PI DMA's completion calls it, and one
 * not the chain's finds the PI busy or nothing in flight), and every io
 * call runs it with interrupts off (Poll; on the host, where a piece
 * lands when launched, that moves the whole plan). It touches only the
 * plan arrays and the PI. Publishing, stats and planning are main-thread
 * work, with interrupts off where they share the chain's state.
 */
typedef struct
{
	io_req_t		*r;	/* NULL: cancelled, finished or published */
	uint32_t		n;	/* bytes planned from r->done at planning */
	uint32_t		pub;	/* playable: bytes published so far */
	volatile uint32_t	moved;	/* bytes landed (the chain) */
} plan_t;

typedef struct
{
	uint8_t			*dst;
	uint32_t		rom, len;
	uint8_t			e;		/* plan entry */
	volatile uint8_t	landed;		/* the chain */
	uint8_t			counted;	/* in the async stats */
} piece_t;

#define IO_PLAN_PIECES	(IO_PLAN_UNITS * (IO_PLAN_UNIT / IO_PIECE))

static plan_t		plan[IO_PLAN_UNITS];
static piece_t		pieces[IO_PLAN_PIECES];
static int		nplan, npieces;
static volatile int	pc_next;	/* next piece to launch */
static volatile int	fly = -1;	/* piece in flight, -1 none */
static volatile int	hold;		/* io_hold depth: launch nothing */
static volatile int	paused;		/* a sync / audio read: launch nothing until io_pump */

#ifdef HL64_TEST
static uint32_t		plan_budget;	/* the budget the frame planned with */
static uint32_t		life_late_frames, life_late_pieces;
#endif

/* ---- the chain (interrupt handler; main thread with interrupts off) ---- */

static void ChainLaunch(void)
{
	while (pc_next < npieces)
	{
		piece_t *p = &pieces[pc_next++];

		if (plan[p->e].r)
		{
			fly = pc_next - 1;
			sys_pi_launch(p->dst, p->rom, p->len);
			return;
		}
	}
}

static void ChainStep(void)
{
	while (fly >= 0 && !sys_pi_busy())
	{
		piece_t *p = &pieces[fly];

		plan[p->e].moved += p->len;
		p->landed = 1;
		fly = -1;
		if (!hold && !paused)
			ChainLaunch();
	}
}

/* ---- main thread ---------------------------------------------------------- */

/* Land what has landed; start the chain when idle, not held nor paused. */
static void Poll(void)
{
	sys_irq_off();
	ChainStep();
	if (!hold && !paused && fly < 0)
	{
		ChainLaunch();
		ChainStep();
	}
	sys_irq_on();
}

/* The landed pieces into the frame's async stats (host: PI time). */
static void Account(void)
{
	int i;

	for (i = 0; i < npieces; i++)
	{
		piece_t *p = &pieces[i];

		if (p->counted || !p->landed)
			continue;
		p->counted = 1;
		fr_async += p->len;
		st.async_bytes += p->len;
		st.async_pieces++;
		fr_pi_us += sys_pi_cost_us(p->len);
	}
}

static void HwWaitIdle(int audio)
{
	uint32_t w = sys_pi_wait(audio);

	fr_blocked_us += w;
	if (!LOADING())
		fr_drain_us += w;
}

/* Before a sync / audio read: the chain launches nothing more until the
 * next io_pump (the reads of one frame step wait for one piece at most)
 * and its piece in flight lands (waited for: drain, blocked time). */
static void Stop(int audio)
{
	sys_irq_off();
	paused = 1;
	ChainStep();
	sys_irq_on();
	if (fly < 0)
		return;
	HwWaitIdle(audio);
	sys_irq_off();
	ChainStep();
	sys_irq_on();
}

void io_hold(int on)
{
	sys_irq_off();
	hold += on ? 1 : -1;
	if (hold < 0)
		sys_error("io: io_hold(0) without io_hold(1)");
	if (!hold && !paused && fly < 0)
	{
		ChainLaunch();
		ChainStep();
	}
	sys_irq_on();
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

static plan_t *PlanOf(const io_req_t *r)
{
	int i;

	for (i = 0; i < nplan; i++)
		if (plan[i].r == r)
			return &plan[i];
	return NULL;
}

/* A plan from the queue alone, `left` bytes at most: requests in priority
 * order, as much of each as `left` and IO_PLAN_UNITS allow (a partial one
 * stays 16-aligned and ends the plan), cut into the chain's pieces. The
 * chain is idle (nothing in flight, every piece launched). */
static void PlanQueue(uint32_t left)
{
	uint32_t units = 0;
	io_req_t *r;
	int p;

	nplan = npieces = 0;
	pc_next = 0;
	for (p = 0; p < IO_NPRIO; p++)
		for (r = q_head[p]; r; r = r->next)
		{
			uint32_t rem = r->len - r->done, n = rem, off;
			plan_t *e;

			if (nplan == IO_PLAN_UNITS)
				return;
			if (n > left)
				n = left & ~15u;
			if (n > (IO_PLAN_UNITS - units) * IO_PLAN_UNIT)
				n = (IO_PLAN_UNITS - units) * IO_PLAN_UNIT;
			if (!n && rem)
				return;
			e = &plan[nplan];
			e->r = r;
			e->n = n;
			e->pub = 0;
			e->moved = 0;
			for (off = 0; off < n; off += IO_PIECE)
			{
				piece_t *pc = &pieces[npieces++];

				pc->dst = r->dst + r->done + off;
				pc->rom = r->rom + r->done + off;
				pc->len = n - off < IO_PIECE ? n - off : IO_PIECE;
				pc->e = (uint8_t)nplan;
				pc->landed = 0;
				pc->counted = 0;
			}
			nplan++;
			r->state = IO_ACTIVE;
			left -= n;
			fr_planned += n;
			units += (n + IO_PLAN_UNIT - 1) / IO_PLAN_UNIT;
			if (n < rem || units >= IO_PLAN_UNITS)
				return;
		}
}

#ifdef HL64_TEST
/* Frame boundary, first: the closing frame's plan moved in full. A piece
 * still in flight is waited for, pieces never launched are read now:
 * late (blocked I/O of the closing frame, logged and counted). */
static void PlanFinish(void)
{
	uint32_t late = 0, bytes = 0, total = 0;
	int i, launched = pc_next;

	for (i = 0; i < nplan; i++)
		total += plan[i].r ? plan[i].n : 0;
	sys_irq_off();
	paused = 1;
	ChainStep();
	sys_irq_on();
	if (fly >= 0)
	{
		late++;
		bytes += pieces[fly].len;
		HwWaitIdle(0);
		sys_irq_off();
		ChainStep();
		sys_irq_on();
	}
	Account();
	for (i = pc_next; i < npieces; i++)
	{
		piece_t *p = &pieces[i];

		if (!plan[p->e].r)
			continue;
		late++;
		bytes += p->len;
		HwSync(p->dst, p->rom, p->len, 0);
		plan[p->e].moved += p->len;
		p->landed = p->counted = 1;
	}
	pc_next = npieces;
	if (!late)
		return;
	life_late_frames++;
	life_late_pieces += late;
	log_printf("io: frame %lu late: %lu pieces %lu B of its plan (%d requests, %lu B) finished "
		"at the frame boundary; the chain had launched %d of %d pieces\n",
		(unsigned long)life_frames, (unsigned long)late, (unsigned long)bytes, nplan,
		(unsigned long)total, launched, npieces);
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
	nplan = npieces = 0;
	pc_next = 0;
}
#else
/* Playable: the landed bytes of each entry published, its request
 * completed once all of it landed. */
static void Publish(void)
{
	int i;

	for (i = 0; i < nplan; i++)
	{
		plan_t *e = &plan[i];
		io_req_t *r = e->r;
		uint32_t moved = e->moved, d = moved - e->pub;

		if (!r)
			continue;
		r->done += d;
		q_bytes -= d;
		e->pub = moved;
		if (moved < e->n)
			continue;
		e->r = NULL;
		if (r->done >= r->len)
			Complete(r);
		else
			r->state = IO_QUEUED;
	}
}

static int PlanLive(void)
{
	int i;

	for (i = 0; i < nplan; i++)
		if (plan[i].r)
			return 1;
	return 0;
}
#endif

void io_init(void)
{
	if (fly >= 0)
		sys_pi_wait(0);
	memset(q_head, 0, sizeof(q_head));
	memset(q_tail, 0, sizeof(q_tail));
	q_len = 0;
	q_bytes = 0;
	loading = 0;
	booting = 1;
	budget = IO_BUDGET;
	fr_async = fr_blocked_us = fr_pi_us = fr_planned = 0;
	fr_sync_calls = fr_sync_bytes = fr_sync_max = fr_sync_max_rom = 0;
	fr_audio_calls = fr_drain_us = 0;
	life_frames = life_blocked_max = life_blocked_1ms = life_budget_over = 0;
	life_blocked_1ms_tele = 0;
	tele_until = 0;
	memset(&st, 0, sizeof(st));
	sys_irq_off();
	nplan = npieces = 0;
	pc_next = 0;
	fly = -1;
	hold = 0;
	paused = 0;
	sys_irq_on();
	sys_pi_set_isr(ChainStep);
#ifdef HL64_TEST
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
#else
	Poll();
	Account();
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
	fr_planned = 0;
	fr_sync_calls = fr_sync_bytes = fr_sync_max = fr_sync_max_rom = 0;
	fr_audio_calls = fr_drain_us = 0;
	st.frames++;
	life_frames++;
#ifdef HL64_TEST
	/* the closing frame's completions first, then the new frame's plan */
	PlanPublish();
	plan_budget = budget;
	PlanQueue(budget);
#else
	/* the plan's unlanded rest lands in this frame: its budget */
	{
		int i;

		for (i = 0; i < nplan; i++)
			if (plan[i].r)
				fr_planned += plan[i].n - plan[i].moved;
	}
#endif
	paused = 0;
	Poll();
	Account();
}

void io_set_seam(int on)
{
	budget = on ? IO_BUDGET_SEAM : IO_BUDGET;
}

uint32_t io_budget_left(void)
{
	return fr_planned >= budget ? 0 : budget - fr_planned;
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

/* Publish (playable), resume a paused chain, land and launch. */
void io_pump(void)
{
	paused = 0;
	Poll();
	Account();
#ifndef HL64_TEST
	Publish();
	if (!PlanLive() && fly < 0)
	{
		Account();
		PlanQueue(io_budget_left());
		Poll();
		Account();
	}
#endif
}

void io_cancel(io_req_t *r)
{
	plan_t *e = PlanOf(r);

	if (e)
	{
		/* its piece in flight lands first; its others never launch and
		 * its landed bytes are dropped */
		if (fly >= 0 && &plan[pieces[fly].e] == e)
			Stop(0);
		sys_irq_off();
		e->r = NULL;
		sys_irq_on();
	}
	if (r->state == IO_QUEUED || r->state == IO_ACTIVE)
		Unlink(r);
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
	plan_t *e;

	if (r->state != IO_QUEUED && r->state != IO_ACTIVE)
		return;
	/* planned: its landed, unpublished bytes publish now, the rest is
	 * read */
	Stop(0);
	Account();
	e = PlanOf(r);
	at = r->done + (e ? e->moved - e->pub : 0);
	if (e)
		e->r = NULL;
	n = r->len - at;
	if (n)
	{
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

	Stop(0);
	SyncCount(what, rom, len);
	HwSync(dst, rom, len, 0);
	LoadAccount(b0, len);
}

void io_read_audio(void *dst, uint32_t rom, uint32_t len)
{
	uint32_t b0;

	Stop(1);
	b0 = fr_blocked_us;
	HwSync(dst, rom, len, 1);
	st.audio_calls++;
	st.audio_bytes += len;
	st.audio_us += fr_blocked_us - b0;
	fr_blocked_us = b0;
}

int io_busy(void)
{
	return fly >= 0 || q_len > 0;
}

int io_in_flight(void)
{
	Poll();
	return fly >= 0;
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
