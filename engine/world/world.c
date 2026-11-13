/* SPDX-License-Identifier: MIT */
/*
 * world.c -- campaign map table, layer open, chunk residency and point ->
 * leaf (docs/architecture.md 5.3-5.5, docs/formats/w64-format.md "Runtime
 * requirements").
 *
 * The directory byte swap (host), the CRC and the layer bookkeeping follow
 * the CLEAN ranges of the old tree's n64/port/w64_world.c (audit entry
 * w64_world.c: 29-345, 355-441); its flagged ranges (submodel table,
 * submodel hull setup, hull walks, point-in-leaf, touched leaves, vis
 * decompression, fat PVS) are not used: point -> leaf here is written from
 * w64-format.md (COLL node encoding, grid start node) and architecture.md
 * 5.5. Layers open through engine/io in stages (header, directory with
 * an incremental CRC, tables, pinned hull 0, ENTS scan): asynchronously
 * for seams, finished at once under load I/O for wld_open. Chunk
 * requests carry their residency class (architecture.md 5.3): a request
 * evicts queued-not-started reads of weaker classes first, then the LRU
 * of chunks of weaker classes (unwanted ones first) not used this frame
 * or the last; prefetch never evicts a chunk used in the last 30 frames.
 * The class policy (target set, pins, prefetch) is engine/world/stream.c.
 *
 * Every cooked byte is untrusted (architecture.md 18.2): the header,
 * directory tables, chunk ranges and each chunk's internal offsets are
 * checked before use; a failure is sys_error(file, offset, rule).
 */
#include <stdio.h>
#include <string.h>

#include "hl64/limits.h"
#include "engine/core/log.h"
#include "engine/core/sys.h"
#include "engine/io/io.h"
#include "engine/mem/carve.h"
#include "engine/mem/pool.h"
#include "engine/render/gpu.h"
#include "engine/world/wld.h"
#include "engine/world/wld_int.h"

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define WLD_SWAP	1
#else
#define WLD_SWAP	0
#endif

enum { POOL_RENDER, POOL_TEXTURE, POOL_COLL, NPOOLS };

#define DESC_RENDER	192
#define DESC_TEXTURE	160
#define DESC_COLL	128
#define DESC_LAYERS	8

typedef struct {
	char		name[HL64_MAP_NAME];
	float		gofs[3];
} wld_map_t;

static wld_map_t	maps[HL64_MAX_MAPS];
static int		nmaps, start_map;
static wld_layer_t	layers[HL64_WORLD_LAYERS];
/* tools/cook/layerdirs.py proves the layer region with these sizes */
_Static_assert(sizeof(wld_chunk_t) == 20, "wld_chunk_t: update tools/cook/layerdirs.py");
_Static_assert(sizeof(wld_mover_t) == 16, "wld_mover_t: update tools/cook/layerdirs.py");

static mem_pool_t	pools[NPOOLS], layer_pool;
static mem_blk_t	desc_render[DESC_RENDER], desc_texture[DESC_TEXTURE];
static mem_blk_t	desc_coll[DESC_COLL], desc_layers[DESC_LAYERS];
static io_req_t		reqs[HL64_WORLD_REQS];
uint32_t		wld_frame_no = 1;
uint16_t		wld_tgen = 1;
uint32_t		wld_layer_gen;
uint32_t		wld_n_promoted, wld_n_evict_queued, wld_n_prefetch_req;
uint32_t		wld_n_open_async, wld_n_open_fail;
static wld_stats_t	st;

/* ---- big-endian access, byte swap (host) --------------------------------- */

static uint16_t be16(const uint8_t *p)
{
	return (uint16_t)(p[0] << 8 | p[1]);
}

static uint32_t be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static float bef(const uint8_t *p)
{
	union { uint32_t u; float f; } v;

	v.u = be32(p);
	return v.f;
}

static uint16_t le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

#if WLD_SWAP
static void S16(void *p, uint32_t n)
{
	uint16_t *q = p;

	while (n--)
	{
		*q = __builtin_bswap16(*q);
		q++;
	}
}

static void S32(void *p, uint32_t n)
{
	uint32_t *q = p;

	while (n--)
	{
		*q = __builtin_bswap32(*q);
		q++;
	}
}
#endif

/* ---- CRC-32 (zlib polynomial, reflected) -------------------------------- */

static uint32_t crc_table[256];

static void crc_init(void)
{
	uint32_t i, c;
	int k;

	for (i = 0; i < 256; i++)
	{
		c = i;
		for (k = 0; k < 8; k++)
			c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
		crc_table[i] = c;
	}
}

static uint32_t crc_update(uint32_t c, const uint8_t *p, uint32_t n)
{
	while (n--)
		c = crc_table[(c ^ *p++) & 0xFF] ^ (c >> 8);
	return c;
}

#ifdef HL64_HOST
static uint32_t crc32(const uint8_t *p, uint32_t n)
{
	return ~crc_update(0xFFFFFFFFu, p, n);
}
#endif

/* ---- campaign map table ----------------------------------------------- */

static void bad(const char *file, uint32_t ofs, const char *rule)
{
	sys_error("%s: offset %lu: %s", file, (unsigned long)ofs, rule);
}

int wld_init(void)
{
	static const char path[] = "maps/campaign.cwg";
	uint8_t *b = mem_region(MEM_SCRATCH);
	uint32_t size, rom = sys_cart_addr(path, &size), cap = mem_region_bytes(MEM_SCRATCH);
	uint32_t n_lm, n_ln, n_seam, n_twin, n_pool, strbytes, strofs, i;
	int k;

	crc_init();
	wld_style_reset();
	if (!rom)
	{
		/* no player content (the M0 gate's ROM): boot without a world */
		log_printf("world: no %s: cook the player's content (make cook)\n", path);
		return 0;
	}
	if (size < 32 || size > cap)
		bad(path, 0, "size");
	io_read_sync(b, rom, (size + 1) & ~1u);
	if (be32(b) != 0x43574733u || be16(b + 4) != 3)
		bad(path, 0, "not CWG3");
	nmaps = be16(b + 8);
	n_lm = be16(b + 10);
	n_ln = be16(b + 12);
	start_map = be16(b + 14);
	n_seam = be16(b + 20);
	n_twin = be16(b + 22);
	n_pool = be16(b + 24);
	strbytes = be32(b + 28);
	strofs = 32 + 48u * nmaps + 16 * n_lm + 8 * n_ln + 136 * n_seam + 4 * n_twin + 2 * n_pool;
	if (nmaps < 1 || nmaps > HL64_MAX_MAPS)
		bad(path, 8, "map count");
	if ((uint64_t)strofs + strbytes != size || !strbytes || b[size - 1])
		bad(path, 28, "table sizes");
	if (start_map >= nmaps)
		start_map = 0;
	for (i = 0; i < (uint32_t)nmaps; i++)
	{
		const uint8_t *m = b + 32 + 48 * i;
		uint32_t s = be16(m);
		size_t len;

		if (s >= strbytes)
			bad(path, 32 + 48 * i, "map name");
		len = strlen((const char *)b + strofs + s);
		if (!len || len >= HL64_MAP_NAME)
			bad(path, 32 + 48 * i, "map name length");
		memcpy(maps[i].name, b + strofs + s, len + 1);
		for (k = 0; k < 3; k++)
			maps[i].gofs[k] = bef(m + 12 + 4 * k);
	}

	mem_pool_init(&pools[POOL_RENDER], "render", mem_region(MEM_POOL_RENDER),
		mem_region_bytes(MEM_POOL_RENDER), desc_render, DESC_RENDER);
	mem_pool_init(&pools[POOL_TEXTURE], "texture", mem_region(MEM_POOL_TEXTURE),
		mem_region_bytes(MEM_POOL_TEXTURE), desc_texture, DESC_TEXTURE);
	mem_pool_init(&pools[POOL_COLL], "collision", mem_region(MEM_POOL_COLLISION),
		mem_region_bytes(MEM_POOL_COLLISION), desc_coll, DESC_COLL);
	for (i = 0; i < HL64_WORLD_REQS; i++)
		reqs[i].user = -1;
	mem_pool_init(&layer_pool, "layerdirs", mem_region(MEM_LAYER_DIRS),
		mem_region_bytes(MEM_LAYER_DIRS), desc_layers, DESC_LAYERS);
	log_printf("world: %d maps, start %s\n", nmaps, maps[start_map].name);
	return 1;
}

int wld_map_count(void)
{
	return nmaps;
}

const char *wld_map_name(int map)
{
	return map >= 0 && map < nmaps ? maps[map].name : "";
}

const float *wld_map_origin(int map)
{
	return maps[map].gofs;
}

