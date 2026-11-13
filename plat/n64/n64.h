/* SPDX-License-Identifier: MIT */
/*
 * n64.h -- functions shared between the plat/n64 files (boot, video,
 * cart, FPU shim, PROFILE counters). The engine sees only engine/core/sys.h.
 */
#ifndef HL64_PLAT_N64_H
#define HL64_PLAT_N64_H

#include <stdint.h>

/* fpu_shim.c */
void		fpu_shim_install(void);
unsigned long	fpu_shim_trap_count(void);

/* sys.c: paint the unused stack once at boot (sys_stack_peak). */
void		sys_stack_paint(void);

/* vid.c */
void		vid_init(void);
/* Detach and show a frame left attached (error screen path). */
void		vid_abort_frame(void);

/* iostat.c (PROFILE=1) */
void		iostat_note_dma(unsigned bytes, uint32_t ticks);

#endif
