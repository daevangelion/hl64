/* SPDX-License-Identifier: MIT */
/*
 * ren.h -- renderer (docs/architecture.md 9): camera, world sectors,
 * static movers, texture animation, lightstyle recolouring, dynamic
 * lights on the world. M1 draws the free camera's view; game_calc_view
 * replaces the camera source in M2.
 */
#ifndef HL64_REN_H
#define HL64_REN_H

#include <stdint.h>

#include "engine/render/gpu.h"

/* ---- camera ------------------------------------------------------------- */

typedef struct {
	int		map;		/* campaign map the eye is in */
	float		org[3];		/* eye, global (map-local + g_M) */
	float		ang[3];		/* pitch (down +), yaw, roll; degrees */
} ren_cam_t;

extern ren_cam_t	ren_cam;

/* Open the campaign start map, place the eye at its player start and
 * register the setpos / look debug commands. */
void	ren_cam_init(void);
/* Free camera (debug pad): left stick / C-buttons move and turn, Z / R
 * down / up, scaled by dt. */
void	ren_cam_update(uint32_t dt_us);
/* Forward, right, up of (pitch, yaw, roll) in degrees (GoldSrc axes). */
void	ren_angle_axes(const float ang[3], float f[3], float r[3], float u[3]);
float	ren_fov_y(float fov_x);

/* ---- frame ---------------------------------------------------------------- */

void	ren_init(void);
/* Draw the camera's view (between sys_frame_try_begin and sys_frame_end)
 * at game time `time` (f64 s; texture animation r10 = floor(time x 10)). */
void	ren_frame(double time);

/* The frame's inward frustum side-plane normals (n . (p - eye) >= 0 inside;
 * eye global), valid during ren_frame. */
const float	(*ren_frustum(void))[3];

typedef struct {
	uint32_t	sectors, meshlets, verts, tris, binds;	/* drawn */
	uint32_t	movers, relit, dlit, missing;
	uint32_t	dropped;	/* over HL64_REN_VIS_MAX / HL64_REN_MOVERS_MAX */
	uint32_t	leaf, r10;	/* eye leaf, floor(time x 10) used */
	uint32_t	world_us;	/* CPU: list build + submit */
} ren_stats_t;
void	ren_stats(ren_stats_t *s);

/* Texture drawn for texture t with entity frame `frame` at r10 =
 * floor(time x 10) (world-surfaces.md 4). */
int	ren_tex_anim(const w64_tex_t *texs, int ntex, int t, int frame, uint32_t r10);

/* ---- dynamic lights (docs/specs/dlights.md 1) ---------------------------- */
typedef struct {
	int		key;		/* entity number or 0 */
	float		origin[3];	/* global */
	float		radius;
	float		minlight;
	float		decay;		/* radius units per second */
	double		die;		/* game time (f64 s); dead once die < time */
	uint8_t		color[3];
	uint8_t		world;		/* lights the world (elights do not) */
} ren_dlight_t;

/* Light for key (reused), else the first dead light, else light 0;
 * cleared, world-lighting. */
ren_dlight_t	*ren_dlight_alloc(int key, double time);
/* Before drawing, at game time `time` after a frame of dt s: kill lights
 * past their die time, decay the rest. */
void		ren_dlight_frame(double time, double dt);
int		ren_dlight_live(int i);
ren_dlight_t	*ren_dlight(int i);
/* World vertex falloff (dlights.md 3): contribution of one light at
 * signed plane distance p and in-plane distance q. */
float		ren_dlight_falloff(float radius, float minlight, float p, float q);

#endif
