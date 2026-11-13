/* SPDX-License-Identifier: MIT */
/*
 * heap_host.c -- the cart's malloc on a cart-sized arena for the host
 * build, imported from the old tree's n64/port/host/q64_heap_host.c
 * (audit entry CLEAN). Third-party algorithm: it re-implements newlib
 * 4.4's mallocr.c, Doug Lea's dlmalloc 2.6.4 (public domain), step for
 * step.
 *
 * The cart links newlib's mallocr.c (o64: 4-byte size words, 8-byte
 * alignment, 16-byte minimum chunk, 4 KB sbrk pages, 128 KB trim
 * threshold) over libdragon's sbrk. This is the same algorithm -- exact
 * small bins, the roving last_remainder that serves every non-exact
 * request before the sorted large bins are searched, binblock skipping,
 * top extension / trimming, memalign's lead / tail give-back -- so the
 * host splits, coalesces and fragments like the cart rather than like an
 * idealised best fit. The sbrk region is HL64_HOST_HEAP_KB
 * (include/hl64/limits.h) of static storage, the cart's heap after the
 * boot carve. On -m32 pointers are 4 bytes, so the chunk header is the
 * cart's byte for byte; on LP64 it is what the same mallocr.c compiles to
 * on a 64-bit target (8-byte size words, 16-byte alignment, 32-byte
 * minimum chunk) with the algorithm unchanged.
 *
 * Pointers outside the arena (libc-internal allocations that reach the
 * engine) are passed to libc via the ld --wrap __real_* symbols.
 */
#include "hl64/limits.h"
#include "heap_host.h"

#include <stdint.h>
#include <string.h>

void *__real_realloc(void *ptr, size_t size);
void __real_free(void *ptr);

#define ARENA_BYTES	((size_t)HL64_HOST_HEAP_KB * 1024)

#if UINTPTR_MAX > 0xffffffffu
typedef size_t isz_t;			/* INTERNAL_SIZE_T, LP64 mallocr.c */
#else
typedef uint32_t isz_t;			/* INTERNAL_SIZE_T on the cart */
#endif

typedef struct mchunk_s {
	isz_t			prev_size;
	isz_t			size;
	struct mchunk_s		*fd, *bk;
} mchunk_t;

_Static_assert(sizeof(mchunk_t) == 2 * sizeof(isz_t) + 2 * sizeof(void *)
	&& sizeof(isz_t) == sizeof(void *),
	"chunk header: two size words + two links, all pointer-sized");

#define SIZE_SZ			((isz_t)sizeof(isz_t))
#define MALLOC_ALIGNMENT	(SIZE_SZ + SIZE_SZ)
#define ALIGN_MASK		(MALLOC_ALIGNMENT - 1)
#define MINSIZE			((isz_t)sizeof(mchunk_t))
#define PREV_INUSE		1u
#define PAGESZ			4096u
#define TRIM_THRESHOLD		(128u * 1024u)
#define TOP_PAD			0u

#define NAV			128
#define MAX_SMALLBIN_SIZE	512u
#define SMALLBIN_WIDTH		8u
#define BINBLOCKWIDTH		4

#define chunk2mem(p)		((void *)((char *)(p) + 2 * SIZE_SZ))
#define mem2chunk(m)		((mchunk_t *)((char *)(m) - 2 * SIZE_SZ))
#define chunksize(p)		((p)->size & ~(isz_t)3)
#define at_offset(p, s)		((mchunk_t *)((char *)(p) + (s)))
#define next_chunk(p)		at_offset(p, (p)->size & ~PREV_INUSE)
#define prev_chunk(p)		((mchunk_t *)((char *)(p) - (p)->prev_size))
#define inuse(p)		(next_chunk(p)->size & PREV_INUSE)
#define inuse_at(p, s)		(at_offset(p, s)->size & PREV_INUSE)
#define set_inuse_at(p, s)	(at_offset(p, s)->size |= PREV_INUSE)
#define set_head_size(p, s)	((p)->size = ((p)->size & PREV_INUSE) | (s))
#define set_head(p, s)		((p)->size = (s))
#define set_foot(p, s)		(at_offset(p, s)->prev_size = (s))

