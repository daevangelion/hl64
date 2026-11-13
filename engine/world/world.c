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
 * 5.5. The residency is new and minimal for M1 (OPEN class only, LRU
 * eviction behind a two-frame fence); M4 adds pins, prefetch and the
 * change-driven target set.
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

static mem_pool_t	pools[NPOOLS], layer_pool;
static mem_blk_t	desc_render[DESC_RENDER], desc_texture[DESC_TEXTURE];
static mem_blk_t	desc_coll[DESC_COLL], desc_layers[DESC_LAYERS];
static io_req_t		reqs[HL64_WORLD_REQS];
static uint32_t		frame_no = 1;
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

static uint32_t crc32(const uint8_t *p, uint32_t n)
{
	uint32_t c = 0xFFFFFFFFu;

	while (n--)
		c = crc_table[(c ^ *p++) & 0xFF] ^ (c >> 8);
	return ~c;
}

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
	/* collision grid: cells and start table (lists are M2's) */
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

static void landed(wld_layer_t *L, int c, uint8_t *p)
{
	uint32_t fc = L->chunks[c].fourcc;

#ifdef HL64_HOST
	/* chunk CRCs: always on the host (architecture.md 18.2) */
	if (crc32(p, L->chunks[c].size) != L->chunks[c].crc)
		bad(maps[L->map].name, L->chunks[c].ofs, "chunk CRC");
#endif
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

static void req_done(io_req_t *r)
{
	wld_layer_t *L = &layers[r->user >> 16];
	int c = r->user & 0xFFFF;
	wld_chunk_t *ch = &L->ch[c];

	r->user = -1;
	ch->blk->locked = 0;
	ch->state = WLD_CH_RESIDENT;
	ch->used = frame_no;	/* not the first eviction candidate before its draw */
	st.loads++;
	landed(L, c, ch->data);
}

/* Evict the least recently used resident chunk of pool p not used this
 * frame or the last; freed behind a fence two frames ahead. */
static int evict_one(int p)
{
	wld_layer_t *bl = NULL;
	int bc = -1, i, c;

	for (i = 0; i < HL64_WORLD_LAYERS; i++)
	{
		wld_layer_t *L = &layers[i];

		if (!L->open)
			continue;
		for (c = 0; c < L->dir->nchunks; c++)
		{
			const wld_chunk_t *ch = &L->ch[c];

			if (ch->state != WLD_CH_RESIDENT || ch->used + 1 >= frame_no
				|| ch->want == L->want_gen || chunk_pool(L->chunks[c].fourcc) != p)
				continue;
			if (bc < 0 || ch->used < bl->ch[bc].used)
			{
				bl = L;
				bc = c;
			}
		}
	}
	if (bc < 0)
		return 0;
	mem_pool_free(&pools[p], bl->ch[bc].blk, frame_no + 2);
	bl->ch[bc].blk = NULL;
	bl->ch[bc].data = NULL;
	bl->ch[bc].state = WLD_CH_ABSENT;
	st.evictions++;
	return 1;
}

static void request(wld_layer_t *L, int c)
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
		return;			/* next frame */
	while (!(ch->blk = mem_pool_alloc(&pools[p], size, &ch->data)))
		if (!evict_one(p))
		{
			st.nofit++;
			return;		/* fenced space frees in two frames */
		}
	ch->blk->locked = 1;
	ch->state = WLD_CH_LOADING;
	memset(r, 0, sizeof(*r));
	r->rom = L->rom + ck->ofs;
	r->dst = ch->data;
	r->len = size;
	r->prio = IO_PRIO_OPEN;
	r->cb = req_done;
	r->user = (int)L->slot << 16 | c;
	io_submit(r);
	st.queued++;
}

uint8_t *wld_chunk_use(wld_layer_t *L, int c)
{
	wld_chunk_t *ch = &L->ch[c];

	if (ch->state >= WLD_CH_RESIDENT)
	{
		ch->used = frame_no;
		return ch->data;
	}
	if (ch->state == WLD_CH_ABSENT)
		request(L, c);
	return NULL;
}

