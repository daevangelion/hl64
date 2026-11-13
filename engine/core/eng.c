/* SPDX-License-Identifier: MIT */
/*
 * eng.c -- engine boot and the frame loop skeleton (docs/architecture.md
 * 3.1, 3.2) and the debug commands the core owns (12).
 *
 * The loop runs the 3.2 steps in order. M0 has the platform steps (1 VI
 * dt, 3 I/O pump, 11-12 frame bracket and paced present) and the test
 * step (13: debug commands, 1 s roll-up, log flush); steps 2 and 4-10
 * are added by their modules (input M2; game, physics, world, entities,
 * residency, sound M1-M3) at the marked places, with `now` advancing by
 * dt after step 6.
 */
#include "hl64/limits.h"
#include "hl64/settings.h"
#include "engine/core/dbg.h"
#include "engine/core/eng.h"
#include "engine/core/log.h"
#include "engine/core/sys.h"
#include "engine/io/io.h"
#include "engine/mem/carve.h"
#include "engine/mem/heap.h"

static struct {
	uint32_t	frame;		/* frames completed */
	uint64_t	now_us;		/* game time */
	uint32_t	last_vi;
	float		timescale;
	uint32_t	fixed_dt_us;	/* test builds; 0 = VI clock */
	uint8_t		rollup;		/* 1 s roll-up line (DBG_PROF) */
	uint32_t	rollup_frames;
	uint64_t	rollup_t0;
} eng;

/* ---- stats record (15) ------------------------------------------------- */

typedef struct {
	uint32_t	frame;
	uint64_t	now_us;
	unsigned	heap_free_kb, heap_largest_kb;
	unsigned	heap_live_kb, heap_peak_kb, heap_fails;
	uint32_t	io_queued, io_blocked_max_us, io_budget_over;
	uint32_t	fpu_traps;
	int		stack_peak;
} stats_t;

static void stats_fill(stats_t *s)
{
	io_stats_t io;
	unsigned total_mal, total_fr, live_n;

	s->frame = eng.frame;
	s->now_us = eng.now_us;
	s->heap_free_kb = (unsigned)(mem_heap_free_total() / 1024u);
	s->heap_largest_kb = (unsigned)(mem_heap_probe_largest(HL64_RDRAM_BYTES) / 1024u);
	mem_heap_lifetime(&total_mal, &total_fr, &s->heap_fails, &live_n,
		&s->heap_live_kb, &s->heap_peak_kb);
	io_stats(&io, 0);
	s->io_queued = io_queued();
	s->io_blocked_max_us = io.frame_blocked_max_us;
	s->io_budget_over = io.frames_budget_over;
	s->fpu_traps = sys_fpu_traps();
	s->stack_peak = sys_stack_peak();
}

/* ---- core debug commands (12) -------------------------------------------- */

static int cmd_set(const dbg_cmd_t *c)
{
	return settings_set((int)c->arg[0].u, c->arg[1].f);
}

static int cmd_get(const dbg_cmd_t *c)
{
	float v;

	if (settings_get((int)c->arg[0].u, &v))
		return -1;
	log_printf("set %s %g\n", settings_desc[c->arg[0].u].name, (double)v);
	return 0;
}

static int cmd_timescale(const dbg_cmd_t *c)
{
	float f = c->arg[0].f;

	if (!(f >= 1.0f / 16 && f <= 16))
		return -1;
	eng.timescale = f;
	return 0;
}

static int cmd_fixed_dt(const dbg_cmd_t *c)
{
	if (c->arg[0].u > HL64_DT_MAX_US)
		return -1;
	eng.fixed_dt_us = c->arg[0].u;
	return 0;
}

static int cmd_shot(const dbg_cmd_t *c)
{
	if (sys_shot())
	{
		log_printf("shot %lu: no framebuffer\n", (unsigned long)c->arg[0].u);
		return -1;
	}
	log_printf("shot %lu frame=%lu\n", (unsigned long)c->arg[0].u,
		(unsigned long)eng.frame);
	return 0;
}

static int cmd_stats(const dbg_cmd_t *c)
{
	stats_t s;
	char pace[128];

	stats_fill(&s);
	sys_pace_stats(pace, sizeof(pace));
	log_printf("stats %lu frame=%lu t=%lu heap=%u/%uKB live=%uKB peak=%uKB "
		"fails=%u ioq=%lu blkmax=%luus over=%lu fpu=%lu stack=%d %s\n",
		(unsigned long)c->arg[0].u, (unsigned long)s.frame,
		(unsigned long)(s.now_us / 1000u), s.heap_free_kb, s.heap_largest_kb,
		s.heap_live_kb, s.heap_peak_kb, s.heap_fails,
		(unsigned long)s.io_queued, (unsigned long)s.io_blocked_max_us,
		(unsigned long)s.io_budget_over, (unsigned long)s.fpu_traps,
		s.stack_peak, pace);
	return 0;
}