int wld_map_find(const char *name)
{
	int i;

	for (i = 0; i < nmaps; i++)
		if (!strcmp(maps[i].name, name))
			return i;
	return -1;
}

int wld_start_map(void)
{
	return start_map;
}

/* ---- directory ------------------------------------------------------------ */

#if WLD_SWAP
static void swap_header(w64_header_t *h)
{
	S32(&h->magic, 1);
	S16(&h->version, 2);
	S32(&h->file_size, 5);
	S16(&h->idx_stride, 4 + 6 + 2);
}
#endif

/* 64-bit range check: [ofs, ofs + n * elem) inside [0, size) */
static int inside(uint64_t ofs, uint64_t n, uint64_t elem, uint64_t size)
{
	return ofs <= size && n * elem <= size - ofs;
}

/* Byte-swap (host) and bounds-check the directory tables in place. */
static void parse_dir(wld_layer_t *L, uint8_t *d, const char *file)
{
	const uint32_t dsz = L->hdr.dir_size, dofs = L->hdr.dir_ofs;
	w64_dir_t *dir = (w64_dir_t *)d;
	w64_grid_t *g;
	uint64_t ncell;
	uint32_t i, end;

#define NEED(cond, at, rule) do { if (!(cond)) bad(file, dofs + (uint32_t)(at), rule); } while (0)
/* every table 8-aligned (w64-format.md) and inside the directory */
#define TABLE(ofs, n, elem) (inside((ofs), (n), (elem), dsz) && !((ofs) & 7))
#if WLD_SWAP
	S16(dir, 8);
	S32(&dir->chunkdir_ofs, 7);
	S16(&dir->coll_first, 6);
	S32(&dir->model_ofs, 4);
#endif
	NEED(dsz >= sizeof(w64_dir_t), 0, "directory size");
	NEED(dir->nchunks == L->hdr.nchunks, 10, "chunk count");
	NEED(TABLE(dir->chunkdir_ofs, dir->nchunks, 16), 16, "chunk table");
	NEED(TABLE(dir->sect_ofs, dir->nsect, 20), 20, "sector table");
	NEED(TABLE(dir->tex_ofs, dir->ntex, 12), 28, "texture table");
	NEED(TABLE(dir->sub_ofs, dir->nsub, 16), 32, "submodel table");
	NEED(TABLE(dir->leaf_ofs, dir->nleaf, 4), 36, "leaf table");
	NEED(TABLE(dir->model_ofs, dir->nsub, 64), 56, "model table");
	NEED(TABLE(dir->grid_ofs, 1, sizeof(w64_grid_t)), 64, "grid header");
	NEED(dir->nleaf >= 2 && dir->nsect && dir->nsub, 0, "counts");
	NEED((uint32_t)dir->texp_first + dir->npages <= dir->nchunks, 12, "pages");
	NEED(dir->npages <= HL64_REN_PAGES_MAX && dir->ntex <= HL64_REN_TEX_MAX, 4,
		"too many pages / textures");
	NEED(dir->coll_pinned >= 1 && dir->coll_pinned <= dir->ncoll
		&& (uint32_t)dir->coll_first + dir->ncoll <= dir->nchunks, 44, "COLL range");
	NEED(dir->ents_chunk == W64_NONE || dir->ents_chunk < dir->nchunks, 52, "ENTS chunk");
#if WLD_SWAP
	S32(d + dir->chunkdir_ofs, 4u * dir->nchunks);
	S16(d + dir->sect_ofs, 10u * dir->nsect);
	for (i = 0; i < dir->ntex; i++)
	{
		w64_tex_t *t = (w64_tex_t *)(d + dir->tex_ofs) + i;

		S16(&t->page, 1);
		S16(&t->pad, 3);
	}
	S16(d + dir->sub_ofs, 8u * dir->nsub);
	S32(d + dir->leaf_ofs, dir->nleaf);
	for (i = 0; i < dir->nsub; i++)
	{
		w64_model_t *m = (w64_model_t *)(d + dir->model_ofs) + i;

		S32(m->mins, 9 + 4 + 1);
		S16(&m->coll_first, 2);
	}
#endif
	L->dir = dir;
	L->chunks = (const w64_chunk_t *)(d + dir->chunkdir_ofs);
	L->sects = (const w64_sector_t *)(d + dir->sect_ofs);
	L->texs = (const w64_tex_t *)(d + dir->tex_ofs);
	L->subs = (const w64_sub_t *)(d + dir->sub_ofs);
	L->leafvis = (const uint32_t *)(d + dir->leaf_ofs);
	L->models = (const w64_model_t *)(d + dir->model_ofs);

	for (i = 0; i < dir->nchunks; i++)
	{
		const w64_chunk_t *c = &L->chunks[i];
		uint32_t min = c->fourcc == W64_CHUNK_TEXP ? 48 : c->fourcc == W64_CHUNK_COLL ? 16 : 24;

		NEED(inside(c->ofs, c->size, 1, L->size) && !(c->ofs & 15) && c->ofs >= dofs + dsz
			&& c->size >= min && c->size < 0x1000000, dir->chunkdir_ofs + 16 * i, "chunk range");
	}
	for (i = 0; i < dir->npages; i++)
		NEED(L->chunks[dir->texp_first + i].fourcc == W64_CHUNK_TEXP,
			dir->chunkdir_ofs + 16 * (dir->texp_first + i), "TEXP chunk");
	for (i = 0; i < dir->ncoll; i++)
		NEED(L->chunks[dir->coll_first + i].fourcc == W64_CHUNK_COLL,
			dir->chunkdir_ofs + 16 * (dir->coll_first + i), "COLL chunk");
	/* page pool: every sector's slice */
	end = 0;
	for (i = 0; i < dir->nsect; i++)
	{
		const w64_sector_t *s = &L->sects[i];

		NEED(s->rend_chunk < dir->nchunks
			&& L->chunks[s->rend_chunk].fourcc == W64_CHUNK_REND,
			dir->sect_ofs + 20 * i, "sector REND chunk");
		if ((uint32_t)s->page_first + s->page_count > end)
			end = (uint32_t)s->page_first + s->page_count;
	}
	NEED(TABLE(dir->pagepool_ofs, end, 2), 24, "page pool");
#if WLD_SWAP
	S16(d + dir->pagepool_ofs, end);
#endif
	L->pagepool = (const uint16_t *)(d + dir->pagepool_ofs);
	for (i = 0; i < end; i++)
		NEED(L->pagepool[i] < dir->npages, dir->pagepool_ofs + 2 * i, "page pool entry");
	for (i = 0; i < dir->ntex; i++)
	{
		const w64_tex_t *t = &L->texs[i];

		NEED(t->page == W64_NONE || t->page < dir->npages, dir->tex_ofs + 12 * i, "texture page");
		NEED(t->anim_next < dir->ntex || !(t->flags & W64_TEXF_ANIM), dir->tex_ofs + 12 * i,
			"animation next");
		NEED(t->alternate < dir->ntex || t->alternate == W64_NONE, dir->tex_ofs + 12 * i,
			"animation alternate");
		NEED(t->anim_min < t->anim_max || !t->anim_total, dir->tex_ofs + 12 * i,
			"animation range");
	}
	for (i = 0; i < dir->nsub; i++)
		NEED(L->subs[i].kind != W64_SUB_MOVR || (L->subs[i].chunk < dir->nchunks
			&& L->chunks[L->subs[i].chunk].fourcc == W64_CHUNK_MOVR),
			dir->sub_ofs + 16 * i, "submodel MOVR chunk");
	/* vis pool: every leaf's list */
	end = 0;
	for (i = 0; i < dir->nleaf; i++)
	{
		uint32_t o = L->leafvis[i];

		NEED(TABLE(dir->vispool_ofs, (uint64_t)o + 1, 2), dir->leaf_ofs + 4 * i, "leaf vis list");
		NEED(TABLE(dir->vispool_ofs, (uint64_t)o + 1 + be16(d + dir->vispool_ofs + 2 * o), 2),
			dir->leaf_ofs + 4 * i, "leaf vis list");
		if (o + 1 + be16(d + dir->vispool_ofs + 2 * o) > end)
			end = o + 1 + be16(d + dir->vispool_ofs + 2 * o);
	}
#if WLD_SWAP
	S16(d + dir->vispool_ofs, end);
#endif
	L->vispool = (const uint16_t *)(d + dir->vispool_ofs);
	for (i = 0; i < dir->nleaf; i++)
	{
		const uint16_t *v = L->vispool + L->leafvis[i];
		uint32_t k;

		for (k = 1; k <= v[0]; k++)
			NEED(v[k] < dir->nsect, dir->vispool_ofs + 2 * (L->leafvis[i] + k), "vis sector");
	}
	/* collision grid: cells, start table, list pool */
	g = (w64_grid_t *)(d + dir->grid_ofs);
#if WLD_SWAP
	S16(g, 8);
	S32(&g->cells_ofs, 3);
#endif
	ncell = (uint64_t)g->dims[0] * g->dims[1] * g->dims[2];
	NEED(TABLE(g->cells_ofs, ncell, 4) && TABLE(g->start_ofs, g->nstart, 4)
		&& g->cell == W64_GRID_CELL, dir->grid_ofs, "grid tables");
#if WLD_SWAP
	S16(d + g->cells_ofs, (uint32_t)(2 * ncell));
	S32(d + g->start_ofs, g->nstart);
#endif
	L->grid = g;
	L->cells = (const w64_gcell_t *)(d + g->cells_ofs);
	L->starts = (const int32_t *)(d + g->start_ofs);
	for (i = 0; i < ncell; i++)
		NEED(L->cells[i].start < g->nstart, g->cells_ofs + 4 * i, "grid start");
	NEED(L->models[0].visleafs >= 1 && L->models[0].visleafs + 1 == dir->nleaf,
		dir->model_ofs + 52, "visleafs");
	/* grid list pool: every cell's list (count, coll index...) */
	end = 1;
	for (i = 0; i < ncell; i++)
	{
		uint32_t o = L->cells[i].list;

		NEED(TABLE(g->lists_ofs, (uint64_t)o + 1, 2), g->cells_ofs + 4 * i, "grid list");
		NEED(TABLE(g->lists_ofs, (uint64_t)o + 1 + be16(d + g->lists_ofs + 2 * o), 2),
			g->cells_ofs + 4 * i, "grid list");
		if (o + 1 + be16(d + g->lists_ofs + 2 * o) > end)
			end = o + 1 + be16(d + g->lists_ofs + 2 * o);
	}
	NEED(TABLE(g->lists_ofs, end, 2), dir->grid_ofs + 24, "grid list pool");
#if WLD_SWAP
	S16(d + g->lists_ofs, end);
#endif
	L->glists = (const uint16_t *)(d + g->lists_ofs);
	for (i = 0; i < ncell; i++)
	{
		const uint16_t *v = L->glists + L->cells[i].list;
		uint32_t k;

		for (k = 1; k <= v[0]; k++)
			NEED(v[k] < dir->ncoll, g->lists_ofs + 2 * (L->cells[i].list + k), "grid list coll");
	}
	/* leaf contents: CONTENTS_* (-1 .. -16) per BSP leaf */
	NEED(dir->nleafs_all >= dir->nleaf && TABLE(dir->leafc_ofs, dir->nleafs_all, 1), 60,
		"leaf contents");
	L->leafc = (const int8_t *)(d + dir->leafc_ofs);
	for (i = 0; i < dir->nleafs_all; i++)
		NEED(L->leafc[i] <= -1 && L->leafc[i] >= -16, dir->leafc_ofs + i, "leaf contents value");
	/* surface materials (optional) */
	L->mat = NULL;
	if (dir->mat_ofs)
	{
		w64_mat_t *m = (w64_mat_t *)(d + dir->mat_ofs);
		const uint16_t *st;

		NEED(TABLE(dir->mat_ofs, 1, sizeof(w64_mat_t)), 68, "material table");
#if WLD_SWAP
		S16(m, 4);
		S32(&m->start_ofs, 4);
#endif
		NEED(TABLE(m->start_ofs, (uint32_t)dir->ncoll + 1, 2) && TABLE(m->ent_ofs, m->nent, 2)
			&& TABLE(m->mix_ofs, m->nmix, 8) && TABLE(m->face_ofs, m->nface, 10),
			dir->mat_ofs, "material tables");
#if WLD_SWAP
		S16(d + m->start_ofs, (uint32_t)dir->ncoll + 1);
		S16(d + m->ent_ofs, m->nent);
		for (i = 0; i < m->nmix; i++)
		{
			w64_matmix_t *x = (w64_matmix_t *)(d + m->mix_ofs) + i;

			S32(&x->gid, 1);
			S16(&x->first, 2);
		}
		S16(d + m->face_ofs, 5u * m->nface);
#endif
		st = (const uint16_t *)(d + m->start_ofs);
		for (i = 0; i < dir->ncoll; i++)
			NEED(st[i] <= st[i + 1] && st[i + 1] <= m->nent, m->start_ofs + 2 * i,
				"material start");
		for (i = 0; i < m->nmix; i++)
		{
			const w64_matmix_t *x = (const w64_matmix_t *)(d + m->mix_ofs) + i;

			NEED((uint32_t)x->first + x->count <= m->nface && x->count, m->mix_ofs + 8 * i,
				"material mix");
		}
		L->mat = m;
		L->mat_start = st;
		L->mat_ent = (const uint16_t *)(d + m->ent_ofs);
		L->mat_mix = (const w64_matmix_t *)(d + m->mix_ofs);
		L->mat_face = (const w64_matface_t *)(d + m->face_ofs);
	}
#undef TABLE
#undef NEED
}