/* bins[0] is unused (dlmalloc keeps top / binblocks in its cells), bins[1]
 * is the last_remainder list, bins[2..127] the size bins. */
static mchunk_t		bins[NAV];
static mchunk_t		initial_top;	/* zero-size top before the first sbrk */
static mchunk_t		*top = &initial_top;
static unsigned long	binblocks;
#define last_remainder	(&bins[1])

static unsigned char	arena[ARENA_BYTES] __attribute__((aligned(PAGESZ)));
static char		*brk_end = (char *)arena;
static char		*sbrk_base = (char *)-1;
static int		inited;

static void heap_init(void)
{
	int i;

	for (i = 0; i < NAV; i++)
		bins[i].fd = bins[i].bk = &bins[i];
	inited = 1;
}

/* libdragon sbrk: the heap end moves inside [arena, arena + ARENA_BYTES]. */
static char *host_sbrk(long incr)
{
	char *old = brk_end;

	if (incr > (long)((char *)arena + ARENA_BYTES - brk_end)
		|| incr < -(long)(brk_end - (char *)arena))
		return (char *)-1;
	brk_end += incr;
	return old;
}

static unsigned bin_index(isz_t sz)
{
	unsigned long s = sz, k = s >> 9;

	if (k == 0)	return (unsigned)(s >> 3);
	if (k <= 4)	return (unsigned)(56 + (s >> 6));
	if (k <= 20)	return (unsigned)(91 + (s >> 9));
	if (k <= 84)	return (unsigned)(110 + (s >> 12));
	if (k <= 340)	return (unsigned)(119 + (s >> 15));
	if (k <= 1364)	return (unsigned)(124 + (s >> 18));
	return 126;
}

#define idx2binblock(i)	(1ul << ((i) / BINBLOCKWIDTH))

static isz_t request2size(size_t req)
{
	if (req + SIZE_SZ + ALIGN_MASK < MINSIZE + ALIGN_MASK)
		return (MINSIZE + ALIGN_MASK) & ~ALIGN_MASK;
	return (isz_t)((req + SIZE_SZ + ALIGN_MASK) & ~(size_t)ALIGN_MASK);
}

static void unlink_chunk(mchunk_t *p)
{
	p->fd->bk = p->bk;
	p->bk->fd = p->fd;
}

/* Bin p by size: small bins LIFO, large bins in decreasing size order. */
static void frontlink(mchunk_t *p, isz_t s)
{
	mchunk_t	*bk, *fd;
	unsigned	idx;

	if (s < MAX_SMALLBIN_SIZE)
	{
		idx = s >> 3;
		binblocks |= idx2binblock(idx);
		bk = &bins[idx];
		fd = bk->fd;
	}
	else
	{
		idx = bin_index(s);
		bk = &bins[idx];
		fd = bk->fd;
		if (fd == bk)
			binblocks |= idx2binblock(idx);
		else
		{
			while (fd != bk && s < chunksize(fd))
				fd = fd->fd;
			bk = fd->bk;
		}
	}
	p->bk = bk;
	p->fd = fd;
	fd->bk = bk->fd = p;
}

static void link_last_remainder(mchunk_t *p)
{
	last_remainder->fd = last_remainder->bk = p;
	p->fd = p->bk = last_remainder;
}

static void clear_last_remainder(void)
{
	last_remainder->fd = last_remainder->bk = last_remainder;
}

static void heap_free(void *mem);

