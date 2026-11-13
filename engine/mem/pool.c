/* SPDX-License-Identifier: MIT */
/*
 * pool.c -- residency pools (pool.h), imported from the old tree's
 * q64_pool.c (audit entry n64/port/q64_pool.c CLEAN).
 *
 * Shared by the cart and the host. A few hundred blocks per pool at most
 * (cooked chunks are 0.1-16 KB in 128-240 KB) on an address-ordered list;
 * the free blocks are also on their own address-ordered list, so a
 * first-fit allocation walks the holes, not every live block.
 */
#include <string.h>

#include "engine/core/sys.h"
#include "engine/mem/pool.h"

#define ALIGN_UP(x)	(((x) + MEM_POOL_ALIGN - 1) & ~(MEM_POOL_ALIGN - 1))

static mem_blk_t *DescGet(mem_pool_t *p)
{
	mem_blk_t *b = p->spare;

	if (!b)
		return NULL;
	p->spare = b->next;
	memset(b, 0, sizeof(*b));
	return b;
}

static void DescPut(mem_pool_t *p, mem_blk_t *b)
{
	b->state = MEM_BLK_SPARE;
	b->next = p->spare;
	p->spare = b;
}

/* ---- free list (address order) ------------------------------------------ */

static void FreeUnlink(mem_pool_t *p, mem_blk_t *b)
{
	if (b->fprev)
		b->fprev->fnext = b->fnext;
	else
		p->fhead = b->fnext;
	if (b->fnext)
		b->fnext->fprev = b->fprev;
	b->fprev = b->fnext = NULL;
}

/* b takes o's place on the free list */
static void FreeReplace(mem_pool_t *p, mem_blk_t *o, mem_blk_t *b)
{
	b->fprev = o->fprev;
	b->fnext = o->fnext;
	if (b->fprev)
		b->fprev->fnext = b;
	else
		p->fhead = b;
	if (b->fnext)
		b->fnext->fprev = b;
	o->fprev = o->fnext = NULL;
}

/* b (free, not listed, no free neighbour) after the free block before it */
static void FreeInsert(mem_pool_t *p, mem_blk_t *b)
{
	mem_blk_t *f = b->prev;

	while (f && f->state != MEM_BLK_FREE)
		f = f->prev;
	b->fprev = f;
	b->fnext = f ? f->fnext : p->fhead;
	if (f)
		f->fnext = b;
	else
		p->fhead = b;
	if (b->fnext)
		b->fnext->fprev = b;
}

void mem_pool_reset(mem_pool_t *p)
{
	int i;

	p->spare = NULL;
	for (i = p->ndesc - 1; i >= 1; i--)
		DescPut(p, &p->desc[i]);
	p->head = &p->desc[0];
	memset(p->head, 0, sizeof(*p->head));
	p->head->size = p->size;
	p->head->state = MEM_BLK_FREE;
	p->fhead = p->head;
	p->used = p->fenced = 0;
	p->nofit = 0xFFFFFFFFu;
}

void mem_pool_init(mem_pool_t *p, const char *name, void *mem, uint32_t size,
	mem_blk_t *desc, int ndesc)
{
	memset(p, 0, sizeof(*p));
	p->name = name;
	p->base = mem;
	p->size = size & ~(MEM_POOL_ALIGN - 1);
	p->desc = desc;
	p->ndesc = ndesc;
	mem_pool_reset(p);
}

/* b just became free (not on the free list): merge it with free neighbours
 * and list the result; returns the surviving block. */
static mem_blk_t *Coalesce(mem_pool_t *p, mem_blk_t *b)
{
	mem_blk_t *n = b->next, *v = b->prev;

	if (v && v->state == MEM_BLK_FREE)
	{
		/* v is listed: it absorbs b (and n) */
		v->size += b->size;
		v->next = n;
		if (n)
			n->prev = v;
		DescPut(p, b);
		if (n && n->state == MEM_BLK_FREE)
		{
			v->size += n->size;
			v->next = n->next;
			if (n->next)
				n->next->prev = v;
			FreeUnlink(p, n);
			DescPut(p, n);
		}
		return v;
	}
	if (n && n->state == MEM_BLK_FREE)
	{
		/* b absorbs n and takes its place on the list */
		b->size += n->size;
		b->next = n->next;
		if (n->next)
			n->next->prev = b;
		FreeReplace(p, n, b);
		DescPut(p, n);
		return b;
	}
	FreeInsert(p, b);
	return b;
}

/* Take size bytes from the bottom of hole b. */
static mem_blk_t *CarveLow(mem_pool_t *p, mem_blk_t *b, uint32_t size, void **owner)
{
	mem_blk_t *rest;

	if (b->size > size)
	{
		rest = DescGet(p);
		if (!rest)
			return NULL;	/* descriptor table full: treat as no hole */
		rest->ofs = b->ofs + size;
		rest->size = b->size - size;
		rest->state = MEM_BLK_FREE;
		rest->prev = b;
		rest->next = b->next;
		if (b->next)
			b->next->prev = rest;
		b->next = rest;
		b->size = size;
		FreeReplace(p, b, rest);
	}
	else
		FreeUnlink(p, b);
	b->state = MEM_BLK_USED;
	b->locked = 0;
	b->fence = 0;
	b->owner = owner;
	if (owner)
		*owner = p->base + b->ofs;
	p->used += b->size;
	if (p->used > p->peak)
		p->peak = p->used;
	return b;
}

