/* SPDX-License-Identifier: MIT */
/*
 * fx_int.h -- internals shared by the effects modules (engine/render/
 * fx.c, tent.c, part.c, beam.c, decal.c, sprite.c, trans.c): the effects
 * clock, the render random stream, frame conversion and the per-module
 * entry points the frame and the draw call. Everything is kept in global
 * coordinates (map-local + g_M); game calls arrive map-local in the
 * caller's frame (eng_frame_layer) and are converted once.
 */
#ifndef HL64_FX_INT_H
#define HL64_FX_INT_H

#include <stdint.h>

#include "hl64/coll.h"
#include "hl64/fx.h"
#include "hl64/types.h"
#include "engine/render/addblend.h"
#include "engine/render/gpu_fx.h"

/* HLSDK common/const.h values the effects interpret (interface constants). */
#define FX_RM_NORMAL		0
#define FX_RM_TRANSCOLOR	1
#define FX_RM_TRANSTEXTURE	2
#define FX_RM_GLOW		3
#define FX_RM_TRANSALPHA	4
#define FX_RM_TRANSADD		5

#define FX_RFX_NONE		0
#define FX_RFX_PULSE_SLOW	1
#define FX_RFX_PULSE_FAST	2
#define FX_RFX_PULSE_SLOW_WIDE	3
#define FX_RFX_PULSE_FAST_WIDE	4
#define FX_RFX_FADE_SLOW	5
#define FX_RFX_FADE_FAST	6
#define FX_RFX_SOLID_SLOW	7
#define FX_RFX_SOLID_FAST	8
#define FX_RFX_STROBE_SLOW	9
#define FX_RFX_STROBE_FAST	10
#define FX_RFX_STROBE_FASTER	11
#define FX_RFX_FLICKER_SLOW	12
#define FX_RFX_FLICKER_FAST	13
#define FX_RFX_NO_DISSIPATION	14
#define FX_RFX_DISTORT		15
#define FX_RFX_HOLOGRAM		16
#define FX_RFX_GLOW_SHELL	19

#define FX_EXPL_NOADDITIVE	1
#define FX_EXPL_NODLIGHTS	2
#define FX_EXPL_NOSOUND		4
#define FX_EXPL_NOPARTICLES	8

#define FX_BREAK_GLASS		0x01
#define FX_BREAK_METAL		0x02
#define FX_BREAK_FLESH		0x04
#define FX_BREAK_WOOD		0x08
#define FX_BREAK_SMOKE		0x10
#define FX_BREAK_TRANS		0x20
#define FX_BREAK_CONCRETE	0x40

#define FX_FADE_OUT		0x1
#define FX_FADE_MODULATE	0x2
#define FX_FADE_STAYOUT		0x4

/* SPR v2 orientation types (file format values). */
#define FX_SPR_PARALLEL_UPRIGHT	0
#define FX_SPR_FACING_UPRIGHT	1
#define FX_SPR_PARALLEL		2
#define FX_SPR_ORIENTED		3
#define FX_SPR_PARALLEL_ORIENTED 4
/* SPR v2 texture formats (file format values) */
#define FX_SPR_TEX_NORMAL	0
#define FX_SPR_TEX_ADDITIVE	1
#define FX_SPR_TEX_INDEXALPHA	2
#define FX_SPR_TEX_ALPHTEST	3

/* ---- shared state (fx.c) ---------------------------------------------------- */

typedef struct {
	double		now;		/* effects clock: game time of the last fx_frame */
	float		dt;
	uint32_t	rng;		/* render random stream (xorshift32), not the game's */
	uint32_t	frame;
	float		eye[3];		/* this frame's eye (global), view axes */
	float		fwd[3], right[3], up[3];
	int		map;		/* map the eye is in */
	fx_stats_t	st;		/* counters of the current window */
} fx_state_t;
extern fx_state_t	fx_s;

uint32_t	fx_rand(void);
float		fx_randf(float lo, float hi);
int		fx_randi(int lo, int hi);		/* inclusive */
void		fx_global(const float in[3], float out[3]);	/* caller frame -> global */
/* Line trace in global coordinates (world and brush entities, no monsters
 * when type = COL_MOVE_NOMONSTERS); tr->end global. */
void		fx_trace(const float a[3], const float b[3], int type, col_trace_t *tr);
float		fx_gravity(void);		/* sv_gravity */
/* Effect sounds registered by the game (fx_sounds): a random member of set. */
void		fx_play(int set, const float org_g[3], float vol, float attn);
/* Camera-relative quarter-unit position; 0 when out of range. */
int		fx_cam_pos(const float g[3], int16_t out[3]);

