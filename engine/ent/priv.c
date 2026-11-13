/* SPDX-License-Identifier: MIT */
/*
 * priv.c -- game private blocks (docs/architecture.md 6.2, 4.3): one
 * zeroed block per entity from size-class slabs (32, 64, 128, 256, 512,
 * 1024 B) in the GAME_PRIVATE region.
 *
 * The region is cut into 4 KB pages; a class takes a free page when its
 * free list is empty and gives it back when its last block is freed, so
 * no fixed split between the classes has to be guessed. A block handle
 * (ent_id_t.priv) is its byte offset / 16 + 1 (0 = none).
 */
#include <string.h>

#include "engine/core/log.h"
#include "engine/core/sys.h"
#include "engine/ent/ent.h"
#include "engine/mem/carve.h"

#define PAGE_BYTES	4096u
#define PAGES_MAX	((HL64_GAME_PRIVATE_KB * 1024u) / PAGE_BYTES)
#define NCLASS		6
#define CLASS_NONE	0xFF
#define FREE_END	0xFFFF

static struct {
	uint8_t		*base;
	uint32_t	npages;
	uint8_t		cls[PAGES_MAX];		/* class of a page, CLASS_NONE free */
	uint16_t	used[PAGES_MAX];	/* live blocks in the page */
	uint16_t	head[NCLASS];		/* free block list (handle), FREE_END */
	ent_priv_stats_t st;
} S;

static uint32_t class_bytes(int c)
{
	return 32u << c;
}

/* Free blocks are chained through their first two bytes (a handle). */
static uint8_t *block(uint16_t h)
{
	return S.base + (uint32_t)(h - 1) * 16u;
}

static uint16_t handle_of(const uint8_t *p)
{
	return (uint16_t)((uint32_t)(p - S.base) / 16u + 1);
}

void ent_priv_init(void)
{
	S.base = mem_region(MEM_GAME_PRIVATE);
	S.npages = mem_region_bytes(MEM_GAME_PRIVATE) / PAGE_BYTES;
	if (S.npages > PAGES_MAX)
		S.npages = PAGES_MAX;
}

void ent_priv_reset(void)
{
	int c;
	uint32_t p;

	for (p = 0; p < S.npages; p++)
	{
		S.cls[p] = CLASS_NONE;
		S.used[p] = 0;
	}
	for (c = 0; c < NCLASS; c++)
		S.head[c] = FREE_END;
	memset(&S.st, 0, sizeof(S.st));
	S.st.pages_total = S.npages;
}

static int take_page(int c)
{
	uint32_t p, n, i;

	for (p = 0; p < S.npages && S.cls[p] != CLASS_NONE; p++)
		;
	if (p == S.npages)
		return 0;
	S.cls[p] = (uint8_t)c;
	S.used[p] = 0;
	n = PAGE_BYTES / class_bytes(c);
	/* chain the page's blocks, lowest address first */
	for (i = n; i-- > 0;)
	{
		uint8_t *b = S.base + p * PAGE_BYTES + i * class_bytes(c);
		uint16_t next = S.head[c];

		memcpy(b, &next, 2);
		S.head[c] = handle_of(b);
	}
	if (++S.st.pages > S.st.pages_peak)
		S.st.pages_peak = S.st.pages;
	return 1;
}

/* Give a page whose blocks are all free back: unchain its blocks. */
static void drop_page(uint32_t p)
{
	int c = S.cls[p];
	uint16_t prev = FREE_END, h = S.head[c];
	const uint8_t *lo = S.base + p * PAGE_BYTES, *hi = lo + PAGE_BYTES;

	while (h != FREE_END)
	{
		uint8_t *b = block(h);
		uint16_t next;

		memcpy(&next, b, 2);
		if (b >= lo && b < hi)
		{
			if (prev == FREE_END)
				S.head[c] = next;
			else
				memcpy(block(prev), &next, 2);
		}
		else
			prev = h;
		h = next;
	}
	S.cls[p] = CLASS_NONE;
	S.st.pages--;
}

void *ent_priv_alloc(int e, uint32_t size)
{
	ent_id_t *id = &ent_c.id[e];
	uint8_t *b;
	uint16_t next;
	int c;

	if (size > class_bytes(NCLASS - 1))
		sys_error("ent %d (%s): private block of %lu B (max %lu)", e, ent_class_name(e),
			(unsigned long)size, (unsigned long)class_bytes(NCLASS - 1));
	ent_priv_free(e);
	for (c = 0; class_bytes(c) < size; c++)
		;
	if (S.head[c] == FREE_END && !take_page(c))
	{
		S.st.refused++;
		return NULL;
	}
	b = block(S.head[c]);
	memcpy(&next, b, 2);
	S.head[c] = next;
	memset(b, 0, class_bytes(c));
	S.used[(uint32_t)(b - S.base) / PAGE_BYTES]++;
	id->priv = handle_of(b);
	if (++S.st.live[c] > S.st.peak[c])
		S.st.peak[c] = S.st.live[c];
	S.st.bytes_live += class_bytes(c);
	if (S.st.bytes_live > S.st.bytes_peak)
		S.st.bytes_peak = S.st.bytes_live;
	return b;
}

void *ent_priv(int e)
{
	uint16_t h = ent_c.id[e].priv;

	return h ? block(h) : NULL;
}

void ent_priv_free(int e)
{
	uint16_t h = ent_c.id[e].priv;
	uint8_t *b;
	uint32_t p;
	int c;

	if (!h)
		return;
	ent_c.id[e].priv = 0;
	b = block(h);
	p = (uint32_t)(b - S.base) / PAGE_BYTES;
	c = S.cls[p];
	memcpy(b, &S.head[c], 2);
	S.head[c] = h;
	S.st.live[c]--;
	S.st.bytes_live -= class_bytes(c);
	if (--S.used[p] == 0)
		drop_page(p);
}

void ent_priv_stats(ent_priv_stats_t *s)
{
	*s = S.st;
}