mem_blk_t *mem_pool_alloc(mem_pool_t *p, uint32_t size, void **owner)
{
	mem_blk_t *b;

	size = ALIGN_UP(size ? size : 1);
	if (size >= p->nofit)
		return NULL;	/* known not to fit since the last hole grew */
	for (b = p->fhead; b; b = b->fnext)
		if (b->size >= size)
			break;
	if (!b)
	{
		p->nofit = size;
		return NULL;
	}
	return CarveLow(p, b, size, owner);
}

mem_blk_t *mem_pool_alloc_high(mem_pool_t *p, uint32_t size, void **owner)
{
	mem_blk_t *b, *best = NULL;

	size = ALIGN_UP(size ? size : 1);
	if (size >= p->nofit)
		return NULL;
	for (b = p->fhead; b; b = b->fnext)
		if (b->size >= size)
			best = b;	/* the list is in address order: keep the last */
	if (!best)
	{
		p->nofit = size;
		return NULL;
	}
	if (best->size > size)
	{
		/* carve the top: the hole keeps its place on the free list */
		b = DescGet(p);
		if (!b)
			return NULL;
		b->ofs = best->ofs + best->size - size;
		b->size = size;
		b->prev = best;
		b->next = best->next;
		if (best->next)
			best->next->prev = b;
		best->next = b;
		best->size -= size;
	}
	else
	{
		b = best;
		FreeUnlink(p, b);
	}
	b->state = MEM_BLK_USED;
	b->locked = 0;
	b->fence = 0;
	b->owner = owner;
	if (owner)
		*owner = p->base + b->ofs;
	p->used += b->size;
	if (p->used > p->peak)
		p->peak = p->used;
	return b;
}

void mem_pool_free(mem_pool_t *p, mem_blk_t *b, uint32_t fence)
{
	if (!b || b->state != MEM_BLK_USED)
		return;
	if (b->owner)
		*b->owner = NULL;
	b->owner = NULL;
	b->locked = 0;
	if (fence)
	{
		b->state = MEM_BLK_FENCED;
		b->fence = fence;
		p->fenced += b->size;
		return;
	}
	b->state = MEM_BLK_FREE;
	p->used -= b->size;
	p->nofit = 0xFFFFFFFFu;
	Coalesce(p, b);
}

void mem_pool_retire(mem_pool_t *p, uint32_t done_frame)
{
	mem_blk_t *b, *n;

	if (!p->fenced)
		return;
	for (b = p->head; b; b = n)
	{
		n = b->next;
		if (b->state != MEM_BLK_FENCED || (int32_t)(b->fence - done_frame) > 0)
			continue;
		b->state = MEM_BLK_FREE;
		p->used -= b->size;
		p->fenced -= b->size;
		p->nofit = 0xFFFFFFFFu;
		b = Coalesce(p, b);
		n = b->next;
	}
}

uint32_t mem_pool_largest(const mem_pool_t *p)
{
	const mem_blk_t *b;
	uint32_t best = 0;

	for (b = p->fhead; b; b = b->fnext)
		if (b->size > best)
			best = b->size;
	return best;
}

uint32_t mem_pool_free_bytes(const mem_pool_t *p)
{
	return p->size - p->used;
}

/*
 * Slide the live blocks of [start, end) down onto the free space among
 * them, leaving one hole at the top; stop before a block that would take
 * the bytes moved past max_move (the hole then sits below it: a later call
 * goes on from there). Stops below a locked or fenced block (the caller's
 * range holds none). Returns the bytes moved.
 */
uint32_t mem_pool_pack(mem_pool_t *p, uint32_t start, uint32_t end, uint32_t max_move)
{
	mem_blk_t *b, *n, *last = NULL, *h;
	uint32_t cur, top = 0, moved = 0;
	int freed = 0;

	for (b = p->head; b && b->ofs + b->size <= start; b = b->next)
		last = b;
	if (!b)
		return 0;
	cur = b->ofs;
	for (; b && b->ofs < end; b = n)
	{
		n = b->next;
		if (b->state == MEM_BLK_FENCED || (b->state == MEM_BLK_USED && b->locked))
			break;
		if (b->state == MEM_BLK_FREE)
		{
			top = b->ofs + b->size;
			if (b->prev)
				b->prev->next = n;
			else
				p->head = n;
			if (n)
				n->prev = b->prev;
			FreeUnlink(p, b);
			DescPut(p, b);
			freed = 1;
			continue;
		}
		if (b->ofs > cur && moved + b->size > max_move)
			break;
		top = b->ofs + b->size;	/* the processed extent's end */
		if (b->ofs > cur)
		{
			memmove(p->base + cur, p->base + b->ofs, b->size);
			sys_cache_writeback(p->base + cur, b->size);
			moved += b->size;
			b->ofs = cur;
			if (b->owner)
				*b->owner = p->base + cur;
		}
		cur = b->ofs + b->size;
		last = b;
	}
	if (freed)
	{
		/* a free descriptor was just returned */
		h = DescGet(p);
		h->ofs = cur;
		h->size = top - cur;
		h->state = MEM_BLK_FREE;
		h->prev = last;
		h->next = b;
		if (last)
			last->next = h;
		else
			p->head = h;
		if (b)
			b->prev = h;
		Coalesce(p, h);
	}
	p->moved += moved;
	p->nofit = 0xFFFFFFFFu;
	p->compactions++;
	return moved;
}
