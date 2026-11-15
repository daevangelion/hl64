/* SPDX-License-Identifier: MIT */
/*
 * fx.h -- the effects API (docs/architecture.md 7.1 "effects", 9.5-9.7;
 * docs/specs/render-fx.md 4-5, dlights.md): direct calls with HLSDK TE_*
 * semantics and parameters (HLSDK common/const.h documents each message's
 * fields; the order below follows it), decals, temp entities, particles,
 * beams, dynamic lights, screen shake / fade and text messages. Positions
 * are map-local in the caller's frame (eng_frame_layer); the engine
 * converts. Pools are fixed (render-fx.md 5); a full pool drops and counts.
 */
#ifndef HL64_FX_H
#define HL64_FX_H

#include <stdint.h>

#include "hl64/coll.h"
#include "hl64/types.h"

/* ---- TE_* effects ---------------------------------------------------------- */

void	fx_sparks(const float org[3]);					/* TE_SPARKS */
void	fx_ricochet(const float org[3], float scale);			/* TE_ARMOR_RICOCHET */
void	fx_gunshot(const float org[3]);					/* TE_GUNSHOT */
/* TE_EXPLOSION: sprite, scale (0.1 units), framerate, TE_EXPLFLAG_* */
void	fx_explosion(const float org[3], res_t sprite, int scale, int framerate, int flags);
void	fx_smoke(const float org[3], res_t sprite, int scale, int framerate);	/* TE_SMOKE */
void	fx_tracer(const float start[3], const float end[3]);		/* TE_TRACER */
void	fx_sprite(const float org[3], res_t sprite, int scale, int brightness);	/* TE_SPRITE */
void	fx_glow_sprite(const float org[3], res_t sprite, int life, int scale, int brightness);
/* TE_BLOODSPRITE: spray sprite, drop sprite, palette colour, scale */
void	fx_blood_sprite(const float org[3], res_t spray, res_t drop, int color, int scale);
void	fx_blood_stream(const float org[3], const float dir[3], int color, int speed);
/* TE_BREAKMODEL: centre, size, velocity, random velocity, model, count,
 * life (0.1 s), BREAK_* flags (material sound set) */
void	fx_break_model(const float org[3], const float size[3], const float vel[3], int rand_vel,
		res_t model, int count, int life, int flags);
/* TE_MODEL: one tempentity model (shell, gib) */
void	fx_temp_model(const float org[3], const float vel[3], float yaw, res_t model,
		int bounce_sound, int life);
/* TE_DLIGHT: radius (10 u), colour, life (0.1 s), decay (10 u/s) */
void	fx_dlight(const float org[3], int radius, int r, int g, int b, int life, int decay);
/* TE_ELIGHT: light following entity e */
void	fx_elight(int e, const float org[3], float radius, int r, int g, int b, int life,
		float decay);
/* Muzzle flash on entity e's attachment (EF_MUZZLEFLASH / client event). */
void	fx_muzzle_flash(int e, int attachment, int type);
void	fx_particles(const float org[3], const float dir[3], int color, int count);
/* Beams (TE_BEAMPOINTS / BEAMENTPOINT / BEAMENTS): ends as entity slots
 * (-1: use the point); frame / framerate the sprite's starting frame and
 * frame rate (0.1 fps); width / noise / brightness 0-255, scroll speed,
 * life (0.1 s, 0 = until killed), FX_BEAM_SHADE* flags (brightness ramps
 * up from the start / down to the end). key names the beam for
 * fx_beam_kill: FX_BEAM_KEY(e, n) for entity e's n-th beam (n <
 * FX_BEAM_KEYS_PER_ENT), -1 for a beam nobody kills; a beam with a live key
 * replaces it, and a keyed beam takes the slot of the unkeyed beam nearest
 * its end when the pool is full. Returns key, also when the pool is full
 * and the beam is dropped: game state (private blocks, saved games) holds
 * keys, functions of game state, never pool slots, which depend on the
 * effects alive (unsaved). */
#define FX_BEAM_KEYS_PER_ENT	8
#define FX_BEAM_KEY(e, n)	((e) * FX_BEAM_KEYS_PER_ENT + (n))
#define FX_BEAM_SHADEIN		1
#define FX_BEAM_SHADEOUT	2
int	fx_beam(int key, int e0, const float p0[3], int e1, const float p1[3], res_t sprite,
		int frame, int framerate, int width, int noise, int r, int g, int b, int brightness,
		int speed, int life, int flags);
void	fx_beam_kill(int key);
/* TE_BEAMCYLINDER: a ring growing to radius over life (0.1 s) */
void	fx_beam_cylinder(const float org[3], float radius, res_t sprite, int life, int width,
		int noise, int r, int g, int b, int bright);
