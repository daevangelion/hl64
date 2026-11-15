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
#include "engine/ent/rec_int.h"
#include "engine/mem/carve.h"
#include "engine/save/sv.h"
#include "engine/save/svmod.h"
#include "hl64/game.h"

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

/* ---- saved games (architecture.md 14) ------------------------------------------- */

/* Every page in use: its free-block mask, then block by block a free
 * block as its free-list link only (alloc zeroes the rest before use), a
 * used one as a delta row against zeros after the game turned its
 * pointers into ids (game_priv_save on a copy). With the page classes,
 * use counts and free-list heads, handles and every later allocation
 * come back exactly. */
#define BLOCKS_MAX	(HL64_GAME_PRIVATE_KB * 1024u / 32u)
#define PAGE_MASK	(PAGE_BYTES / 32u / 8u)	/* mask bytes of a 32-byte-block page */
static uint32_t freebit[(BLOCKS_MAX + 31) / 32];

static void mark_free(void)
{
	int c;

	memset(freebit, 0, sizeof(freebit));
	for (c = 0; c < NCLASS; c++)
	{
		uint16_t h = S.head[c];
		uint32_t guard = 0;

		while (h != FREE_END && guard++ < BLOCKS_MAX)
		{
			uint32_t i = (uint32_t)(h - 1) / 2u;	/* 32-byte block index */

			freebit[i >> 5] |= 1u << (i & 31);
			memcpy(&h, block(h), 2);
		}
	}
}

static int owner_of(uint16_t h)
{
	int e;

	for (e = ent_next_live(-1); e >= 0; e = ent_next_live(e))
		if (ent_c.id[e].priv == h)
			return e;
	return -1;
}

static uint32_t priv_size(uint16_t h)
{
	return class_bytes(S.cls[(uint32_t)(block(h) - S.base) / PAGE_BYTES]);
}

/* The entity class of block b's owner, -1 none; *e the owner. */
static int block_class(const uint8_t *b, int *e)
{
	*e = owner_of(handle_of(b));
	return *e >= 0 && ent_c.id[*e].cls < RC__N ? (int)ent_c.id[*e].cls : -1;
}

/* Per entity class the handle of its last block saved / loaded (0 none):
 * a block is a delta row against it (blocks of a class share most of
 * their fields), the first of a class against zeros. */
static uint16_t cls_last[RC__N];

/* A used block as it is saved: the game's ids for its pointers. */
static int priv_copy(int e, const uint8_t *b, uint8_t *copy, uint32_t bs)
{
	memcpy(copy, b, bs);
	if (e >= 0 && game_priv_save(e, copy, bs))
	{
		log_printf("save: entity %d (%s): private block not savable\n", e, ent_class_name(e));
		sv_fail("private block not savable");
		return -1;
	}
	return 0;
}

void ent_priv_save_all(void)
{
	static uint8_t tmp[1024] __attribute__((aligned(16)));
	static uint8_t ref[1024] __attribute__((aligned(16)));
	uint32_t p;

	sv_u32(S.npages);
	sv_put(S.cls, S.npages);
	sv_put(S.used, 2u * S.npages);
	sv_put(S.head, sizeof(S.head));
	mark_free();
	memset(cls_last, 0, sizeof(cls_last));
	for (p = 0; p < S.npages; p++)
	{
		uint8_t mask[PAGE_MASK];
		uint32_t bs, o, k;

		if (S.cls[p] == CLASS_NONE)
			continue;
		bs = class_bytes(S.cls[p]);
		memset(mask, 0, sizeof(mask));
		for (o = 0, k = 0; o < PAGE_BYTES; o += bs, k++)
		{
			uint32_t i = (p * PAGE_BYTES + o) / 32u;

			if (freebit[i >> 5] & (1u << (i & 31)))
				mask[k >> 3] |= (uint8_t)(1u << (k & 7));
		}
		sv_put(mask, (PAGE_BYTES / bs + 7) / 8);
		for (o = 0, k = 0; o < PAGE_BYTES; o += bs, k++)
		{
			uint8_t *b = S.base + p * PAGE_BYTES + o;
			const uint8_t *r = NULL;
			int e, c;

			if (mask[k >> 3] & (1u << (k & 7)))
			{
				sv_put(b, 2);
				continue;
			}
			c = block_class(b, &e);
			if (priv_copy(e, b, tmp, bs))
				continue;
			if (c >= 0 && cls_last[c] && priv_size(cls_last[c]) == bs)
			{
				const uint8_t *pb = block(cls_last[c]);
				int pe;

				block_class(pb, &pe);
				if (!priv_copy(pe, pb, ref, bs))
					r = ref;
			}
			sv_put_delta(tmp, r, bs);
			if (c >= 0)
				cls_last[c] = handle_of(b);
		}
	}
}

