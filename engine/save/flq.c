/* SPDX-License-Identifier: MIT */
/*
 * flq.c -- the flash writer (flq.h, docs/architecture.md 14): erases the
 * sectors of a range and programs its non-blank pages one operation at a
 * time. The chip is busy for a long time after each (an erase up to
 * hundreds of ms, a page program ~ms), so a step polls the operation in
 * flight first, starts the next one only when the chip is idle, and leaves
 * as soon as one is running or its time budget is spent: a write spans
 * many frames and nothing ever waits on the chip. A chip that finishes at
 * once (the host's image) lets one step run several operations, up to the
 * budget.
 *
 * Between the frame's steps flq_tick polls again (eng.c's frame steps and
 * io pumps, the active pass's entity steps, col_move, the stream's
 * naming, the studio pose and draw loops: main-thread points spread over
 * the frame), so the next operation starts soon after the chip ends one
 * rather than at the next frame: a page program of a few ms would
 * otherwise cost a whole frame, and a 13 KB save some 100 frames (ares'
 * MX29L1101: 3.5 ms a page, 85 ms a sector erase). A tick polls only once
 * the operation has run as long as the shortest of its kind seen so far
 * ended (est[]: an upper bound of the chip's time, so that poll finds it
 * ended; a tick up to FQ_SPIN_US before that waits for it), then again
 * after an eighth of its run so far (FQ_POLL_US at least). Every tick
 * counts against the frame's budget that flq_step started and starts only
 * with FQ_TICK_US of it left, so the frame's flash time stays within it.
 * Each poll / start holds the I/O chain (plat/n64/flash.c), so a tick at
 * any of those points is ordered against the streamer's DMA. With no
 * write running a tick is one test.
 *
 * Per sector: erase, then program its pages in order. Pages whose 128
 * bytes are all 0xFF are not programmed (the erase left them so); the
 * partial last page of the range is padded with 0xFF.
 */
#include <stdint.h>
#include <string.h>

#include "engine/core/log.h"
#include "engine/core/sys.h"
#include "engine/save/flq.h"

#define FQ_PAGE		128u
#define FQ_SECTOR	(128u * FQ_PAGE)
#define FQ_PAGES	(FQ_SECTOR / FQ_PAGE)	/* per sector */
#define FQ_BYTES	(8u * FQ_SECTOR)
#define FQ_POLL_US	500u	/* least gap of a tick's polls of a busy chip */
#define FQ_SPIN_US	500u	/* a tick this close to est[] waits for the end */
/* A tick's worst cost: a poll, a program's start (~0.2 ms on ares), and
 * the wait for a ROM piece in flight when it holds the chain (IO_PIECE,
 * ~0.4 ms): a tick starts only with this much of the frame's budget left */
#define FQ_TICK_US	600u

enum { OP_NONE, OP_ERASE, OP_PROGRAM };

/* Call times of the platform driver (the write's log line, the self-test). */
typedef struct {
	uint32_t	n, sum, max;	/* calls, us total, us longest */
} tm_t;

static struct {
	const uint8_t	*src;
	uint32_t	len;
	uint32_t	sector0;	/* first sector of the range */
	uint32_t	nsec;
	uint32_t	npages;		/* pages of src, the last one partial */
	uint32_t	sec;		/* sector of the range being written */
	uint32_t	page;		/* its next page to look at */
	uint8_t		running;
	uint8_t		op;		/* operation in flight on the chip */
	uint8_t		erase_next;	/* the sector's erase starts next */
	int		result;
	uint64_t	op_t0;		/* the operation in flight started */
	uint64_t	poll_t;		/* a poll last found it running */
	/* the write's log */
	uint32_t	off;
	uint64_t	t0;
	uint32_t	steps, ticks, erased, programmed, blank, step_max;
	uint32_t	fr_max;		/* the most flash time in one frame */
	tm_t		erase_t, program_t, poll_t_us;
} q;

/* Per operation kind: the shortest start-to-end a poll saw (0 none yet).
 * Kept across writes: the chip's times do not change. */
static uint32_t est[3];
/* The frame's flash time (flq_step's and the ticks') and its budget (the
 * last flq_step's); the ticks' time since flq_take_tick_us. */
static uint32_t fr_us, fr_budget, tick_us;

/* The 16-byte-aligned page the driver programs from when a page of src is
 * partial or src is not 16-aligned. The chip's page buffer is loaded from
 * RAM when the operation starts (ares: when it executes), so stage stays
 * untouched until the operation ends: the next page is staged only then. */