static void extend_top(isz_t nb)
{
	char		*brk, *new_brk;
	isz_t		front_misalign, correction, top_size;
	mchunk_t	*old_top = top;
	isz_t		old_top_size = chunksize(old_top);
	char		*old_end = (char *)at_offset(old_top, old_top_size);
	isz_t		sbrk_size = nb + TOP_PAD + MINSIZE;

	if (sbrk_base != (char *)-1)
		sbrk_size = (sbrk_size + (PAGESZ - 1)) & ~(PAGESZ - 1);
	brk = host_sbrk((long)sbrk_size);
	if (brk == (char *)-1 || (brk < old_end && old_top != &initial_top))
		return;
	if (brk == old_end && ((uintptr_t)old_end & (PAGESZ - 1)) == 0)
	{
		set_head(top, (sbrk_size + old_top_size) | PREV_INUSE);
		return;
	}
	if (sbrk_base == (char *)-1)
		sbrk_base = brk;
	front_misalign = (isz_t)(uintptr_t)chunk2mem(brk) & ALIGN_MASK;
	correction = 0;
	if (front_misalign > 0)
	{
		correction = MALLOC_ALIGNMENT - front_misalign;
		brk += correction;
	}
	correction += PAGESZ - ((uintptr_t)(brk + sbrk_size) & (PAGESZ - 1));
	correction &= PAGESZ - 1;
	new_brk = host_sbrk((long)correction);
	if (new_brk == (char *)-1)
	{
		correction = 0;
		new_brk = brk + sbrk_size;
		if (front_misalign > 0)
			new_brk -= MALLOC_ALIGNMENT - front_misalign;
	}
	top = (mchunk_t *)brk;
	top_size = (isz_t)(new_brk - brk) + correction;
	set_head(top, top_size | PREV_INUSE);
	if (old_top != &initial_top)
	{
		/* foreign sbrk between: fence off the old top (never on the host) */
		if (old_top_size < MINSIZE)
		{
			set_head(top, PREV_INUSE);
			return;
		}
		old_top_size = (old_top_size - 3 * SIZE_SZ) & ~ALIGN_MASK;
		set_head_size(old_top, old_top_size);
		at_offset(old_top, old_top_size)->size = SIZE_SZ | PREV_INUSE;
		at_offset(old_top, old_top_size + SIZE_SZ)->size = SIZE_SZ | PREV_INUSE;
		if (old_top_size >= MINSIZE)
			heap_free(chunk2mem(old_top));
	}
}

static void heap_trim(void)
{
	long	top_size = (long)chunksize(top);
	long	extra = ((top_size - (long)TOP_PAD - (long)MINSIZE + (long)(PAGESZ - 1))
			/ (long)PAGESZ - 1) * (long)PAGESZ;

	if (extra < (long)PAGESZ)
		return;
	if (host_sbrk(0) != (char *)top + top_size)
		return;
	if (host_sbrk(-extra) == (char *)-1)
		return;
	set_head(top, (isz_t)(top_size - extra) | PREV_INUSE);
}

