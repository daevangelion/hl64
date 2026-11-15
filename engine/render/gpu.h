/* SPDX-License-Identifier: MIT */
/*
 * gpu.h -- the renderer's platform seam (docs/architecture.md 9, D5).
 * plat/n64/gpu.c submits through tiny3d / rdpq; plat/host/gpu.c is the
 * draw-list recorder: same calls, it counts binds, meshlets, vertices and
 * triangles and hashes the list, and its "shot" logs them.
 *
 * engine/render calls these between sys_frame_try_begin and
 * sys_frame_end. Matrices are float, tiny3d's layout (m[axis][component],
 * m[3] = translation), camera-relative; the platform converts and keeps
 * them until the GPU has read them.
 */
#ifndef HL64_GPU_H
#define HL64_GPU_H

#include <stdint.h>

#include "engine/world/w64.h"

typedef struct {
	float		m[4][4];
} ren_mat_t;

typedef struct {
	float		fwd[3], right[3], up[3];	/* view axes, world space */
	float		fov_y;			/* degrees */
	float		znear, zfar;
	uint8_t		nvg;			/* night vision (below) */
} gpu_view_t;

/* Night vision (architecture.md D12, the local player's EF_DIMLIGHT):
 * from gpu_begin to the next gpu_begin the world, its movers (opaque and
 * TransAlpha) and the opaque / TransTexture studio models draw rgb =
 * texel x GPU_NVG_PRIM + GPU_NVG_ENV in the same one-cycle RDP mode
 * (vertex shade and model light ignored; alpha = the texel's, so fence
 * cutouts stay, or a studio model's blend alpha); the opaque world and
 * opaque models darken to black with eye depth over [GPU_NVG_NEAR,
 * GPU_NVG_FAR] (RDP fog). Additive and effect passes and the HUD draw as
 * without it. */
#define GPU_NVG_PRIM_R	56
#define GPU_NVG_PRIM_G	255
#define GPU_NVG_PRIM_B	56
#define GPU_NVG_ENV_R	0
#define GPU_NVG_ENV_G	28
#define GPU_NVG_ENV_B	0
#define GPU_NVG_NEAR	64.0f
#define GPU_NVG_FAR	1024.0f

/* Index encoding self-check (w64-format.md "Runtime requirements"):
 * nonzero when the running tiny3d encodes strip indices differently from
 * (stride, base); gpu_index_reencode then converts a cooked strip in
 * place. The host recorder reads the cooked encoding: always 0. */
int	gpu_index_check(unsigned stride, unsigned base);
void	gpu_index_reencode(int16_t *idx, int n, unsigned stride, unsigned base);

/* Frame: clear, projection and view rotation (eye at the origin). */
void	gpu_begin(const gpu_view_t *v);
void	gpu_end(void);
/* Model matrix for the following draws (camera-relative). */
void	gpu_matrix_push(const ren_mat_t *m);
void	gpu_matrix_pop(void);
/* Bind a resident TEXP page; state = its WLD_PAGE_STATE bytes in the
 * layer (prebuilt RDP bind list, rebuilt when the page moves). */
void	gpu_bind_page(const w64_texp_t *p, uint8_t *state);
/* Another renderer (the effects pass) set the RDP alpha compare: the next
 * gpu_bind_page applies its page's fence state whatever it was. */
void	gpu_state_lost(void);
/* Additive pages (render-fx.md 2a: brush and studio TransAdd / Glow):
 * tint_blend = rgb tint and blend; until gpu_page_ramp(NULL) every
 * gpu_bind_page also loads its page's cooked IA16 ramp TLUT (addblend.h
 * add_page_ramp) and sets PRIM / ENV to its ends for the ramp combiner
 * the caller set; a page the cook gave no ramp draws with blend 0
 * (nothing added; add_stats counts it). */
void	gpu_page_ramp(const uint8_t tint_blend[4]);
/* One meshlet: vertex pairs (nv vertices) and its encoded strip. v and idx
 * are 8-byte aligned: the cart's RSP DMAs both and drops the address's
 * low 3 bits (a shifted strip draws wrong triangles and can run past its
 * end: an RSP hang). The host build fails on a misaligned call. */
void	gpu_draw(const w64_vert_t *v, int nv, const int16_t *idx, int ni);
/* Vertex bytes the CPU rewrote (recolour, dlight scratch). */
void	gpu_writeback(const void *p, uint32_t n);

/* Last completed frame as drawn: binds, meshlets, vertices, triangles. */
typedef struct {
	uint32_t	binds, meshlets, verts, tris;
	uint32_t	hash;		/* host: FNV-1a of the draw list; cart 0 */
} gpu_stats_t;
void	gpu_stats(gpu_stats_t *s);

#endif
