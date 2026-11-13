/* SPDX-License-Identifier: MIT */
/*
 * heap.c -- system-heap instrumentation and probes (imported from the old
 * tree's r64_malloc.c, audit entry n64/port/r64_malloc.c CLEAN).
 *
 * ld --wrap catches malloc / free / calloc / realloc / memalign and
 * libdragon's malloc_uncached[_aligned] (Makefile), so every heap block
 * is counted with its caller: live and peak bytes, window and lifetime
 * counts, failed requests, and the heap map of DBG_MEM. The probes size
 * the heap left after the boot carve (carve.c).
 *
 * Re-entrancy: libdragon may allocate inside a counted call; nested calls
 * pass straight through so the counters are never updated mid-change.
 *
 * Region redirect (heap.h mem_heap_redirect): while a redirect is active
 * the allocating wraps hand out its region's blocks instead (plat/n64/
 * audio.c: libdragon's mixer and wav64 blocks in the SOUND region); frees
 * and reallocs of its blocks go back to it whenever they happen. Those
 * blocks are not counted here (the region reports its own use).
 *
 * Host build (HL64_HOST): same --wrap and counters over plat/host's
 * heap_host.c, a fixed arena the size of the cart's post-carve heap, so an
 * allocation that fails on the cart fails on the host.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "hl64/limits.h"
#include "engine/core/log.h"
#include "engine/mem/heap.h"

#ifdef HL64_HOST
#include "heap_host.h"
#define __real_malloc	heap_host_malloc
#define __real_free	heap_host_free
#define __real_calloc	heap_host_calloc
#define __real_realloc	heap_host_realloc
#else
#include <malloc.h>
/* Provided by ld --wrap. */
void *__real_malloc(size_t size);
void __real_free(void *ptr);
void *__real_calloc(size_t nmemb, size_t size);
void *__real_realloc(void *ptr, size_t size);
#endif

/* Window counters (cleared each PROFILE dump / memstats reset of window). */
static uint32_t w_n_malloc;
static uint32_t w_n_free;
static uint32_t w_n_calloc;
static uint32_t w_n_realloc;
static uint32_t w_n_fail;
static uint64_t w_bytes_req;	/* sum of sizes passed to malloc/calloc/realloc */

/* Lifetime / live set. */
static uint32_t live_count;
static uint64_t live_bytes;
static uint64_t peak_live_bytes;
static uint32_t total_malloc;
static uint32_t total_free;
static uint32_t total_fail;
static uint64_t total_bytes_req;

/* ptr → size for free accounting (bounded open hash). */
#ifndef HEAP_TRACK
#define HEAP_TRACK	1024
#endif
typedef struct {
	void		*p;
	uint32_t	sz;
	void		*ra;	/* caller (heap map attribution) */
} mal_slot_t;

static mal_slot_t track[HEAP_TRACK];
static uint32_t track_miss_free;	/* free of unknown ptr (no size) */
static uint32_t track_miss_full;	/* insert failed (table full) */

static int reenter;
static mem_heap_redirect_t *redir;

void mem_heap_redirect(mem_heap_redirect_t *r)
{
	redir = r;
}

static void *redir_alloc(size_t align, size_t size)
{
	return redir->alloc(align, size);
}

/* realloc of an owned block, or of NULL while the redirect is active */
static void *redir_realloc(void *ptr, size_t size)
{
	void *p = size ? redir->alloc(16, size) : NULL;
	size_t old;

	if (ptr && p)
	{
		old = redir->size(ptr);
		memcpy(p, ptr, old < size ? old : size);
	}
	if (ptr && (p || !size))
		redir->release(ptr);
	return p;
}


static unsigned
hash_ptr(void *p)
{
	uintptr_t u = (uintptr_t)p;
	return (unsigned)((u >> 4) ^ (u >> 12)) % HEAP_TRACK;
}

static void
track_insert(void *p, uint32_t sz, void *ra)
{
	unsigned h, i;

	if (!p || sz == 0)
		return;
	h = hash_ptr(p);
	for (i = 0; i < HEAP_TRACK; i++)
	{
		unsigned j = (h + i) % HEAP_TRACK;
		if (track[j].p == NULL || track[j].p == p)
		{
			track[j].p = p;
			track[j].sz = sz;
			track[j].ra = ra;
			return;
		}
	}
	track_miss_full++;
}