static void *heap_malloc(size_t bytes)
{
	mchunk_t	*victim, *remainder, *bin, *q;
	isz_t		victim_size, nb;
	long		remainder_size;
	int		idx, startidx;
	unsigned long	block;

	if (!inited)
		heap_init();
	nb = request2size(bytes);
	if (bytes > 0x7fffffffu || nb < bytes)
		return NULL;

	if (nb < MAX_SMALLBIN_SIZE - SMALLBIN_WIDTH)
	{
		idx = (int)(nb >> 3);
		q = &bins[idx];
		victim = q->bk;
		if (victim == q)
		{
			q = &bins[idx + 1];
			victim = q->bk;
		}
		if (victim != q)
		{
			victim_size = chunksize(victim);
			unlink_chunk(victim);
			set_inuse_at(victim, victim_size);
			return chunk2mem(victim);
		}
		idx += 2;
	}
	else
	{
		idx = (int)bin_index(nb);
		bin = &bins[idx];
		for (victim = bin->bk; victim != bin; victim = victim->bk)
		{
			victim_size = chunksize(victim);
			remainder_size = (long)victim_size - (long)nb;
			if (remainder_size >= (long)MINSIZE)
			{
				--idx;
				break;
			}
			if (remainder_size >= 0)
			{
				unlink_chunk(victim);
				set_inuse_at(victim, victim_size);
				return chunk2mem(victim);
			}
		}
		++idx;
	}

	/* the most recent split's remainder: roving first fit */
	if ((victim = last_remainder->fd) != last_remainder)
	{
		victim_size = chunksize(victim);
		remainder_size = (long)victim_size - (long)nb;
		if (remainder_size >= (long)MINSIZE)
		{
			remainder = at_offset(victim, nb);
			set_head(victim, nb | PREV_INUSE);
			link_last_remainder(remainder);
			set_head(remainder, (isz_t)remainder_size | PREV_INUSE);
			set_foot(remainder, (isz_t)remainder_size);
			return chunk2mem(victim);
		}
		clear_last_remainder();
		if (remainder_size >= 0)
		{
			set_inuse_at(victim, victim_size);
			return chunk2mem(victim);
		}
		frontlink(victim, victim_size);
	}

	/* first big-enough chunk, bin by bin, skipping empty binblocks */
	if ((block = idx2binblock(idx)) <= binblocks)
	{
		if ((block & binblocks) == 0)
		{
			idx = (idx & ~(BINBLOCKWIDTH - 1)) + BINBLOCKWIDTH;
			block <<= 1;
			while ((block & binblocks) == 0)
			{
				idx += BINBLOCKWIDTH;
				block <<= 1;
			}
		}
		for (;;)
		{
			startidx = idx;
			q = bin = &bins[idx];
			do
			{
				for (victim = bin->bk; victim != bin; victim = victim->bk)
				{
					victim_size = chunksize(victim);
					remainder_size = (long)victim_size - (long)nb;
					if (remainder_size >= (long)MINSIZE)
					{
						remainder = at_offset(victim, nb);
						set_head(victim, nb | PREV_INUSE);
						unlink_chunk(victim);
						link_last_remainder(remainder);
						set_head(remainder, (isz_t)remainder_size | PREV_INUSE);
						set_foot(remainder, (isz_t)remainder_size);
						return chunk2mem(victim);
					}
					if (remainder_size >= 0)
					{
						set_inuse_at(victim, victim_size);
						unlink_chunk(victim);
						return chunk2mem(victim);
					}
				}
				bin++;
			} while ((++idx & (BINBLOCKWIDTH - 1)) != 0);

			do
			{
				if ((startidx & (BINBLOCKWIDTH - 1)) == 0)
				{
					binblocks &= ~block;
					break;
				}
				--startidx;
				q--;
			} while (q->fd == q);

			if ((block <<= 1) <= binblocks && block != 0)
			{
				while ((block & binblocks) == 0)
				{
					idx += BINBLOCKWIDTH;
					block <<= 1;
				}
			}
			else
				break;
		}
	}

	remainder_size = (long)chunksize(top) - (long)nb;
	if (chunksize(top) < nb || remainder_size < (long)MINSIZE)
	{
		extend_top(nb);
		remainder_size = (long)chunksize(top) - (long)nb;
		if (chunksize(top) < nb || remainder_size < (long)MINSIZE)
			return NULL;
	}
	victim = top;
	set_head(victim, nb | PREV_INUSE);
	top = at_offset(victim, nb);
	set_head(top, (isz_t)remainder_size | PREV_INUSE);
	return chunk2mem(victim);
}

