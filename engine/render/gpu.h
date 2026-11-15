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
} gpu_view_t;

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
/* Alpha-compare threshold of fence pages (W64_TEXF_FENCE: TLUT[0] alpha 0,
 * the rest 1, bilinear-filtered): GPU_FENCE_REF, half coverage, while the
 * combiner's alpha is the texel's alpha x 255 (the world, TransAlpha
 * brushes, Normal studio models), so a pixel draws only where the opaque
 * texels outweigh the transparent slot; a caller whose combiner scales the
 * texel alpha by a constant k sets (k + 1) / 2, or 1 when the texel alpha
 * does not take part (clamped to 1..255). gpu_begin resets it; a change
 * applies at once to a bound fence page. */
#define GPU_FENCE_REF	128
void	gpu_fence_ref(int ref);
/* Additive pages (render-fx.md 2a: brush and studio TransAdd / Glow):
 * tint_blend = rgb tint and blend; until gpu_page_ramp(NULL) every
 * gpu_bind_page also loads its page's cooked IA16 ramp TLUT (addblend.h
 * add_page_ramp) and sets PRIM / ENV to its ends for the ramp combiner
 * the caller set; a page the cook gave no ramp draws with blend 0
 * (nothing added; add_stats counts it). */
void	gpu_page_ramp(const uint8_t tint_blend[4]);
/* Decal depth for the following draws (on: Z test against the coplanar
 * base, no Z write; off: the world's opaque state). */
void	gpu_decal_depth(int on);
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
