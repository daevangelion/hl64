/* SPDX-License-Identifier: MIT */
/*
 * seamprof.c -- the seam-window profiler (seamprof.h). Built into PROFILE
 * carts and every host build (the host tests drive the window logic);
 * eng_frame calls the hooks only in PROFILE builds.
 */
#if defined(HL64_PROFILE) || defined(HL64_HOST)

#include <string.h>

#include "hl64/ent.h"
#include "hl64/fx.h"
#include "hl64/limits.h"
#include "hl64/snd.h"
#include "hl64/studio.h"
#include "engine/core/dbg.h"
#include "engine/core/log.h"
#include "engine/core/sys.h"
#include "engine/ent/ent.h"
#include "engine/mem/carve.h"
#include "engine/mem/pool.h"
#include "engine/phys/motion.h"
#include "engine/phys/player.h"
#include "engine/render/ren.h"
#include "engine/test/seamprof.h"
#include "engine/world/wld.h"

const char *const seamprof_zone_name[SEAMPROF_ZONE_COUNT] = {
#define SEAMPROF_ZONE_NAME(id, name) name,
	SEAMPROF_ZONES(SEAMPROF_ZONE_NAME)
#undef SEAMPROF_ZONE_NAME
};

const char *const seamprof_region_name[SEAMPROF_REGION_COUNT] = {
#define SEAMPROF_REGION_NAME(id, name, carve) name,
	SEAMPROF_REGIONS(SEAMPROF_REGION_NAME)
#undef SEAMPROF_REGION_NAME
};

static const uint8_t region_carve[SEAMPROF_REGION_COUNT] = {
#define SEAMPROF_REGION_CARVE(id, name, carve) carve,
	SEAMPROF_REGIONS(SEAMPROF_REGION_CARVE)
#undef SEAMPROF_REGION_CARVE
};

static struct {
	seamprof_frame_t	ring[SEAMPROF_RING];
	uint32_t		head, count;	/* next slot, frames held */
	uint64_t		t_us;
	uint32_t		last_switch;	/* seam_last_switch_frame seen */
	uint32_t		n;		/* windows opened */
	seamprof_win_t		win[SEAMPROF_WINDOWS];
	seamprof_win_t		last;
	uint8_t			have_last;
	/* hooks */
	uint8_t			mark;		/* seamprof_mark pending */
	uint64_t		t0, tmark;
	seamprof_frame_t	cur;
	uint32_t		prev[SEAMPROF_ZONE_COUNT];	/* counter sums last read */
} sp;

/* ---- core ----------------------------------------------------------------- */

void seamprof_reset(void)
{
	memset(&sp, 0, sizeof(sp));
}

void seamprof_counters(seamprof_frame_t *f, const uint32_t cum[SEAMPROF_ZONE_COUNT])
{
	int z;

	/* u32 sums: the difference is right across a wrap too */
	for (z = SEAMPROF_Z_COUNTERS; z < SEAMPROF_Z_WORLD; z++)
	{
		f->us[z] = cum[z] - sp.prev[z];
		sp.prev[z] = cum[z];
	}
}

static void win_add(seamprof_win_t *w, const seamprof_frame_t *f)
{
	int z;

	for (z = 0; z < SEAMPROF_ZONE_COUNT; z++)
	{
		w->sum[z] += f->us[z];
		if (!w->frames || f->us[z] > w->max[z])
		{
			w->max[z] = f->us[z];
			w->max_frame[z] = f->frame;
		}
	}
	for (z = 0; z < SEAMPROF_REGION_COUNT; z++)
		if (!w->frames || f->mem[z] > w->peak[z])
		{
			w->peak[z] = f->mem[z];
			w->peak_frame[z] = f->frame;
		}
	w->frames++;
}

/* "none" without a player, "?" for a map index the campaign lacks */
static const char *map_name(int m)
{
	const char *s;

	if (m < 0)
		return "none";
	s = wld_map_name(m);
	return *s ? s : "?";
}