static int cmd_mem(const dbg_cmd_t *c)
{
	unsigned total_mal, total_fr, fails, live_n, live_kb, peak_kb;

	(void)c;
	mem_carve_log();
	mem_heap_lifetime(&total_mal, &total_fr, &fails, &live_n, &live_kb, &peak_kb);
	log_printf("mem: allocs %u frees %u fails %u live %u (%u KB) peak %u KB stack %d\n",
		total_mal, total_fr, fails, live_n, live_kb, peak_kb, sys_stack_peak());
	mem_heap_map_dump();
	return 0;
}

static int cmd_prof(const dbg_cmd_t *c)
{
	eng.rollup = c->arg[0].u != 0;
	eng.rollup_frames = 0;
	eng.rollup_t0 = sys_time_us();
	return 0;
}

static int cmd_flush(const dbg_cmd_t *c)
{
	(void)c;
	log_flush();
	return 0;
}

static int cmd_quit(const dbg_cmd_t *c)
{
	sys_exit((int)c->arg[0].u);
}

/* ---- boot, frame ----------------------------------------------------------- */

void eng_boot(const char *platform)
{
#ifdef HL64_TEST
	const char *test = " test";
#else
	const char *test = "";
#endif
#ifdef HL64_PROFILE
	const char *prof = " profile";
#else
	const char *prof = "";
#endif

	log_printf("hl64: boot %s%s%s\n", platform, test, prof);
	log_printf("vid: vi period %lu us, present every %d vblanks\n",
		(unsigned long)sys_vi_period_us(), HL64_PRESENT_VBL);
	mem_boot_carve();
	settings_reset();
	io_init();
	dbg_init();
	dbg_handler_set(DBG_SET_SETTING, cmd_set);
	dbg_handler_set(DBG_GET_SETTING, cmd_get);
	dbg_handler_set(DBG_TIMESCALE, cmd_timescale);
	dbg_handler_set(DBG_FIXED_DT, cmd_fixed_dt);
	dbg_handler_set(DBG_SHOT, cmd_shot);
	dbg_handler_set(DBG_STATS, cmd_stats);
	dbg_handler_set(DBG_MEM, cmd_mem);
	dbg_handler_set(DBG_PROF, cmd_prof);
	dbg_handler_set(DBG_LOG_FLUSH, cmd_flush);
	dbg_handler_set(DBG_QUIT, cmd_quit);

	eng.frame = 0;
	eng.now_us = 0;
	eng.timescale = 1.0f;
	eng.fixed_dt_us = 0;
#ifdef HL64_PROFILE
	eng.rollup = 1;
#endif
	eng.rollup_t0 = sys_time_us();
	eng.last_vi = sys_vi_count();
	log_printf("boot ok\n");
	log_flush();
}

/* Once a second of wall time: frames, mean frame time, pacing, I/O. */
static void rollup(void)
{
	uint64_t t = sys_time_us(), span = t - eng.rollup_t0;
	char pace[128];
	io_stats_t io;

	eng.rollup_frames++;
#ifdef HL64_PROFILE
	sys_prof_frame();
#endif
	if (!eng.rollup || span < 1000000u)
		return;
	sys_pace_stats(pace, sizeof(pace));
	io_stats(&io, 1);
	log_printf("prof frames=%lu frame_us=%lu %s io=%luB blk=%luus\n",
		(unsigned long)eng.rollup_frames,
		(unsigned long)(span / eng.rollup_frames), pace,
		(unsigned long)io.async_bytes, (unsigned long)io.blocked_us);
#ifdef HL64_PROFILE
	sys_prof_dump(eng.rollup_frames);
#endif
	eng.rollup_frames = 0;
	eng.rollup_t0 = t;
}

void eng_frame(void)
{
	uint32_t vi = sys_vi_count();
	uint32_t dt = (vi - eng.last_vi) * sys_vi_period_us();

	/* 1 plat: dt from the VI clock, clamped (D6 i); test builds may fix it */
	eng.last_vi = vi;
	if (dt < HL64_DT_MIN_US)
		dt = HL64_DT_MIN_US;
	if (dt > HL64_DT_MAX_US)
		dt = HL64_DT_MAX_US;
	if (eng.fixed_dt_us)
		dt = eng.fixed_dt_us;
	if (eng.timescale != 1.0f)
		dt = (uint32_t)(dt * eng.timescale + 0.5f);

	/* 2 in: pad -> usercmd (M2) */

	/* 3 io */
	io_frame_begin(dt);
	io_pump();

	/* 4-6 game, player, active set (M3) */
	eng.now_us += dt;
	/* 7-10 world, hydration, residency, sound (M1-M4) */

	/* 11-12 render into a free framebuffer; submit for paced present */
	while (!sys_frame_try_begin())
		io_pump();
	io_pump();
	sys_frame_end();

	/* 13 test: debug commands at the frame boundary, roll-up, log */
	eng.frame++;
	dbg_run(eng.frame);
	rollup();
	log_flush();
}
