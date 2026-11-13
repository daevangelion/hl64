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
#include <math.h>
#include <string.h>

#include "hl64/ent.h"
#include "hl64/grid.h"
#include "hl64/game.h"
#include "hl64/limits.h"
#include "hl64/rec.h"
#include "hl64/seam.h"
#include "hl64/settings.h"
#include "hl64/snd.h"
#include "engine/core/dbg.h"
#include "engine/core/eng.h"
#include "engine/core/hlmath.h"
#include "engine/core/log.h"
#include "engine/core/rng.h"
#include "engine/core/sys.h"
#include "engine/ent/rec_int.h"
#include "engine/io/io.h"
#include "engine/res/res.h"
#include "engine/mem/carve.h"
#include "engine/mem/heap.h"
#include "engine/render/gpu.h"
#include "engine/render/gpu_2d.h"
#include "hl64/draw2d.h"
#include "engine/render/gpu_studio.h"
#include "hl64/studio.h"
#include "hl64/fx.h"
#include "engine/render/gpu_fx.h"
#include "engine/render/trans.h"
#include "engine/render/ren.h"
#include "engine/world/wld.h"
#include "engine/in/in.h"
#include "engine/phys/phys.h"
#include "engine/phys/player.h"
#include "engine/phys/motion.h"
#include "engine/test/route.h"
#include "engine/test/seamprof.h"
#include "engine/view/view.h"

const char *eng_route_path = "route.rbc";
#ifdef HL64_TEST
int eng_play;
#else
int eng_play = 1;
#endif

static struct {
	uint32_t	frame;		/* frames completed */
	double		time;		/* game clock, f64 s from 1.0 (think-and-time.md 1) */
	uint32_t	last_vi;
	float		timescale;
	uint32_t	fixed_dt_us;	/* test builds; 0 = VI clock */
	uint8_t		rollup;		/* 1 s roll-up line (DBG_PROF) */
	uint8_t		world;		/* player content present */
	uint8_t		ents_dump;	/* DBG_ENTS_DUMP: per-entity stats lines */
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
	s->io_blocked_max_us = io.life_blocked_max_us;
	s->io_budget_over = io.life_frames_budget_over;
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
		"dlit=%lu missing=%lu dropped=%lu world_us=%lu oseam=%lu oseam_ents=%lu "
		"other_ents=%lu\n", what,
		(unsigned long)label,
		wld_map_name(ren_cam.map), (double)ren_cam.org[0], (double)ren_cam.org[1],
		(double)ren_cam.org[2], (double)ren_cam.ang[0], (double)ren_cam.ang[1],
		(double)ren_cam.ang[2], (unsigned long)r.leaf, (unsigned long)r.r10,
		(unsigned long)r.sectors, (unsigned long)r.movers, (unsigned long)r.meshlets,
		(unsigned long)r.verts, (unsigned long)r.tris, (unsigned long)r.binds,
		(unsigned long)r.relit, (unsigned long)r.dlit, (unsigned long)r.missing,
		(unsigned long)r.dropped, (unsigned long)r.world_us,
		(unsigned long)r.oseam_meshlets, (unsigned long)r.oseam_ents,
		(unsigned long)r.other_ents);
	log_printf("%s %lu gpu: binds=%lu meshlets=%lu verts=%lu tris=%lu hash=%08lx\n", what,
		(unsigned long)label, (unsigned long)g.binds, (unsigned long)g.meshlets,
		(unsigned long)g.verts, (unsigned long)g.tris, (unsigned long)g.hash);
	{
		gpu_studio_stats_t sg;

		gpu_studio_stats(&sg);
		log_printf("%s %lu studio: ents=%lu runs=%lu meshlets=%lu verts=%lu tris=%lu "
			"mats=%lu hash=%08lx\n", what, (unsigned long)label, (unsigned long)sg.ents,
			(unsigned long)sg.runs, (unsigned long)sg.meshlets, (unsigned long)sg.verts,
			(unsigned long)sg.tris, (unsigned long)sg.mats, (unsigned long)sg.hash);
	}
	{
		gpu_fx_stats_t gf;
		fx_draw_stats_t fd;
		int m;
		unsigned long calls = 0;

		gpu_fx_stats(&gf);
		fx_draw_stats(&fd);
		for (m = 0; m < GPU_FX_MODES; m++)
			calls += gf.calls[m];
		log_printf("%s %lu fx: items=%lu cutouts=%lu dropped=%lu sprites=%lu studio=%lu "
			"brushes=%lu temps=%lu beams=%lu occluded=%lu decals=%lu dverts=%lu calls=%lu "
			"tris=%lu points=%lu tex=%lu pairs=%lu drops=%lu hash=%08lx\n", what,
			(unsigned long)label,
			(unsigned long)fd.items, (unsigned long)fd.cutouts, (unsigned long)fd.dropped,
			(unsigned long)fd.sprites, (unsigned long)fd.studio, (unsigned long)fd.brushes,
			(unsigned long)fd.temps, (unsigned long)fd.beams, (unsigned long)fd.glow_occluded,
			(unsigned long)fd.decals, (unsigned long)fd.decal_verts, calls,
			(unsigned long)gf.tris, (unsigned long)gf.points, (unsigned long)gf.texloads,
			(unsigned long)gf.pairs, (unsigned long)gf.drops, (unsigned long)gf.hash);
	}
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
	{
		gpu_2d_stats_t h;

		gpu_2d_stats(&h);
		log_printf("shot %lu hud: pics=%lu fills=%lu texels=%lu hash=%08lx\n",
			(unsigned long)c->arg[0].u, (unsigned long)h.pics, (unsigned long)h.fills,
			(unsigned long)h.texels, (unsigned long)h.hash);
	}
	return 0;
}