static void win_close(seamprof_win_t *w)
{
	int z;

	log_printf("seamprof: switch %lu %s->%s frame=%lu frames=%lu%s%s\n",
		(unsigned long)w->n, map_name(w->a), map_name(w->b), (unsigned long)w->frame,
		(unsigned long)w->frames, w->mark ? " mark" : "", w->cut ? " cut" : "");
	for (z = 0; z < SEAMPROF_ZONE_COUNT; z++)
		log_printf("seamprof: zone=%s max_us=%lu frame=%lu avg_us=%lu switch_us=%lu\n",
			seamprof_zone_name[z], (unsigned long)w->max[z],
			(unsigned long)w->max_frame[z],
			(unsigned long)(w->frames ? w->sum[z] / w->frames : 0),
			(unsigned long)w->at_switch[z]);
	for (z = 0; z < SEAMPROF_REGION_COUNT; z++)
		log_printf("seamprof: region=%s peak=%lu frame=%lu size=%lu\n",
			seamprof_region_name[z], (unsigned long)w->peak[z],
			(unsigned long)w->peak_frame[z],
			(unsigned long)mem_region_bytes(region_carve[z]));
	w->open = 0;
	sp.last = *w;
	sp.have_last = 1;
}

/* Open a window on frame f (the newest in the ring): the ring's frames
 * within SEAMPROF_HALF_US before it, oldest first, then f. */
static void win_open(const seamprof_frame_t *f, int mark)
{
	seamprof_win_t *w = NULL;
	uint32_t i, k;
	int j;

	for (j = 0; j < SEAMPROF_WINDOWS; j++)
		if (!sp.win[j].open)
		{
			w = &sp.win[j];
			break;
		}
	if (!w)
	{
		/* both busy: the older one ends here */
		w = sp.win[0].n < sp.win[1].n ? &sp.win[0] : &sp.win[1];
		w->cut = 1;
		win_close(w);
	}
	memset(w, 0, sizeof(*w));
	w->open = 1;
	w->mark = (uint8_t)(mark != 0);
	w->n = ++sp.n;
	w->frame = f->frame;
	w->t_us = f->t_us;
	w->b = f->map;
	w->a = f->map;
	memcpy(w->at_switch, f->us, sizeof(w->at_switch));
	if (sp.count >= 2)
		w->a = sp.ring[(sp.head + SEAMPROF_RING - 2) % SEAMPROF_RING].map;
	/* k = frames back from f that are inside the window */
	for (k = 1; k < sp.count; k++)
	{
		const seamprof_frame_t *p = &sp.ring[(sp.head + SEAMPROF_RING - 1 - k) % SEAMPROF_RING];

		if (f->t_us - p->t_us > SEAMPROF_HALF_US)
			break;
	}
	for (i = k; i-- > 0;)
		win_add(w, &sp.ring[(sp.head + SEAMPROF_RING - 1 - i) % SEAMPROF_RING]);
}

void seamprof_push(seamprof_frame_t *f, uint32_t switch_frame, int mark)
{
	int j;

	sp.t_us += f->dt_us;
	f->t_us = sp.t_us;
	sp.ring[sp.head] = *f;
	sp.head = (sp.head + 1) % SEAMPROF_RING;
	if (sp.count < SEAMPROF_RING)
		sp.count++;
	/* windows already open: this frame is after their switch frame */
	for (j = 0; j < SEAMPROF_WINDOWS; j++)
	{
		seamprof_win_t *w = &sp.win[j];

		if (!w->open)
			continue;
		if (f->t_us - w->t_us > SEAMPROF_HALF_US)
		{
			win_close(w);
			continue;
		}
		win_add(w, f);
		if (f->t_us - w->t_us == SEAMPROF_HALF_US)
			win_close(w);
	}
	if (switch_frame != sp.last_switch)
	{
		sp.last_switch = switch_frame;
		if (switch_frame)
			win_open(f, 0);
	}
	if (mark)
		win_open(f, 1);
}

