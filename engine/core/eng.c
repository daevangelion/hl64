/* SPDX-License-Identifier: MIT */
/*
 * eng.c -- engine boot and the frame loop skeleton (docs/architecture.md
 * 3.1, 3.2) and the debug commands the core owns (12).
 *
 * The loop runs the 3.2 steps in order. M0 has the platform steps (1 VI
 * dt, 3 I/O pump, 11-12 frame bracket and paced present) and the test
 * step (13: debug commands, 1 s roll-up, log flush); M1 adds the free
 * camera at step 2, the world frame (residency fences, lightstyle tick,
 * dynamic lights) at steps 7-9 and the world draw at step 11. M2 adds the
 * pad -> usercmd at step 2 once the player exists (the free camera flies
 * before), the player move at step 5, the player's view as the camera at
 * step 11 and the route VM at step 13. Steps 4, 6, 8 and 10 are added by
 * their modules (game, entities, sound M3-M4). The game clock (f64 s from
 * 1.0, think-and-time.md 1) advances by dt after step 6; the player, the
 * world effects (lightstyles, dynamic lights), the draw, the view and the
 * route all run on it. Integer microseconds remain only for engine-internal
 * timing (dt from the VI clock, I/O, pacing, profiling).
 */
#include "hl64/ent.h"
#include "hl64/grid.h"
#include "hl64/limits.h"
#include "hl64/settings.h"
#include "engine/core/dbg.h"
#include "engine/core/eng.h"
#include "engine/core/log.h"
#include "engine/core/sys.h"
#include "engine/io/io.h"
#include "engine/mem/carve.h"
#include "engine/mem/heap.h"
#include "engine/render/gpu.h"
#include "engine/render/ren.h"
#include "engine/world/wld.h"
#include "engine/in/in.h"
#include "engine/phys/phys.h"
#include "engine/phys/player.h"
#include "engine/test/route.h"
#include "engine/view/view.h"

const char *eng_route_path = "route.rbc";
static struct {
	uint32_t	frame;		/* frames completed */
	double		time;		/* game clock, f64 s from 1.0 (think-and-time.md 1) */
	uint32_t	last_vi;
	float		timescale;
	uint32_t	fixed_dt_us;	/* test builds; 0 = VI clock */
	uint8_t		rollup;		/* 1 s roll-up line (DBG_PROF) */
	uint8_t		world;		/* player content present */
	uint32_t	rollup_frames;
	uint64_t	rollup_t0;
	uint64_t	rollup_world_us;	/* world draw CPU in the roll-up window */
	uint32_t	rollup_world_max;
} eng;

/* ---- stats record (15) ------------------------------------------------- */

typedef struct {
	uint32_t	frame;
	double		time;		/* game clock */
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
	s->time = eng.time;
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

/* The drawn frame's world counts (renderer) and the GPU calls (recorder on
 * the host); identical lines on the cart and the host. */
static void log_draw(const char *what, uint32_t label)
{
	ren_stats_t r;
	gpu_stats_t g;

	if (!eng.world)
		return;
	ren_stats(&r);
	gpu_stats(&g);
	log_printf("%s %lu ren: map=%s eye=%.9g,%.9g,%.9g ang=%.9g,%.9g,%.9g leaf=%lu r10=%lu "
		"sectors=%lu movers=%lu meshlets=%lu verts=%lu tris=%lu binds=%lu relit=%lu "
		"dlit=%lu missing=%lu dropped=%lu world_us=%lu\n", what, (unsigned long)label,
		wld_map_name(ren_cam.map), (double)ren_cam.org[0], (double)ren_cam.org[1],
		(double)ren_cam.org[2], (double)ren_cam.ang[0], (double)ren_cam.ang[1],
		(double)ren_cam.ang[2], (unsigned long)r.leaf, (unsigned long)r.r10,
		(unsigned long)r.sectors, (unsigned long)r.movers, (unsigned long)r.meshlets,
		(unsigned long)r.verts, (unsigned long)r.tris, (unsigned long)r.binds,
		(unsigned long)r.relit, (unsigned long)r.dlit, (unsigned long)r.missing,
		(unsigned long)r.dropped, (unsigned long)r.world_us);
	log_printf("%s %lu gpu: binds=%lu meshlets=%lu verts=%lu tris=%lu hash=%08lx\n", what,
		(unsigned long)label, (unsigned long)g.binds, (unsigned long)g.meshlets,
		(unsigned long)g.verts, (unsigned long)g.tris, (unsigned long)g.hash);
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
	log_draw("shot", c->arg[0].u);
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
		(unsigned long)(s.time * 1000.0), s.heap_free_kb, s.heap_largest_kb,
		s.heap_live_kb, s.heap_peak_kb, s.heap_fails,
		(unsigned long)s.io_queued, (unsigned long)s.io_blocked_max_us,
		(unsigned long)s.io_budget_over, (unsigned long)s.fpu_traps,
		s.stack_peak, pace);
	if (eng.world)
	{
		wld_stats_t w;

		wld_stats(&w);
		log_printf("stats %lu world: loads=%lu evict=%lu nofit=%lu render=%lu/%luKB "
			"texture=%lu/%luKB coll=%lu/%luKB\n", (unsigned long)c->arg[0].u,
			(unsigned long)w.loads, (unsigned long)w.evictions, (unsigned long)w.nofit,
			(unsigned long)(w.pool_used[0] / 1024), (unsigned long)(w.pool_peak[0] / 1024),
			(unsigned long)(w.pool_used[1] / 1024), (unsigned long)(w.pool_peak[1] / 1024),
			(unsigned long)(w.pool_used[2] / 1024), (unsigned long)(w.pool_peak[2] / 1024));
		log_draw("stats", c->arg[0].u);
	}
	if (phy_player_active())
	{
		const ent_player_t *pl = phy_player();

		log_printf("stats %lu player: map=%s org=%.9g,%.9g,%.9g vel=%.9g,%.9g,%.9g "
			"flags=%lu hull=%u ground=%d water=%d\n", (unsigned long)c->arg[0].u,
			wld_map_name(phy_player_map()), (double)pl->origin[0], (double)pl->origin[1],
			(double)pl->origin[2], (double)pl->velocity[0], (double)pl->velocity[1],
			(double)pl->velocity[2], (unsigned long)pl->flags, pl->usehull,
			pl->groundentity, pl->waterlevel);
	}
	return 0;
}