/* ---- chunk validation and byte swap --------------------------------------- */

static void check_coll(wld_layer_t *L, int c, uint8_t *p)
{
	const w64_chunk_t *ck = &L->chunks[c];
	w64_coll_t *h = (w64_coll_t *)p;
	w64_cnode_t *nd;
	uint32_t i;

#if WLD_SWAP
	S16(h, 6);
#endif
	if (h->nnodes > W64_COLL_MAX_NODES || h->nx > h->ny || h->ny > h->nz || h->nz > h->nnodes
		|| 16u + 16u * h->nplanes + 8u * h->nnodes + 4u * h->nextern > ck->size)
		bad(maps[L->map].name, ck->ofs, "COLL header");
	nd = (w64_cnode_t *)(p + 16 + 16 * h->nplanes);
#if WLD_SWAP
	S32(p + 16, 4u * h->nplanes);
	for (i = 0; i < h->nnodes; i++)
	{
		S32(&nd[i].u, 1);
		S16(nd[i].child, 2);
	}
	S32(nd + h->nnodes, h->nextern);
#endif
	for (i = h->nz; i < h->nnodes; i++)
		if (nd[i].u.plane >= h->nplanes)
			bad(maps[L->map].name, ck->ofs + 16 + 16 * h->nplanes + 8 * i, "COLL plane");
	for (i = 0; i < h->nnodes; i++)
		if (nd[i].child[0] >= h->nnodes + h->nextern || nd[i].child[1] >= h->nnodes + h->nextern)
			bad(maps[L->map].name, ck->ofs + 16 + 16 * h->nplanes + 8 * i, "COLL child");
}

/*
 * REND / MOVR (w64-format.md): header, meshlet records, style records,
 * then the vertex region [vert_ofs, idx_ofs), the index region
 * [idx_ofs, style_ofs), style luxels and probes after style_ofs. Each
 * meshlet's vertices and strip lie in their region after the previous
 * meshlet's, so the regions are swapped (host) and re-encoded (cart) once.
 */
