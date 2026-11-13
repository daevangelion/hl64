/* SPDX-License-Identifier: MIT */
/*
 * sys.c -- host system services (engine/core/sys.h) under the cart's
 * rules (docs/architecture.md 3.4): the VI is a counter that every
 * submitted frame advances by HL64_PRESENT_VBL, so the host runs the
 * cart's paced 30 fps timeline deterministically; the carve block comes
 * from outside the heap arena, as on the cart it comes before the heap
 * that is left. There is no framebuffer (the draw-list recorder, D5,
 * arrives with the renderer).
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "hl64/limits.h"
#include "engine/core/log.h"
#include "engine/core/sys.h"

/* NTSC VI rate 59.826 Hz, the cart's "vid: vi period" boot line */
#define HOST_VI_PERIOD_US	16715u
#define PACE_BINS		8

static uint32_t host_vi, pace_vbl_mark, pace_frames;

void sys_error(const char *fmt, ...)
{
	va_list ap;

	log_printf("error: ");
	va_start(ap, fmt);
	log_vprintf(fmt, ap);
	va_end(ap);
	log_printf("\n");
	log_flush();
	fflush(stdout);
	exit(SYS_EXIT_ERROR);
}

void sys_exit(int code)
{
	log_printf("exit %d\n", code);
	log_flush();
	fflush(stdout);
	exit(code);
}

uint64_t sys_time_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

void sys_log_write(const char *s, uint32_t n)
{
	fwrite(s, 1, n, stdout);
}

uint32_t sys_fpu_traps(void)
{
	return 0;
}

/* ---- video model ---------------------------------------------------------- */

uint32_t sys_vi_count(void)
{
	return host_vi;
}

uint32_t sys_vi_period_us(void)
{
	return HOST_VI_PERIOD_US;
}

int sys_frame_try_begin(void)
{
	return 1;
}

void sys_frame_end(void)
{
	host_vi += HL64_PRESENT_VBL;
	pace_frames++;
}

/* Same line as the cart's; every host frame is a HL64_PRESENT_VBL interval. */
int sys_pace_stats(char *buf, int size)
{
	uint32_t vbl = host_vi - pace_vbl_mark, h[PACE_BINS] = { 0 };
	int len;

	h[HL64_PRESENT_VBL < PACE_BINS ? HL64_PRESENT_VBL : PACE_BINS - 1] = pace_frames;
	len = snprintf(buf, (size_t)size, "pace vbl=%lu fps=%.1f v1..v%d+=%lu/%lu/%lu/%lu/%lu/%lu/%lu",
		(unsigned long)vbl, vbl ? pace_frames * 1e6 / ((double)vbl * HOST_VI_PERIOD_US) : 0.0,
		PACE_BINS - 1, (unsigned long)h[1], (unsigned long)h[2], (unsigned long)h[3],
		(unsigned long)h[4], (unsigned long)h[5], (unsigned long)h[6],
		(unsigned long)h[7]);
	pace_vbl_mark = host_vi;
	pace_frames = 0;
	return len < size ? len : size - 1;
}

int sys_shot(void)
{
	return -1;
}

/* ---- memory ---------------------------------------------------------------- */

void *sys_carve_alloc(uint32_t bytes)
{
	void *p;

	return posix_memalign(&p, 16, bytes) ? NULL : p;
}

uint32_t sys_image_bytes(void)
{
	return 0;
}

int sys_stack_peak(void)
{
	return -1;
}

void sys_cache_writeback(const void *p, uint32_t n)
{
	(void)p;
	(void)n;
}
