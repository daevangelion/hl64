/* SPDX-License-Identifier: MIT */
/*
 * cart.c -- cart (ROM) reads by PI DMA: the loose-file DMA of the old
 * tree's n64/port/r64_cart.c (audit entry CLEAN; its r64_local.h /
 * qboolean binding replaced by sys.h) and the PI backend of its
 * n64/port/q64_io.c (audit entry CLEAN) behind sys.h's sys_pi_*.
 *
 * The file DMA path was proved out in our OpenLara N64 port
 * (room_stream.cpp, level_prefetch.cpp; the N64 platform code there is
 * ours). Same discipline:
 *
 *  - dfs_rom_addr() gives the PI address of a loose DFS file; DMA goes
 *    straight from cart to RDRAM, no stdio buffering or copies.
 *
 *  - dma_read requires the RAM and PI addresses to share their low bit.
 *    That precondition is CHECKED, never assumed: violating it silently
 *    corrupts RDRAM rather than crashing, so a failed check returns 0.
 *
 *  - The destination is invalidated *before* the DMA, not after: the
 *    buffer may still hold dirty cache lines from whatever previously
 *    owned that memory, and those would be written back over the DMA'd
 *    bytes. Uncached (KSEG1) destinations need no cache op at all.
 */
#include <string.h>

#include <libdragon.h>

#include "engine/core/log.h"
#include "engine/core/sys.h"
#include "plat/n64/n64.h"

/* libdragon's DFS lookups dereference its file table unchecked: with no
 * DFS mounted, dfs_rom_addr reads through NULL. */
static int cart_dfs;

void cart_mount(void)
{
	cart_dfs = dfs_init(DFS_DEFAULT_LOCATION) == DFS_ESUCCESS;
	if (!cart_dfs)
		log_printf("io: no DFS in this ROM\n");
}

/* DFS wants a root-relative path; "rom:/x" and "x" name the same file. */
static const char *cart_dfs_path(const char *path)
{
	if (path && !strncmp(path, "rom:/", 5))
		return path + 5;
	return path;
}

uint32_t sys_cart_addr(const char *path, uint32_t *out_size)
{
	const char *rel = cart_dfs_path(path);
	pi_addr_t rom;
	int sz;

	if (out_size)
		*out_size = 0;
	if (!cart_dfs || !rel || !rel[0])
		return 0;
	rom = dfs_rom_addr(rel);
	if (!rom)
		return 0;
	sz = dfs_rom_size(rel);
	if (sz <= 0)
		return 0;
	if (out_size)
		*out_size = (uint32_t)sz;
	return rom;
}

int sys_cart_read(void *dst, size_t dst_cap, uint32_t rom, uint32_t rom_size,
	uint32_t off, uint32_t len)
{
	pi_addr_t src;
	uint32_t dma_len;

	if (!dst || !rom || len == 0)
		return 0;
	if ((uint64_t)off + (uint64_t)len > (uint64_t)rom_size)
		return 0;

	/* PI moves whole halfwords; round up and make sure that still fits. */
	dma_len = (len + 1u) & ~1u;
	if ((size_t)dma_len > dst_cap)
		return 0;

	src = rom + off;
	/* Precondition, not an assumption -- see file header. */
	if (((uintptr_t)dst | (uintptr_t)src | (uintptr_t)dma_len) & 1u)
		return 0;

	if ((((uintptr_t)dst) & 0x20000000u) == 0)
		data_cache_hit_writeback_invalidate(dst, dma_len);
#ifdef HL64_PROFILE
	{
		uint32_t t0 = TICKS_READ();

		dma_read(dst, src, dma_len);
		iostat_note_dma(dma_len, (uint32_t)TICKS_DISTANCE(t0, TICKS_READ()));
	}
#else
	dma_read(dst, src, dma_len);
#endif
	return 1;
}

/* ---- engine/io backend: one PI piece at a time -------------------------- */

static uint32_t pi_addr(uint32_t rom)
{
	return (rom | 0x10000000u) & 0x1FFFFFFFu;
}

void sys_pi_launch(void *dst, uint32_t rom, uint32_t len)
{
	/* invalidate before, not after: stale dirty lines would land on the DMA */
	data_cache_hit_writeback_invalidate(dst, (len + 15) & ~15u);
	dma_read_raw_async(dst, pi_addr(rom), len);
}

int sys_pi_busy(void)
{
	return (*PI_STATUS & (PI_STATUS_DMA_BUSY | PI_STATUS_IO_BUSY)) != 0;
}

uint32_t sys_pi_wait(int audio)
{
	uint32_t t0;

	(void)audio;
	if (!sys_pi_busy())
		return 0;
	t0 = TICKS_READ();
	dma_wait();
	return (uint32_t)TICKS_TO_US(TICKS_DISTANCE(t0, TICKS_READ()));
}

uint32_t sys_pi_read(void *dst, uint32_t rom, uint32_t len)
{
	uint32_t t0 = TICKS_READ();

	data_cache_hit_writeback_invalidate(dst, (len + 15) & ~15u);
	dma_read_raw_async(dst, pi_addr(rom), len);
	dma_wait();
	return (uint32_t)TICKS_TO_US(TICKS_DISTANCE(t0, TICKS_READ()));
}

uint32_t sys_pi_cost_us(uint32_t len)
{
	(void)len;
	return 0;
}