static uint32_t
track_remove(void *p)
{
	unsigned h, i;

	if (!p)
		return 0;
	h = hash_ptr(p);
	for (i = 0; i < HEAP_TRACK; i++)
	{
		unsigned j = (h + i) % HEAP_TRACK;
		if (track[j].p == p)
		{
			uint32_t sz = track[j].sz;
			track[j].p = NULL;
			track[j].sz = 0;
			track[j].ra = NULL;
			return sz;
		}
	}
	track_miss_free++;
	return 0;
}

/* Last failed requests (heap map). */
#define MAL_FAILS	4
static uint32_t fail_sz[MAL_FAILS];
static void *fail_ra[MAL_FAILS];
static unsigned fail_next;

static void
note_alloc(void *p, size_t sz, void *ra)
{
	if (!p)
	{
		w_n_fail++;
		total_fail++;
		fail_sz[fail_next % MAL_FAILS] = (uint32_t)sz;
		fail_ra[fail_next % MAL_FAILS] = ra;
		fail_next++;
		return;
	}
	if (sz > 0xFFFFFFFFu)
		sz = 0xFFFFFFFFu;
	track_insert(p, (uint32_t)sz, ra);
	live_count++;
	live_bytes += sz;
	if (live_bytes > peak_live_bytes)
		peak_live_bytes = live_bytes;
	w_bytes_req += sz;
	total_bytes_req += sz;
}

static void
note_free(void *p)
{
	uint32_t sz;

	if (!p)
		return;
	sz = track_remove(p);
	if (live_count > 0)
		live_count--;
	/* unknown size (sz == 0): leave live_bytes as a lower bound */
	if (sz > 0 && live_bytes >= sz)
		live_bytes -= sz;
}

void *
__wrap_malloc(size_t size)
{
	void *p;

	if (redir && redir->active)
		return redir_alloc(16, size);
	if (reenter)
		return __real_malloc(size);
	reenter = 1;
	p = __real_malloc(size);
	w_n_malloc++;
	total_malloc++;
	note_alloc(p, size, __builtin_return_address(0));
	reenter = 0;
	return p;
}

void
__wrap_free(void *ptr)
{
	if (ptr && redir && redir->owns(ptr))
	{
		redir->release(ptr);
		return;
	}
	if (reenter)
	{
		__real_free(ptr);
		return;
	}
	reenter = 1;
	if (ptr)
	{
		w_n_free++;
		total_free++;
		note_free(ptr);
	}
	__real_free(ptr);
	reenter = 0;
}

void *
__wrap_calloc(size_t nmemb, size_t size)
{
	void *p;
	size_t bytes;

	if (redir && redir->active)
	{
		p = redir_alloc(16, nmemb * size);
		if (p)
			memset(p, 0, nmemb * size);
		return p;
	}
	if (reenter)
		return __real_calloc(nmemb, size);
	reenter = 1;
	bytes = nmemb * size;
	/* overflow: real calloc handles; we still count request */
	p = __real_calloc(nmemb, size);
	w_n_calloc++;
	total_malloc++;
	note_alloc(p, bytes, __builtin_return_address(0));
	reenter = 0;
	return p;
}

void *
__wrap_realloc(void *ptr, size_t size)
{
	void *p;
	uint32_t old_sz = 0;

	if (redir && (ptr ? redir->owns(ptr) : redir->active))
		return redir_realloc(ptr, size);
	if (reenter)
		return __real_realloc(ptr, size);
	reenter = 1;
	if (ptr)
		old_sz = track_remove(ptr);
	p = __real_realloc(ptr, size);
	w_n_realloc++;
	if (!p && size > 0)
	{
		/* failed: realloc keeps ptr, restore its tracking */
		if (ptr && old_sz)
		{
			track_insert(ptr, old_sz, __builtin_return_address(0));
			/* live already decremented by remove — restore */
			live_count++;
			live_bytes += old_sz;
		}
		note_alloc(NULL, size, __builtin_return_address(0));
	}
	else if (p)
	{
		if (ptr && live_count > 0)
			live_count--; /* removed from live via track_remove */
		if (old_sz > 0 && live_bytes >= old_sz)
			live_bytes -= old_sz;
		/* treat as new alloc of `size` */
		total_malloc++;
		note_alloc(p, size, __builtin_return_address(0));
	}
	else if (size == 0)
	{
		/* realloc(p,0) ~ free */
		w_n_free++;
		total_free++;
		if (live_count > 0)
			live_count--;
		if (old_sz > 0 && live_bytes >= old_sz)
			live_bytes -= old_sz;
	}
	reenter = 0;
	return p;
}