/* FNV-1a over a 32-bit value, byte order independent (host = cart) */
static uint32_t hash_u32(uint32_t h, uint32_t v)
{
	int k;

	for (k = 0; k < 4; k++, v >>= 8)
		h = (h ^ (v & 0xFFu)) * 16777619u;
	return h;
}

static uint32_t hash_f(uint32_t h, const float *f, int n)
{
	uint32_t u;

	while (n--)
	{
		memcpy(&u, f++, sizeof(u));
		h = hash_u32(h, u);
	}
	return h;
}

/* One part of a live entity's simulated state into the hash (ENT_PARTS,
 * in this order: identity, collision view, motion, physics, think times
 * and functions, animation). */
#define ENT_PARTS	6
static uint32_t ent_part_hash(uint32_t h, int e, int part)
{
	const col_ent_t *cv = ent_cv(e);
	const ent_mot_t *m = ent_mot(e);
	const ent_phys_t *ph = ent_phys(e);
	const ent_tm_t *tm = ent_tm(e);
	const ent_rnd_t *r = ent_rnd(e);
	const ent_id_t *id = ent_idc(e);

	switch (part)
	{
	case 0:
		h = hash_u32(h, (uint32_t)e << 16 | id->cls);
		h = hash_u32(h, (uint32_t)id->gid << 16 | id->targetname);
		break;
	case 1:
		h = hash_f(h, cv->origin, 3);
		h = hash_f(h, cv->angles, 3);
		h = hash_f(h, cv->mins, 3);
		h = hash_f(h, cv->maxs, 3);
		h = hash_u32(h, cv->flags);
		h = hash_u32(h, (uint32_t)cv->owner << 16 | cv->model);
		h = hash_u32(h, (uint32_t)(uint16_t)cv->skin << 16 | (uint32_t)cv->solid << 8 | cv->movetype);
		break;
	case 2:
		h = hash_f(h, m->velocity, 3);
		h = hash_f(h, m->avelocity, 3);
		h = hash_f(h, m->basevelocity, 3);
		break;
	case 3:
		h = hash_u32(h, (uint32_t)ph->groundentity << 16 | ph->aiment);
		h = hash_u32(h, (uint32_t)(uint8_t)ph->waterlevel << 8 | (uint8_t)ph->watertype);
		h = hash_f(h, &ph->ideal_yaw, 1);
		break;
	case 4:
		h = hash_f(h, &tm->nextthink, 1);
		h = hash_f(h, &tm->ltime, 1);
		h = hash_u32(h, (uint32_t)tm->think << 16 | tm->touch);
		h = hash_u32(h, (uint32_t)tm->use << 16 | tm->blocked);
		break;
	default:
		h = hash_u32(h, (uint32_t)r->model << 16 | r->sequence);
		h = hash_f(h, &r->frame, 1);
		h = hash_u32(h, (uint32_t)r->effects << 16 | (uint32_t)r->body << 8 | r->renderamt);
		break;
	}
	return h;
}