static void heap_free(void *mem)
{
	mchunk_t	*p, *next;
	isz_t		hd, sz, nextsz, prevsz;
	int		islr;

	if (!mem)
		return;
	p = mem2chunk(mem);
	hd = p->size;
	sz = hd & ~PREV_INUSE;
	next = at_offset(p, sz);
	nextsz = chunksize(next);

	if (next == top)
	{
		sz += nextsz;
		if (!(hd & PREV_INUSE))
		{
			prevsz = p->prev_size;
			p = prev_chunk(p);
			sz += prevsz;
			unlink_chunk(p);
		}
		set_head(p, sz | PREV_INUSE);
		top = p;
		if (sz >= TRIM_THRESHOLD)
			heap_trim();
		return;
	}

	set_head(next, nextsz);
	islr = 0;
	if (!(hd & PREV_INUSE))
	{
		prevsz = p->prev_size;
		p = prev_chunk(p);
		sz += prevsz;
		if (p->fd == last_remainder)
			islr = 1;
		else
			unlink_chunk(p);
	}
	if (!inuse_at(next, nextsz))
	{
		sz += nextsz;
		if (!islr && next->fd == last_remainder)
		{
			islr = 1;
			link_last_remainder(p);
		}
		else
			unlink_chunk(next);
	}
	set_head(p, sz | PREV_INUSE);
	set_foot(p, sz);
	if (!islr)
		frontlink(p, sz);
}

static void *heap_realloc(void *oldmem, size_t bytes)
{
	mchunk_t	*oldp, *newp, *next, *prev, *remainder;
	isz_t		nb, oldsize, newsize, nextsize = 0, prevsize;
	long		remainder_size;
	void		*newmem;

	if (!oldmem)
		return heap_malloc(bytes);
	newp = oldp = mem2chunk(oldmem);
	newsize = oldsize = chunksize(oldp);
	nb = request2size(bytes);
	if (bytes > 0x7fffffffu || nb < bytes)
		return NULL;

	if ((long)oldsize < (long)nb)
	{
		next = at_offset(oldp, oldsize);
		if (next == top || !inuse(next))
		{
			nextsize = chunksize(next);
			if (next == top)
			{
				if ((long)(nextsize + newsize) >= (long)(nb + MINSIZE))
				{
					newsize += nextsize;
					top = at_offset(oldp, nb);
					set_head(top, (newsize - nb) | PREV_INUSE);
					set_head_size(oldp, nb);
					return chunk2mem(oldp);
				}
			}
			else if ((long)(nextsize + newsize) >= (long)nb)
			{
				unlink_chunk(next);
				newsize += nextsize;
				goto split;
			}
		}
		else
		{
			next = NULL;
			nextsize = 0;
		}

		if (!(oldp->size & PREV_INUSE))
		{
			prev = prev_chunk(oldp);
			prevsize = chunksize(prev);
			if (next)
			{
				if (next == top)
				{
					if ((long)(nextsize + prevsize + newsize) >= (long)(nb + MINSIZE))
					{
						unlink_chunk(prev);
						newp = prev;
						newsize += prevsize + nextsize;
						newmem = chunk2mem(newp);
						memmove(newmem, oldmem, oldsize - SIZE_SZ);
						top = at_offset(newp, nb);
						set_head(top, (newsize - nb) | PREV_INUSE);
						set_head_size(newp, nb);
						return newmem;
					}
				}
				else if ((long)(nextsize + prevsize + newsize) >= (long)nb)
				{
					unlink_chunk(next);
					unlink_chunk(prev);
					newp = prev;
					newsize += nextsize + prevsize;
					memmove(chunk2mem(newp), oldmem, oldsize - SIZE_SZ);
					goto split;
				}
			}
			if ((long)(prevsize + newsize) >= (long)nb)
			{
				unlink_chunk(prev);
				newp = prev;
				newsize += prevsize;
				memmove(chunk2mem(newp), oldmem, oldsize - SIZE_SZ);
				goto split;
			}
		}

		newmem = heap_malloc(bytes);
		if (!newmem)
			return NULL;
		if ((newp = mem2chunk(newmem)) == next_chunk(oldp))
		{
			newsize += chunksize(newp);
			newp = oldp;
			goto split;
		}
		memcpy(newmem, oldmem, oldsize - SIZE_SZ);
		heap_free(oldmem);
		return newmem;
	}

split:
	remainder_size = (long)newsize - (long)nb;
	if (remainder_size >= (long)MINSIZE)
	{
		remainder = at_offset(newp, nb);
		set_head_size(newp, nb);
		set_head(remainder, (isz_t)remainder_size | PREV_INUSE);
		set_inuse_at(remainder, (isz_t)remainder_size);
		heap_free(chunk2mem(remainder));
	}
	else
	{
		set_head_size(newp, newsize);
		set_inuse_at(newp, newsize);
	}
	return chunk2mem(newp);
}

