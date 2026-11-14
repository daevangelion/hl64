/* SPDX-License-Identifier: MIT */
/*
 * iostat.c -- PROFILE-only cart I/O counters, imported from the old
 * tree's n64/port/r64_iostat.c (audit entry CLEAN): stdio fread / fseek
 * (ld --wrap, PROFILE builds only) and file DMA (sys_cart_read) call
 * counts, bytes and CPU-blocked ticks, and the worst per-frame I/O stall.
 * The old per-item working-set touch counters followed the old world
 * format and are not imported. Logged once per second as "io ...".
 * Linked only in PROFILE=1 builds (Makefile).
 */
#include <stdio.h>

#include <libdragon.h>

#include "engine/core/log.h"
#include "engine/core/sys.h"
#include "plat/n64/n64.h"

static uint64_t io_rd_ticks, io_sk_ticks, io_dma_ticks;
static unsigned io_rd_calls, io_sk_calls, io_dma_calls;
static uint64_t io_rd_bytes, io_dma_bytes;
static uint64_t io_frame_ticks, io_frame_max;
static unsigned io_frames_over4ms;

void iostat_note_dma(unsigned bytes, uint32_t ticks)
{
	io_dma_calls++;
	io_dma_bytes += bytes;
	io_dma_ticks += ticks;
	io_frame_ticks += ticks;
}

size_t __real_fread(void *ptr, size_t size, size_t n, FILE *f);
int __real_fseek(FILE *f, long off, int whence);
size_t __wrap_fread(void *ptr, size_t size, size_t n, FILE *f);
int __wrap_fseek(FILE *f, long off, int whence);

size_t __wrap_fread(void *ptr, size_t size, size_t n, FILE *f)
{
	uint32_t t0 = TICKS_READ();
	size_t got = __real_fread(ptr, size, n, f);
	uint32_t dt = (uint32_t)TICKS_DISTANCE(t0, TICKS_READ());

	io_rd_calls++;
	io_rd_bytes += (uint64_t)got * size;
	io_rd_ticks += dt;
	io_frame_ticks += dt;
	return got;
}

int __wrap_fseek(FILE *f, long off, int whence)
{
	uint32_t t0 = TICKS_READ();
	int r = __real_fseek(f, off, whence);
	uint32_t dt = (uint32_t)TICKS_DISTANCE(t0, TICKS_READ());

	io_sk_calls++;
	io_sk_ticks += dt;
	io_frame_ticks += dt;
	return r;
}

void sys_prof_frame(void)
{
	if (io_frame_ticks > io_frame_max)
		io_frame_max = io_frame_ticks;
	if (io_frame_ticks > (uint64_t)TICKS_FROM_MS(4))
		io_frames_over4ms++;
	io_frame_ticks = 0;
}

static unsigned us_of(uint64_t t, unsigned n)
{
	if (n < 1)
		n = 1;
	return (unsigned)(t * 1000000ull / TICKS_PER_SECOND / n);
}

/* libdragon's time accounting (accounting.c): ticks spent in its
 * spin-waits (RSP, display, rspq buffers / syncs, VI); its IRQ bucket is
 * not fed by this libdragon */
uint64_t acct_get_ticks(int cat);
enum { ACCT_RSP = 2, ACCT_DISP = 3, ACCT_RSPQ = 4, ACCT_VI = 5, ACCT_N = 6 };
static uint64_t acct_mark[ACCT_N];

static unsigned acct_us(int cat, unsigned n)
{
	uint64_t t = acct_get_ticks(cat), d = t - acct_mark[cat];

	acct_mark[cat] = t;
	return us_of(d, n);
}

void sys_prof_dump(unsigned nframes)
{
	unsigned nf = nframes ? nframes : 1;

	log_printf("io rd=%u/f %uB/f %uus/f seek=%u/f %uus/f "
		"dma=%u/f %uB/f %uus/f frame_max=%uus over4ms=%u\n",
		io_rd_calls / nf, (unsigned)(io_rd_bytes / nf), us_of(io_rd_ticks, nf),
		io_sk_calls / nf, us_of(io_sk_ticks, nf),
		io_dma_calls / nf, (unsigned)(io_dma_bytes / nf), us_of(io_dma_ticks, nf),
		us_of(io_frame_max, 1), io_frames_over4ms);
	log_printf("acct rsp=%uus/f disp=%uus/f rspq=%uus/f vi=%uus/f\n",
		acct_us(ACCT_RSP, nf), acct_us(ACCT_DISP, nf),
		acct_us(ACCT_RSPQ, nf), acct_us(ACCT_VI, nf));
	{
		unsigned n[2];
		uint64_t t[2];

		audio_pump_take(n, t);
		/* the mixer pump (mixer_try_play) with the last frame's RDP work
		 * still running (busy) or finished (idle): pumps, us per frame */
		log_printf("acct pump_busy=%u pump_busy_us=%u pump_idle=%u pump_idle_us=%u\n",
			n[1], us_of(t[1], nf), n[0], us_of(t[0], nf));
	}
	io_rd_calls = io_sk_calls = io_dma_calls = 0;
	io_rd_bytes = io_dma_bytes = 0;
	io_rd_ticks = io_sk_ticks = io_dma_ticks = 0;
	io_frame_max = 0;
	io_frames_over4ms = 0;
}
