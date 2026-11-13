/* SPDX-License-Identifier: MIT */
/*
 * io.c -- cart read scheduler (io.h), imported from the old tree's
 * q64_io.c (audit entry n64/port/q64_io.c CLEAN); the cart / host
 * backends moved behind sys.h's sys_pi_* (plat/n64/cart.c,
 * plat/host/io_host.c).
 */
#include <string.h>

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
static int		loading;
static uint32_t		q_bytes;	/* remaining bytes of queued + active requests */
static uint32_t		fr_pi_us;	/* modelled PI busy time this frame (host) */

/* ---- backend (sys_pi_*) ------------------------------------------------ */

static void HwLaunch(uint8_t *dst, uint32_t rom, uint32_t len)
{
	sys_pi_launch(dst, rom, len);
	fr_pi_us += sys_pi_cost_us(len);
}

static void HwWaitIdle(void)
{
	fr_blocked_us += sys_pi_wait();
}

static void HwSync(uint8_t *dst, uint32_t rom, uint32_t len)
{
	fr_blocked_us += sys_pi_read(dst, rom, len);
	if (!loading)
		fr_pi_us += sys_pi_cost_us(len);	/* layer-open reads are not a playing frame's PI time */
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

static void Complete(io_req_t *r)
{
	Unlink(r);
	r->state = IO_DONE;
	if (r->cb)
		r->cb(r);
}

/* Retire the piece in flight once the PI is idle. */
static void Retire(void)
{
	io_req_t *r = act;

	if (!r || sys_pi_busy())
		return;
	act = NULL;
	r->done += act_len;
	q_bytes -= act_len;
	act_len = 0;
	if (r->done >= r->len)
		Complete(r);
	else
		r->state = IO_QUEUED;
}

void io_init(void)
{
	memset(q_head, 0, sizeof(q_head));
	memset(q_tail, 0, sizeof(q_tail));
	q_len = 0;
	q_bytes = 0;
	loading = 0;
	act = NULL;
	act_len = 0;
	budget = IO_BUDGET;
	memset(&st, 0, sizeof(st));
}

void io_frame_begin(uint32_t frame_us)
{
	/* close the previous frame */
	if (st.frames || fr_async || fr_blocked_us)
	{
		if (fr_async > st.frame_async_max)
			st.frame_async_max = fr_async;
		st.hist[!fr_async ? 0 : fr_async <= 8192 ? 1 : fr_async <= 16384 ? 2
			: fr_async <= 32768 ? 3 : fr_async <= 65536 ? 4 : 5]++;
		{
			uint32_t kb = (fr_async + 1023) / 1024;

			st.kb_hist[kb < IO_KB_BINS - 1 ? kb : IO_KB_BINS - 1]++;
		}
		if (fr_async > budget)
			st.frames_budget_over++;
		if (fr_blocked_us > st.frame_blocked_max_us)
			st.frame_blocked_max_us = fr_blocked_us;
		if (fr_blocked_us > 1000)
			st.frames_blocked_1ms++;
		st.blocked_us += fr_blocked_us;
		if (fr_us && fr_pi_us > fr_us)
			st.frames_pi_over++;
	}
	last_async = fr_async;
	last_blocked = fr_blocked_us;
	fr_async = 0;
	fr_blocked_us = 0;
	fr_us = frame_us;
	fr_pi_us = 0;
	st.frames++;
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
	r->next = NULL;
	r->state = IO_QUEUED;
	q_bytes += r->len;
	if (q_tail[r->prio])
		q_tail[r->prio]->next = r;
	else
		q_head[r->prio] = r;
	q_tail[r->prio] = r;
	if (++q_len > (int)st.queue_max)
		st.queue_max = (uint32_t)q_len;
}

void io_pump(void)
{
	io_req_t *r = NULL;
	uint32_t left, n;
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
	act = r;
	act_len = n;
	fr_async += n;
	st.async_bytes += n;
	st.async_pieces++;
	HwLaunch(r->dst + r->done, r->rom + r->done, n);
}

/* Wait for the piece in flight (someone else's or r's) and retire it. */
static void Drain(void)
{
	if (!act)
		return;
	HwWaitIdle();
	Retire();
}

void io_cancel(io_req_t *r)
{
	if (r->state == IO_ACTIVE)
		Drain();
	if (r->state == IO_QUEUED)
		Unlink(r);
	if (r->state != IO_DONE)
		r->state = IO_IDLE;
}

/* Map-load reads: move this call's blocked time to the load counters. */
static void LoadAccount(uint32_t blocked_before, uint32_t n)
{
	if (!loading)
		return;
	st.load_us += fr_blocked_us - blocked_before;
	st.load_bytes += n;
	fr_blocked_us = blocked_before;
}

void io_set_load(int on)
{
	loading = on;
}

uint32_t io_queued(void)
{
	return q_bytes;
}

void io_finish(io_req_t *r)
{
	uint32_t n, b0 = fr_blocked_us;

	if (r->state == IO_ACTIVE)
		Drain();
	if (r->state != IO_QUEUED)
		return;
	Drain();
	n = r->len - r->done;
	if (!loading)
	{
		st.sync_calls++;
		st.sync_bytes += n;
	}
	HwSync(r->dst + r->done, r->rom + r->done, n);
	q_bytes -= n;
	r->done = r->len;
	Complete(r);
	LoadAccount(b0, n);
}

void io_read_sync(void *dst, uint32_t rom, uint32_t len)
{
	uint32_t b0 = fr_blocked_us;

	Drain();
	if (!loading)
	{
		st.sync_calls++;
		st.sync_bytes += len;
	}
	HwSync(dst, rom, len);
	LoadAccount(b0, len);
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