/* Stats record `ents` (K9 step-all equivalence, K10 host = cart): live
 * entity count and a hash of every live entity's simulated state. With
 * `entsdump 1` (test builds) every live entity also logs a line of its
 * parts' hashes (`stats N ent E cls= gid= tn= id= cv= mot= ph= tm= rnd=`):
 * the first differing line of two runs names the entity and the part. */
static int ents_hash(uint32_t *out, uint32_t label)
{
	uint32_t h = 2166136261u;
	int e, p, live = 0;

	for (e = 0; e < HL64_MAX_ENTITIES; e++)
	{
		if (!ent_is_live(e))
			continue;
		live++;
		for (p = 0; p < ENT_PARTS; p++)
			h = ent_part_hash(h, e, p);
		if (eng.ents_dump)
		{
			const ent_id_t *id = ent_idc(e);
			const ent_rnd_t *r = ent_rnd(e);
			const col_ent_t *cv = ent_cv(e);
			uint32_t ph[ENT_PARTS];

			for (p = 0; p < ENT_PARTS; p++)
				ph[p] = ent_part_hash(2166136261u, e, p);
			log_printf("stats %lu ent %d cls=%u gid=%04x tn=%u id=%08lx cv=%08lx mot=%08lx "
				"ph=%08lx tm=%08lx rnd=%08lx (model=%lx seq=%u frame=%.9g fx=%u body=%u "
				"amt=%u)\n", (unsigned long)label, e, (unsigned)id->cls,
				(unsigned)id->gid, (unsigned)id->targetname, (unsigned long)ph[0],
				(unsigned long)ph[1], (unsigned long)ph[2], (unsigned long)ph[3],
				(unsigned long)ph[4], (unsigned long)ph[5], (unsigned long)r->model,
				(unsigned)r->sequence, (double)r->frame, (unsigned)r->effects,
				(unsigned)r->body, (unsigned)r->renderamt);
			/* the collision view's fields (cv= above) */
			log_printf("stats %lu entcv %d org=%.9g,%.9g,%.9g ang=%.9g,%.9g,%.9g "
				"box=%.9g,%.9g,%.9g/%.9g,%.9g,%.9g fl=%lx solid=%u mt=%u\n",
				(unsigned long)label, e, (double)cv->origin[0], (double)cv->origin[1],
				(double)cv->origin[2], (double)cv->angles[0], (double)cv->angles[1],
				(double)cv->angles[2], (double)cv->mins[0], (double)cv->mins[1],
				(double)cv->mins[2], (double)cv->maxs[0], (double)cv->maxs[1],
				(double)cv->maxs[2], (unsigned long)cv->flags, (unsigned)cv->solid,
				(unsigned)cv->movetype);
			log_flush();	/* 200 lines overrun the log ring */
		}
	}
	*out = h;
	return live;
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
		{
			seam_stats_t ss;

			seam_stats(&ss);
			log_printf("stats %lu seam: switches=%lu forced=%lu open_fail=%lu late=%lu "
				"twins=%lu carried=%lu continuity_bad=%lu closes=%lu open_wait=%lu\n",
				(unsigned long)c->arg[0].u, (unsigned long)ss.switches,
				(unsigned long)ss.forced, (unsigned long)ss.open_fail,
				(unsigned long)ss.late, (unsigned long)ss.twins,
				(unsigned long)ss.carried, (unsigned long)ss.continuity_bad,
				(unsigned long)ss.closes, (unsigned long)ss.open_wait);
		}
		{
			wld_stream_stats_t ss;
			io_stats_t io;
			studio_stats_t sd;

			wld_stream_stats(&ss, 0);
			io_stats(&io, 0);
			studio_stats(&sd, 0);
			log_printf("stats %lu stream: popin=%lu popin_tele=%lu blk1ms=%lu "
				"blk1ms_tele=%lu studio_sync=%lu pins=%lu prefetch=%lu recompute=%lu "
				"reassert=%lu over=%lu tele=%lu open_async=%lu open_fail=%lu late=%lu/%lu\n",
				(unsigned long)c->arg[0].u, (unsigned long)ss.popin,
				(unsigned long)ss.popin_tele, (unsigned long)io.life_frames_blocked_1ms,
				(unsigned long)io.life_frames_blocked_1ms_tele,
				(unsigned long)sd.sync_play, (unsigned long)ss.pins,
				(unsigned long)ss.prefetch, (unsigned long)ss.recompute,
				(unsigned long)ss.reassert, (unsigned long)io.life_frames_budget_over,
				(unsigned long)ss.teleports, (unsigned long)ss.open_async,
				(unsigned long)ss.open_fail, (unsigned long)io.life_late_frames,
				(unsigned long)io.life_late_pieces);
		}
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
	{
		uint32_t h;
		int live = ents_hash(&h, c->arg[0].u);

		log_printf("stats %lu ents: live=%d hash=%08lx\n", (unsigned long)c->arg[0].u, live,
			(unsigned long)h);
	}
	{
		rec_stats_t r;

		rec_stats(&r, 0);
		log_printf("stats %lu rec: live=%lu pending=%lu dry=%lu released=%lu store=%lu/%lu "
			"expired=%lu\n", (unsigned long)c->arg[0].u, (unsigned long)r.live,
			(unsigned long)r.pending, (unsigned long)r.dry, (unsigned long)r.released_total,
			(unsigned long)r.store_used, (unsigned long)r.store_peak,
			(unsigned long)r.open_expired);
	}
	return 0;
}