static void check_rend(wld_layer_t *L, int c, uint8_t *p)
{
	const w64_chunk_t *ck = &L->chunks[c];
	const char *file = maps[L->map].name;
	w64_rend_t *r = (w64_rend_t *)p;
	w64_meshlet_t *m;
	w64_style_t *s;
	uint32_t i, size = ck->size, vend, iend;

#if WLD_SWAP
	S16(r, 2);
	S32(&r->vert_ofs, 4);
#endif
	if (24u + 24u * r->nmeshlets + 8u * r->nstyled > r->vert_ofs || r->vert_ofs > r->idx_ofs
		|| r->idx_ofs > r->style_ofs || r->style_ofs > size || (r->vert_ofs & 15)
		|| ((r->idx_ofs - r->vert_ofs) & 31) || ((r->style_ofs - r->idx_ofs) & 1)
		|| (r->probe_ofs && (r->probe_ofs < r->style_ofs || !inside(r->probe_ofs, 1, 6, size))))
		bad(file, ck->ofs, "REND header");
	m = (w64_meshlet_t *)(p + 24);
	s = (w64_style_t *)(m + r->nmeshlets);
#if WLD_SWAP
	for (i = 0; i < r->nmeshlets; i++)
	{
		S16(&m[i], 6);
		S32(&m[i].vert, 2);
		S16(&m[i].tex, 2);
	}
	for (i = 0; i < r->nstyled; i++)
		S32(&s[i].data_ofs, 1);
	if (r->probe_ofs)
		S16(p + r->probe_ofs, 3);
	{
		w64_vert_t *v = (w64_vert_t *)(p + r->vert_ofs);
		uint32_t k, nk = (r->idx_ofs - r->vert_ofs) / 32;

		for (k = 0; k < nk; k++)
		{
			S16(v[k].posA, 8);
			S32(&v[k].rgbaA, 2);
			S16(v[k].stA, 4);
		}
		S16(p + r->idx_ofs, (r->style_ofs - r->idx_ofs) / 2);
	}
#endif
	vend = r->vert_ofs;
	iend = r->idx_ofs;
	for (i = 0; i < r->nmeshlets; i++)
	{
		uint32_t nv = (uint32_t)W64_MESHLET_NV(&m[i]), ni = (uint32_t)W64_MESHLET_NI(&m[i]);
		uint32_t vo = W64_MESHLET_VOFS(&m[i]), io = W64_MESHLET_IOFS(&m[i]);

		if (nv < 3 || nv > W64_MAX_MESHLET_VERTS || ni < 3 || ni > W64_MAX_MESHLET_IDX
			|| (vo & 15) || (io & 7) || vo < vend || vo + 16 * ((nv + 1) & ~1u) > r->idx_ofs
			|| io < iend || io + 2 * ni > r->style_ofs || m[i].tex >= L->dir->ntex
			|| (m[i].style != W64_NONE && m[i].style >= r->nstyled))
			bad(file, ck->ofs + 24 + 24 * i, "meshlet");
		vend = vo + 16 * ((nv + 1) & ~1u);
		iend = io + 2 * ni;
		if (m[i].style != W64_NONE)
		{
			const w64_style_t *st0 = &s[m[i].style];
			uint32_t ns = 0;

			while (ns < 4 && st0->styles[ns] != 255)
				ns++;
			if (!ns || st0->data_ofs < r->style_ofs || !inside(st0->data_ofs, nv, ns, size))
				bad(file, ck->ofs + 24 + 24 * r->nmeshlets + 8 * m[i].style, "style record");
		}
		if (L->reencode)
			gpu_index_reencode((int16_t *)(p + io), (int)ni, L->hdr.idx_stride, L->hdr.idx_base);
	}
}

static void check_texp(wld_layer_t *L, int c, uint8_t *p)
{
	const w64_chunk_t *ck = &L->chunks[c];
	w64_texp_t *t = (w64_texp_t *)p;

#if WLD_SWAP
	S16(t, 2);
	S32(&t->ci4_ofs, 1);
	S16(p + 16, 16);
#endif
	if (t->w < 8 || t->h < 1 || t->w > 1024 || t->h > 1024 || (t->w & 1) || t->ci4_ofs < 48
		|| (t->ci4_ofs & 7) || !inside(t->ci4_ofs, (uint32_t)t->w * t->h / 2, 1, ck->size))
		bad(maps[L->map].name, ck->ofs, "TEXP header");
}

/* ---- residency ------------------------------------------------------------- */

static int chunk_pool(uint32_t fourcc)
{
	return fourcc == W64_CHUNK_TEXP ? POOL_TEXTURE : fourcc == W64_CHUNK_COLL ? POOL_COLL
		: POOL_RENDER;
}

static int cls_prio(int cls)
{
	return cls >= WLD_CLS_OPEN ? IO_PRIO_OPEN : cls == WLD_CLS_PIN ? IO_PRIO_SEAM
		: IO_PRIO_PREFETCH;
}

static int prio_cls(int prio)
{
	return prio == IO_PRIO_OPEN ? WLD_CLS_OPEN : prio == IO_PRIO_SEAM ? WLD_CLS_PIN
		: WLD_CLS_PREFETCH;
}

static void landed(wld_layer_t *L, int c, uint8_t *p)
{
	uint32_t fc = L->chunks[c].fourcc;

#ifdef HL64_HOST
	/* chunk CRCs: always on the host (architecture.md 18.2) */
	if (crc32(p, L->chunks[c].size) != L->chunks[c].crc)
		bad(maps[L->map].name, L->chunks[c].ofs, "chunk CRC");
#endif
	if (fc == W64_CHUNK_ENTS)
		return;		/* scanned by the open (scan_ents) */
	if (fc == W64_CHUNK_TEXP)
		check_texp(L, c, p);
	else if (fc == W64_CHUNK_COLL)
		check_coll(L, c, p);
	else
	{
		check_rend(L, c, p);
		wld_style_chunk_landed(L, c);
	}
	sys_cache_writeback(p, L->chunks[c].size);
}

/* Never evicted: world hull 0 while the layer is open, ENTS until the
 * open scanned it. */
static int pinned(const wld_layer_t *L, int c)
{
	return (c >= L->dir->coll_first && c < L->dir->coll_first + L->dir->coll_pinned)
		|| (c == L->dir->ents_chunk && L->ready != 1);
}

static void req_done(io_req_t *r)
{
	wld_layer_t *L = &layers[r->user >> 16];
	int c = r->user & 0xFFFF;
	wld_chunk_t *ch = &L->ch[c];

	r->user = -1;
	ch->blk->locked = 0;
	ch->state = pinned(L, c) ? WLD_CH_PINNED : WLD_CH_RESIDENT;
	ch->used = wld_frame_no;	/* not the first eviction candidate before its draw */
	st.loads++;
	landed(L, c, ch->data);
}

static io_req_t *req_of(const wld_layer_t *L, int c)
{
	int key = (int)L->slot << 16 | c, i;

	for (i = 0; i < HL64_WORLD_REQS; i++)
		if (reqs[i].user == key)
			return &reqs[i];
	return NULL;
}

/* Free chunk c's bytes: REND / TEXP / MOVR behind a fence two frames ahead
 * (the GPU may still read them); COLL bytes only the CPU reads, and no
 * walk holds a chunk it did not use this frame, so they free at once, as
 * do the blocks of reads that never landed. */
static void drop(wld_layer_t *L, int c)
{
	wld_chunk_t *ch = &L->ch[c];
	int p = chunk_pool(L->chunks[c].fourcc);

	mem_pool_free(&pools[p], ch->blk, p == POOL_COLL || ch->state == WLD_CH_LOADING ? 0
		: wld_frame_no + 2);
	ch->blk = NULL;
	ch->data = NULL;
	ch->state = WLD_CH_ABSENT;
}

/* Cancel a queued read nothing has landed of. */
static void cancel_queued(io_req_t *r)
{
	wld_layer_t *L = &layers[r->user >> 16];

	io_cancel(r);
	drop(L, r->user & 0xFFFF);
	r->user = -1;
	wld_n_evict_queued++;
}

/* The weakest queued-not-started read (of pool p, or any pool when p < 0)
 * whose class is weaker than cls; NULL none. */
static io_req_t *weaker_queued(int p, int cls)
{
	io_req_t *best = NULL;
	int i;

	for (i = 0; i < HL64_WORLD_REQS; i++)
	{
		io_req_t *r = &reqs[i];

		if (r->user < 0 || r->state != IO_QUEUED || r->done || prio_cls(r->prio) >= cls
			|| (p >= 0 && chunk_pool(layers[r->user >> 16].chunks[r->user & 0xFFFF].fourcc) != p))
			continue;
		if (!best || r->prio > best->prio)
			best = r;
	}
	return best;
}

/* Make room in pool p for a request of class cls: a queued-not-started
 * read of a weaker class first, then the least recently used resident
 * chunk of a weaker class (not named this generation = NONE first) not
 * used this frame or the last; prefetch never evicts a chunk used in the
 * last 30 frames. 0 when nothing qualifies. */
static int evict_one(int p, int cls)
{
	io_req_t *q = weaker_queued(p, cls);
	wld_layer_t *bl = NULL;
	int bc = -1, bk = 0, i, c;

	if (q)
	{
		cancel_queued(q);
		return 1;
	}
	for (i = 0; i < HL64_WORLD_LAYERS; i++)
	{
		wld_layer_t *L = &layers[i];

		if (!L->open || !L->ch)
			continue;
		for (c = 0; c < L->dir->nchunks; c++)
		{
			const wld_chunk_t *ch = &L->ch[c];
			int k;

			if (ch->state != WLD_CH_RESIDENT || ch->used + 1 >= wld_frame_no
				|| chunk_pool(L->chunks[c].fourcc) != p)
				continue;
			k = ch->want == wld_tgen ? ch->cls : WLD_CLS_NONE;
			if (k >= cls || (cls == WLD_CLS_PREFETCH && ch->used + 30 >= wld_frame_no))
				continue;
			if (bc < 0 || k < bk || (k == bk && ch->used < bl->ch[bc].used))
			{
				bl = L;
				bc = c;
				bk = k;
			}
		}
	}
	if (bc < 0)
		return 0;
	drop(bl, bc);
	st.evictions++;
	return 1;
}

