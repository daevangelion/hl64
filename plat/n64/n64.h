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

/* fpu_shim.c */
void		fpu_shim_install(void);
unsigned long	fpu_shim_trap_count(void);

/* sys.c: paint the unused stack once at boot (sys_stack_peak). */
void		sys_stack_paint(void);

/* vid.c */
void		vid_init(void);
/* Detach and show a frame left attached (error screen path). */
void		vid_abort_frame(void);

/* cart.c: mount the ROM's DFS (player content) once at boot; without one
 * (the M0 gate's ROM) sys_cart_addr finds no file. */
void		cart_mount(void);

/* iostat.c (PROFILE=1) */
void		iostat_note_dma(unsigned bytes, uint32_t ticks);

#endif