static uint8_t stage[FQ_PAGE] __attribute__((aligned(16)));

static void tm_add(tm_t *t, uint64_t t0)
{
	uint32_t us = (uint32_t)(sys_time_us() - t0);

	t->n++;
	t->sum += us;
	if (us > t->max)
		t->max = us;
}

static uint32_t tm_avg(const tm_t *t)
{
	return t->n ? t->sum / t->n : 0;
}

static void fail(const char *what)
{
	q.running = 0;
	q.op = OP_NONE;
	q.result = -1;
	log_printf("flash: write 0x%x failed: %s (sector %u, page %u)\n", (unsigned)q.off, what,
		(unsigned)(q.sector0 + q.sec), (unsigned)q.page);
}

static void finish(void)
{
	q.running = 0;
	q.result = 0;
	if (fr_us > q.fr_max)
		q.fr_max = fr_us;
	log_printf("flash: wrote 0x%x + %u B: %u erases, %u programs, %u blank pages skipped, "
		"%u steps + %u ticks, %u ms, longest step %u us, most in a frame %u us\n",
		(unsigned)q.off, (unsigned)q.len, (unsigned)q.erased, (unsigned)q.programmed,
		(unsigned)q.blank, (unsigned)q.steps, (unsigned)q.ticks,
		(unsigned)((sys_time_us() - q.t0) / 1000u), (unsigned)q.step_max, (unsigned)q.fr_max);
	log_printf("flash: call us (n avg max): erase %u %u %u, program %u %u %u, poll %u %u %u\n",
		(unsigned)q.erase_t.n, (unsigned)tm_avg(&q.erase_t), (unsigned)q.erase_t.max,
		(unsigned)q.program_t.n, (unsigned)tm_avg(&q.program_t), (unsigned)q.program_t.max,
		(unsigned)q.poll_t_us.n, (unsigned)tm_avg(&q.poll_t_us), (unsigned)q.poll_t_us.max);
}

/* An operation started: its clock (est[] and the ticks' gaps count from
 * here). */
static void started(int op)
{
	q.op = (uint8_t)op;
	q.op_t0 = sys_time_us();
	q.poll_t = q.op_t0;
}

/* Poll the operation in flight: 1 it runs, 0 it ended (the chip is idle),
 * -1 it failed (the write ended). */
static int poll_op(void)
{
	uint64_t tp = sys_time_us();
	uint32_t run = (uint32_t)(tp - q.op_t0);
	int r = sys_flash_poll();

	tm_add(&q.poll_t_us, tp);
	if (r > 0)
	{
		q.poll_t = tp;
		return 1;
	}
	if (r < 0)
	{
		fail(q.op == OP_ERASE ? "erase reported an error" : "program reported an error");
		return -1;
	}
	if (!est[q.op] || run < est[q.op])
		est[q.op] = run;
	q.op = OP_NONE;
	return 0;
}

/* Page pg of src (pg < npages): bytes in range, the rest 0xFF. */
static uint32_t page_bytes(uint32_t pg)
{
	uint32_t left = q.len - pg * FQ_PAGE;

	return left < FQ_PAGE ? left : FQ_PAGE;
}

static int page_is_blank(uint32_t pg)
{
	const uint8_t *p = q.src + pg * FQ_PAGE;
	uint32_t i, n = page_bytes(pg);

	for (i = 0; i < n; i++)
		if (p[i] != 0xFF)
			return 0;
	return 1;
}

static const uint8_t *page_data(uint32_t pg)
{
	const uint8_t *p = q.src + pg * FQ_PAGE;
	uint32_t n = page_bytes(pg);

	if (n == FQ_PAGE && !((uintptr_t)p & 15u))
		return p;
	memcpy(stage, p, n);
	memset(stage + n, 0xFF, FQ_PAGE - n);
	return stage;
}

/* The chip is idle: start the write's next operation. 1 started, 0 the
 * write ended (finished or failed). */