static void *heap_memalign(size_t alignment, size_t bytes)
{
	mchunk_t	*p, *newp, *remainder;
	isz_t		nb, newsize, leadsize;
	long		remainder_size;
	char		*m, *brk;

	if (alignment <= MALLOC_ALIGNMENT)
		return heap_malloc(bytes);
	if (alignment < MINSIZE)
		alignment = MINSIZE;
	nb = request2size(bytes);
	if (nb > 0xffffffffu - (alignment + MINSIZE) || nb < bytes)
		return NULL;
	m = heap_malloc(nb + alignment + MINSIZE);
	if (!m)
		return NULL;
	p = mem2chunk(m);
	if ((uintptr_t)m % alignment != 0)
	{
		brk = (char *)mem2chunk(((uintptr_t)m + alignment - 1) & -(uintptr_t)alignment);
		if ((long)(brk - (char *)p) < (long)MINSIZE)
			brk += alignment;
		newp = (mchunk_t *)brk;
		leadsize = (isz_t)(brk - (char *)p);
		newsize = chunksize(p) - leadsize;
		set_head(newp, newsize | PREV_INUSE);
		set_inuse_at(newp, newsize);
		set_head_size(p, leadsize);
		heap_free(chunk2mem(p));
		p = newp;
	}
	remainder_size = (long)chunksize(p) - (long)nb;
	if (remainder_size >= (long)MINSIZE)
	{
		remainder = at_offset(p, nb);
		set_head(remainder, (isz_t)remainder_size | PREV_INUSE);
		set_head_size(p, nb);
		heap_free(chunk2mem(remainder));
	}
	return chunk2mem(p);
}

#define IN_ARENA(p)	((unsigned char *)(p) >= arena \
			 && (unsigned char *)(p) < arena + ARENA_BYTES)

void *heap_host_malloc(size_t n)
{
	return heap_malloc(n);
}

void heap_host_free(void *p)
{
	if (p && !IN_ARENA(p))
	{
		__real_free(p);
		return;
	}
	heap_free(p);
}

void *heap_host_calloc(size_t nmemb, size_t size)
{
	void	*p;
	size_t	n;

	if (__builtin_mul_overflow(nmemb, size, &n))
		return NULL;
	p = heap_malloc(n);
	if (p)
		memset(p, 0, chunksize(mem2chunk(p)) - SIZE_SZ);
	return p;
}

void *heap_host_realloc(void *p, size_t n)
{
	if (p && !IN_ARENA(p))
		return __real_realloc(p, n);
	return heap_realloc(p, n);
}

void *heap_host_memalign(size_t alignment, size_t n)
{
	return heap_memalign(alignment, n);
}

/* Free bytes: binned chunks + the last remainder + top + unclaimed sbrk. */
void heap_host_stats(size_t *arena_out, size_t *free_out)
{
	size_t		sum;
	mchunk_t	*c;
	int		i;

	if (!inited)
		heap_init();
	sum = chunksize(top) + (size_t)((char *)arena + ARENA_BYTES - brk_end);
	for (i = 1; i < NAV; i++)
		for (c = bins[i].fd; c != &bins[i]; c = c->fd)
			sum += chunksize(c);
	if (arena_out)
		*arena_out = ARENA_BYTES;
	if (free_out)
		*free_out = sum;
}
