/* SPDX-License-Identifier: MIT */
/*
 * psamp.c -- PC-sampling profiler, cart builds with PSAMP=1 only (M6
 * render/CPU work; architecture.md 15). A libdragon timer interrupts the
 * CPU every PSAMP_US us; the interrupted program counter (the EPC saved in
 * libdragon's interrupt frame) is counted in 64-byte buckets of the text
 * section. Each SHOT (sys_shot) logs the segment since the previous one as
 * `psamp <seg> <addr> <count>` lines (non-zero buckets) after a
 * `psamp <seg> samples=<n>` line and starts the next segment;
 * test/m6/psamp.py maps the buckets onto the ELF's symbols.
 */
#include <libdragon.h>

#include "engine/core/log.h"
#include "engine/core/sys.h"
#include "plat/n64/n64.h"

#define PSAMP_US	1000
#define BUCKET_SHIFT	6
#define MAX_BUCKETS	20480		/* 1.25 MB of text */

extern char __text_start[], __text_end[];
extern void *interrupt_exception_frame;

static uint16_t		hist[MAX_BUCKETS];
static uint32_t		nbuckets, samples, outside, seg;
static timer_link_t	*timer;

static void tick(int ovfl)
{
	const reg_block_t *r = (const reg_block_t *)((uint8_t *)interrupt_exception_frame + 32);
	uint32_t b = (r->epc - (uint32_t)__text_start) >> BUCKET_SHIFT;

	(void)ovfl;
	samples++;
	if (b < nbuckets)
	{
		if (hist[b] != 0xFFFF)
			hist[b]++;
	}
	else
		outside++;
}

void psamp_init(void)
{
	nbuckets = ((uint32_t)(__text_end - __text_start) + (1u << BUCKET_SHIFT) - 1) >> BUCKET_SHIFT;
	if (nbuckets > MAX_BUCKETS)
		sys_error("psamp: text %lu B over %u buckets", (unsigned long)(__text_end - __text_start),
			MAX_BUCKETS);
	timer = new_timer(TICKS_FROM_US(PSAMP_US), TF_CONTINUOUS, tick);
	log_printf("psamp: %u us, text %08lx, %lu buckets of %u B\n", PSAMP_US,
		(unsigned long)(uint32_t)__text_start, (unsigned long)nbuckets, 1u << BUCKET_SHIFT);
}

void psamp_dump(void)
{
	uint32_t i, n = 0;

	stop_timer(timer);
	log_printf("psamp %lu samples=%lu outside=%lu\n", (unsigned long)seg, (unsigned long)samples,
		(unsigned long)outside);
	log_flush();
	for (i = 0; i < nbuckets; i++)
	{
		if (!hist[i])
			continue;
		log_printf("psamp %lu %08lx %u\n", (unsigned long)seg,
			(unsigned long)((uint32_t)__text_start + (i << BUCKET_SHIFT)), hist[i]);
		hist[i] = 0;
		if (++n % 64 == 0)
			log_flush();
	}
	log_flush();
	samples = outside = 0;
	seg++;
	restart_timer(timer);
}
