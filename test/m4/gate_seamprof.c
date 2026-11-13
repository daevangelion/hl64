/* SPDX-License-Identifier: MIT */
/*
 * gate_seamprof.c -- host driver for the seam-window profiler
 * (engine/test/seamprof.c): the window logic on synthetic frames, the
 * counter differencing across roll-up resets and u32 wraps, and the
 * engine hooks with the `seamprof_mark` debug command.
 * test/m4/gate_seamprof.py runs it and parses its `seamprof:` lines with
 * the gate's parser.
 *
 *   hl64-seamprof      prints the profiler's log lines and
 *                      "case <name> PASS|FAIL" lines; exit 1 on a failure
 */
#include <stdio.h>
#include <string.h>

#include "engine/core/dbg.h"
#include "engine/core/log.h"
#include "engine/test/seamprof.h"

static int fails, tfails;

#define CHECK(cond, ...) do { if (!(cond)) { tfails++; log_flush(); printf("  fail %s:%d: ", \
	__FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void begin(void)
{
	tfails = 0;
	seamprof_reset();
}

static void finish(const char *name)
{
	log_flush();
	printf("case %s %s\n", name, tfails ? "FAIL" : "PASS");
	fails += tfails != 0;
}

/* A synthetic frame: every step zone and region = value, map. */
static void feed(uint32_t frame, uint32_t dt, int map, uint32_t value, uint32_t sw, int mark)
{
	seamprof_frame_t f;
	int z;

	memset(&f, 0, sizeof(f));
	f.frame = frame;
	f.dt_us = dt;
	f.map = (int16_t)map;
	for (z = 0; z < SEAMPROF_ZONE_COUNT; z++)
		f.us[z] = value;
	for (z = 0; z < SEAMPROF_REGION_COUNT; z++)
		f.mem[z] = value * 16;
	seamprof_push(&f, sw, mark);
}

/* Switch at frame 100 of 200 at 30 fps: +-2 s = frames 40..160 (60 x
 * 33333 us = 1999980 us <= 2 s; 61 frames is past). Zone value = frame,
 * so the worst frame is the last of the window and the mean its middle. */
static void case_switch(void)
{
	const seamprof_win_t *w;
	uint32_t i, sw = 0;

	begin();
	for (i = 1; i <= 200; i++)
	{
		if (i == 100)
			sw = 7;		/* the engine's switch frame number: any change counts */
		feed(i, 33333, i < 100 ? 0 : 1, i, sw, 0);
		if (i == 160)
			CHECK(!seamprof_last(), "window closed before +2 s");
	}
	w = seamprof_last();
	CHECK(w != NULL, "no window");
	if (w)
	{
		CHECK(w->n == 1 && !w->mark && !w->cut, "n %u mark %u cut %u", w->n, w->mark, w->cut);
		CHECK(w->frame == 100, "switch frame %u", w->frame);
		CHECK(w->a == 0 && w->b == 1, "maps %d->%d", w->a, w->b);
		CHECK(w->frames == 121, "frames %u (want 121: 40..160)", w->frames);
		CHECK(w->max[SEAMPROF_Z_FRAME] == 160 && w->max_frame[SEAMPROF_Z_FRAME] == 160,
			"max %u at %u", w->max[SEAMPROF_Z_FRAME], w->max_frame[SEAMPROF_Z_FRAME]);
		CHECK(w->sum[SEAMPROF_Z_WORLD] / w->frames == 100, "avg %u",
			(unsigned)(w->sum[SEAMPROF_Z_WORLD] / w->frames));
		CHECK(w->at_switch[SEAMPROF_Z_SEAM] == 100, "switch frame's own %u",
			w->at_switch[SEAMPROF_Z_SEAM]);
		CHECK(w->peak[SEAMPROF_R_RENDER] == 160 * 16 && w->peak_frame[SEAMPROF_R_RENDER] == 160,
			"peak %u at %u", w->peak[SEAMPROF_R_RENDER], w->peak_frame[SEAMPROF_R_RENDER]);
	}
	finish("switch");
}

/* The worst frame before the switch and a region peak inside the window
 * are found; a spike just outside it (frame S - 61) is not. 60 fps:
 * 119 x 16667 us = 1983373 <= 2 s, 120 x = 2000040 is past. */
static void case_spike(void)
{
	const seamprof_win_t *w;
	uint32_t i, sw = 0;

	begin();
	for (i = 1; i <= 450; i++)
	{
		uint32_t v = 10;

		if (i == 300 - 120)
			v = 9000;	/* just outside */
		if (i == 300 - 119)
			v = 5000;	/* first frame inside */
		if (i == 300)
			sw = 300;
		feed(i, 16667, 2, v, sw, 0);
	}
	w = seamprof_last();
	CHECK(w != NULL, "no window");
	if (w)
	{
		CHECK(w->frames == 239, "frames %u (want 119 + 1 + 119)", w->frames);
		CHECK(w->max[SEAMPROF_Z_RENDER] == 5000 && w->max_frame[SEAMPROF_Z_RENDER] == 181,
			"max %u at %u", w->max[SEAMPROF_Z_RENDER], w->max_frame[SEAMPROF_Z_RENDER]);
		CHECK(w->peak[SEAMPROF_R_COLLISION] == 5000 * 16, "peak %u",
			w->peak[SEAMPROF_R_COLLISION]);
	}
	finish("spike");
}