static int cmd_mem(const dbg_cmd_t *c)
{
	unsigned total_mal, total_fr, fails, live_n, live_kb, peak_kb;

	(void)c;
	mem_carve_log();
	ent_mem_log();
	res_mem_line();
	rec_mem_line();
	if (eng.world)
		seam_mem_line();
	decal_mem_line();
	if (eng.world)
	{
		wld_stats_t w;

		wld_stats(&w);
		log_printf("mem world: layerdirs=%lu peak=%lu size=%lu render_peak=%lu "
			"texture_peak=%lu collision_peak=%lu render_want_peak=%lu "
			"texture_want_peak=%lu collision_want_peak=%lu\n", (unsigned long)w.dirs_used,
			(unsigned long)w.dirs_peak, (unsigned long)w.dirs_size,
			(unsigned long)w.pool_peak[0], (unsigned long)w.pool_peak[1],
			(unsigned long)w.pool_peak[2], (unsigned long)w.want_peak[0],
			(unsigned long)w.want_peak[1], (unsigned long)w.want_peak[2]);
	}
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

static int cmd_step_all(const dbg_cmd_t *c)
{
	phy_set_step_all((int)c->arg[0].u);
	return 0;
}

static int cmd_ents_dump(const dbg_cmd_t *c)
{
	eng.ents_dump = c->arg[0].u != 0;
	return 0;
}

#ifdef HL64_TEST
/* The frame's simulated state: the random stream, the entities (with
 * `entsdump 1` also one line per live entity, labelled by the frame) and
 * the player. */
static void trace_frame(void)
{
	uint32_t h, draws, rs = hl64_rand_state(&draws);
	int live = ents_hash(&h, eng.frame);

	log_printf("trace %lu frame rng=%08lx draws=%lu live=%d ents=%08lx\n",
		(unsigned long)eng.frame, (unsigned long)rs, (unsigned long)draws, live,
		(unsigned long)h);
	if (phy_player_active())
	{
		const ent_player_t *pl = phy_player();

		log_printf("trace %lu player org=%.9g,%.9g,%.9g vel=%.9g,%.9g,%.9g ang=%.9g,%.9g,%.9g "
			"flags=%lu ground=%d\n", (unsigned long)eng.frame, (double)pl->origin[0],
			(double)pl->origin[1], (double)pl->origin[2], (double)pl->velocity[0],
			(double)pl->velocity[1], (double)pl->velocity[2], (double)pl->angles[0],
			(double)pl->angles[1], (double)pl->angles[2], (unsigned long)pl->flags,
			pl->groundentity);
	}
}
#endif

static int cmd_quit(const dbg_cmd_t *c)
{
	sys_exit((int)c->arg[0].u);
}

/* ---- boot, frame ----------------------------------------------------------- */

/* New Game on map: the game's campaign state, the map's records, then the
 * player at its first info_player_start (+1 u, as HLSDK spawns). */
static void eng_new_game(int map)
{
	float org[3], ang[3];
	int slot;

	if (map < 0)
		sys_error("new game: no start map");
	game_new_campaign();
	rec_new_campaign(map);
	slot = wld_open(map)->slot;
	if (!rec_player_start(slot, org, ang))
		sys_error("%s: no info_player_start", wld_map_name(map));
	org[2] += 1;
	if (phy_player_spawn(map, org, ang))
		sys_error("new game: player spawn on %s", wld_map_name(map));
	seam_new_game(map);
	/* the models with interest so far: their infos now (load I/O) */
	studio_preload();
}

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
	dbg_handler_set(DBG_STEP_ALL, cmd_step_all);
	dbg_handler_set(DBG_ENTS_DUMP, cmd_ents_dump);
	SEAMPROF_INIT();
	ent_init();
	eng.world = (uint8_t)wld_init();
	if (eng.world)
		seam_init();
	res_init();
	snd_init();
	rec_init();
	col_init();
	if (eng.world)
	{
		ren_init();
		studio_init();
		ren_cam_init();
		d2_init();
	}
	fx_init();
	in_init();
	phy_player_init();
	/* game/: dispatch tables, engine hooks (think / touch / blocked) */
	game_init();
	if (eng.world)
	{
		int r = rt_load(eng_route_path);

		if (r < 0)
		{
			log_printf("route FAIL load %s\n", eng_route_path);
			log_flush();
			sys_exit(1);
		}
		/* New Game (architecture 3.1 step 5): a route plays from the
		 * campaign start (test builds: its START map instead); without a
		 * route the playable build starts there too (test builds only
		 * when asked, eng_play: their routeless boots fly the free
		 * camera) */
		if (r > 0 || eng_play)
		{
			int map = wld_start_map();

#ifdef HL64_TEST
			if (r > 0 && rt_start_map() >= 0 && rt_start_map() != map)
			{
				if (map >= 0)
					wld_close(wld_open(map));
				map = rt_start_map();
				log_printf("route: start map %s\n", wld_map_name(map));
			}
#endif
			eng_new_game(map);
		}
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
	if (eng.world)
	{
		wld_stream_stats_t ss;
		unsigned long n;

		wld_stream_stats(&ss, 1);
		n = ss.frames ? ss.frames : 1;
		log_printf("prof stream_us avg=%lu max=%lu frames=%lu recompute=%lu reassert=%lu "
			"delta=%lu blk1ms=%lu blk1ms_tele=%lu blkmax=%lu promoted=%lu\n",
			(unsigned long)ss.us_sum / n, (unsigned long)ss.us_max, (unsigned long)ss.frames,
			(unsigned long)ss.recompute, (unsigned long)ss.reassert, (unsigned long)ss.delta,
			(unsigned long)io.life_frames_blocked_1ms,
			(unsigned long)io.life_frames_blocked_1ms_tele,
			(unsigned long)io.life_blocked_max_us, (unsigned long)ss.promoted);
	}
	if (eng.world)
	{
		studio_stats_t ss;

		studio_stats(&ss, 1);
		log_printf("prof studio_us avg=%lu max=%lu drawn=%lu bones_us=%lu draw_us=%lu "
			"pool=%lu pool_peak=%lu want_peak=%lu soft=%lu sync=%lu models=%lu anims=%lu "
			"culled=%lu pending=%lu nofit=%lu dropped=%lu\n",
			(unsigned long)(ss.frames ? ss.us_sum / ss.frames : 0), (unsigned long)ss.us_max,
			(unsigned long)(ss.frames ? ss.drawn / ss.frames : 0),
			(unsigned long)(ss.frames ? ss.bones_us / ss.frames : 0),
			(unsigned long)(ss.frames ? ss.draw_us / ss.frames : 0),
			(unsigned long)ss.pool_used, (unsigned long)ss.pool_peak,
			(unsigned long)ss.want_peak, (unsigned long)ss.soft,
			(unsigned long)ss.sync_loads, (unsigned long)ss.models, (unsigned long)ss.anims,
			(unsigned long)ss.culled, (unsigned long)ss.pending, (unsigned long)ss.nofit,
			(unsigned long)ss.dropped);
	}
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
	{
		phy_motion_prof_t m;
		unsigned long n;

		phy_motion_prof(&m, 1);
		n = m.frames ? m.frames : 1;
		log_printf("prof phys_us avg=%lu max=%lu toss=%lu step=%lu push=%lu pushstep=%lu "
			"toss_us=%lu step_us=%lu push_us=%lu pushstep_us=%lu traces=%lu\n",
			(unsigned long)m.us / n, (unsigned long)m.us_max,
			(unsigned long)m.steps[PHY_K_TOSS] / n, (unsigned long)m.steps[PHY_K_STEP] / n,
			(unsigned long)m.steps[PHY_K_PUSH] / n,
			(unsigned long)m.steps[PHY_K_PUSHSTEP] / n,
			(unsigned long)m.kind_us[PHY_K_TOSS] / n, (unsigned long)m.kind_us[PHY_K_STEP] / n,
			(unsigned long)m.kind_us[PHY_K_PUSH] / n,
			(unsigned long)m.kind_us[PHY_K_PUSHSTEP] / n, (unsigned long)m.traces / n);
	}
	rec_prof_line();
	if (eng.world)
		seam_prof_line();
	game_prof_line();
	game_mon_prof();
	if (eng.world)
	{
		uint32_t avg, max, draws, bytes;

		d2_rollup(&avg, &max, &draws, &bytes);
		log_printf("prof hud_us avg=%lu max=%lu draws=%lu hud_bytes=%lu\n", (unsigned long)avg,
			(unsigned long)max, (unsigned long)draws, (unsigned long)bytes);
	}
	{
		fx_stats_t f;
		unsigned long n;

		fx_stats(&f, 1);
		n = f.frames ? f.frames : 1;
		log_printf("prof fx_us avg=%lu max=%lu sim=%lu part=%lu trans_us=%lu decal=%lu "
			"trans=%lu temps_peak=%lu parts_peak=%lu beams_peak=%lu decals=%lu drops=%lu "
			"dlights=%lu\n", (unsigned long)f.us / n, (unsigned long)f.us_max,
			(unsigned long)f.sim_us / n, (unsigned long)f.part_us / n,
			(unsigned long)f.trans_us / n, (unsigned long)f.decal_us / n,
			(unsigned long)f.trans_drawn / n,
			(unsigned long)f.temps_peak, (unsigned long)f.particles_peak,
			(unsigned long)f.beams_peak, (unsigned long)f.decals,
			(unsigned long)(f.decal_drops + f.trans_dropped), (unsigned long)f.dlights);
	}
	{
		snd_stats_t s;

		snd_stats(&s, 1);
		log_printf("prof snd_us avg=%lu max=%lu voices_peak=%lu loops_peak=%lu waves_peak=%lu "
			"started=%lu stolen=%lu dropped=%lu mem=%lu mem_peak=%lu region=%lu "
			"audio_io=%luB audio_io_us=%lu pump_us=%lu\n",
			(unsigned long)s.us, (unsigned long)s.us_max, (unsigned long)s.voices_peak,
			(unsigned long)s.loops_peak, (unsigned long)s.waves_peak,
			(unsigned long)s.started, (unsigned long)s.stolen, (unsigned long)s.dropped,
			(unsigned long)s.mem, (unsigned long)s.mem_peak, (unsigned long)s.mem_region,
			(unsigned long)io.audio_bytes, (unsigned long)io.audio_us, (unsigned long)s.pump_us);
	}
#ifdef HL64_PROFILE
	sys_prof_dump(eng.rollup_frames);
#endif
	/* the roll-up APIs just reset their window sums (reset = 1) */
	SEAMPROF_ROLLUP_RESET();
	eng.rollup_frames = 0;
	eng.rollup_t0 = t;
}

uint32_t eng_frame_count(void)
{
	return eng.frame;
}

void eng_frame(void)
{
	uint32_t vi = sys_vi_count();
	uint32_t dt = (vi - eng.last_vi) * sys_vi_period_us();
	in_cmd_t cmd;
	double gdt;

	/* 1 plat: dt from the VI clock, clamped (D6 i); test builds may fix it */
	SEAMPROF_BEGIN();
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
	SEAMPROF_STEP(INPUT);

	/* 3 io (test builds: completions published, the frame's pieces
	 * planned; the PI interrupt chains them through the frame, io.h) */
	io_frame_begin(dt);
	io_pump();
	SEAMPROF_STEP(IO);

	/* 4 game: the frame's clock (dt 0 in the settle frames), due logic
	 * timers (think-and-time.md 5), the game's frame start */
	gdt = eng_clock_frame((double)dt * 1e-6);
	rec_run_timers(eng_clock_time(), gdt);
	game_start_frame();
	SEAMPROF_STEP(LOGIC);
	/* 5 player: pmove host */
	phy_player_frame(&cmd, eng.time);
	SEAMPROF_STEP(PLAYER);
	/* 6 active set */
	phy_entities_frame(eng.time, gdt);
	/* the clock advances by the frame time (f64) after step 6 */
	eng_clock_advance();
	eng.time = eng_clock_time();
	SEAMPROF_STEP(ACTIVE);
	/* io: resume a chain paused by the steps' sync / audio reads (wave
	 * loads of the sounds the game started), io.h */
	io_pump();
	/* 7-10 world, hydration, residency, sound (M1-M4) */
	if (eng.world)
	{
		wld_frame(eng.frame);
		SEAMPROF_STEP(STREAM);
		/* 7 seams: approach, layer / HLE opens, pins, switch, crossing,
		 * leave */
		seam_frame();
		SEAMPROF_STEP(SEAM);
		/* 8 hydration pump around the seam anchors (global positions:
		 * the player, and through the approached seam's T) */
		{
			float anchors[2][3];
			int n = seam_anchors(anchors, 2);

			rec_frame(n ? (const float (*)[3])anchors : NULL, n);
		}
		SEAMPROF_STEP(HYDRATE);
		/* 9 residency target set (engine/world/stream.c): the player's
		 * eye and velocity in its home frame; the free camera's view */
		if (phy_player_active())
		{
			const ent_player_t *pl = phy_player();
			float eye[3];
			int k;

			for (k = 0; k < 3; k++)
				eye[k] = pl->origin[k] + pl->view_ofs[k];
			wld_stream_frame(phy_player_map(), eye, pl->velocity);
		}
		else if (ren_cam.map >= 0)
		{
			static const float still[3] = { 0, 0, 0 };
			const float *gm = wld_map_origin(ren_cam.map);
			float eye[3] = { ren_cam.org[0] - gm[0], ren_cam.org[1] - gm[1],
				ren_cam.org[2] - gm[2] };

			wld_stream_frame(ren_cam.map, eye, still);
		}
		SEAMPROF_STEP(STREAM);
		io_pump();
		wld_style_tick(eng.time);
		ren_dlight_frame(eng.time, (double)dt * 1e-6);
		ren_dlight_sources(eng.time);
		studio_frame();
		SEAMPROF_STEP(SCENE);
	}
	/* effects on the game clock: temp entities, particles, beams, decal
	 * placements, persistent render fades, shake */
	fx_frame(eng.time, gdt);
	SEAMPROF_STEP(FXSIM);
	/* 10 sound: listener = the view (global eye, right vector) of the last
	 * drawn frame */
	{
		float yaw = ren_cam.ang[1] * (3.14159265f / 180.0f);
		float pitch = ren_cam.ang[0] * (3.14159265f / 180.0f);
		float roll = ren_cam.ang[2] * (3.14159265f / 180.0f);
		float sy = hl_sinf(yaw), cy = hl_cosf(yaw), sp = hl_sinf(pitch), cp = hl_cosf(pitch);
		float sr = hl_sinf(roll), cr = hl_cosf(roll);
		float right[3] = { -sr * sp * cy + cr * sy, -sr * sp * sy - cr * cy, -sr * cp };

		snd_frame(ren_cam.org, right, (double)dt * 1e-6);
	}
	SEAMPROF_STEP(SND);
	io_pump();

	/* 11-12 render into a free framebuffer; submit for paced present */
	while (!sys_frame_try_begin())
		io_pump();
	io_pump();
	SEAMPROF_STEP(WAIT);
	if (eng.world)
	{
		ren_stats_t r;

		ren_view_frame(eng.time, (double)dt * 1e-6);
		ren_frame(eng.time);
		SEAMPROF_STEP(RENDER);
		/* the HUD after the 3D frame (architecture.md 9.8) */
		game_hud_frame(eng.time, (double)dt * 1e-6);
		d2_begin();
		game_hud_draw();
		d2_end();
		SEAMPROF_STEP(HUD);
		ren_stats(&r);
		eng.rollup_world_us += r.world_us;
		if (r.world_us > eng.rollup_world_max)
			eng.rollup_world_max = r.world_us;
		io_pump();
	}
	sys_frame_end();
	SEAMPROF_STEP(SUBMIT);

	/* 13 test: debug commands at the frame boundary, route VM, roll-up, log */
#ifdef HL64_TEST
	if (dbg_trace())
		trace_frame();
#endif
	eng.frame++;
	dbg_run(eng.frame);
	if (rt_active())
		rt_frame(eng.time);
	SEAMPROF_STEP(TEST);
	SEAMPROF_END(eng.frame, dt, seam_last_switch_frame());
	rollup();
	log_flush();
}
