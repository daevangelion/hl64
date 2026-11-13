/* SPDX-License-Identifier: MIT */
/*
 * gpu_2d.h -- the 2D renderer's platform seam (engine/render/draw2d.c,
 * docs/architecture.md 9.8). Sheets are I4 coverage in the HUD region
 * (two texels a byte, high nibble first, `stride` bytes a row);
 * plat/n64/gpu_2d.c blits them through rdpq tinted by a primitive
 * colour, plat/host/gpu_2d.c records the calls (D5).
 *
 * Calls happen after the 3D frame (ren_frame) and before sys_frame_end.
 */
#ifndef HL64_GPU_2D_H
#define HL64_GPU_2D_H

#include <stdint.h>

/* Register sheet id over resident I4 texels (once, at boot). */
void	gpu_2d_sheet(int id, const uint8_t *i4, int w, int h, int stride);
void	gpu_2d_begin(void);
void	gpu_2d_end(void);
/* mode: draw2d.h D2_* */
void	gpu_2d_pic(int id, int sx, int sy, int w, int h, int x, int y, const uint8_t rgba[4],
		int mode);
void	gpu_2d_fill(int x, int y, int w, int h, const uint8_t rgba[4], int mode);

typedef struct {
	uint32_t	pics, fills, texels;	/* last completed frame */
	uint32_t	hash;			/* host: FNV-1a of the 2D list; cart 0 */
} gpu_2d_stats_t;
void	gpu_2d_stats(gpu_2d_stats_t *s);

#endif