#ifndef HL64_HOST
/*
 * memalign: malloc_uncached (libdragon surfaces, uncached pools, wav64
 * buffers) allocates here, and its free goes through __wrap_free. The
 * malloc_uncached[_aligned] wraps record their caller so the heap map
 * names the pool owner instead of libdragon's n64sys.c.
 */
void *__real_memalign(size_t align, size_t size);
void *__real_malloc_uncached(size_t size);
void *__real_malloc_uncached_aligned(int align, size_t size);
static void *uncached_ra;

void *
__wrap_memalign(size_t align, size_t size)
{
	void *p;

	if (redir && redir->active)
		return redir_alloc(align, size);
	if (reenter)
		return __real_memalign(align, size);
	reenter = 1;
	p = __real_memalign(align, size);
	w_n_malloc++;
	total_malloc++;
	note_alloc(p, size, uncached_ra ? uncached_ra : __builtin_return_address(0));
	reenter = 0;
	return p;
}

void *
__wrap_malloc_uncached(size_t size)
{
	void *p;

	uncached_ra = __builtin_return_address(0);
	p = __real_malloc_uncached(size);
	uncached_ra = NULL;
	return p;
}

void *
__wrap_malloc_uncached_aligned(int align, size_t size)
{
	void *p;

	uncached_ra = __builtin_return_address(0);
	p = __real_malloc_uncached_aligned(align, size);
	uncached_ra = NULL;
	return p;
}
#endif

/*
 * Snapshot for PROFILE dump. *window* fields are means over nframes if nframes>0.
 * Clears window counters after fill when clear_window is true.
 */
void
mem_heap_stats(unsigned *n_malloc, unsigned *n_free, unsigned *n_calloc,
	unsigned *n_realloc, unsigned *n_fail, unsigned *bytes_req,
	unsigned *live_kb, unsigned *peak_live_kb, unsigned *track_full,
	unsigned *track_unk_free, int clear_window)
{
	if (n_malloc)
		*n_malloc = w_n_malloc;
	if (n_free)
		*n_free = w_n_free;
	if (n_calloc)
		*n_calloc = w_n_calloc;
	if (n_realloc)
		*n_realloc = w_n_realloc;
	if (n_fail)
		*n_fail = w_n_fail;
	if (bytes_req)
		*bytes_req = (unsigned)(w_bytes_req > 0xFFFFFFFFu
			? 0xFFFFFFFFu : w_bytes_req);
	if (live_kb)
		*live_kb = (unsigned)(live_bytes / 1024u);
	if (peak_live_kb)
		*peak_live_kb = (unsigned)(peak_live_bytes / 1024u);
	if (track_full)
		*track_full = track_miss_full;
	if (track_unk_free)
		*track_unk_free = track_miss_free;

	if (clear_window)
	{
		w_n_malloc = w_n_free = w_n_calloc = w_n_realloc = w_n_fail = 0;
		w_bytes_req = 0;
	}
}

void
mem_heap_lifetime(unsigned *total_mal, unsigned *total_fr, unsigned *total_fl,
	unsigned *live_n, unsigned *live_kb, unsigned *peak_kb)
{
	if (total_mal)
		*total_mal = total_malloc;
	if (total_fr)
		*total_fr = total_free;
	if (total_fl)
		*total_fl = total_fail;
	if (live_n)
		*live_n = live_count;
	if (live_kb)
		*live_kb = (unsigned)(live_bytes / 1024u);
	if (peak_kb)
		*peak_kb = (unsigned)(peak_live_bytes / 1024u);
}

/*
 * Largest single malloc that currently succeeds (binary search, <= cap).
 *
 * Calls __real_malloc / __real_free directly: a plain malloc(n); free(p)
 * pair whose result is only null-tested is dead code to GCC, which deletes
 * both calls and folds the test to "succeeded" — every probe then reported
 * ~cap (all of RDRAM) and the carve check passed blind.
 * Bypassing the wrap also keeps probe traffic out of the alloc counters.
 */