static void want(wld_layer_t *L, int c, int *pending)
{
	wld_chunk_t *ch = &L->ch[c];

	ch->want = L->want_gen;
	if (ch->state == WLD_CH_ABSENT)
	{
		request(L, c);
		if (ch->state == WLD_CH_ABSENT)
			*pending = 1;
	}
}

void wld_want_open(wld_layer_t *L, int leaf)
{
	const uint16_t *list;
	int i, k, cnt, pending = 0;

	if (leaf == L->want_leaf && !L->want_pending)
		return;
	L->want_leaf = leaf;
	if (!++L->want_gen)
		L->want_gen = 1;
	list = wld_leaf_sectors(L, leaf, &cnt);
	for (i = 0; i < cnt; i++)
	{
		const w64_sector_t *s = &L->sects[list[i]];

		want(L, s->rend_chunk, &pending);
		for (k = 0; k < s->page_count; k++)
			want(L, L->dir->texp_first + L->pagepool[s->page_first + k], &pending);
	}
	L->want_pending = (uint8_t)pending;
}

void wld_frame(uint32_t frame)
{
	int p;

	frame_no = frame + 1;	/* 0 means never used */
	for (p = 0; p < NPOOLS; p++)
		mem_pool_retire(&pools[p], frame_no);
	mem_pool_retire(&layer_pool, frame_no);
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

static void scan_ents(wld_layer_t *L)
{
	const char *file = maps[L->map].name;
	const w64_chunk_t *ck;
	uint8_t *b = mem_region(MEM_HLE_BLOBS);
	uint32_t size, nrecs, nkn, nkeys, nfloats, strsize, ncls;
	uint32_t o_recs, o_cls, o_kn, o_keys, o_floats, o_str, i, k;
	int kn_model = -1, kn_origin = -1, kn_angles = -1, kn_rmode = -1;
	int kn_style = -1, kn_flags = -1, kn_pattern = -1;

	if (L->dir->ents_chunk == W64_NONE)
		return;
	ck = &L->chunks[L->dir->ents_chunk];
	size = ck->size;
	if (size > mem_region_bytes(MEM_HLE_BLOBS) || size < 96)
		bad(file, ck->ofs, "ENTS size");
	io_read_sync(b, L->rom + ck->ofs, (size + 1) & ~1u);
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
		if (hle_str(b, size, o_str, le32(b + o_cls + 8 * cls), "info_player_start"))
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

static void *carve(uint8_t **cur, uint32_t bytes)
{
	void *p = *cur;

	*cur += (bytes + 15) & ~15u;
	return p;
}

wld_layer_t *wld_open(int map)
{
	char path[HL64_MAP_NAME + 16];
	wld_layer_t *L = NULL;
	uint8_t hb[64] __attribute__((aligned(16)));
	void *mem;
	uint8_t *cur;
	uint32_t bytes, i, size;
	int s;

	if (map < 0 || map >= nmaps)
		sys_error("world: no map %d", map);
	for (s = 0; s < HL64_WORLD_LAYERS; s++)
		if (layers[s].open && layers[s].map == map)
			return &layers[s];
	for (s = 0; s < HL64_WORLD_LAYERS && !L; s++)
		if (!layers[s].open)
			L = &layers[s];
	if (!L)
		sys_error("world: %d layers open", HL64_WORLD_LAYERS);
	memset(L, 0, sizeof(*L));
	L->slot = (uint8_t)(L - layers);
	L->want_leaf = -1;
	L->map = (uint16_t)map;
	memcpy(L->gofs, maps[map].gofs, sizeof(L->gofs));
	snprintf(path, sizeof(path), "maps/%s.w64", maps[map].name);
	L->rom = sys_cart_addr(path, &L->size);
	if (!L->rom || L->size < 64 + sizeof(w64_dir_t))
		sys_error("%s missing: cook the player's content (make cook)", path);

	io_set_load(1);
	io_read_sync(hb, L->rom, 64);
	memcpy(&L->hdr, hb, 64);
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

	/* layer region: the directory block, then one block of the tables
	 * sized from it (chunk records, style epochs, per-page GPU state,
	 * movers) */
	bytes = (L->hdr.dir_size + 15) & ~15u;
	L->mem = mem_pool_alloc(&layer_pool, bytes, &mem);
	if (!L->mem)
		sys_error("world: %s: directory needs %lu B of the layer region (%lu B free)", path,
			(unsigned long)bytes, (unsigned long)mem_pool_largest(&layer_pool));
	io_read_sync(mem, L->rom + L->hdr.dir_ofs, (L->hdr.dir_size + 1) & ~1u);
	if (crc32(mem, L->hdr.dir_size) != L->hdr.dir_crc)
		bad(path, 24, "directory CRC");
	parse_dir(L, mem, path);
	size = ((L->dir->nchunks * (uint32_t)sizeof(wld_chunk_t) + 15) & ~15u) + WLD_STYLE_SLOTS * 4
		+ ((L->dir->npages * WLD_PAGE_STATE + 15) & ~15u)
		+ ((L->dir->nsub * (uint32_t)sizeof(wld_mover_t) + 15) & ~15u);
	L->tables = mem_pool_alloc(&layer_pool, size, &mem);
	if (!L->tables)
		sys_error("world: %s: tables need %lu B of the layer region (%lu B free)", path,
			(unsigned long)size, (unsigned long)mem_pool_largest(&layer_pool));
	bytes += size;
	cur = mem;
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

	/* pinned world hull 0 */
	for (i = 0; i < L->dir->coll_pinned; i++)
	{
		int c = L->dir->coll_first + (int)i;
		wld_chunk_t *ch = &L->ch[c];

		size = (L->chunks[c].size + 15) & ~15u;
		ch->blk = mem_pool_alloc(&pools[POOL_COLL], size, &ch->data);
		if (!ch->blk)
			sys_error("world: %s: pinned COLL does not fit the collision pool", path);
		io_read_sync(ch->data, L->rom + L->chunks[c].ofs, size);
		ch->state = WLD_CH_PINNED;
		landed(L, c, ch->data);
	}
	scan_ents(L);
	io_set_load(0);
	L->open = 1;
	{
		int head = L->models[0].head[0];

		if (head >= 0 && W64_GID_COLL(head) >= L->dir->coll_pinned)
			bad(path, L->hdr.dir_ofs + L->dir->model_ofs + 36, "world hull 0 not pinned");
	}
	log_printf("world: open %s slot %d: %u B dir, %u sectors, %u pages, %u chunks, "
		"%u movers, layer block %lu B\n", maps[map].name, L->slot, (unsigned)L->hdr.dir_size,
		L->dir->nsect, L->dir->npages, L->dir->nchunks, L->nmovers, (unsigned long)bytes);
	return L;
}

void wld_close(wld_layer_t *L)
{
	uint32_t c;

	if (!L || !L->open)
		return;
	for (c = 0; c < L->dir->nchunks; c++)
	{
		wld_chunk_t *ch = &L->ch[c];

		if (ch->state == WLD_CH_LOADING)
			for (int i = 0; i < HL64_WORLD_REQS; i++)
				if (reqs[i].user == ((int)L->slot << 16 | (int)c))
				{
					io_cancel(&reqs[i]);
					reqs[i].user = -1;
				}
		if (ch->blk)
			mem_pool_free(&pools[chunk_pool(L->chunks[c].fourcc)], ch->blk, frame_no + 2);
	}
	/* page_state holds RDP bind lists the RDP may still read */
	mem_pool_free(&layer_pool, L->tables, frame_no + 2);
	mem_pool_free(&layer_pool, L->mem, 0);
	for (c = 32; c < 64; c++)
		wld_style_set(wld_style_slot(L, (int)c), NULL, 0);
	L->open = 0;
}

wld_layer_t *wld_layer(int slot)
{
	return slot >= 0 && slot < HL64_WORLD_LAYERS && layers[slot].open ? &layers[slot] : NULL;
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