/* ---- temp entities (tent.c) ------------------------------------------------- */

#define TF_GRAVITY	0x0001
#define TF_SLOWGRAV	0x0002
#define TF_COLLIDE	0x0004	/* bounce on the world */
#define TF_ANIMATE	0x0008	/* frames advance at framerate; dies at the end */
#define TF_LOOP		0x0010	/* animate, wrap */
#define TF_FADEOUT	0x0020	/* alpha fades over the last second */
#define TF_ROTATE	0x0040	/* angles += avelocity */
#define TF_SMOKE	0x0080	/* leaves a particle trail */
#define TF_SPARKS	0x0100	/* spark shower: emits sparks while alive */

typedef struct {
	float		org[3], vel[3], ang[3], avel[3];
	double		die;
	float		frame, framerate, scale;
	res_t		model;		/* SPRITE or MODEL */
	uint16_t	flags;		/* TF_* */
	uint8_t		rendermode, renderfx, amt, live;
	uint8_t		color[3], body;
	uint8_t		bounce;		/* fx_sounds set played on impact, 0 none */
	uint8_t		hits;
	uint16_t	pad;
} fx_tent_t;

void		tent_init(void);
fx_tent_t	*tent_alloc(const float org_g[3], res_t model);	/* NULL: pool full (counted) */
void		tent_frame(void);
/* Add the visible sprite temps to the translucent list, draw the studio
 * temps (nearest HL64_FX_TEMPS_STUDIO). */
void		tent_draw_studio(void);
void		tent_list_sprites(void);
int		tent_live(void);

/* ---- beams (beam.c) ---------------------------------------------------------- */

void		beam_init(void);
void		beam_frame(void);
void		beam_list(void);		/* into the translucent list */
void		beam_draw(int b, uint8_t blend);	/* trans.c callback */
int		beam_live(void);
/* Rings (TE_BEAMCYLINDER) share the pool: radius grows to r over life. */
int		beam_ring(const float org_g[3], float r, res_t sprite, float life, int width, int noise,
			const uint8_t rgb[3], int bright);

/* ---- particles (part.c) ----------------------------------------------------- */

#define PT_STATIC	0	/* no gravity */
#define PT_GRAV		1	/* sv_gravity */
#define PT_SLOWGRAV	2	/* half */
#define PT_FADE		3	/* gravity, colour ramps down over its life */
#define PT_TRACER	4	/* streak along its velocity, no gravity */
#define PT_TRACER_GRAV	5	/* streak with gravity (spark streaks) */

void		part_init(void);
/* One particle; 0 when the pool is full (counted). vel u/s; life s. */
int		part_add_rgb(const float org_g[3], const float vel[3], const uint8_t rgb[3], int type,
			float life);
void		part_frame(void);
void		part_draw(void);
int		part_live(void);
/* Palette colour of a palette index (gsfx/palette.lmp; grey ramp without it). */
void		part_color(uint8_t index, uint8_t rgb[3]);
/* Profile: effects draw time (renderer side) joins fx_stats us, by part. */
enum { FX_US_PART, FX_US_TRANS, FX_US_DECAL };
void		fx_add_us(int part, uint32_t us);

/* ---- decals (decal.c) --------------------------------------------------------- */

void		decal_init(void);
void		decal_frame(void);		/* placement queue, 3 ms cap */
void		decal_draw(uint32_t *ndecals, uint32_t *nverts);	/* counts what it submits */
int		decal_count(void);
/* Queue a placement: point (global), hit normal, world (-1) or the brush
 * entity e, a sheet texture (the fx_decal_* calls resolve decal ids). */
void		decal_queue(const float p_g[3], const float n[3], int e, int decal);
/* Test builds: place now (no queue, no cap): decal id as fx_decal_static
 * (map-local ids in eng_frame_layer's map); returns the pieces placed,
 * -1 not now. */
int		decal_place_now(const float p_g[3], const float n[3], int e, int decal);

typedef struct {
	uint16_t	live, placed, pending, dropped, replaced, pieces, verts_max, nosheet;
	uint16_t	statics;	/* fx_decal_static placements */
	uint32_t	place_us;
	uint32_t	region_peak;	/* decal region bytes: sheet + verts + slot sheets (peak) */
} decal_stats_t;
void		decal_stats(decal_stats_t *s, int reset);
/* Test builds: decal i's pieces (global vertices) for checks. */
int		decal_info(int i, int *owner, int *verts, float (*out)[3], int max);

/* ---- sprites (sprite.c) -------------------------------------------------------- */