static int next_op(void)
{
	for (;;)
	{
		if (q.erase_next)
		{
			uint64_t t0 = sys_time_us();
			int r = sys_flash_erase(q.sector0 + q.sec);

			tm_add(&q.erase_t, t0);
			if (r < 0)
			{
				fail("erase did not start");
				return 0;
			}
			q.erase_next = 0;
			q.page = 0;
			started(OP_ERASE);
			q.erased++;
			return 1;
		}
		while (q.page < FQ_PAGES)
		{
			uint32_t pg = q.sec * FQ_PAGES + q.page++;
			uint64_t t0;
			int r;

			if (pg >= q.npages)
			{
				q.page = FQ_PAGES;
				break;
			}
			if (page_is_blank(pg))
			{
				q.blank++;
				continue;
			}
			t0 = sys_time_us();
			r = sys_flash_program(q.off / FQ_PAGE + pg, page_data(pg));
			tm_add(&q.program_t, t0);
			if (r < 0)
			{
				fail("program did not start");
				return 0;
			}
			started(OP_PROGRAM);
			q.programmed++;
			return 1;
		}
		if (++q.sec >= q.nsec)
		{
			finish();
			return 0;
		}
		q.erase_next = 1;
	}
}

int flq_write(uint32_t off, const void *src, uint32_t len)
{
	uint64_t t0;
	int r;

	if (q.running || !src || !len || (off & (FQ_SECTOR - 1)) || off > FQ_BYTES || len > FQ_BYTES - off)
		return -1;
	t0 = sys_time_us();
	r = sys_flash_erase(off / FQ_SECTOR);
	if (r < 0)
		return -1;
	memset(&q, 0, sizeof(q));
	q.src = src;
	q.len = len;
	q.off = off;
	q.sector0 = off / FQ_SECTOR;
	q.nsec = (len + FQ_SECTOR - 1) / FQ_SECTOR;
	q.npages = (len + FQ_PAGE - 1) / FQ_PAGE;
	q.running = 1;
	started(OP_ERASE);
	q.erased = 1;
	q.t0 = t0;
	tm_add(&q.erase_t, t0);
	return 0;
}

void flq_step(uint32_t budget_us)
{
	uint64_t t0;
	uint32_t us;

	if (q.running && fr_us > q.fr_max)
		q.fr_max = fr_us;
	fr_budget = budget_us;
	fr_us = 0;
	if (!q.running)
		return;
	t0 = sys_time_us();
	q.steps++;
	for (;;)
	{
		if (q.op != OP_NONE && poll_op())
			break;
		if (!next_op())
			break;
		if (sys_time_us() - t0 >= budget_us)
			break;
	}
	us = (uint32_t)(sys_time_us() - t0);
	fr_us = us;
	if (us > q.step_max)
		q.step_max = us;
}

void flq_tick(void)
{
	uint64_t t0;
	uint32_t us;

	if (!q.running || fr_us + FQ_TICK_US > fr_budget)
		return;
	t0 = sys_time_us();
	if (q.op != OP_NONE)
	{
		uint32_t run = (uint32_t)(t0 - q.op_t0), gap = run / 8u, e = est[q.op];

		if (gap < FQ_POLL_US)
			gap = FQ_POLL_US;
		if (run < e)
		{
			/* about to end: wait for it here rather than for a later
			 * tick, within the frame's budget */
			if (e - run > FQ_SPIN_US || fr_us + (e - run) + FQ_TICK_US > fr_budget)
				return;
			while ((uint32_t)(sys_time_us() - q.op_t0) < e)
				;
		}
		else if (t0 - q.poll_t < gap)
			return;
	}
	q.ticks++;
	if (q.op == OP_NONE || !poll_op())
		next_op();
	us = (uint32_t)(sys_time_us() - t0);
	fr_us += us;
	tick_us += us;
}

uint32_t flq_take_tick_us(void)
{
	uint32_t us = tick_us;

	tick_us = 0;
	return us;
}

int flq_busy(void)
{
	return q.running;
}

int flq_result(void)
{
	return q.result;
}

int flq_read(void *dst, uint32_t off, uint32_t len)
{
	if (q.running)
		return -1;
	return sys_flash_read(dst, off, len);
}

/* ---- the test build's `flash` debug command ------------------------------ */

#ifdef HL64_TEST

#define SELF_SECTOR	7u		/* the last sector */
#define SELF_STEP_US	2000u		/* budget of the self-test's steps */
#define SELF_LIMIT_US	20000000u	/* a write that takes longer fails */

/* Two patterns, so each run overwrites the other's and the erase is
 * exercised (a page the old pattern programmed is blank in the new one):
 * A leaves pages 1 and 100 blank (all 0xFF), B pages 5 and 77; every other
 * page has a non-0xFF byte. */
