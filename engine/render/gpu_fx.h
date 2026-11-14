/* SPDX-License-Identifier: MIT */
/*
 * gpu_fx.h -- the effects renderer's platform seam (docs/architecture.md
 * 9.5-9.7, docs/specs/render-fx.md 2-5): sprites, temp-entity sprites,
 * beams, particles, decals, translucent brush movers and the screen fade.
 * plat/n64/gpu_fx.c submits through tiny3d / TinyPX / rdpq;
 * plat/host/gpu_fx.c is the draw-list recorder (D5): it counts and hashes
 * every call per mode and logs the RDP state each mode chose (X1).
 *
 * Calls happen between gpu_begin and gpu_end. Positions are
 * camera-relative (eye at the origin, world axes) in quarter units:
 * s16 = 4 x (p - eye), so +-8191 u are reachable; anything farther is
 * culled by the engine before it gets here. Decals instead draw in their
 * surface's own frame (gpu_fx_frame), so their decal depth compare holds.
 */
#ifndef HL64_GPU_FX_H
#define HL64_GPU_FX_H

#include <stdint.h>

#include "hl64/limits.h"
#include "engine/render/gpu.h"

#define GPU_FX_POS_SCALE	4.0f	/* s16 position units per world unit */

/* The frame's vertex ring, in vertex pairs (32 B; both platforms count
 * it the same way): the translucent pass's share (sprites, beams,
 * streaks, brushes, temp sprites) plus every decal at its worst (all
 * HL64_FX_DECALS in view, each HL64_FX_DECAL_VERTS vertices and its
 * frame matrix). A batch that does not fit is not drawn and is counted
 * (gpu_fx_stats_t.drops; the effects pass logs the first full frame). */
#define GPU_FX_MAT_PAIRS	2	/* a gpu_fx_frame matrix (64 B) */
#define GPU_FX_TRANS_PAIRS	1024
#define GPU_FX_RING_PAIRS	(GPU_FX_TRANS_PAIRS + HL64_FX_DECALS \
	* ((HL64_FX_DECAL_VERTS + 1) / 2 + GPU_FX_MAT_PAIRS))

/* RDP state of each effects mode (render-fx.md 2 table, as the RDP does it). */
enum {
	GPU_FX_CUTOUT,		/* sprite Normal / TransAlpha: TLUT alpha compare, Z test + write */
	GPU_FX_BLEND,		/* sprite TransTexture / TransColor / TransAlpha x blend */
	GPU_FX_ADD,		/* sprite / beam TransAdd: ramp TLUT, screen blend (2a), Z test */
	GPU_FX_GLOW,		/* sprite Glow: as ADD, no Z test */
	GPU_FX_DECAL,		/* I4 decal: rgb = tint, a = texel x a, decal Z */
	GPU_FX_POINT,		/* particles: colour, opaque, Z test + write */
	GPU_FX_STREAK,		/* tracer streaks: unit shade colour, a = brightness (2a), Z test */
	GPU_FX_BRUSH_BLEND,	/* brush TransTexture: texture fullbright x blend */
	GPU_FX_BRUSH_ADD,	/* brush TransAdd / Glow: page ramp TLUTs (2a), Z test */
	GPU_FX_BRUSH_COLOR,	/* brush TransColor: flat rendercolor, alpha = blend */
	GPU_FX_SCREEN,		/* screen fade: flat colour, no Z */
	GPU_FX_IALPHA,		/* index-alpha sprite blended: level TLUT (ramp combiner), Z test */
	GPU_FX_MODES
};

/* blender (no additive mode: the RDP adder wraps, render-fx.md 2a) */
#define GPU_FX_BL_NONE		0	/* opaque */
#define GPU_FX_BL_ALPHA		1	/* in x a + mem x (1 - a) */
/* colour combiner */
#define GPU_FX_CC_TEX_SHADE	0	/* rgb = tex x shade, a = tex a x shade a */
#define GPU_FX_CC_SHADE		1	/* rgb = shade, a = shade a */
#define GPU_FX_CC_SHADE_TEXA	2	/* rgb = shade, a = tex (I4) x shade a */
#define GPU_FX_CC_PRIM		3	/* rgb = prim, a = 1 */
#define GPU_FX_CC_TEX_PRIM	4	/* rgb = tex x prim, a = prim a */
#define GPU_FX_CC_PRIM_A	5	/* rgb = prim, a = prim a */
#define GPU_FX_CC_RAMP		6	/* rgb = env + (prim - env) x tex I, a = tex A x prim a */
/* TLUT */
#define GPU_FX_TLUT_NONE	0
#define GPU_FX_TLUT_RGBA16	1
#define GPU_FX_TLUT_IA16	2	/* the additive ramp / index-alpha level TLUT (addblend.h) */