typedef struct {
	int16_t		left, up;	/* frame origin offset (texels = units) */
	uint16_t	w, h;		/* source size (world units at scale 1) */
	uint8_t		tw, th;		/* CI4 sides */
	uint16_t	pad;
	uint32_t	ci4;		/* offset in the file */
} fx_sprframe_t;			/* 16 */

typedef struct {
	const uint8_t	*base;		/* resident .sc4 bytes, NULL */
	const uint16_t	*tlut;
	const fx_sprframe_t *frames;	/* images (singles and group members) */
	const uint16_t	*groups;	/* per entity frame: first image, image count */
	const uint16_t	*ends;		/* per image: cumulative group interval end, ms */
	int		nframes;	/* entity frames (a group counts once) */
	int		nimages;
	uint8_t		type;		/* FX_SPR_* */
	uint8_t		texformat;	/* FX_SPR_TEX_* */
	const add_ramp_t *ramp;		/* the cooked additive ramp (render-fx.md 2a); for
					 * FX_SPR_TEX_INDEXALPHA the alpha levels + colour */
} fx_sprite_t;

void		sprite_init(void);
void		sprite_frame(void);		/* LRU aging, fences */
/* The sprite's data when resident (marks it used); else requests it and
 * returns NULL (the caller skips this frame). */
const fx_sprite_t *sprite_get(res_t r);
/* Quad of frame f of s at org (global): angles for oriented types, scale,
 * blend mode decided by the caller. */
void		sprite_draw(const fx_sprite_t *s, int f, const float org[3], const float ang[3],
			float scale, int gpu_mode, const uint8_t rgba[4]);
/* Mode and texture of a draw of image fr of s (gpu_mode GPU_FX_*); the
 * ramp modes take their tint from rgba's colour and their blend from its
 * alpha (render-fx.md 2a); an index-alpha sprite always draws through its
 * alpha-level TLUT (render-fx.md 3), GPU_FX_IALPHA for the blended modes. */
void		sprite_bind(const fx_sprite_t *s, const fx_sprframe_t *fr, int gpu_mode,
			const uint8_t rgba[4]);
/* After sprite_bind of the same sprite and mode: only the blend / tint of
 * the following draws changes (the ramp's ends; the texture stays). */
void		sprite_reblend(const fx_sprite_t *s, int gpu_mode, const uint8_t rgba[4]);
/* Image of entity frame f at effects time t (render-fx.md 3: a group
 * frame picks its member by time within the group's intervals). */
int		sprite_image(const fx_sprite_t *s, int f, double t);

typedef struct {
	uint32_t	resident, bytes, peak, loads, evicted, skipped;
} sprite_stats_t;
void		sprite_stats(sprite_stats_t *s, int reset);

/* ---- translucent list (trans.c) --------------------------------------------- */

enum { TK_SPRITE_ENT, TK_STUDIO_ENT, TK_BRUSH_ENT, TK_TEMP, TK_BEAM };

/* Add an item at squared eye distance d2; kept: nearest HL64_FX_TRANS_MAX. */
void		trans_add(int kind, int id, float d2, uint8_t blend, uint8_t mode);
/* Blend of an entity this frame (render-fx.md 1). */
int		trans_blend(int e);
int		trans_blend_amt(int rendermode, int renderfx, int amt, int e, float dist);
/* GPU mode for (render mode, model kind) and the capped blend (X1). */
int		trans_gpu_mode(int rendermode, int kind, uint8_t *blend);
void		trans_frame(double dt);		/* persistent fades (5-8) */
/* Translucent pass callback of the temp entities (tent.c). */
void		tent_draw(int i, uint8_t blend, int mode);

/* Pool drops since the last reset (counted by each pool). */
uint32_t	tent_dropped(int reset);
uint32_t	part_dropped(int reset);
uint32_t	beam_dropped(int reset);

#ifdef HL64_HOST
/* Tests: a cooked .sc4 image (big-endian, caller-owned) as resident sprite r. */
int		sprite_test_inject(res_t r, uint8_t *bytes, uint32_t size);
/* X2: the last translucent pass's items, back to front. */
int		trans_last_order(uint8_t *kind, uint16_t *id, uint8_t *blend, int max);
/* Tests: every decal pool slot a copy of live decal i at its worst
 * (HL64_FX_DECAL_VERTS vertices, fans of 3): the vertex ring's worst
 * case. Slots filled, or -1. */
int		decal_test_fill_worst(int i);
/* Tests: the entity frame of the live beam holding key at effects time t
 * (-1: no such beam, its sprite not resident). */
int		beam_test_frame(int key, double t);
#endif

#endif