static uint8_t self_byte(int variant, uint32_t i)
{
	uint32_t page = i / FQ_PAGE;

	if (variant ? (page == 5 || page == 77) : (page == 1 || page == 100))
		return 0xFF;
	return (uint8_t)((i * (variant ? 29u : 31u) + page * (variant ? 11u : 17u) + (variant ? 0x71u : 0x35u)) & 0xFFu);
}

static uint8_t self_buf[FQ_SECTOR] __attribute__((aligned(16)));

/* First mismatch of buf against variant's pattern, FQ_SECTOR if none. */
static uint32_t self_first_diff(int variant)
{
	uint32_t i;

	for (i = 0; i < FQ_SECTOR; i++)
		if (self_buf[i] != self_byte(variant, i))
			return i;
	return FQ_SECTOR;
}

static void log_head(uint32_t sector)
{
	uint8_t *h = self_buf;

	if (flq_read(h, sector * FQ_SECTOR, 16))
	{
		log_printf("flash: sector %u: unreadable\n", (unsigned)sector);
		return;
	}
	log_printf("flash: sector %u: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
		(unsigned)sector, h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7], h[8], h[9], h[10], h[11],
		h[12], h[13], h[14], h[15]);
}

int flq_dbg_flash(uint32_t arg)
{
	uint64_t t0;
	uint32_t i, diff;
	int variant, r;

	if (arg > 1)
	{
		log_printf("flash: argument 0 (probe) or 1 (self-test)\n");
		return -1;
	}
	if (q.running)
	{
		log_printf("flash: a write is running\n");
		return -1;
	}

	/* what the last sector holds: pattern A, B or neither */
	t0 = sys_time_us();
	r = flq_read(self_buf, SELF_SECTOR * FQ_SECTOR, FQ_SECTOR);
	if (r)
	{
		log_printf("flash: no chip or unreadable\n");
		return -1;
	}
	log_printf("flash: read %u B in %u us\n", (unsigned)FQ_SECTOR, (unsigned)(sys_time_us() - t0));
	if (self_first_diff(0) == FQ_SECTOR)
		variant = 0;
	else if (self_first_diff(1) == FQ_SECTOR)
		variant = 1;
	else
		variant = -1;

	if (arg == 0)
	{
		for (i = 0; i < 8; i++)
			log_head(i);
		log_printf("flash: sector %u holds %s\n", SELF_SECTOR,
			variant < 0 ? "no self-test pattern" : variant ? "pattern B" : "pattern A");
		return 0;
	}

	/* write the other pattern over the last sector, step it to the end */
	variant = variant == 0;
	for (i = 0; i < FQ_SECTOR; i++)
		self_buf[i] = self_byte(variant, i);
	if (flq_write(SELF_SECTOR * FQ_SECTOR, self_buf, FQ_SECTOR))
	{
		log_printf("flash: selftest: write did not start\n");
		return -1;
	}
	t0 = sys_time_us();
	while (flq_busy())
	{
		flq_step(SELF_STEP_US);
		if (sys_time_us() - t0 > SELF_LIMIT_US)
		{
			log_printf("flash: selftest: write still running after %u s\n", SELF_LIMIT_US / 1000000u);
			return -1;
		}
	}
	if (flq_result())
	{
		log_printf("flash: selftest: write failed\n");
		return -1;
	}

	/* a fresh buffer: the read must not be the pattern left in it */
	memset(self_buf, 0xA5, sizeof(self_buf));
	t0 = sys_time_us();
	if (flq_read(self_buf, SELF_SECTOR * FQ_SECTOR, FQ_SECTOR))
	{
		log_printf("flash: selftest: read back failed\n");
		return -1;
	}
	log_printf("flash: read %u B in %u us\n", (unsigned)FQ_SECTOR, (unsigned)(sys_time_us() - t0));
	diff = self_first_diff(variant);
	if (diff != FQ_SECTOR)
	{
		log_printf("flash: selftest: mismatch at sector %u + 0x%x: read %02x, wanted %02x\n",
			SELF_SECTOR, (unsigned)diff, self_buf[diff], self_byte(variant, diff));
		return -1;
	}
	log_printf("flash: selftest ok (pattern %c)\n", variant ? 'B' : 'A');
	return 0;
}

#else

int flq_dbg_flash(uint32_t arg)
{
	(void)arg;
	log_printf("flash: the flash command is a test-build command\n");
	return -1;
}

#endif