/* TE_IMPLOSION: count tracers converging on org from radius over life (0.1 s) */
void	fx_implosion(const float org[3], int radius, int count, int life);
/* TE_BEAMFOLLOW: a trail of decaying beam segments behind entity e (sprite,
 * life of each segment in 0.1 s, width in 0.1 units, colour, brightness);
 * it outlives the entity until its segments die. One trail per entity
 * (a new call replaces it); a full pool (HL64_FX_TRAILS) drops. */
void	fx_beam_follow(int e, res_t sprite, int life, int width, int r, int g, int b,
		int brightness);
/* TE_FIZZ: density + 1 bubbles (sprite) at random points of entity e's
 * box, rising until the water surface or a solid ceiling; e's speed hack
 * (HLSDK CBubbling: rendercolor x << 8 | y, z = 1 negative) is the
 * horizontal current along its yaw. */
void	fx_fizz(int e, res_t sprite, int density);

/* ---- decals (render-fx.md 4) -------------------------------------------- */

/* decal = HLSDK decals.h index (dlls/world.cpp gDecals order); the
 * effects map it by name onto the cooked decal sheet (gsfx/decals.i4);
 * decals the sheet lacks are not placed. 42 + i: entry i of the caller's
 * map's placed-decal sheet (rec_key_decal; w64-format.md "DECL"). */
void	fx_decal_trace(const col_trace_t *tr, int decal);
void	fx_decal_at(int e, const float org[3], int decal);
/* HLSDK pfnStaticDecal / TE_BSPDECAL (infodecal): placed now on model e
 * (0 = the world) at org. > 0 placed, 0 nothing takes it (or no such
 * decal), < 0 not now (a sector around org is not resident yet, the map's
 * sheet is not read yet, or the frame's 3 ms placement budget is spent):
 * call again later. */
int	fx_decal_static(int e, const float org[3], int decal);

/* ---- screen (HLSDK UTIL_ScreenShake / ScreenFade / HudMessage) ---------- */

void	fx_shake(const float org[3], float amplitude, float frequency, float duration,
		float radius);
/* FFADE_* flags; colour; duration and hold in s */
void	fx_fade(int r, int g, int b, int a, float duration, float hold, int flags);
/* pfnLightStyle: style 0-63 plays cooked pattern id (rtb PATN;
 * lightstyles.md 2). */
void	fx_lightstyle(int style, uint16_t pattern);
/* Cooked titles.txt entry id, or a game text (HUD draws it). */
void	fx_text(uint16_t title);

/* ---- effect assets ------------------------------------------------------------ */

/* The game names the sounds and sprites the client effects use (its
 * res.def ids) at game_init: per set up to FX_ASSET_MAX ids; sounds are
 * picked at random, muzzle-flash sprites by flash type. Unregistered sets
 * play / draw nothing. */
enum {
	FX_A_RICOCHET_SND,	/* TE_ARMOR_RICOCHET, TE_GUNSHOT, sparks */
	FX_A_EXPLODE_SND,	/* TE_EXPLOSION unless NOSOUND */
	FX_A_SHELL_SND,		/* TE_BOUNCE_SHELL */
	FX_A_SHOTSHELL_SND,	/* TE_BOUNCE_SHOTSHELL */
	FX_A_GLASS_SND,		/* break-model debris bounces per BREAK_* material */
	FX_A_METAL_SND,
	FX_A_FLESH_SND,
	FX_A_WOOD_SND,
	FX_A_CONCRETE_SND,
	FX_A_RICOCHET_SPR,	/* sparks / ricochet sprite (sprites/richo1.spr) */
	FX_A_MUZZLE_SPR,	/* muzzle flash sprites by type */
	FX_A_SETS
};
#define FX_ASSET_MAX	6
void	fx_assets(int set, const res_t *ids, int n);

/* ---- engine side ------------------------------------------------------------- */

void	fx_init(void);
/* Per frame after the view: simulate temp entities, particles, beams,
 * fades (time f64 s, dt). */
void	fx_frame(double time, double dt);
/* Screen state for the view and HUD: shake offset / angle, fade colour. */
void	fx_view(float org_ofs[3], float ang_ofs[3], uint8_t fade_rgba[4]);
/* The showing title (0xFFFF none) and s since fx_text; the HUD draws it
 * (game/hud: colour, scan-out, position). */
uint16_t fx_text_current(float *time);

typedef struct {
	uint32_t	temps, temps_peak, particles_peak, beams_peak, decals, decal_drops;
	uint32_t	trans_drawn, trans_dropped;
	uint32_t	us, us_max;	/* effects time per frame: sum, worst frame (sim + draw) */
	uint32_t	sim_us, part_us, trans_us, decal_us;	/* sums of us by part */
	uint32_t	frames, dlights;	/* fx_frame calls, lights allocated */
} fx_stats_t;
void	fx_stats(fx_stats_t *s, int reset);

#endif