/* Queue chunk c at class cls's priority; 0 when no request slot or no
 * room could be made (the caller retries next frame). */
static int request(wld_layer_t *L, int c, int cls)
{
	const w64_chunk_t *ck = &L->chunks[c];
	wld_chunk_t *ch = &L->ch[c];
	int p = chunk_pool(ck->fourcc), i;
	uint32_t size = (ck->size + 15) & ~15u;
	io_req_t *r = NULL;

	for (i = 0; i < HL64_WORLD_REQS && !r; i++)
		if (reqs[i].user < 0)
			r = &reqs[i];
	if (!r)
	{
		/* every slot busy: a weaker queued read gives its slot up */
		if (!(r = weaker_queued(-1, cls)))
			return 0;
		cancel_queued(r);
	}
	while (!(ch->blk = mem_pool_alloc(&pools[p], size, &ch->data)))
	{
		const mem_pool_t *pl = &pools[p];
		io_req_t *q = weaker_queued(p, cls);

		/* a queued-not-started read of a weaker class gives its room
		 * up first: its block frees at once (nothing landed in it) */
		if (q)
		{
			cancel_queued(q);
			continue;
		}
		/* evicted REND / TEXP bytes free behind their fence: once the
		 * fenced bytes cover the request, wait for them (two frames)
		 * instead of evicting the rest of the pool */
		if ((pl->fenced && pl->size - pl->used + pl->fenced >= size) || !evict_one(p, cls))
		{
			st.nofit++;
			return 0;
		}
	}
	ch->blk->locked = 1;
	ch->state = WLD_CH_LOADING;
	memset(r, 0, sizeof(*r));
	r->rom = L->rom + ck->ofs;
	r->dst = ch->data;
	r->len = size;
	r->prio = (uint8_t)cls_prio(cls);
	r->cb = req_done;
	r->user = (int)L->slot << 16 | c;
	io_submit(r);
	st.queued++;
	if (cls == WLD_CLS_PREFETCH)
		wld_n_prefetch_req++;
	return 1;
}

/* A loading chunk is needed at class cls: promote its queued read. */
static void promote(wld_layer_t *L, int c, int cls)
{
	io_req_t *r = req_of(L, c);

	if (r && cls_prio(cls) < r->prio)
	{
		io_promote(r, cls_prio(cls));
		wld_n_promoted++;
	}
}

uint8_t *wld_chunk_use(wld_layer_t *L, int c)
{
	wld_chunk_t *ch = &L->ch[c];

	if (ch->state >= WLD_CH_RESIDENT)
	{
		ch->used = wld_frame_no;
		return ch->data;
	}
	if (ch->state == WLD_CH_ABSENT)
		request(L, c, WLD_CLS_OPEN);
	else
		promote(L, c, WLD_CLS_OPEN);
	return NULL;
}

int wld_name(wld_layer_t *L, int c, int cls)
{
	wld_chunk_t *ch = &L->ch[c];

	if (ch->want != wld_tgen)
	{
		ch->want = wld_tgen;
		ch->cls = (uint8_t)cls;
	}
	else if (cls > ch->cls)
		ch->cls = (uint8_t)cls;
	if (ch->state == WLD_CH_ABSENT)
		return request(L, c, ch->cls);
	if (ch->state == WLD_CH_LOADING)
		promote(L, c, ch->cls);
	return 1;
}

void wld_want_open(wld_layer_t *L, int leaf)
{
	const uint16_t *list;
	int i, k, cnt;

	if (!++wld_tgen)
		wld_tgen = 1;
	L->want_leaf = leaf;
	list = wld_leaf_sectors(L, leaf, &cnt);
	for (i = 0; i < cnt; i++)
	{
		const w64_sector_t *s = &L->sects[list[i]];

		wld_name(L, s->rend_chunk, WLD_CLS_OPEN);
		for (k = 0; k < s->page_count; k++)
			wld_name(L, L->dir->texp_first + L->pagepool[s->page_first + k], WLD_CLS_OPEN);
	}
}

uint8_t *wld_chunk_sync(wld_layer_t *L, int c)
{
	const w64_chunk_t *ck = &L->chunks[c];
	wld_chunk_t *ch = &L->ch[c];
	uint32_t size = (ck->size + 15) & ~15u;
	io_req_t *r;

	if (ch->state == WLD_CH_LOADING && (r = req_of(L, c)))
		io_finish(r);	/* req_done lands it */
	if (ch->state == WLD_CH_ABSENT)
	{
		while (!(ch->blk = mem_pool_alloc(&pools[POOL_COLL], size, &ch->data)))
			if (!evict_one(POOL_COLL, WLD_CLS_OPEN))
				sys_error("world: %s: COLL chunk %d does not fit the collision pool",
					maps[L->map].name, c);
		io_read_sync(ch->data, L->rom + ck->ofs, size);
		ch->state = WLD_CH_RESIDENT;
		st.loads++;
		landed(L, c, ch->data);
	}
	ch->used = wld_frame_no;
	return ch->data;
}

void wld_frame(uint32_t frame)
{
	int p;

	wld_frame_no = frame + 1;	/* 0 means never used */
	for (p = 0; p < NPOOLS; p++)
		mem_pool_retire(&pools[p], wld_frame_no);
	mem_pool_retire(&layer_pool, wld_frame_no);
}

void wld_stats(wld_stats_t *s)
{
	int p;

	*s = st;
	for (p = 0; p < NPOOLS; p++)
	{
		s->pool_used[p] = pools[p].used;
		s->pool_peak[p] = pools[p].peak;
	}
	s->dirs_used = layer_pool.used;
	s->dirs_peak = layer_pool.peak;
	s->dirs_size = layer_pool.size;
}

/* ---- ENTS scan: mover placements, player start (M1) ----------------------- */

/* HLE1 (docs/formats/hle-format-v1.md), little-endian as cooked; read in
 * place with byte accessors, every offset checked. */
static int hle_str(const uint8_t *b, uint32_t size, uint32_t strings, uint32_t s, const char *want)
{
	size_t n = strlen(want) + 1;

	return (uint64_t)strings + s + n <= size && !memcmp(b + strings + s, want, n);
}

static int hle_vec(const uint8_t *b, uint32_t floats, uint32_t nfloats, uint32_t i, float out[3])
{
	union { uint32_t u; float f; } v;
	int k;

	if ((uint64_t)i + 3 > nfloats)
		return 0;
	for (k = 0; k < 3; k++)
	{
		v.u = le32(b + floats + 4 * (i + (uint32_t)k));
		out[k] = v.f;
	}
	return 1;
}

/* b: the ENTS chunk's bytes (resident in the render pool while the open
 * scans it). */