size_t
mem_heap_probe_largest(size_t cap)
{
	size_t lo = 0, hi = cap;
	int steps;

	for (steps = 0; steps < 24 && lo + 256 < hi; steps++)
	{
		size_t mid = lo + (hi - lo) / 2;
		void *p = __real_malloc(mid);

		if (p)
		{
			__real_free(p);
			lo = mid;
		}
		else
			hi = mid;
	}
	return lo;
}

/*
 * Free heap bytes in total (every free chunk, top included, plus the sbrk
 * room left to grow into): the cart's dlmalloc mallinfo + the gap up to
 * libdragon's heap top; the parity host's identical allocator likewise.
 * Largest-free over this is the fragmentation ratio.
 */
size_t
mem_heap_free_total(void)
{
#ifdef HL64_HOST
	size_t free_bytes;

	heap_host_stats(NULL, &free_bytes);
	return free_bytes;
#else
	extern char *__heap_end, *__heap_top;
	struct mallinfo mi = mallinfo();

	return (size_t)mi.fordblks + (size_t)(__heap_top - __heap_end);
#endif
}

/*
 * Heap map (DBG_MEM): tracked live blocks in address order with
 * their caller, the gaps between them (free chunks, or untracked blocks:
 * newlib-internal _malloc_r users such as FILE buffers), and the last
 * failed requests. Blocks < 1 KB are summed into the next line. Symbolize
 * ra= with mips64-elf-addr2line -f -e hl64.elf.
 */
static int
map_cmp(const void *a, const void *b)
{
	uintptr_t x = (uintptr_t)((const mal_slot_t *)a)->p;
	uintptr_t y = (uintptr_t)((const mal_slot_t *)b)->p;

	return x < y ? -1 : x > y;
}

void
mem_heap_map_dump(void)
{
	static mal_slot_t sorted[HEAP_TRACK];
	int i, n = 0, small_n = 0;
	unsigned small_b = 0, gap_total = 0;
	uintptr_t end = 0;

	for (i = 0; i < HEAP_TRACK; i++)
		if (track[i].p)
			sorted[n++] = track[i];
	qsort(sorted, n, sizeof(sorted[0]), map_cmp);
	log_printf("heapmap: %d tracked blocks, largest free %u KB of %u KB free\n",
		n, (unsigned)(mem_heap_probe_largest(HL64_RDRAM_BYTES) / 1024),
		(unsigned)(mem_heap_free_total() / 1024));
	for (i = 0; i < n; i++)
	{
		uintptr_t p = (uintptr_t)sorted[i].p;

		if (end && p > end + 64)
		{
			unsigned gap = (unsigned)(p - end);

			gap_total += gap;
			if (gap >= 4096)
			{
				if (small_n)
					log_printf("heapmap:  +%d small %u B\n", small_n, small_b);
				small_n = 0;
				small_b = 0;
				log_printf("heapmap:  gap %u KB\n", gap / 1024);
			}
		}
		if (sorted[i].sz >= 1024)
		{
			if (small_n)
				log_printf("heapmap:  +%d small %u B\n", small_n, small_b);
			small_n = 0;
			small_b = 0;
			log_printf("heapmap: %08lx %6u KB ra=%08lx\n", (unsigned long)p,
				(unsigned)(sorted[i].sz / 1024), (unsigned long)(uintptr_t)sorted[i].ra);
		}
		else
		{
			small_n++;
			small_b += sorted[i].sz;
		}
		end = p + sorted[i].sz;
	}
	if (small_n)
		log_printf("heapmap:  +%d small %u B\n", small_n, small_b);
	log_printf("heapmap: end %08lx, gaps %u KB; fails %u, last:", (unsigned long)end,
		gap_total / 1024, (unsigned)total_fail);
	for (i = 0; i < MAL_FAILS && i < (int)fail_next; i++)
	{
		unsigned k = (fail_next - 1 - i) % MAL_FAILS;

		log_printf(" %u@%08lx", (unsigned)fail_sz[k], (unsigned long)(uintptr_t)fail_ra[k]);
	}
	log_printf("\n");
}