/* A switch 5 frames after boot: the window starts at frame 1. */
static void case_early(void)
{
	const seamprof_win_t *w;
	uint32_t i;

	begin();
	for (i = 1; i <= 80; i++)
		feed(i, 33333, 0, 1, i >= 5 ? 5 : 0, 0);
	w = seamprof_last();
	CHECK(w != NULL && w->frames == 65, "frames %u (want 1..65)", w ? w->frames : 0);
	finish("early");
}

/* Two switches 1 s apart both get windows; a third while both are open
 * cuts the older; marks open windows of their own. */
static void case_overlap(void)
{
	const seamprof_win_t *w;
	uint32_t i, sw = 0, closed = 0, last_n = 0;
	int cut_seen = 0;

	begin();
	for (i = 1; i <= 400; i++)
	{
		if (i == 100 || i == 130 || i == 150)
			sw = i;
		feed(i, 33333, i < 100 ? 0 : 1, 1, sw, i == 300);
		w = seamprof_last();
		if (w && w->n != last_n)
		{
			last_n = w->n;
			closed++;
			if (w->n == 1)
				cut_seen = w->cut && w->frames == 111;	/* 40..150: cut at 150 */
			if (w->n == 4)
				CHECK(w->mark && w->a == 1 && w->b == 1 && w->frames == 121,
					"mark window n %u mark %u maps %d->%d frames %u", w->n, w->mark,
					w->a, w->b, w->frames);
		}
	}
	CHECK(closed == 4, "%u windows closed (want 4)", closed);
	CHECK(cut_seen, "window 1 not cut by the third switch");
	finish("overlap");
}

/* Counter zones: growth of cumulative sums; a roll-up reset re-zeroes the
 * snapshot; u32 wrap. */
static void case_counters(void)
{
	static const uint32_t cum_seq[] = { 100, 250, 400 };
	seamprof_frame_t f;
	uint32_t cum[SEAMPROF_ZONE_COUNT];
	int z, i;

	begin();
	for (i = 0; i < 3; i++)
	{
		memset(&f, 0, sizeof(f));
		for (z = 0; z < SEAMPROF_ZONE_COUNT; z++)
			cum[z] = cum_seq[i] + (uint32_t)z;
		seamprof_counters(&f, cum);
		for (z = SEAMPROF_Z_COUNTERS; z < SEAMPROF_Z_WORLD; z++)
			CHECK(f.us[z] == (i ? 150u : 100u + (uint32_t)z), "step %d zone %d: %u", i, z,
				f.us[z]);
		CHECK(f.us[SEAMPROF_Z_WORLD] == 0 && f.us[SEAMPROF_Z_FRAME] == 0,
			"non-counter zones written");
	}
	seamprof_rollup_reset();
	for (z = 0; z < SEAMPROF_ZONE_COUNT; z++)
		cum[z] = 30;
	seamprof_counters(&f, cum);
	CHECK(f.us[SEAMPROF_Z_PMOVE] == 30, "after reset %u", f.us[SEAMPROF_Z_PMOVE]);
	seamprof_rollup_reset();
	cum[SEAMPROF_Z_FX] = 0xfffffff0u;
	seamprof_counters(&f, cum);
	cum[SEAMPROF_Z_FX] = 0x10u;
	seamprof_counters(&f, cum);
	CHECK(f.us[SEAMPROF_Z_FX] == 0x20u, "wrap %u", f.us[SEAMPROF_Z_FX]);
	finish("counters");
}

/* The engine hooks: the `seamprof_mark` command at frame 10, steps timed,
 * 2 s of frames later the window is logged. */
static void case_hooks(void)
{
	const seamprof_win_t *w;
	dbg_cmd_t c;
	uint32_t i;

	begin();
	dbg_init();
	seamprof_init();
	memset(&c, 0, sizeof(c));
	c.frame = 10;
	c.op = DBG_SEAMPROF_MARK;
	CHECK(dbg_push(&c) == 0, "queue");
	for (i = 1; i <= 80; i++)
	{
		seamprof_begin();
		seamprof_step(SEAMPROF_Z_IO);
		seamprof_step(SEAMPROF_Z_RENDER);
		CHECK(dbg_run(i) == 0, "dbg failures");
		seamprof_end(i, 33333, 0);
	}
	w = seamprof_last();
	CHECK(w != NULL, "no window");
	if (w)
		CHECK(w->mark && w->n == 1 && w->frame == 10 && w->frames == 10 + 60
			&& w->a == -1 && w->b == -1, "mark %u n %u frame %u frames %u maps %d->%d",
			w->mark, w->n, w->frame, w->frames, w->a, w->b);
	finish("hooks");
}

int main(void)
{
	case_switch();
	case_spike();
	case_early();
	case_overlap();
	case_counters();
	case_hooks();
	log_flush();
	printf("seamprof %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
	return fails ? 1 : 0;
}
