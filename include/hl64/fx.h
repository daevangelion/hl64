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
 * (-1: use the point); width / noise / brightness 0-255, scroll speed,
 * life (0.1 s, 0 = until killed); returns a beam id (-1 dropped). */
int	fx_beam(int e0, const float p0[3], int e1, const float p1[3], res_t sprite, int width,
		int noise, int r, int g, int b, int brightness, int speed, int life);
void	fx_beam_kill(int beam);
/* TE_BEAMCYLINDER: a ring growing to radius over life (0.1 s) */
void	fx_beam_cylinder(const float org[3], float radius, res_t sprite, int life, int width,
		int noise, int r, int g, int b, int bright);
/* TE_IMPLOSION: count tracers converging on org from radius over life (0.1 s) */
void	fx_implosion(const float org[3], int radius, int count, int life);

/* ---- decals (render-fx.md 4) -------------------------------------------- */

/* Decal texture index of the cooked decal sheet (game/defs decal names). */
void	fx_decal_trace(const col_trace_t *tr, int decal);
void	fx_decal_at(int e, const float org[3], int decal);

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

/* ---- engine side ------------------------------------------------------------- */

void	fx_init(void);
/* Per frame after the view: simulate temp entities, particles, beams,
 * fades (time f64 s, dt). */
void	fx_frame(double time, double dt);
/* Screen state for the view and HUD: shake offset / angle, fade colour. */
void	fx_view(float org_ofs[3], float ang_ofs[3], uint8_t fade_rgba[4]);
uint16_t fx_text_current(float *alpha);

typedef struct {
	uint32_t	temps, temps_peak, particles_peak, beams_peak, decals, decal_drops;
	uint32_t	trans_drawn, trans_dropped, us, us_max;
} fx_stats_t;
void	fx_stats(fx_stats_t *s, int reset);

#endif
