/* SPDX-License-Identifier: MIT */
/*
 * n64.h -- functions shared between the plat/n64 files (boot, video,
 * cart, FPU shim, PROFILE counters). The engine sees only engine/core/sys.h.
 */
#ifndef HL64_PLAT_N64_H
#define HL64_PLAT_N64_H

#include <stdint.h>

/* Face culling of every 3D pass (world, movers, effects, studio models):
 * GoldSrc front faces -- BSP faces and studio triangles alike -- wind
 * clockwise seen from outside (tools/studio/test_studiocook.py
 * test_triangles_wind_like_the_world), which is tiny3d's front side.
 * Expands to a T3D_FLAG_* value: include <t3d/t3d.h> where used. */
#define N64_CULL	T3D_FLAG_CULL_FRONT

/* Colour dithering of every 3D pass (world, movers, effects, studio):
 * a lit texel (texture x grey vertex light) truncated per channel to the
 * 16-bit framebuffer's 5 bits loses up to 7/255 of each channel
 * independently, a hue shift that grows as the light darkens and bands
 * light gradients in steps of different colour. The RDP's magic-square
 * pattern is the one the VI dedither filter (main.c display_init,
 * FILTERS_RESAMPLE_ANTIALIAS_DEDITHER) folds back to ~8 bits per channel;
 * alpha stays undithered so alpha-compare cutouts keep exact edges.
 * N64_DITHER_MODE() is the mode the passes set: N64_DITHER, or none
 * while settings.r_dither is 0 (the M1 gate's old-cart comparison).
 * Expand to an rdpq_dither_t: include <libdragon.h> and "hl64/settings.h"
 * where used. */
#define N64_DITHER	DITHER_SQUARE_NONE
#define N64_DITHER_MODE()	(settings.r_dither ? N64_DITHER : DITHER_NONE_NONE)

/* fpu_shim.c */
void		fpu_shim_install(void);
unsigned long	fpu_shim_trap_count(void);

/* sys.c: paint the unused stack once at boot (sys_stack_peak). */
void		sys_stack_paint(void);

/* vid.c */
void		vid_init(void);
/* Detach and show a frame left attached (error screen path). */
void		vid_abort_frame(void);
/* The last submitted frame's RDP work is not finished yet. */
int		vid_gpu_busy(void);

/* gpu.c: this frame's projection (gpu_begin) for the following matrix
 * pushes: view 0 the world's, 1 the view model's (gpu_studio_ent_t.view),
 * whose depth fills only the front of the depth range (gpu.c VIEW_DEPTH). */
void		gpu_view_proj(int view);
/* gpu.c: this frame is night vision (gpu_view_t.nvg, engine/render/gpu.h). */
int		gpu_nvg(void);
/* gpu.c: the world pass's combiner, colours, fog and tiny3d flags, set by
 * gpu_begin and put back by the studio and effects passes when they end:
 * texel x vertex shade, or under night vision N64_CC_NVG with the
 * GPU_NVG_* colours and the RDP / tiny3d distance fog to black. */
void		gpu_world_state(void);
/* Night vision: rgb = texel x PRIM + ENV, alpha = the texel's (the world's
 * fence cutouts; shade alpha carries the fog), one cycle. The studio form
 * keeps the entity's PRIM alpha (its blend) as CC_TEX_PRIM does. Expand
 * to an rdpq_combiner_t: include <libdragon.h> where used. */
#define N64_CC_NVG	RDPQ_COMBINER1((TEX0, 0, PRIM, ENV), (0, 0, 0, TEX0))
#define N64_CC_NVG_STUDIO	RDPQ_COMBINER1((TEX0, 0, PRIM, ENV), (TEX0, 0, PRIM, 0))

/* cart.c: mount the ROM's DFS (player content) once at boot; without one
 * (the M0 gate's ROM) sys_cart_addr finds no file. */
void		cart_mount(void);

/* iostat.c (PROFILE=1) */
void		iostat_note_dma(unsigned bytes, uint32_t ticks);
/* audio.c (PROFILE=1): mixer pumps since the last call, split by whether
 * the last frame's RDP work was still running (vid_gpu_busy): calls,
 * ticks; then zeroed */
void		audio_pump_take(unsigned n[2], uint64_t ticks[2]);
/* psamp.c (PSAMP=1) */
void		psamp_init(void);
void		psamp_dump(void);

#endif
