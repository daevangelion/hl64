/* SPDX-License-Identifier: MIT */
/*
 * heap.h -- system-heap counters and probes (heap.c). Engine code does
 * not allocate after boot (architecture 4.3); these watch libdragon and
 * the boot path, and size the heap left by the carve.
 */
#ifndef HL64_HEAP_H
#define HL64_HEAP_H

#include <stddef.h>

/* Window counters (cleared when clear_window) and live / peak KB. */
void	mem_heap_stats(unsigned *n_malloc, unsigned *n_free, unsigned *n_calloc,
		unsigned *n_realloc, unsigned *n_fail, unsigned *bytes_req,
		unsigned *live_kb, unsigned *peak_live_kb, unsigned *track_full,
		unsigned *track_unk_free, int clear_window);
void	mem_heap_lifetime(unsigned *total_mal, unsigned *total_fr, unsigned *total_fl,
		unsigned *live_n, unsigned *live_kb, unsigned *peak_kb);
/* Largest single allocation that succeeds now (binary search, <= cap). */
size_t	mem_heap_probe_largest(size_t cap);
/* Free bytes in total: free chunks + top + unclaimed sbrk room. */
size_t	mem_heap_free_total(void);
/* Live blocks with callers, gaps and the last failures (DBG_MEM). */
void	mem_heap_map_dump(void);

/*
 * Region redirect (plat/n64/audio.c: libdragon's mixer and wav64 blocks
 * in the SOUND region, architecture 4.3): while active, malloc / calloc /
 * memalign go to alloc; free / realloc of a block owns() recognises go to
 * release at any time. One redirect at a time; NULL removes it.
 */
typedef struct {
	void	*(*alloc)(size_t align, size_t size);	/* NULL when full */
	void	(*release)(void *p);
	size_t	(*size)(void *p);			/* usable bytes of an owned block */
	int	(*owns)(const void *p);
	int	active;
} mem_heap_redirect_t;
void	mem_heap_redirect(mem_heap_redirect_t *r);

#endif