static int cmd_mem(const dbg_cmd_t *c)
{
	unsigned total_mal, total_fr, fails, live_n, live_kb, peak_kb;

	(void)c;
	mem_carve_log();
	ent_mem_log();
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
	eng.rollup_world_us = 0;
	eng.rollup_world_max = 0;
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
	ent_init();
	eng.world = (uint8_t)wld_init();
	col_init();
	if (eng.world)
	{
		ren_init();
		ren_cam_init();
	}
	in_init();
	phy_player_init();
	if (eng.world)
	{
		int r = rt_load(eng_route_path);

		if (r < 0)
		{
			log_printf("route FAIL load %s\n", eng_route_path);
			log_flush();
			sys_exit(1);
		}
		/* a route plays from the campaign start (New Game) */
		if (r > 0 && phy_player_spawn_start(wld_start_map()))
			sys_error("route: no start map");
	}

	/* New Game clock: time 1.0, two settle frames (think-and-time.md 6) */
	eng_clock_new_game();
	eng.frame = 0;
	eng.time = eng_clock_time();
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
	if (eng.world)
		log_printf("prof world_us avg=%lu max=%lu frames=%lu\n",
			(unsigned long)(eng.rollup_world_us / eng.rollup_frames),
			(unsigned long)eng.rollup_world_max, (unsigned long)eng.rollup_frames);
	eng.rollup_world_us = 0;
	eng.rollup_world_max = 0;
	if (phy_player_active())
	{
		unsigned long n;
		phy_player_prof_t p;

		phy_player_prof(&p, 1);
		n = p.frames ? p.frames : 1;
		log_printf("prof pmove_us avg=%lu max=%lu frames=%lu physents_max=%lu "
			"overflow=%lu sounds=%lu trace_us=%lu traces=%lu clips=%lu "
			"contents_us=%lu contents=%lu\n",
			(unsigned long)p.us_sum / n, (unsigned long)p.us_max,
			(unsigned long)p.frames, (unsigned long)p.physents_max,
			(unsigned long)p.overflow, (unsigned long)p.sounds,
			(unsigned long)p.trace_us / n, (unsigned long)p.traces / n,
			(unsigned long)p.clips / n, (unsigned long)p.contents_us / n,
			(unsigned long)p.contents / n);
	}
	{
		phy_ent_stats_t s;
		unsigned long n;

		phy_ent_stats(&s, 1);
		n = s.frames ? s.frames : 1;
		log_printf("prof ent_us avg=%lu max=%lu stepped=%lu thinks=%lu movers_max=%lu "
			"heap_max=%lu\n", (unsigned long)s.us / n, (unsigned long)s.us_max,
			(unsigned long)s.stepped / n, (unsigned long)s.thinks / n,
			(unsigned long)s.movers_max, (unsigned long)s.heap_max);
	}
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
	in_cmd_t cmd;
	double gdt;

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

	/* 2 in: pad -> usercmd once the player exists; before: the free camera */
	if (phy_player_active())
		in_frame(dt, phy_player()->maxspeed, &cmd);
	else if (eng.world)
		ren_cam_update(dt);

	/* 3 io */
	io_frame_begin(dt);
	io_pump();

	/* 4 game (M3): the frame's clock (dt 0 in the settle frames) */
	gdt = eng_clock_frame((double)dt * 1e-6);
	/* 5 player: pmove host */
	phy_player_frame(&cmd, eng.time);
	/* 6 active set */
	phy_entities_frame(eng.time, gdt);
	/* the clock advances by the frame time (f64) after step 6 */
	eng_clock_advance();
	eng.time = eng_clock_time();
	/* 7-10 world, hydration, residency, sound (M1-M4) */
	if (eng.world)
	{
		wld_frame(eng.frame);
		wld_style_tick(eng.time);
		ren_dlight_frame(eng.time, (double)dt * 1e-6);
	}

	/* 11-12 render into a free framebuffer; submit for paced present */
	while (!sys_frame_try_begin())
		io_pump();
	io_pump();
	if (eng.world)
	{
		ren_stats_t r;

		ren_view_frame(eng.time, (double)dt * 1e-6);
		ren_frame(eng.time);
		ren_stats(&r);
		eng.rollup_world_us += r.world_us;
		if (r.world_us > eng.rollup_world_max)
			eng.rollup_world_max = r.world_us;
		io_pump();
	}
	sys_frame_end();

	/* 13 test: debug commands at the frame boundary, route VM, roll-up, log */
	eng.frame++;
	dbg_run(eng.frame);
	if (rt_active())
		rt_frame(eng.time);
	rollup();
	log_flush();
}