static void scan_ents(wld_layer_t *L, const uint8_t *b)
{
	const char *file = maps[L->map].name;
	const w64_chunk_t *ck = &L->chunks[L->dir->ents_chunk];
	uint32_t size, nrecs, nkn, nkeys, nfloats, strsize, ncls;
	uint32_t o_recs, o_cls, o_kn, o_keys, o_floats, o_str, i, k;
	int kn_model = -1, kn_origin = -1, kn_angles = -1, kn_rmode = -1;
	int kn_style = -1, kn_flags = -1, kn_pattern = -1;

	size = ck->size;
	if (size < 96)
		bad(file, ck->ofs, "ENTS size");
	if (le32(b) != 0x31454C48u || le16(b + 4) != 1 || (le16(b + 6) & 1) || le32(b + 8) > size)
		bad(file, ck->ofs, "ENTS not a little-endian HLE1 v1 chunk");
	size = le32(b + 8);
	nrecs = le16(b + 18);
	ncls = le16(b + 20);
	nkn = le16(b + 22);
	nkeys = le32(b + 28);
	nfloats = le32(b + 40);
	strsize = le32(b + 44);
	o_recs = le32(b + 52);
	o_cls = le32(b + 60);
	o_kn = le32(b + 64);
	o_keys = le32(b + 76);
	o_floats = le32(b + 88);
	o_str = le32(b + 92);
	if ((uint64_t)o_recs + 24u * nrecs > size || (uint64_t)o_cls + 8u * ncls > size
		|| (uint64_t)o_kn + 8u * nkn > size || (uint64_t)o_keys + 8u * nkeys > size
		|| (uint64_t)o_floats + 4u * nfloats > size || (uint64_t)o_str + strsize > size)
		bad(file, ck->ofs, "ENTS tables");
	for (k = 0; k < nkn; k++)
	{
		uint32_t s = le32(b + o_kn + 8 * k);

		if (hle_str(b, size, o_str, s, "model"))
			kn_model = (int)k;
		else if (hle_str(b, size, o_str, s, "origin"))
			kn_origin = (int)k;
		else if (hle_str(b, size, o_str, s, "angles"))
			kn_angles = (int)k;
		else if (hle_str(b, size, o_str, s, "rendermode"))
			kn_rmode = (int)k;
		else if (hle_str(b, size, o_str, s, "style"))
			kn_style = (int)k;
		else if (hle_str(b, size, o_str, s, "spawnflags"))
			kn_flags = (int)k;
		else if (hle_str(b, size, o_str, s, "pattern"))
			kn_pattern = (int)k;
	}
	for (i = 0; i < nrecs; i++)
	{
		const uint8_t *r = b + o_recs + 24 * i;
		uint32_t cls = le16(r + 2), kf = le16(r + 8), kc = le16(r + 10);
		float org[3] = { 0, 0, 0 }, ang[3] = { 0, 0, 0 };
		int sub = -1, rmode = 0, style = -1, flags = 0;
		uint32_t pattern = ~0u;

		if (cls >= ncls || kf + kc > nkeys)
			bad(file, ck->ofs + o_recs + 24 * i, "ENTS record");
		for (k = kf; k < kf + kc; k++)
		{
			uint32_t kn = le16(b + o_keys + 8 * k), val = le32(b + o_keys + 8 * k + 4);
			union { uint32_t u; float f; } v;

			if ((int)kn == kn_model && (uint64_t)o_str + val + 2 < size
				&& b[o_str + val] == '*')
			{
				const uint8_t *s = b + o_str + val + 1;
				int n = 0;

				while (s < b + size && *s >= '0' && *s <= '9' && n < 0x10000)
					n = n * 10 + (*s++ - '0');
				if (s < b + size && !*s)
					sub = n;
			}
			else if ((int)kn == kn_origin)
				hle_vec(b, o_floats, nfloats, val, org);
			else if ((int)kn == kn_angles)
				hle_vec(b, o_floats, nfloats, val, ang);
			else if ((int)kn == kn_rmode)
			{
				v.u = val;
				rmode = v.f != 0.0f;
			}
			else if ((int)kn == kn_style || (int)kn == kn_flags)
			{
				v.u = val;
				if (v.f >= 0 && v.f < 65536)
					*((int)kn == kn_style ? &style : &flags) = (int)v.f;
			}
			else if ((int)kn == kn_pattern && (uint64_t)o_str + val < size)
				pattern = val;
		}
		/* the first info_player_start in lump order: HLSDK
		 * EntSelectSpawnPoint (single player, no startspot) takes
		 * UTIL_FindEntityByClassname(NULL, ...) */
		if (!L->has_start
			&& hle_str(b, size, o_str, le32(b + o_cls + 8 * cls), "info_player_start"))
		{
			memcpy(L->start_org, org, sizeof(org));
			memcpy(L->start_ang, ang, sizeof(ang));
			L->has_start = 1;
		}
		/* M1 map-start lightstyles of the switchable lights (the rule of
		 * w64-format.md's probe values: their `pattern`, else "a" with
		 * START_OFF, else "m"); the game's light entities own them from
		 * M3 on */
		if (style >= 32 && style < 64 && (hle_str(b, size, o_str, le32(b + o_cls + 8 * cls), "light")
			|| hle_str(b, size, o_str, le32(b + o_cls + 8 * cls), "light_spot")
			|| hle_str(b, size, o_str, le32(b + o_cls + 8 * cls), "light_fluoro")))
		{
			static const uint8_t on = 12, off = 0;
			int slot = wld_style_slot(L, style), n = 0;
			uint8_t *pat = L->patterns[style - 32];

			if (pattern != ~0u)
				while (n < WLD_PATTERN_MAX && o_str + pattern + (uint32_t)n < size
					&& b[o_str + pattern + n] >= 'a' && b[o_str + pattern + n] <= 'z')
				{
					pat[n] = (uint8_t)(b[o_str + pattern + n] - 'a');
					n++;
				}
			if (flags & 1)
				wld_style_set(slot, &off, 1);	/* START_OFF first */
			else if (n)
				wld_style_set(slot, pat, n);
			else
				wld_style_set(slot, &on, 1);
		}
		/* M1 static placement: the origin key, no rotation (brush
		 * entities spawn unrotated except rotating doors / func_rotating,
		 * whose closed pose is their model); translucent render modes
		 * are the translucent pass's (M3) */
		if (sub > 0 && sub < L->dir->nsub && L->subs[sub].kind == W64_SUB_MOVR && !rmode
			&& L->nmovers < L->dir->nsub)
		{
			wld_mover_t *mv = &L->movers[L->nmovers++];

			memcpy(mv->origin, org, sizeof(org));
			mv->sub = (uint16_t)sub;
		}
	}
}

/* ---- layers ----------------------------------------------------------------- */

/* An open in flight (one per slot): the header / directory read, the
 * incremental directory CRC, retries of a full layer region. */
enum { OS_IDLE, OS_HDR, OS_DIR, OS_CRC, OS_PIN, OS_ENTS, OS_DONE };
typedef struct {
	io_req_t	req;
	uint8_t		hb[64] __attribute__((aligned(16)));
	uint8_t		stage, tries;
	uint32_t	crc, crc_pos;
	void		*dir;		/* directory bytes (L->mem's block; its owner) */
	void		*tab;		/* tables bytes (L->tables' block; its owner) */
} open_t;
static open_t opens[HL64_WORLD_LAYERS];

#define OPEN_TRIES	8	/* frames a full layer region may wait for fenced frees */

static void *carve(uint8_t **cur, uint32_t bytes)
{
	void *p = *cur;

	*cur += (bytes + 15) & ~15u;
	return p;
}

uint32_t wld_layer_bytes(uint32_t dir_size, uint32_t nchunks, uint32_t npages, uint32_t nsub)
{
	return ((dir_size + 15) & ~15u) + ((nchunks * (uint32_t)sizeof(wld_chunk_t) + 15) & ~15u)
		+ WLD_STYLE_SLOTS * 4 + ((npages * WLD_PAGE_STATE + 15) & ~15u)
		+ ((nsub * (uint32_t)sizeof(wld_mover_t) + 15) & ~15u);
}

wld_layer_t *wld_slot(int s)
{
	return &layers[s];
}

static const char *pkg_path(const wld_layer_t *L)
{
	static char path[HL64_MAP_NAME + 16];

	snprintf(path, sizeof(path), "maps/%s.w64", maps[L->map].name);
	return path;
}

/* Layer-region block for L: both of a layer's blocks at one end, the other
 * open layer's at the other, so the free space between them stays one
 * hole and any two layers whose sum fits the region open (the cook proof,
 * tools/cook/layerdirs.py). */
static mem_blk_t *layer_alloc(wld_layer_t *L, uint32_t bytes, void **owner)
{
	return L->high ? mem_pool_alloc_high(&layer_pool, bytes, owner)
		: mem_pool_alloc(&layer_pool, bytes, owner);
}

static void open_fail(wld_layer_t *L, const char *what, uint32_t bytes)
{
	log_printf("world: open_fail %s: %s needs %lu B of the layer region (%lu B free, "
		"largest %lu B)\n", maps[L->map].name, what, (unsigned long)bytes,
		(unsigned long)mem_pool_free_bytes(&layer_pool),
		(unsigned long)mem_pool_largest(&layer_pool));
	L->ready = -1;
	opens[L->slot].stage = OS_DONE;
	wld_n_open_fail++;
}

/* Take a free slot for map and queue its header read; NULL when no slot. */
static wld_layer_t *open_begin(int map)
{
	wld_layer_t *L = NULL;
	open_t *o;
	int s, other_low = 0;

	if (map < 0 || map >= nmaps)
		sys_error("world: no map %d", map);
	if ((L = wld_layer_of_map(map)))
		return L;
	for (s = 0; s < HL64_WORLD_LAYERS; s++)
		if (!layers[s].open)
		{
			if (!L)
				L = &layers[s];
		}
		else if (!layers[s].high)
			other_low = 1;
	if (!L)
		return NULL;
	memset(L, 0, sizeof(*L));
	L->slot = (uint8_t)(L - layers);
	L->want_leaf = -1;
	L->map = (uint16_t)map;
	L->high = (uint8_t)other_low;
	memcpy(L->gofs, maps[map].gofs, sizeof(L->gofs));
	L->rom = sys_cart_addr(pkg_path(L), &L->size);
	if (!L->rom || L->size < 64 + sizeof(w64_dir_t))
		sys_error("%s missing: cook the player's content (make cook)", pkg_path(L));
	L->open = 1;
	o = &opens[L->slot];
	memset(o, 0, sizeof(*o));
	o->stage = OS_HDR;
	o->req.rom = L->rom;
	o->req.dst = o->hb;
	o->req.len = 64;
	o->req.prio = IO_PRIO_OPEN;
	o->req.user = -1;
	io_submit(&o->req);
	return L;
}

