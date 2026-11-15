/* SPDX-License-Identifier: MIT */
/*
 * settings.h -- the one typed settings record (docs/architecture.md 12).
 *
 * The game and the engine read fields directly; there are no string
 * variables. The menu edits the persistent subset and saves store it.
 * Movevars carry the HL defaults listed in specs/pmove-host.md 4. The
 * descriptor table (field id, name, type, offset, range) exists only for
 * the debug channel (DBG_SET_SETTING / DBG_GET_SETTING and the host CLI).
 */
#ifndef HL64_SETTINGS_H
#define HL64_SETTINGS_H

#include <stdint.h>

typedef struct {
	/* physics movevars (pm_shared movevars_t order) */
	float		gravity;
	float		stopspeed;
	float		maxspeed;
	float		spectatormaxspeed;
	float		accelerate;
	float		airaccelerate;
	float		wateraccelerate;
	float		friction;
	float		edgefriction;
	float		waterfriction;
	float		entgravity;
	float		bounce;
	float		stepsize;
	float		maxvelocity;
	float		zmax;
	float		waveheight;
	float		rollangle;
	float		rollspeed;
	/* controls */
	float		yawspeed;	/* deg/s at full stick */
	float		pitchspeed;
	/* render: horizontal field of view in degrees (GoldSrc default 90) */
	float		fov;
	uint8_t		footsteps;
	/* game */
	uint8_t		skill;		/* 1 easy .. 3 hard */
	/* audio, percent */
	uint8_t		vol_master;
	uint8_t		vol_sfx;
	uint8_t		invert_y;
	uint8_t		stick_deadzone;	/* of 128 */
	/* render / debug */
	uint8_t		show_fps;
	uint8_t		debug_overlay;
	/* OPEN-seam cross-layer draw (cwg-format.md): 0 off, 1 on, 2 on and
	 * log the meshlets drawn per frame (`ren: open seam ...`) */
	uint8_t		r_openseam;
	/* cart colour dithering of the 3D passes (plat/n64/n64.h N64_DITHER):
	 * 1 on, 0 off (truncated 16-bit output; the M1 gate's old-cart
	 * comparison renders that way) */
	uint8_t		r_dither;
	/* monster thinks over the frame's AI work budget (game/hl/aithink.c):
	 * 0 run whole (HLSDK), 1 pause and continue next frame, 2 test: pause
	 * at every check and continue at once after scribbling the shared
	 * scratch (decides as 0); ai_budget in work units per frame; ai_hold
	 * a held think's age (seconds of game time) from which it may start
	 * over the budget */
	uint8_t		ai_spread;
	float		ai_budget;
	float		ai_hold;
	/* menu (engine/ui/menu.c): brightness 0..100 (a black-level lift of
	 * the 3D passes, SETTINGS_BRIGHT_LIFT, 0 = the cooked light), look
	 * sensitivity in percent of yawspeed / pitchspeed */
	uint8_t		brightness;
	uint8_t		look_sens;
} settings_t;

/* The 3D passes' colour lift (of 255) at brightness b: added to every
 * opaque world / studio pixel by the combiner. */
#define SETTINGS_BRIGHT_LIFT(b)	((int)(b) * 48 / 100)

extern settings_t settings;

/* X(id, field, type, lo, hi, persist): the descriptor table, in id order */
#define SETTINGS_FIELDS(X) \
	X(GRAVITY, gravity, F32, 0, 10000, 1) \
	X(STOPSPEED, stopspeed, F32, 0, 10000, 1) \
	X(MAXSPEED, maxspeed, F32, 0, 10000, 1) \
	X(SPECTATORMAXSPEED, spectatormaxspeed, F32, 0, 10000, 1) \
	X(ACCELERATE, accelerate, F32, 0, 100, 1) \
	X(AIRACCELERATE, airaccelerate, F32, 0, 100, 1) \
	X(WATERACCELERATE, wateraccelerate, F32, 0, 100, 1) \
	X(FRICTION, friction, F32, 0, 100, 1) \
	X(EDGEFRICTION, edgefriction, F32, 0, 100, 1) \
	X(WATERFRICTION, waterfriction, F32, 0, 100, 1) \
	X(ENTGRAVITY, entgravity, F32, 0, 100, 1) \
	X(BOUNCE, bounce, F32, 0, 100, 1) \
	X(STEPSIZE, stepsize, F32, 0, 1000, 1) \
	X(MAXVELOCITY, maxvelocity, F32, 0, 100000, 1) \
	X(ZMAX, zmax, F32, 0, 100000, 1) \
	X(WAVEHEIGHT, waveheight, F32, 0, 1000, 1) \
	X(ROLLANGLE, rollangle, F32, 0, 90, 1) \
	X(ROLLSPEED, rollspeed, F32, 0, 10000, 1) \
	X(YAWSPEED, yawspeed, F32, 0, 1000, 1) \
	X(PITCHSPEED, pitchspeed, F32, 0, 1000, 1) \
	X(FOV, fov, F32, 10, 170, 1) \
	X(FOOTSTEPS, footsteps, U8, 0, 1, 1) \
	X(SKILL, skill, U8, 1, 3, 1) \
	X(VOL_MASTER, vol_master, U8, 0, 100, 1) \
	X(VOL_SFX, vol_sfx, U8, 0, 100, 1) \
	X(INVERT_Y, invert_y, U8, 0, 1, 1) \
	X(STICK_DEADZONE, stick_deadzone, U8, 0, 64, 1) \
	X(SHOW_FPS, show_fps, U8, 0, 1, 1) \
	X(DEBUG_OVERLAY, debug_overlay, U8, 0, 1, 0) \
	X(R_OPENSEAM, r_openseam, U8, 0, 2, 0) \
	X(R_DITHER, r_dither, U8, 0, 1, 0) \
	X(AI_SPREAD, ai_spread, U8, 0, 2, 0) \
	X(AI_BUDGET, ai_budget, F32, 0, 1000000, 0) \
	X(AI_HOLD, ai_hold, F32, 0, 2, 0) \
	X(BRIGHTNESS, brightness, U8, 0, 100, 1) \
	X(LOOK_SENS, look_sens, U8, 25, 200, 1)

enum {
#define SETTINGS_ENUM(id, field, type, lo, hi, persist) SET_##id,
	SETTINGS_FIELDS(SETTINGS_ENUM)
#undef SETTINGS_ENUM
	SET_COUNT
};

enum { SET_T_F32, SET_T_U8 };

typedef struct {
	const char	*name;
	uint16_t	offset;
	uint8_t		type;		/* SET_T_* */
	uint8_t		persist;	/* menu-editable, saved */
	float		lo, hi;
} setting_desc_t;

extern const setting_desc_t settings_desc[SET_COUNT];

/* HL defaults. */
void	settings_reset(void);
/* Debug channel: value as float, clamped to the field's range (U8 fields
 * round to nearest). Return 0, or -1 for an unknown id. */
int	settings_set(int id, float value);
int	settings_get(int id, float *value);
/* Field id by name (host CLI), -1 if none. */
int	settings_find(const char *name);

#endif