typedef struct {
	uint8_t		blender, combiner;
	uint8_t		ztest, zwrite, zdecal;
	uint8_t		acompare;	/* alpha compare (TLUT alpha bit / threshold) */
	uint8_t		textured;	/* 0 none, 1 CI4 + TLUT, 2 I4, 3 world page */
	uint8_t		tlut;		/* GPU_FX_TLUT_* */
} gpu_fx_rdp_t;

/* The one table both platforms apply (engine/render/trans.c). */
extern const gpu_fx_rdp_t	gpu_fx_rdp[GPU_FX_MODES];
extern const char		*const gpu_fx_mode_name[GPU_FX_MODES];

typedef struct {
	int16_t		pos[3];		/* camera-relative, GPU_FX_POS_SCALE units */
	int16_t		st[2];		/* texel coordinates, s10.5 */
	uint8_t		rgba[4];	/* shade */
} gpu_fx_vert_t;			/* 14 */

typedef struct {
	int16_t		pos[3];		/* camera-relative, GPU_FX_POS_SCALE units */
	uint8_t		size;		/* screen pixels at 1 */
	uint8_t		rgba[4];
	uint8_t		pad;
} gpu_fx_point_t;			/* 12 */

/* Effects pass bracket (after the studio entities, before the view model). */
void	gpu_fx_begin(void);
void	gpu_fx_end(void);
/* Mode for the following draws; prim colour for the PRIM combiners. The
 * cart applies a mode's state only when the mode changes. */
void	gpu_fx_mode(int mode, const uint8_t prim[4]);
/* Ramp ends of the following GPU_FX_CC_RAMP draws: PRIM (hi, alpha =
 * blend) and ENV (lo); addblend.h add_ramp_ends. */
void	gpu_fx_ramp(const uint8_t prim[4], const uint8_t env[4]);
/* Another renderer drew inside the effects pass (studio entities and
 * temp models through gpu_studio) and left its own RDP / tiny3d state:
 * the next gpu_fx_mode applies its whole state, whatever the previous
 * mode was. */
void	gpu_fx_state_lost(void);
/* Texture for the following draws: CI4 w x h with its 16-entry TLUT
 * (sprites: RGBA5551, or the IA16 ramp TLUT in the ramp modes), or I4
 * w x h (decals); sides are powers of two <= 64. The bytes stay valid
 * until the GPU has read them (two frames). */
void	gpu_fx_tex_ci4(const uint16_t *tlut, const uint8_t *ci4, int w, int h);
void	gpu_fx_tex_i4(const uint8_t *i4, int w, int h);
/* Frame of the following gpu_fx_tris (until the next call): positions are
 * s16 under m, the way the world and its movers reach the GPU (rows 0-2:
 * the frame's axes / GPU_FX_POS_SCALE, row 3: frame origin - eye). Keep
 * the origin near the vertices: the RSP folds 1 / GPU_FX_POS_SCALE into
 * the view matrix with 2^-16 steps, an error that grows with |pos|.
 * NULL = camera-relative quarter units (the default after gpu_fx_begin). */
void	gpu_fx_frame(const ren_mat_t *m);
/* Triangles over nv vertices (nv <= 64): idx holds 3 x ntri indices. */
void	gpu_fx_tris(const gpu_fx_vert_t *v, int nv, const uint8_t *idx, int ntri);
/* Particles (one call per frame). */
void	gpu_fx_points(const gpu_fx_point_t *p, int n);

typedef struct {
	uint32_t	calls[GPU_FX_MODES];	/* triangle / point batches per mode */
	uint32_t	tris, verts, points, texloads;
	uint32_t	drops;		/* batches not drawn: the ring was full */
	uint32_t	pairs;		/* ring pairs used (of GPU_FX_RING_PAIRS) */
	uint32_t	hash;		/* host: FNV-1a of the effects draw list; cart 0 */
} gpu_fx_stats_t;
/* Last completed frame. */
void	gpu_fx_stats(gpu_fx_stats_t *s);

#ifdef HL64_HOST
/* Recorder: the vertices of the last gpu_fx_tris call and its mode (tests);
 * its gpu_fx_frame matrix (returns 0 when it was camera-relative); the
 * last gpu_fx_ramp ends and returns the last gpu_fx_tex_ci4 TLUT. */
int	gpu_fx_host_last(gpu_fx_vert_t *out, int max, int *mode);
int	gpu_fx_host_last_frame(ren_mat_t *m);
const uint16_t *gpu_fx_host_last_ramp(uint8_t prim[4], uint8_t env[4]);
#endif

#endif