wld_layer_t *wld_open_async(int map)
{
	wld_layer_t *L = wld_layer_of_map(map);

	if (L)
		return L;
	if (!(L = open_begin(map)))
	{
		log_printf("world: open_fail %s: no free layer slot\n", wld_map_name(map));
		wld_n_open_fail++;
		return NULL;
	}
	wld_n_open_async++;
	return L;
}

int wld_layer_ready(const wld_layer_t *L)
{
	return L->ready;
}

/* Header landed: validate, take the directory block, queue its read. */
static void open_header(wld_layer_t *L, open_t *o)
{
	const char *path = pkg_path(L);
	uint32_t bytes;

	memcpy(&L->hdr, o->hb, 64);
#if WLD_SWAP
	swap_header(&L->hdr);
#endif
	if (L->hdr.magic != W64_MAGIC || L->hdr.version != W64_VERSION
		|| !(L->hdr.flags & W64_HF_COLL) || L->hdr.file_size != L->size)
		bad(path, 0, "not a version-2 .w64 with COLL of this size");
	if (L->hdr.dir_ofs != 64 || L->hdr.dir_size < sizeof(w64_dir_t)
		|| (uint64_t)L->hdr.dir_ofs + L->hdr.dir_size > L->size || L->hdr.lightscale_x100 == 0
		|| L->hdr.idx_stride == 0)
		bad(path, 12, "directory range");
	if (!(L->hdr.nchunks < 0xFFFF))
		bad(path, 20, "chunk count");
	bytes = (L->hdr.dir_size + 15) & ~15u;
	if (!(L->mem = layer_alloc(L, bytes, &o->dir)))
	{
		if (layer_pool.fenced && ++o->tries < OPEN_TRIES)
			return;		/* the region's fenced frees retire within two frames */
		open_fail(L, "directory", bytes);
		return;
	}
	memset(&o->req, 0, sizeof(o->req));
	o->req.rom = L->rom + L->hdr.dir_ofs;
	o->req.dst = o->dir;
	o->req.len = (L->hdr.dir_size + 1) & ~1u;
	o->req.prio = IO_PRIO_OPEN;
	o->req.user = -1;
	io_submit(&o->req);
	o->stage = OS_DIR;
	o->tries = 0;
}

/* Directory checked: the tables block (chunk records, style epochs,
 * per-page GPU state, movers). 0 while the region has no room yet. */
static int open_tables(wld_layer_t *L, open_t *o)
{
	uint32_t size = wld_layer_bytes(L->hdr.dir_size, L->dir->nchunks, L->dir->npages,
		L->dir->nsub) - ((L->hdr.dir_size + 15) & ~15u), i;
	uint8_t *cur;

	if (!(L->tables = layer_alloc(L, size, &o->tab)))
	{
		if (layer_pool.fenced && ++o->tries < OPEN_TRIES)
			return 0;
		open_fail(L, "tables", size);
		return 0;
	}
	cur = o->tab;
	L->ch = carve(&cur, L->dir->nchunks * (uint32_t)sizeof(wld_chunk_t));
	memset(L->ch, 0, L->dir->nchunks * sizeof(wld_chunk_t));
	for (i = 0; i < L->dir->nchunks; i++)
		L->ch[i].style_base = W64_NONE;
	L->style_epoch = carve(&cur, WLD_STYLE_SLOTS * 4);
	L->nstyle_slots = WLD_STYLE_SLOTS;
	L->page_state = carve(&cur, L->dir->npages * WLD_PAGE_STATE);
	memset(L->page_state, 0, (size_t)L->dir->npages * WLD_PAGE_STATE);
	L->movers = carve(&cur, L->dir->nsub * (uint32_t)sizeof(wld_mover_t));

	/* tiny3d index encoding self-check (one answer per package encoding) */
	L->reencode = (uint8_t)gpu_index_check(L->hdr.idx_stride, L->hdr.idx_base);
	/* common style 0 is the light the cook baked (header style0 = a
	 * letter's value, 264 = "m") until the game sets its pattern */
	{
		static uint8_t letter;

		if (L->hdr.style0 % 22 == 0 && L->hdr.style0 / 22 <= 25 && wld_style_value(0) == 256)
		{
			letter = (uint8_t)(L->hdr.style0 / 22);
			wld_style_set(0, &letter, 1);
		}
	}
	{
		int head = L->models[0].head[0];

		if (head >= 0 && W64_GID_COLL(head) >= L->dir->coll_pinned)
			bad(pkg_path(L), L->hdr.dir_ofs + L->dir->model_ofs + 36, "world hull 0 not pinned");
	}
	return 1;
}

void wld_open_step(wld_layer_t *L, uint32_t crc_budget)
{
	open_t *o = &opens[L->slot];
	uint32_t n, i;
	int all;

	switch (o->stage)
	{
	case OS_HDR:
		if (o->req.state != IO_DONE)
			return;
		open_header(L, o);
		return;
	case OS_DIR:
		if (o->req.state != IO_DONE)
			return;
		o->stage = OS_CRC;
		o->crc = 0xFFFFFFFFu;
		o->crc_pos = 0;
		/* fall through */
	case OS_CRC:
		if (o->crc_pos < L->hdr.dir_size)
		{
			n = L->hdr.dir_size - o->crc_pos;
			if (crc_budget && n > crc_budget)
				n = crc_budget;
			o->crc = crc_update(o->crc, (const uint8_t *)o->dir + o->crc_pos, n);
			o->crc_pos += n;
			if (o->crc_pos < L->hdr.dir_size)
				return;
			if (~o->crc != L->hdr.dir_crc)
				bad(pkg_path(L), 24, "directory CRC");
			parse_dir(L, o->dir, pkg_path(L));
		}
		if (!open_tables(L, o))
			return;
		o->stage = OS_PIN;
		/* fall through */
	case OS_PIN:
		for (all = 1, i = 0; i < L->dir->coll_pinned; i++)
		{
			int c = L->dir->coll_first + (int)i;

			if (L->ch[c].state == WLD_CH_ABSENT)
				request(L, c, WLD_CLS_OPEN);
			all &= L->ch[c].state == WLD_CH_PINNED;
		}
		if (!all)
			return;
		o->stage = OS_ENTS;
		/* fall through */
	case OS_ENTS:
		if (L->dir->ents_chunk != W64_NONE)
		{
			wld_chunk_t *ch = &L->ch[L->dir->ents_chunk];

			if (ch->state == WLD_CH_ABSENT)
				request(L, L->dir->ents_chunk, WLD_CLS_OPEN);
			if (ch->state != WLD_CH_PINNED)
				return;
			scan_ents(L, ch->data);
			drop(L, L->dir->ents_chunk);
		}
		L->ready = 1;
		o->stage = OS_DONE;
		wld_layer_gen++;
		log_printf("world: open %s slot %d: %u B dir, %u sectors, %u pages, %u chunks, "
			"%u movers, layer block %lu B\n", maps[L->map].name, L->slot,
			(unsigned)L->hdr.dir_size, L->dir->nsect, L->dir->npages, L->dir->nchunks,
			L->nmovers, (unsigned long)wld_layer_bytes(L->hdr.dir_size, L->dir->nchunks,
			L->dir->npages, L->dir->nsub));
		return;
	default:
		return;
	}
}

wld_layer_t *wld_open(int map)
{
	wld_layer_t *L = open_begin(map);
	open_t *o;
	int i, spins = 0;

	if (!L)
		sys_error("world: %d layers open", HL64_WORLD_LAYERS);
	o = &opens[L->slot];
	/* load I/O: land the open's reads now, stage by stage */
	io_set_load(1);
	while (!L->ready)
	{
		uint8_t stage = o->stage;

		if (o->req.state == IO_QUEUED || o->req.state == IO_ACTIVE)
			io_finish(&o->req);
		for (i = 0; i < HL64_WORLD_REQS; i++)
			if (reqs[i].user >= 0)
				io_finish(&reqs[i]);
		wld_open_step(L, 0);
		spins = o->stage == stage ? spins + 1 : 0;
		if (spins > OPEN_TRIES + 2)
			sys_error("world: %s: the open cannot proceed (no collision / render pool room)",
				pkg_path(L));
	}
	io_set_load(0);
	if (L->ready < 0)
		sys_error("world: %s: no layer-region room (see the open_fail line)", pkg_path(L));
	return L;
}