const seamprof_win_t *seamprof_last(void)
{
	return sp.have_last ? &sp.last : NULL;
}

/* ---- engine hooks ------------------------------------------------------------ */

static int cmd_mark(const dbg_cmd_t *c)
{
	(void)c;
	sp.mark = 1;
	return 0;
}

void seamprof_init(void)
{
	seamprof_reset();
	dbg_handler_set(DBG_SEAMPROF_MARK, cmd_mark);
	sp.t0 = sp.tmark = sys_time_us();
}

void seamprof_begin(void)
{
	memset(sp.cur.us, 0, sizeof(sp.cur.us));
	sp.t0 = sp.tmark = sys_time_us();
}

void seamprof_step(int z)
{
	uint64_t t = sys_time_us();

	sp.cur.us[z] += (uint32_t)(t - sp.tmark);
	sp.tmark = t;
}

void seamprof_rollup_reset(void)
{
	memset(&sp.prev, 0, sizeof(sp.prev));
}

/* Bytes in use of the layer-directory region: each open layer's directory
 * and tables blocks (world.c layer_pool). */
static uint32_t layerdirs_used(void)
{
	uint32_t b = 0;
	int s;

	for (s = 0; s < HL64_WORLD_LAYERS; s++)
	{
		const wld_layer_t *L = wld_layer(s);

		if (!L)
			continue;
		if (L->mem)
			b += L->mem->size;
		if (L->tables)
			b += L->tables->size;
	}
	return b;
}

void seamprof_end(uint32_t frame, uint32_t dt_us, uint32_t switch_frame)
{
	seamprof_frame_t *f = &sp.cur;
	uint32_t cum[SEAMPROF_ZONE_COUNT];
	phy_player_prof_t pp;
	phy_ent_stats_t es;
	phy_motion_prof_t mp;
	studio_stats_t ss;
	fx_stats_t fs;
	snd_stats_t sn;
	ren_stats_t rs;
	wld_stats_t ws;
	ent_priv_stats_t ps;
	int mark = sp.mark;

	f->us[SEAMPROF_Z_FRAME] = (uint32_t)(sys_time_us() - sp.t0);
	/* counter zones: this frame's growth of the roll-up sums */
	memset(cum, 0, sizeof(cum));
	phy_player_prof(&pp, 0);
	phy_ent_stats(&es, 0);
	phy_motion_prof(&mp, 0);
	studio_stats(&ss, 0);
	fx_stats(&fs, 0);
	cum[SEAMPROF_Z_PMOVE] = pp.us_sum;
	cum[SEAMPROF_Z_ENT] = es.us;
	cum[SEAMPROF_Z_PHYS] = mp.us;
	cum[SEAMPROF_Z_STUDIO] = ss.us_sum;
	cum[SEAMPROF_Z_FX] = fs.us;
	seamprof_counters(f, cum);
	/* world_us is the drawn frame's own */
	ren_stats(&rs);
	f->us[SEAMPROF_Z_WORLD] = rs.world_us;
	/* regions */
	wld_stats(&ws);
	snd_stats(&sn, 0);
	ent_priv_stats(&ps);
	f->mem[SEAMPROF_R_RENDER] = ws.pool_used[0];
	f->mem[SEAMPROF_R_TEXTURE] = ws.pool_used[1];
	f->mem[SEAMPROF_R_COLLISION] = ws.pool_used[2];
	f->mem[SEAMPROF_R_STUDIO] = ss.pool_used;
	f->mem[SEAMPROF_R_LAYERDIRS] = layerdirs_used();
	f->mem[SEAMPROF_R_GAMEPRIVATE] = ps.bytes_live;
	f->mem[SEAMPROF_R_COMPONENTS] = ent_s.region_bytes;
	f->mem[SEAMPROF_R_SOUND] = sn.mem;
	f->frame = frame;
	f->dt_us = dt_us;
	f->map = (int16_t)(phy_player_active() ? phy_player_map() : -1);
	sp.mark = 0;
	seamprof_push(f, switch_frame, mark);
}

#endif
