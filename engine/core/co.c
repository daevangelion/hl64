/* SPDX-License-Identifier: MIT */
/*
 * co.c -- cooperative contexts (include/hl64/co.h) in the boot carve's
 * aithink region: per context [stack HL64_AI_STACK_BYTES][scratch
 * HL64_AI_SCRATCH_BYTES]. Stacks are painted once (first co_start) so
 * co_stack_peak measures the deepest use; their lowest CO_GUARD bytes
 * are checked after every co_resume (overflow: fatal), and the first use
 * past 3/4 of a stack logs a `co: ... past 3/4` line (once per context).
 */
#include <stdint.h>

#include "hl64/co.h"
#include "hl64/limits.h"
#include "engine/core/log.h"
#include "engine/core/sys.h"
#include "engine/mem/carve.h"

#define CO_PAINT	0xc0ffee11u
#define CO_GUARD	64
#define CO_HIGH		(HL64_AI_STACK_BYTES / 4)	/* bytes left at 3/4 use */

typedef char co_stack_aligned[(HL64_AI_STACK_BYTES % 16) == 0 && (HL64_AI_SCRATCH_BYTES % 16) == 0 ? 1 : -1];

static sys_co_t	co_ctx[HL64_AI_CONTEXTS];
static int	co_cur = -1, co_painted;
static uint8_t	co_high_logged[HL64_AI_CONTEXTS];

static uint8_t *co_base(int i)
{
	uint8_t *r = mem_region(MEM_AI_THINK);

	if ((uint32_t)HL64_AI_CONTEXTS * (HL64_AI_STACK_BYTES + HL64_AI_SCRATCH_BYTES) >
		mem_region_bytes(MEM_AI_THINK))
		sys_error("co: aithink region %lu B < %d contexts x %d B",
			(unsigned long)mem_region_bytes(MEM_AI_THINK), HL64_AI_CONTEXTS,
			HL64_AI_STACK_BYTES + HL64_AI_SCRATCH_BYTES);
	return r + (uint32_t)i * (HL64_AI_STACK_BYTES + HL64_AI_SCRATCH_BYTES);
}

void *co_scratch(int i)
{
	return co_base(i) + HL64_AI_STACK_BYTES;
}

void co_start(int i, void (*entry)(void))
{
	uint32_t *p;
	int k, n;

	if (co_cur >= 0)
		sys_error("co_start(%d) inside context %d", i, co_cur);
	if (!co_painted)
	{
		for (k = 0; k < HL64_AI_CONTEXTS; k++)
		{
			p = (uint32_t *)(void *)co_base(k);
			for (n = 0; n < HL64_AI_STACK_BYTES / 4; n++)
				p[n] = CO_PAINT;
		}
		co_painted = 1;
	}
	sys_co_init(&co_ctx[i], co_base(i), HL64_AI_STACK_BYTES, entry);
}

void co_resume(int i)
{
	const uint32_t *p = (const uint32_t *)(void *)co_base(i);
	int n;

	if (co_cur >= 0)
		sys_error("co_resume(%d) inside context %d", i, co_cur);
	co_cur = i;
	sys_co_resume(&co_ctx[i]);
	co_cur = -1;
	for (n = 0; n < CO_GUARD / 4; n++)
		if (p[n] != CO_PAINT)
			sys_error("co: context %d overflowed its %d B stack", i, HL64_AI_STACK_BYTES);
	if (!co_high_logged[i] && p[CO_HIGH / 4 - 1] != CO_PAINT)
	{
		co_high_logged[i] = 1;
		log_printf("co: context %d stack past 3/4 of %d B (peak %lu B)\n", i,
			HL64_AI_STACK_BYTES, (unsigned long)co_stack_peak(i));
	}
}

void co_yield(void)
{
	if (co_cur < 0)
		sys_error("co_yield on the main stack");
	sys_co_yield(&co_ctx[co_cur]);
}

int co_running(void)
{
	return co_cur;
}

uint32_t co_stack_peak(int i)
{
	const uint32_t *p = (const uint32_t *)(void *)co_base(i);
	uint32_t n = 0;

	if (!co_painted)
		return 0;
	while (n < HL64_AI_STACK_BYTES / 4 && p[n] == CO_PAINT)
		n++;
	return HL64_AI_STACK_BYTES - n * 4;
}