void wld_close(wld_layer_t *L)
{
	open_t *o;
	uint32_t c;

	if (!L || !L->open)
		return;
	o = &opens[L->slot];
	if (o->req.state == IO_QUEUED || o->req.state == IO_ACTIVE)
		io_cancel(&o->req);
	for (c = 0; L->ch && c < L->dir->nchunks; c++)
	{
		wld_chunk_t *ch = &L->ch[c];
		io_req_t *r;

		if (ch->state == WLD_CH_LOADING && (r = req_of(L, (int)c)))
		{
			io_cancel(r);
			r->user = -1;
		}
		if (ch->blk)
			mem_pool_free(&pools[chunk_pool(L->chunks[c].fourcc)], ch->blk, wld_frame_no + 2);
	}
	/* page_state holds RDP bind lists the RDP may still read */
	if (L->tables)
		mem_pool_free(&layer_pool, L->tables, wld_frame_no + 2);
	if (L->mem)
		mem_pool_free(&layer_pool, L->mem, 0);
	if (L->ch)
		for (c = 32; c < 64; c++)
			wld_style_set(wld_style_slot(L, (int)c), NULL, 0);
	wld_stream_closed(L->slot);
	o->stage = OS_IDLE;
	L->open = 0;
	L->ready = 0;
	L->ch = NULL;
	wld_layer_gen++;
}

wld_layer_t *wld_layer(int slot)
{
	return slot >= 0 && slot < HL64_WORLD_LAYERS && layers[slot].open && layers[slot].ready == 1
		? &layers[slot] : NULL;
}

wld_layer_t *wld_layer_of_map(int map)
{
	int s;

	for (s = 0; s < HL64_WORLD_LAYERS; s++)
		if (layers[s].open && layers[s].map == map)
			return &layers[s];
	return NULL;
}

/* ---- point -> leaf (architecture.md 5.5, w64-format.md COLL / grid) ------- */

int wld_point_leaf(const wld_layer_t *L, const float p[3])
{
	const w64_grid_t *g = L->grid;
	int32_t node = L->models[0].head[0];
	int k, cell = 0, steps, ix[3];

	/* cell = floor((p - origin) / 256); points outside walk from the root */
	for (k = 0; k < 3; k++)
	{
		float f = (p[k] - g->origin[k]) / (float)g->cell;

		if (!(f >= 0) || f >= g->dims[k])
			break;
		ix[k] = (int)f;
	}
	if (k == 3)
	{
		cell = ix[0] + g->dims[0] * (ix[1] + g->dims[1] * ix[2]);
		node = L->starts[L->cells[cell].start];
	}
	/* every hull-0 node is in a pinned chunk; the walk visits each node
	 * at most once (a tree), bounded by the pinned node total */
	for (steps = 0; node >= 0 && steps < L->dir->coll_pinned * W64_COLL_MAX_NODES; steps++)
	{
		uint32_t ci = W64_GID_COLL(node), li = W64_GID_LOCAL(node);
		const uint8_t *cp;
		const w64_coll_t *h;
		const w64_cnode_t *nd;
		float d;
		int child;

		if (ci >= L->dir->coll_pinned)
			sys_error("%s: hull-0 node %ld outside the pinned chunks", maps[L->map].name,
				(long)node);
		cp = L->ch[L->dir->coll_first + ci].data;
		h = (const w64_coll_t *)cp;
		if (li >= h->nnodes)
			sys_error("%s: hull-0 node %ld past its chunk", maps[L->map].name, (long)node);
		nd = (const w64_cnode_t *)(cp + 16 + 16 * h->nplanes) + li;
		if (li < h->nz)
			d = p[li < h->nx ? 0 : li < h->ny ? 1 : 2] - nd->u.dist;
		else
		{
			const w64_cplane_t *pl = (const w64_cplane_t *)(cp + 16) + nd->u.plane;

			d = pl->normal[0] * p[0] + pl->normal[1] * p[1] + pl->normal[2] * p[2] - pl->dist;
		}
		child = nd->child[d >= 0 ? 0 : 1];
		if (child < 0)
			return W64_LEAF_OF(child) < L->dir->nleaf ? W64_LEAF_OF(child) : 0;
		node = child < h->nnodes ? (int32_t)(ci << W64_GID_LOCAL_BITS | (uint32_t)child)
			: (int32_t)((const uint32_t *)(nd - li + h->nnodes))[child - h->nnodes];
	}
	if (node >= 0)
		return 0;
	return W64_LEAF_OF(node) < L->dir->nleaf ? W64_LEAF_OF(node) : 0;
}

const uint16_t *wld_leaf_sectors(const wld_layer_t *L, int leaf, int *count)
{
	const uint16_t *list;

	if (leaf < 0 || leaf >= L->dir->nleaf)
		leaf = 0;
	list = L->vispool + L->leafvis[leaf];
	*count = list[0];
	return list + 1;
}

/* ---- PVS rows (architecture.md 5.5; w64-format.md VISR) ---------------------- */

/* One decoded row per layer slot; only the offset pair and the row's own
 * compressed bytes are read (synchronously, counted as blocked I/O), the
 * VISR chunk itself is never resident. */
#define PVS_ROW_MAX	1024		/* BSP30: at most 8192 visleafs */
typedef struct {
	uint8_t		set;		/* map and visleafs valid */
	uint8_t		all;		/* the cached leaf has no row */
	uint16_t	map;
	int32_t		leaf;		/* cached leaf, -1 none */
	int32_t		visleafs;	/* 0: no vis */
	uint8_t		row[PVS_ROW_MAX];
} pvs_cache_t;
static pvs_cache_t pvs[HL64_WORLD_LAYERS];
/* PI destination: whole 16 B lines (architecture.md 4.3) */
static uint8_t pvs_buf[PVS_ROW_MAX + 64] __attribute__((aligned(16)));

/* Bytes [off, off + len) of L's VISR chunk. */
static const uint8_t *visr_read(const wld_layer_t *L, uint32_t off, uint32_t len)
{
	const w64_chunk_t *ck = &L->chunks[L->dir->visr_chunk];
	uint32_t a = ck->ofs + off, a0 = a & ~15u, n = (a - a0 + len + 15) & ~15u;

	if (off > ck->size || len > ck->size - off || n > sizeof(pvs_buf))
		bad(maps[L->map].name, a, "VISR row");
	io_read_sync(pvs_buf, L->rom + a0, n);
	return pvs_buf + (a - a0);
}

const uint8_t *wld_pvs_row(wld_layer_t *L, int leaf, int *visleafs)
{
	pvs_cache_t *c = &pvs[L->slot];
	const uint8_t *src, *end;
	uint32_t a, b, nbytes, o;

	if (!c->set || c->map != L->map)
	{
		c->set = 1;
		c->map = L->map;
		c->leaf = -1;
		c->visleafs = 0;
		if (L->dir->visr_chunk != W64_NONE)
		{
			c->visleafs = (int32_t)(be32(visr_read(L, 0, 4)) / 4u) - 1;
			if (c->visleafs < 0 || (uint32_t)(c->visleafs + 7) >> 3 > PVS_ROW_MAX)
				bad(maps[L->map].name, L->chunks[L->dir->visr_chunk].ofs, "VISR visleafs");
		}
	}
	*visleafs = c->visleafs;
	if (!c->visleafs || leaf <= 0 || leaf > c->visleafs)
		return NULL;
	if (c->leaf == leaf)
		return c->all ? NULL : c->row;
	src = visr_read(L, 4u * (uint32_t)(leaf - 1), 8);
	a = be32(src);
	b = be32(src + 4);
	c->leaf = leaf;
	c->all = a == b;
	if (c->all)
		return NULL;
	if (b < a)
		bad(maps[L->map].name, L->chunks[L->dir->visr_chunk].ofs, "VISR row order");
	nbytes = ((uint32_t)c->visleafs + 7u) >> 3;
	src = visr_read(L, a, b - a);
	end = src + (b - a);
	/* a non-zero byte is 8 literal bits; zero + count = that many zeros */
	for (o = 0; o < nbytes && src < end; src++)
	{
		if (*src)
			c->row[o++] = *src;
		else if (src + 1 < end)
		{
			uint32_t n = *++src;

			while (n-- && o < nbytes)
				c->row[o++] = 0;
		}
	}
	while (o < nbytes)
		c->row[o++] = 0;
	return c->row;
}
