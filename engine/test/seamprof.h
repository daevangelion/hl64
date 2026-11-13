/* SPDX-License-Identifier: MIT */
/*
 * seamprof.h -- the seam-window profiler (M4 gate, test/run/m4.sh;
 * PROFILE builds only, no code or cost otherwise).
 *
 * Per frame it records the CPU of each eng_frame step (marks around steps
 * 3-12) and of the subsystems behind their own roll-up APIs (cumulative
 * counters read with reset = 0 and differenced frame to frame, re-zeroed
 * when the 1 s roll-up resets them), plus the bytes in use of the carve
 * regions a crossing loads. The last 2 s of frames stay in a ring. When a
 * seam switch happens (seam_last_switch_frame changes) or the test-only
 * `seamprof_mark` command opens one, a window collects until 2 s of frame
 * time after the switch frame and then logs, for the frames within
 * +-2 s of it:
 *
 *   seamprof: switch N A->B frame=F frames=K [mark] [cut]
 *   seamprof: zone=<z> max_us=<v> frame=<f> avg_us=<v> switch_us=<v>
 *   seamprof: region=<r> peak=<bytes> frame=<f> size=<bytes>
 *
 * one zone line per CPU zone, one region line per region. max_us is the
 * worst frame of the zone, frame its eng frame counter (the stats
 * records' frame=), avg_us the mean over the window's frames, switch_us
 * the switch frame's own; peak the largest use of the region in the
 * window. A third switch while two windows are open closes the older one
 * early (`cut`).
 */
#ifndef HL64_SEAMPROF_H
#define HL64_SEAMPROF_H

#include <stdint.h>

/* CPU zones: eng_frame steps (seamprof_step), then the subsystem roll-up
 * counters. FRAME = frame start to seamprof_end. */
#define SEAMPROF_ZONES(X) \
	X(FRAME, "frame") \
	X(INPUT, "input")		/* 1-2 dt, pad / free camera */ \
	X(IO, "io")			/* 3 io_frame_begin, io_pump */ \
	X(LOGIC, "logic")		/* 4 clock, rec_run_timers, game_start_frame */ \
	X(PLAYER, "player")		/* 5 phy_player_frame */ \
	X(ACTIVE, "active")		/* 6 phy_entities_frame, clock advance */ \
	X(SEAM, "seam")			/* 7 seam_frame */ \
	X(HYDRATE, "hydrate")		/* 8 rec_frame */ \
	X(STREAM, "stream")		/* 9 wld_stream_frame; wld_frame */ \
	X(SCENE, "scene")		/* styles, dlights, studio_frame */ \
	X(FXSIM, "fxsim")		/* fx_frame */ \
	X(SND, "snd")			/* 10 snd_frame */ \
	X(WAIT, "wait")			/* 11 free framebuffer wait */ \
	X(RENDER, "render")		/* 11 ren_view_frame, ren_frame */ \
	X(HUD, "hud")			/* game_hud_frame, d2 HUD draw */ \
	X(SUBMIT, "submit")		/* 12 sys_frame_end */ \
	X(TEST, "test")			/* 13 dbg_run, rt_frame */ \
	X(PMOVE, "pmove")		/* phy_player_prof us_sum */ \
	X(ENT, "ent")			/* phy_ent_stats us */ \
	X(PHYS, "phys")			/* phy_motion_prof us */ \
	X(STUDIO, "studio")		/* studio_stats us_sum */ \
	X(FX, "fx")			/* fx_stats us (sim + draw) */ \
	X(WORLD, "world")		/* ren_stats world_us */

enum {
#define SEAMPROF_ZONE_ENUM(id, name) SEAMPROF_Z_##id,
	SEAMPROF_ZONES(SEAMPROF_ZONE_ENUM)
#undef SEAMPROF_ZONE_ENUM
	SEAMPROF_ZONE_COUNT
};
/* Zones fed by a cumulative roll-up counter: [SEAMPROF_Z_COUNTERS,
 * SEAMPROF_Z_WORLD); the ones before are steps, WORLD is the drawn
 * frame's own value. */
#define SEAMPROF_Z_COUNTERS	SEAMPROF_Z_PMOVE

/* Carve regions sampled every frame (engine/mem/carve.h names). */
#define SEAMPROF_REGIONS(X) \
	X(RENDER, "render", MEM_POOL_RENDER) \
	X(TEXTURE, "texture", MEM_POOL_TEXTURE) \
	X(COLLISION, "collision", MEM_POOL_COLLISION) \
	X(STUDIO, "studio", MEM_POOL_STUDIO) \
	X(HLE, "hle", MEM_HLE_BLOBS) \
	X(DEHYDRATE, "dehydrate", MEM_DEHYDRATE) \
	X(LAYERDIRS, "layerdirs", MEM_LAYER_DIRS) \
	X(GAMEPRIVATE, "gameprivate", MEM_GAME_PRIVATE) \
	X(COMPONENTS, "components", MEM_COMPONENTS) \
	X(SOUND, "sound", MEM_SOUND)

