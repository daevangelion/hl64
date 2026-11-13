/* SPDX-License-Identifier: MIT */
/*
 * pool.h -- residency pools (render / texture / collision / studio),
 * imported from the old tree's q64_pool.h (audit entry source/q64_pool.h
 * CLEAN).
 *
 * One fixed arena per pool, a region of the boot carve (sizes:
 * include/hl64/limits.h HL64_POOL_*_KB, identical on the cart and the
 * host). Variable-size first-fit over an address-ordered block list,
 * 16-byte granularity (whole VR4300 data-cache lines, so a PI DMA into
 * one block never shares a line with its neighbour). Measured why not
 * size-class slabs: power-of-two classes round the cooked REND chunks
 * (2-16 KB) up to 1.4x the worst leaf open set (stream-arch-v2 4.3).
 *
 * Blocks the RSP/RDP may still read are freed with a fence frame and only
 * returned to the free list once that frame has retired (mem_pool_retire).
 * Chunks are position-independent (offsets only), so live blocks can slide
 * (mem_pool_pack): a range's live blocks are packed down and each owner
 * pointer rewritten. The residency manager uses that only when making room
 * for a chunk cannot be done by eviction alone.
 */
#ifndef HL64_POOL_H
#define HL64_POOL_H

#include <stdint.h>

typedef struct mem_blk_s
{
	struct mem_blk_s	*prev, *next;	/* address order, used and free */
	struct mem_blk_s	*fprev, *fnext;	/* address order, free blocks only */
	uint32_t		ofs, size;
	uint32_t		fence;		/* frame the block may be reused after */
	uint8_t			state;		/* MEM_BLK_* */
	uint8_t			locked;		/* DMA in flight: never moved */
	void			**owner;	/* rewritten when the block slides */
	int32_t			user;		/* caller's tag (residency: layer/chunk key) */
	uint32_t		memo[2];	/* caller's cache (residency: its make-room
						 * classification of the block) */
} mem_blk_t;

enum { MEM_BLK_FREE, MEM_BLK_USED, MEM_BLK_FENCED, MEM_BLK_SPARE };

typedef struct
{
	const char	*name;
	uint8_t		*base;
	uint32_t	size;
	mem_blk_t	*desc;		/* descriptor storage */
	int		ndesc;
	mem_blk_t	*head;		/* address-ordered list */
	mem_blk_t	*fhead;		/* address-ordered free blocks */
	mem_blk_t	*spare;		/* unused descriptors */
	uint32_t	used;		/* live + fenced bytes */
	uint32_t	fenced;
	uint32_t	peak;
	uint32_t	compactions;
	uint32_t	moved;		/* bytes slid by mem_pool_pack (cumulative) */
	uint32_t	nofit;		/* smallest size known to find no hole (until one grows) */
} mem_pool_t;

#define MEM_POOL_ALIGN	16u

void		mem_pool_init(mem_pool_t *p, const char *name, void *mem, uint32_t size,
			mem_blk_t *desc, int ndesc);
/* Drop every block (map change); the arena stays. */
void		mem_pool_reset(mem_pool_t *p);
/* First fit; *owner = data pointer. NULL when no hole is large enough. */
mem_blk_t	*mem_pool_alloc(mem_pool_t *p, uint32_t size, void **owner);
/* Last fit, carved from the top of the highest hole that fits (keeps
 * short-lived blocks away from long-lived ones allocated first-fit). */
mem_blk_t	*mem_pool_alloc_high(mem_pool_t *p, uint32_t size, void **owner);
/* fence == 0: free now; else reusable once mem_pool_retire(p, >= fence). */
void		mem_pool_free(mem_pool_t *p, mem_blk_t *b, uint32_t fence);
void		mem_pool_retire(mem_pool_t *p, uint32_t done_frame);
uint32_t	mem_pool_largest(const mem_pool_t *p);
uint32_t	mem_pool_free_bytes(const mem_pool_t *p);
/* Slide the live blocks of [start, end) down onto the free space among them
 * (the range holds no locked or fenced block), leaving one hole at its top;
 * at most max_move bytes are moved (the hole then sits where it stopped).
 * Returns the bytes moved. */
uint32_t	mem_pool_pack(mem_pool_t *p, uint32_t start, uint32_t end, uint32_t max_move);

#endif