int ent_priv_load_all(void)
{
	uint32_t p, n = sv_get_u32();
	int c;

	if (n != S.npages)
	{
		sv_fail("ent: private region size");
		return -1;
	}
	sv_get(S.cls, S.npages);
	sv_get(S.used, 2u * S.npages);
	sv_get(S.head, sizeof(S.head));
	memset(&S.st, 0, sizeof(S.st));
	memset(cls_last, 0, sizeof(cls_last));
	S.st.pages_total = S.npages;
	for (p = 0; p < S.npages && !sv_failed(); p++)
	{
		if (S.cls[p] != CLASS_NONE && S.cls[p] >= NCLASS)
		{
			sv_fail("ent: private page class");
			break;
		}
	}
	for (p = 0; p < S.npages && !sv_failed(); p++)
	{
		uint8_t mask[PAGE_MASK];
		uint32_t bs, o, k;

		if (S.cls[p] == CLASS_NONE)
			continue;
		bs = class_bytes(S.cls[p]);
		sv_get(mask, (PAGE_BYTES / bs + 7) / 8);
		for (o = 0, k = 0; o < PAGE_BYTES; o += bs, k++)
		{
			uint8_t *b = S.base + p * PAGE_BYTES + o;
			const uint8_t *r = NULL;
			int e, cl;

			if (mask[k >> 3] & (1u << (k & 7)))
			{
				memset(b, 0, bs);
				sv_get(b, 2);
				continue;
			}
			cl = block_class(b, &e);
			if (cl >= 0 && cls_last[cl] && priv_size(cls_last[cl]) == bs)
				r = block(cls_last[cl]);
			sv_get_delta(b, r, bs, NULL);
			if (cl >= 0)
				cls_last[cl] = handle_of(b);
		}
		S.st.pages++;
		S.st.live[S.cls[p]] += S.used[p];
		S.st.bytes_live += S.used[p] * bs;
	}
	for (c = 0; c < NCLASS; c++)
	{
		S.st.peak[c] = S.st.live[c];
		if (S.head[c] != FREE_END && (S.head[c] == 0
			|| (uint32_t)(S.head[c] - 1) * 16u >= S.npages * PAGE_BYTES))
			sv_fail("ent: private free list");
	}
	S.st.pages_peak = S.st.pages;
	S.st.bytes_peak = S.st.bytes_live;
	return sv_failed() ? -1 : 0;
}

int ent_priv_fixup(void)
{
	int e;

	for (e = ent_next_live(-1); e >= 0; e = ent_next_live(e))
	{
		uint16_t h = ent_c.id[e].priv;

		if (!h)
			continue;
		if ((uint32_t)(h - 1) * 16u >= S.npages * PAGE_BYTES
			|| S.cls[(uint32_t)(h - 1) * 16u / PAGE_BYTES] >= NCLASS)
		{
			log_printf("load: entity %d: private handle %u\n", e, (unsigned)h);
			return -1;
		}
		if (game_priv_load(e, block(h), priv_size(h)))
		{
			log_printf("load: entity %d (%s): private block of another build\n", e,
				ent_class_name(e));
			return -1;
		}
	}
	return 0;
}
