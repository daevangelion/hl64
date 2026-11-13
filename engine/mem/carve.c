/* SPDX-License-Identifier: MIT */
/*
 * carve.c -- the boot carve (carve.h). Sizes are whole KB, so with a
 * 16-byte aligned block every region starts on a cache line and a DMA
 * into one region never shares a line with its neighbour (4.3).
 */
#include "hl64/limits.h"
#include "engine/core/log.h"
#include "engine/core/sys.h"
#include "engine/mem/carve.h"
#include "engine/mem/heap.h"

typedef struct {
	const char	*name;
	uint16_t	kb;
	uint8_t		est;
} region_desc_t;

static const region_desc_t regions[MEM_REGION_COUNT] = {
#define MEM_REGION_DESC(id, name, kb, est) { name, kb, est },
	MEM_REGIONS(MEM_REGION_DESC)
#undef MEM_REGION_DESC
};

static uint8_t	*base;
static uint32_t	offset_kb[MEM_REGION_COUNT];
static uint32_t	total_kb;

void mem_carve_log(void)
{
	int i;

	log_printf("carve: %-13s %6s %6s\n", "region", "off KB", "KB");
	for (i = 0; i < MEM_REGION_COUNT; i++)
		log_printf("carve: %-13s %6lu %6u%s\n", regions[i].name,
			(unsigned long)offset_kb[i], regions[i].kb,
			regions[i].est ? " est" : "");
	log_printf("carve: total %lu KB in %d regions\n",
		(unsigned long)total_kb, MEM_REGION_COUNT);
}

void mem_boot_carve(void)
{
	uint32_t free_b, free_kb, largest_kb, image;
	int i;

	total_kb = 0;
	for (i = 0; i < MEM_REGION_COUNT; i++)
	{
		offset_kb[i] = total_kb;
		total_kb += regions[i].kb;
	}
	base = sys_carve_alloc(total_kb * 1024u);
	if (!base)
		sys_error("carve: %lu KB do not fit the heap", (unsigned long)total_kb);
	mem_carve_log();

	free_b = (uint32_t)mem_heap_free_total();
	free_kb = free_b / 1024u;
	largest_kb = (uint32_t)(mem_heap_probe_largest(HL64_RDRAM_BYTES) / 1024u);
	log_printf("mem: heap free %lu KB largest %lu KB (min %u) free %lu B\n",
		(unsigned long)free_kb, (unsigned long)largest_kb, HL64_HEAP_MIN_KB,
		(unsigned long)free_b);
	image = sys_image_bytes();
	if (image)
		log_printf("mem: image %lu KB (budget %u)\n",
			(unsigned long)((image + 1023u) / 1024u), HL64_IMAGE_BUDGET_KB);
	if (free_kb < HL64_HEAP_MIN_KB)
		sys_error("carve: heap left %lu KB < HL64_HEAP_MIN_KB %u",
			(unsigned long)free_kb, HL64_HEAP_MIN_KB);
}

void *mem_region(int id)
{
	return base + offset_kb[id] * 1024u;
}

uint32_t mem_region_bytes(int id)
{
	return (uint32_t)regions[id].kb * 1024u;
}
