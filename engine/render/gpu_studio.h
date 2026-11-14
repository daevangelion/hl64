/* SPDX-License-Identifier: MIT */
/*
 * gpu_studio.h -- the studio renderer's platform seam (docs/architecture.md
 * 9.4; engine/render/studio.c). plat/n64/gpu_studio.c records each cooked
 * mesh run (docs/formats/studio-format.md "Draw mesh") once into an rspq
 * block that loads every bone run under its matrix slot (tiny3d segment
 * placeholder) and draws the meshlet's triangles; per entity it writes the
 * bone matrices into a double-buffered frame pool, binds each run's skin
 * and replays the run's block. plat/host/gpu_studio.c is the draw-list
 * recorder: it counts and hashes what would be drawn (D5).
 *
 * Calls happen between gpu_begin and gpu_end (engine/render/gpu.h).
 */
#ifndef HL64_GPU_STUDIO_H
#define HL64_GPU_STUDIO_H

#include <stdint.h>

#include "hl64/studio_fmt.h"
#include "engine/render/gpu.h"

/* Studio render modes the platform blends (render-fx.md 2 "studio"). */
#define GPU_STUDIO_OPAQUE	0
#define GPU_STUDIO_BLEND	1	/* alpha = rgba[3] (TransTexture / TransColor) */
#define GPU_STUDIO_ADD		2	/* additive x rgba[3] (Glow / TransAdd): page ramps,
					 * tint rgba[0..2] (render-fx.md 2a) */

/* Boot: the matrix frame pools (bytes from the studio scratch region;
 * the platform keeps two halves, one per frame in flight). Returns the
 * matrix slots per frame. */
int		gpu_studio_init(void *mem, uint32_t bytes);
/* Bytes of per-model platform state for a resident (native-order) mesh. */
uint32_t	gpu_studio_state_size(const sfmt_mesh_t *mesh);
/* Record the mesh's runs into state (once per residency); 0 ok, -1 when
 * the platform cannot (out of rspq memory: the model is not drawn). */
int		gpu_studio_build(void *state, const sfmt_mesh_t *mesh);
/* Drop what gpu_studio_build made; the caller guarantees the GPU no
 * longer reads it (two-frame fence). */
void		gpu_studio_release(void *state);

typedef struct {
	ren_mat_t		model;		/* model space -> camera-relative */
	const float		(*bones)[3][4];	/* per matrix slot, model space;
						 * rotation pre-scaled by 2^-pos_shift */
	int			nbones;
	const uint16_t		*runs;		/* run indices to draw */
	int			nruns;
	const uint16_t		*tex;		/* texture of each drawn run */
	uint8_t			rgba[4];	/* light (rgb) and blend (a) */
	uint8_t			mode;		/* GPU_STUDIO_* */
	uint8_t			pad[3];
} gpu_studio_ent_t;

/* Once per frame before any studio draw: flips the matrix pool halves,
 * resets the frame's counters. */
void		gpu_studio_frame(void);
/* Around each group of studio draws (render mode state; the world's state
 * is restored by gpu_studio_end). */
void		gpu_studio_begin(void);
void		gpu_studio_end(void);
/* One entity; 0 drawn, -1 when the frame's matrix pool is full. */
int		gpu_studio_draw(void *state, const sfmt_mesh_t *mesh, const gpu_studio_ent_t *ent);

/* CPU-skinned draw (view model): ready vertex pairs (camera-relative, at
 * 2^-pos_shift scale under `model`) replace the mesh's packed vertices;
 * meshlets of the listed runs are loaded and drawn without bone
 * matrices, in the view model's depth range: the front of the depth
 * buffer's range, before every world surface not right at the eye. */
int		gpu_studio_draw_cpu(void *state, const sfmt_mesh_t *mesh, const w64_vert_t *verts,
			const gpu_studio_ent_t *ent);

typedef struct {
	uint32_t	ents, runs, meshlets, verts, tris, mats;
	uint32_t	hash;		/* host: FNV-1a of the studio draw list; cart 0 */
} gpu_studio_stats_t;
/* Last completed frame. */
void		gpu_studio_stats(gpu_studio_stats_t *s);

#endif