enum {
#define SEAMPROF_REGION_ENUM(id, name, carve) SEAMPROF_R_##id,
	SEAMPROF_REGIONS(SEAMPROF_REGION_ENUM)
#undef SEAMPROF_REGION_ENUM
	SEAMPROF_REGION_COUNT
};

/* Window half-width (frame time, us) and the ring: 2 s of the shortest
 * frame (HL64_DT_MIN_US) plus the switch frame. */
#define SEAMPROF_HALF_US	2000000u
#define SEAMPROF_RING		(SEAMPROF_HALF_US / 16667u + 2u)
#define SEAMPROF_WINDOWS	2

/* One frame. */
typedef struct {
	uint32_t	frame;		/* eng frame counter after the frame */
	uint32_t	dt_us;		/* the frame's dt (eng_frame step 1) */
	int16_t		map;		/* the player's home map, -1 none */
	uint16_t	pad;
	uint64_t	t_us;		/* sum of dt_us up to and including this frame */
	uint32_t	us[SEAMPROF_ZONE_COUNT];
	uint32_t	mem[SEAMPROF_REGION_COUNT];
} seamprof_frame_t;

/* A window (open or just closed). */
typedef struct {
	uint8_t		open, mark, cut;
	uint32_t	n;		/* switch number, 1-based (marks count too) */
	uint32_t	frame;		/* switch frame */
	uint64_t	t_us;		/* its t_us */
	int16_t		a, b;		/* maps before / after */
	uint32_t	frames;		/* frames in the window */
	uint32_t	max[SEAMPROF_ZONE_COUNT], max_frame[SEAMPROF_ZONE_COUNT];
	uint64_t	sum[SEAMPROF_ZONE_COUNT];
	uint32_t	at_switch[SEAMPROF_ZONE_COUNT];	/* the switch frame's */
	uint32_t	peak[SEAMPROF_REGION_COUNT], peak_frame[SEAMPROF_REGION_COUNT];
} seamprof_win_t;

extern const char *const seamprof_zone_name[SEAMPROF_ZONE_COUNT];
extern const char *const seamprof_region_name[SEAMPROF_REGION_COUNT];

/* ---- core (window logic; host tests drive it with synthetic frames) ------ */

/* Forget every frame and window. */
void		seamprof_reset(void);
/* Feed one frame (t_us is filled here). switch_frame: seam_last_switch_frame()
 * after the frame (a change opens a window); mark: 1 opens one regardless. */
void		seamprof_push(seamprof_frame_t *f, uint32_t switch_frame, int mark);
/* The most recently closed window, NULL before the first. */
const seamprof_win_t *seamprof_last(void);
/* The counter zones of f from the roll-up APIs' cumulative sums cum[]
 * (only the counter zones are read): their growth since the previous call
 * or since seamprof_rollup_reset. */
void		seamprof_counters(seamprof_frame_t *f, const uint32_t cum[SEAMPROF_ZONE_COUNT]);

/* ---- engine hooks (eng_frame; PROFILE builds) ------------------------------ */

/* Boot: register the `seamprof_mark` debug command, reset. */
void		seamprof_init(void);
/* Frame start (step 1). */
void		seamprof_begin(void);
/* The time since the previous mark (or seamprof_begin) goes to zone z. */
void		seamprof_step(int z);
/* Frame end: sample the counters and regions, feed the frame. */
void		seamprof_end(uint32_t frame, uint32_t dt_us, uint32_t switch_frame);
/* The 1 s roll-up just reset the subsystem counters (reset = 1). */
void		seamprof_rollup_reset(void);

#ifdef HL64_PROFILE
#define SEAMPROF_INIT()			seamprof_init()
#define SEAMPROF_BEGIN()		seamprof_begin()
#define SEAMPROF_STEP(z)		seamprof_step(SEAMPROF_Z_##z)
#define SEAMPROF_END(f, dt, sw)		seamprof_end((f), (dt), (sw))
#define SEAMPROF_ROLLUP_RESET()		seamprof_rollup_reset()
#else
#define SEAMPROF_INIT()			((void)0)
#define SEAMPROF_BEGIN()		((void)0)
#define SEAMPROF_STEP(z)		((void)0)
#define SEAMPROF_END(f, dt, sw)		((void)0)
#define SEAMPROF_ROLLUP_RESET()		((void)0)
#endif

#endif
