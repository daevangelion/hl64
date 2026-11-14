/* SPDX-License-Identifier: MIT */
/*
 * studio.c -- studio models in the engine (docs/architecture.md 9.4, D3(b);
 * docs/formats/studio-format.md; docs/specs/trace-and-hulls.md 4.5,
 * lightstyles.md 4, dlights.md 4, physics-movetypes.md 7, render-fx.md 2):
 *
 *  - residency in POOL_STUDIO of three units per cooked model: the info
 *    block (loaded synchronously on first use, pinned while a live entity
 *    uses the model), the draw mesh (read asynchronously when the model is
 *    first visible, recorded once by the platform) and the per-sequence
 *    animation chunks (read asynchronously when a sequence is wanted;
 *    studio_anim returns NULL meanwhile and the pose holds); LRU eviction
 *    of what the current frame does not use when an allocation finds no
 *    room, and soft residency (soft_residency: the meshes and animations
 *    of models no live entity uses are freed after 5 s unused);
 *  - the draw of the visible studio entities: the eye leaf's sector list
 *    and the view frustum against the entity's linked box, bones from
 *    game_studio_setup_bones into the platform's double-buffered matrix
 *    pool, one block replay per drawn run; light = the REND probe under
 *    the entity (base value; the cooked probes carry no style term yet)
 *    plus dynamic lights, floor 32, clamp 255; STEP entities interpolated
 *    between their think steps; render modes by col_ent_t.rendermode;
 *  - the view model: CPU-skinned with chrome (game_studio_chrome_*);
 *  - the hitbox source of engine/coll: the server pose
 *    (game_studio_hitbox_pose) of the entity, its cooked hitboxes on their
 *    bones (those the line can meet).
 *
 * Residency follows the old tree's studio pool (source/mod_studio.c,
 * provenance-audit.md entry QUAKE-INTERFACE: retargeted; its units are the
 * D3(b) blocks here, its model_t / COM_* / Con_* / host_framecount
 * bindings replaced by res_t, engine/io, engine/mem and the frame
 * counter); the draw split from n64/port/r64_studio.c (QUAKE-INTERFACE;
 * its 599-609 frame wrap lives in game/studio as HLSDK code) and the
 * STB2 draw of r64_studio_bake.c (plat/n64/gpu_studio.c).
 */
#include <math.h>
#include <string.h>

#include "hl64/coll.h"
#include "hl64/ent.h"
#include "hl64/game.h"
#include "hl64/limits.h"
#include "hl64/rec.h"
#include "hl64/seam.h"
#include "hl64/settings.h"
#include "hl64/studio.h"
#include "hl64/studio_fmt.h"
#include "engine/core/dbg.h"
#include "engine/core/log.h"
#include "engine/core/sys.h"
#include "engine/io/io.h"
#include "engine/mem/carve.h"
#include "engine/mem/pool.h"
#include "engine/render/gpu_studio.h"
#include "engine/render/ren.h"
#include "engine/render/trans.h"
#include "engine/world/wld.h"

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define ST_SWAP	1
#else
#define ST_SWAP	0
#endif

#define MAXBONES	128
#define REQS		16		/* reads in flight */
#define MAT_KB		64		/* studio scratch: matrix frame pools */
#define VIEW_KB		64		/* studio scratch: view model vertices (2 halves) */
#define MESH_GRACE	3		/* frames a mesh stays after its last draw (GPU) */
#define STEP_SNAP	64.0f		/* physics-movetypes.md 7 */
#define STEP_MAX	0.2
/* Models whose animations total at most this many bytes (view / world
 * weapon models, gibs, items) have every sequence prefetched once their
 * info lands, so the render paths that pose them (view model, temp
 * entities) never read an animation synchronously. */
#define SMALL_ANIMS	(32 * 1024)
#define PF_FREE_REQS	8	/* the prefetch leaves this many read slots free */
#define SOFT_GRACE_S	5.0f	/* a soft block unused this long is freed (soft_residency) */
#define TIME_RING	256	/* studio frames whose eng_time is kept (age_s) */

enum { U_ABSENT, U_LOADING, U_LANDED, U_RESIDENT, U_BAD };

typedef struct {
	uint32_t	rom, size, arom, asize;
	uint8_t		files;		/* rom / arom resolved */
	uint8_t		info_st, mesh_st;
	uint8_t		info_try;	/* an async info prefetch was started (once) */
	uint8_t		anim_pf;	/* every sequence of a small model requested */
	uint16_t	numseq, ntex;
	sfmt_info_t	*info;
	mem_blk_t	*info_blk;
	sfmt_mesh_t	*mesh;
	mem_blk_t	*mesh_blk;
	void		*gstate;
	mem_blk_t	*gstate_blk;
	uint32_t	info_used, mesh_used, pinned;
	uint32_t	info_ofs, info_size, mesh_ofs, mesh_size;
} smodel_t;

typedef struct {
	uint16_t	model;		/* MODEL index + 1, 0 free */
	uint16_t	seq;
	uint8_t		st;
	uint8_t		req;		/* in-flight request slot + 1 */
	uint16_t	pad;
	uint16_t	*data;
	mem_blk_t	*blk;
	uint32_t	used;
} sanim_t;

typedef struct {
	io_req_t	r;
	uint8_t		kind;		/* 0 free, 1 mesh, 2 anim */
	uint16_t	idx;
} sreq_t;

/* Async info loads (studio_frame's prefetch of interest models): the
 * 64-byte header, then the info block, at SEAM priority (D3(b): an info
 * must be resident before its record hydrates). */
#define ILOADS		8
typedef struct {
	io_req_t	r;
	sfmt_file_t	hdr __attribute__((aligned(16)));
	int16_t		idx;		/* model, -1 free */
	uint8_t		phase;		/* 1 header in flight, 2 info in flight */
} iload_t;
static iload_t		iloads[ILOADS];

/* STEP interpolation: the last two think-step placements per slot. */
typedef struct {
	uint16_t	serial;
	uint8_t		valid, pad;
	float		org[2][3], ang[2][3];
	double		t[2];
} sstep_t;

static mem_pool_t	pool, spool;
static mem_blk_t	desc[HL64_STUDIO_BLOCKS], sdesc[256];
static smodel_t		models[HL64_STUDIO_MODELS];
static sanim_t		anims[HL64_STUDIO_ANIMS];
static sreq_t		reqs[REQS];
static sstep_t		steps[HL64_MAX_ENTITIES];
/* Slots whose steps[] entry may be valid: studio_frame walks the live
 * slots only and voids the entries of slots freed since (a reused slot
 * starts over, whatever its serial). */
#define STEP_WORDS	((HL64_MAX_ENTITIES + 31) / 32)
static uint32_t		step_on[STEP_WORDS];
static uint32_t		frame = 1;
static float		ftime[TIME_RING];	/* eng_time of studio frame f at [f % TIME_RING] */
static studio_stats_t	st;
static studio_view_t	view;
static int		view_set;
static w64_vert_t	*view_verts[2];
static uint32_t		view_slots;
static int		view_flip;
#ifdef HL64_HOST
static const char	*bound[HL64_STUDIO_MODELS][2];
#endif

/* ---- byte order (host) ------------------------------------------------------- */

static void sw16(void *p, uint32_t n)
{
#if ST_SWAP
	uint16_t *q = p;

	while (n--)
	{
		*q = __builtin_bswap16(*q);
		q++;
	}
#else
	(void)p;
	(void)n;
#endif
}

static void sw32(void *p, uint32_t n)
{
#if ST_SWAP
	uint32_t *q = p;

	while (n--)
	{
		*q = __builtin_bswap32(*q);
		q++;
	}
#else
	(void)p;
	(void)n;
#endif
}

static int in(uint32_t ofs, uint32_t n, uint32_t size)
{
	return ofs <= size && n <= size - ofs;
}

/* Swap (host) and check an info block in place; 0 ok. */
static int fix_info(sfmt_info_t *h, uint32_t size, int nb_check)
{
	uint8_t *b = (uint8_t *)h;
	uint32_t i;

	if (size < sizeof(*h))
		return -1;
	sw32(&h->magic, 1);
	sw16(&h->numseq, 1);
	sw32(&h->flags, 1);
	sw32(h->eyeposition, 15);
	sw16(&h->numevents, 2);
	sw32(&h->bone_ofs, 12);
	if (h->magic != SFMT_INFO_MAGIC || (nb_check && h->numbones > MAXBONES)
		|| !in(h->bone_ofs, 56u * h->numbones, size)
		|| !in(h->ctl_ofs, 16u * h->numctl, size) || h->numctl > 8
		|| !in(h->hitbox_ofs, 28u * h->numhitboxes, size)
		|| !in(h->attach_ofs, 16u * h->numattach, size)
		|| !in(h->seq_ofs, 96u * h->numseq, size)
		|| !in(h->event_ofs, 12u * h->numevents, size)
		|| !in(h->bodypart_ofs, 8u * h->numbodyparts, size)
		|| !in(h->trans_ofs, (uint32_t)h->numtransitions * h->numtransitions, size)
		|| !in(h->str_ofs, h->str_size, size) || !h->str_size
		|| b[h->str_ofs + h->str_size - 1] != 0)
		return -1;
	for (i = 0; i < h->numbones; i++)
	{
		sfmt_bone_t *bn = (sfmt_bone_t *)(b + h->bone_ofs) + i;

		sw32(bn->value, 12);
		if (bn->parent >= (int)i || bn->parent < -1)
			return -1;
		for (int k = 0; k < 6; k++)
			if (bn->ctl[k] >= h->numctl)
				return -1;
	}
	for (i = 0; i < h->numctl; i++)
	{
		sfmt_ctl_t *c = (sfmt_ctl_t *)(b + h->ctl_ofs) + i;

		sw16(&c->type, 1);
		sw32(&c->start, 2);
	}
	for (i = 0; i < h->numhitboxes; i++)
	{
		sfmt_hitbox_t *x = (sfmt_hitbox_t *)(b + h->hitbox_ofs) + i;

		sw32(x->bbmin, 6);
		if (x->bone >= h->numbones)
			return -1;
	}
	for (i = 0; i < h->numattach; i++)
	{
		sfmt_attach_t *a = (sfmt_attach_t *)(b + h->attach_ofs) + i;

		sw32(a->org, 3);
		if (a->bone >= h->numbones)
			return -1;
	}
	for (i = 0; i < h->numseq; i++)
	{
		sfmt_seq_t *s = (sfmt_seq_t *)(b + h->seq_ofs) + i;

		sw16(&s->label, 1);
		sw32(&s->fps, 2);
		sw16(&s->activity, 5);
		sw32(&s->motiontype, 10);
		sw16(s->blendtype, 2);
		sw32(s->blendstart, 4);
		sw16(&s->nextseq, 1);
		sw32(&s->anim_ofs, 2);
		if (s->label >= h->str_size || (uint32_t)s->event_first + s->numevents > h->numevents
			|| !s->numblends || s->numblends > 4 || s->motionbone >= h->numbones)
			return -1;
	}
	for (i = 0; i < h->numevents; i++)
	{
		sfmt_event_t *e = (sfmt_event_t *)(b + h->event_ofs) + i;

		sw32(&e->frame, 2);
		sw16(&e->options, 2);
		if (e->options >= h->str_size)
			return -1;
	}
	for (i = 0; i < h->numbodyparts; i++)
	{
		sfmt_bodypart_t *p = (sfmt_bodypart_t *)(b + h->bodypart_ofs) + i;

		sw16(p, 4);
		if (!p->base || !p->nummodels || (uint32_t)p->sub_first + p->nummodels > h->nsub)
			return -1;
	}
	return 0;
}

/* Swap (host) and check a mesh block; 0 ok. */
static int fix_mesh(sfmt_mesh_t *m, uint32_t size, const sfmt_info_t *h)
{
	uint8_t *b = (uint8_t *)m;
	uint32_t i, slot = 0;

	if (size < sizeof(*m))
		return -1;
	sw32(&m->magic, 1);
	sw16(&m->nsub, 6);
	sw16(&m->ntex, 2);
	sw32(&m->bslot_ofs, 10);
	if (m->magic != SFMT_MESH_MAGIC || m->nsub != h->nsub || (m->nslots & 1)
		|| !in(m->bslot_ofs, m->nbslots, size) || !in(m->sub_ofs, 4u * m->nsub, size)
		|| !in(m->run_ofs, 8u * m->nruns, size) || !in(m->meshlet_ofs, 8u * m->nmeshlets, size)
		|| !in(m->load_ofs, 4u * m->nloads, size) || (m->vert_ofs & 15)
		|| !in(m->vert_ofs, 16u * m->nslots, size) || !in(m->tri_ofs, 3u * m->ntris, size)
		|| !in(m->skin_ofs, 2u * m->nskinref * m->nfamilies, size)
		|| !in(m->tex_ofs, 4u * m->ntex, size) || !m->nfamilies
		|| (m->nbone_ofs && !in(m->nbone_ofs, m->nslots, size)))
		return -1;
	for (i = 0; i < m->nbslots; i++)
		if (b[m->bslot_ofs + i] >= h->numbones)
			return -1;
	sw16(b + m->sub_ofs, 2u * m->nsub);
	for (i = 0; i < m->nsub; i++)
	{
		const sfmt_sub_t *s = (const sfmt_sub_t *)(b + m->sub_ofs) + i;

		if ((uint32_t)s->run_first + s->run_count > m->nruns)
			return -1;
	}
	sw16(b + m->run_ofs, 4u * m->nruns);
	for (i = 0; i < m->nruns; i++)
	{
		const sfmt_run_t *r = (const sfmt_run_t *)(b + m->run_ofs) + i;

		if (r->skinref >= m->nskinref || (uint32_t)r->meshlet_first + r->meshlet_count
			> m->nmeshlets)
			return -1;
	}
	for (i = 0; i < m->nmeshlets; i++)
	{
		sfmt_meshlet_t *ml = (sfmt_meshlet_t *)(b + m->meshlet_ofs) + i;
		uint32_t l, cache = 0, t;

		sw16(&ml->load_first, 1);
		sw16(&ml->tri_first, 2);
		if ((uint32_t)ml->load_first + ml->nloads > m->nloads || !ml->nverts
			|| ml->nverts > 64 || (uint32_t)ml->tri_first + ml->ntris > m->ntris)
			return -1;
		for (l = ml->load_first; l < (uint32_t)ml->load_first + ml->nloads; l++)
		{
			const sfmt_load_t *ld = (const sfmt_load_t *)(b + m->load_ofs) + l;

			if (ld->bslot >= m->nbslots || ld->cache != cache || !ld->count)
				return -1;
			cache += ld->count;
			slot += ld->count + (ld->count & 1u);
		}
		if (cache != ml->nverts)
			return -1;
		for (t = 3u * ml->tri_first; t < 3u * ((uint32_t)ml->tri_first + ml->ntris); t++)
			if (b[m->tri_ofs + t] >= ml->nverts)
				return -1;
	}
	if (slot != m->nslots)
		return -1;
	for (i = 0; i < m->nslots / 2u; i++)
	{
		w64_vert_t *v = (w64_vert_t *)(b + m->vert_ofs) + i;

		sw16(v->posA, 8);
		sw32(&v->rgbaA, 2);
		sw16(v->stA, 4);
	}
	sw16(b + m->skin_ofs, (uint32_t)m->nskinref * m->nfamilies);
	for (i = 0; i < (uint32_t)m->nskinref * m->nfamilies; i++)
		if (((const uint16_t *)(b + m->skin_ofs))[i] >= m->ntex)
			return -1;
	sw32(b + m->tex_ofs, m->ntex);
	for (i = 0; i < m->ntex; i++)
	{
		uint32_t o = ((const uint32_t *)(b + m->tex_ofs))[i], first;
		w64_texp_t *t = (w64_texp_t *)(b + o);

		if ((o & 15) || !in(o, 48, size))
			return -1;
		sw16(&t->w, 2);
		sw32(&t->ci4_ofs, 2);
		/* CI4 after the TLUT, or after the cooked additive ramp */
		first = t->flags & W64_TEXF_RAMP ? W64_TEXP_RAMP + 40 : 48;
		if (t->ci4_ofs != first || !t->w || !t->h || (t->w & (t->w - 1)) || (t->h & (t->h - 1))
			|| !in(o, first + (uint32_t)t->w * t->h / 2u, size))
			return -1;
		sw16(b + o + 16, first == 48 ? 16 : 32);	/* TLUT, the ramp's IA16 TLUT */
	}
	return 0;
}

/* ---- model table ---------------------------------------------------------------- */

static int model_index(res_t r)
{
	int i = RES_INDEX(r);

	if (RES_KIND(r) != RES_K_MODEL || i >= HL64_STUDIO_MODELS)
		return -1;
	return i;
}

static int model_files(int i)
{
	smodel_t *m = &models[i];
	int file;
	uint32_t ofs, size;

	if (m->files)
		return m->rom ? 0 : -1;
	m->files = 1;
#ifdef HL64_HOST
	if (bound[i][0])
	{
		m->rom = sys_cart_addr(bound[i][0], &m->size);
		m->arom = sys_cart_addr(bound[i][1], &m->asize);
		return m->rom && m->arom ? 0 : -1;
	}
#endif
	if (!res_file(RES_MAKE(RES_K_MODEL, i), &file, &ofs, &size))
		return -1;
	m->rom = res_file_rom(file, NULL) + ofs;
	m->size = size;
	if (!res_file(RES_MAKE(RES_K_ANIM, i), &file, &ofs, &size))
		return -1;
	m->arom = res_file_rom(file, NULL) + ofs;
	m->asize = size;
	return m->rom && m->arom ? 0 : -1;
}

/* A synchronous read of model i's info or animation: sync_loads; outside
 * load I/O (boot / New Game / setpos map loads) also sync_play, which
 * blocks a playing frame, logged by model. */
static void sync_read(int i, const char *what)
{
	st.sync_loads++;
	if (io_loading())
		return;
	st.sync_play++;
	log_printf("studio: sync %s read in play: model %d %s\n", what, i,
		res_name(RES_MAKE(RES_K_MODEL, i)));
}

#ifdef HL64_HOST
void studio_test_bind(int idx, const char *s64, const char *a64)
{
	if (idx < 0 || idx >= HL64_STUDIO_MODELS)
		sys_error("studio_test_bind: index %d", idx);
	bound[idx][0] = s64;
	bound[idx][1] = a64;
	models[idx].files = 0;
}
#endif

/* ---- pool ------------------------------------------------------------------------- */

/* Free the least recently used block nobody needs this frame; 0 when none. */
static int evict_one(void)
{
	uint32_t best = 0xFFFFFFFF;
	int kind = 0, idx = -1, i;

	for (i = 0; i < HL64_STUDIO_ANIMS; i++)
	{
		const sanim_t *a = &anims[i];

		if (a->model && a->st == U_RESIDENT && a->used + 1 < frame && a->used < best)
		{
			best = a->used;
			kind = 2;
			idx = i;
		}
	}
	for (i = 0; i < HL64_STUDIO_MODELS; i++)
	{
		const smodel_t *m = &models[i];

		if (m->mesh_st == U_RESIDENT && m->mesh_used + MESH_GRACE < frame && m->mesh_used < best)
		{
			best = m->mesh_used;
			kind = 1;
			idx = i;
		}
		if (m->info_st == U_RESIDENT && m->pinned + 1 < frame && m->info_used + 1 < frame
			&& m->mesh_st == U_ABSENT && m->info_used < best)
		{
			best = m->info_used;
			kind = 3;
			idx = i;
		}
	}
	if (idx < 0)
		return 0;
	if (kind == 2)
	{
		mem_pool_free(&pool, anims[idx].blk, 0);
		anims[idx].blk = NULL;
		anims[idx].model = 0;
		anims[idx].st = U_BAD;	/* a used slot: anim_slot probes past it */
	}
	else if (kind == 1)
	{
		smodel_t *m = &models[idx];

		gpu_studio_release(m->gstate);
		mem_pool_free(&spool, m->gstate_blk, 0);
		mem_pool_free(&pool, m->mesh_blk, 0);
		m->mesh = NULL;
		m->mesh_st = U_ABSENT;
	}
	else
	{
		smodel_t *m = &models[idx];

		mem_pool_free(&pool, m->info_blk, 0);
		m->info = NULL;
		m->info_st = U_ABSENT;
		m->info_try = 0;	/* an interest model's info is prefetched again */
	}
	return 1;
}

static mem_blk_t *alloc(uint32_t size, void **owner)
{
	mem_blk_t *b;

	size = (size + 15u) & ~15u;
	while (!(b = mem_pool_alloc(&pool, size, owner)))
		if (!evict_one())
		{
			st.nofit++;
			return NULL;
		}
	if (pool.used > st.pool_peak)
		st.pool_peak = pool.used;
	return b;
}

/* ---- info -------------------------------------------------------------------------- */

/* Check a landed .s64 header (byte-swapped here) against the model's files;
 * on success the info / mesh ranges are set. */
static int info_header(smodel_t *m, int i, sfmt_file_t *hdr)
{
	sw32(&hdr->magic, 1);
	sw16(&hdr->version, 2);
	sw32(&hdr->info_ofs, 6);
	if (hdr->magic != SFMT_MODEL_MAGIC || hdr->version != SFMT_VERSION || hdr->info_ofs != 64
		|| (hdr->info_size & 15) || hdr->mesh_ofs != 64 + hdr->info_size || (hdr->mesh_size & 15)
		|| hdr->mesh_ofs + hdr->mesh_size != m->size || hdr->anim_size != m->asize)
	{
		log_printf("studio: model %d: bad .s64 header\n", i);
		m->info_st = U_BAD;
		return -1;
	}
	m->info_ofs = hdr->info_ofs;
	m->info_size = hdr->info_size;
	m->mesh_ofs = hdr->mesh_ofs;
	m->mesh_size = hdr->mesh_size;
	return 0;
}

/* The info block landed: check it, the model's info is resident. */
static int info_landed(smodel_t *m, int i)
{
	if (fix_info(m->info, m->info_size, 1))
	{
		log_printf("studio: model %d: bad info block\n", i);
		mem_pool_free(&pool, m->info_blk, 0);
		m->info = NULL;
		m->info_st = U_BAD;
		return -1;
	}
	m->numseq = m->info->numseq;
	m->info_st = U_RESIDENT;
	m->info_used = frame;
	st.models++;
	return 0;
}

/* Advance async info load l (sync: land it now, a blocked read). */
static void iload_step(iload_t *l, int sync)
{
	smodel_t *m = &models[l->idx];

	if (sync && (l->r.state == IO_QUEUED || l->r.state == IO_ACTIVE))
		io_finish(&l->r, "studio info (queued)");
	if (l->r.state != IO_DONE)
		return;
	if (l->phase == 1)
	{
		/* no eviction for a prefetch: the pool keeps what is in use */
		if (info_header(m, l->idx, &l->hdr)
			|| !(m->info_blk = mem_pool_alloc(&pool, m->info_size, (void **)&m->info)))
		{
			if (m->info_st != U_BAD)
				m->info_st = U_ABSENT;
			l->idx = -1;
			return;
		}
		if (pool.used > st.pool_peak)
			st.pool_peak = pool.used;
		m->info_blk->locked = 1;
		memset(&l->r, 0, sizeof(l->r));
		l->r.rom = m->rom + m->info_ofs;
		l->r.dst = (uint8_t *)m->info;
		l->r.len = m->info_size;
		l->r.prio = IO_PRIO_SEAM;
		io_submit(&l->r);
		l->phase = 2;
		if (sync)
			iload_step(l, 1);
		return;
	}
	m->info_blk->locked = 0;
	info_landed(m, l->idx);
	l->idx = -1;
}

static smodel_t *info_of(res_t r)
{
	static sfmt_file_t hdr __attribute__((aligned(16)));
	int i = model_index(r), k;
	smodel_t *m;

	if (i < 0)
		return NULL;
	m = &models[i];
	if (m->info_st == U_LOADING)
		for (k = 0; k < ILOADS; k++)
			if (iloads[k].idx == i)
			{
				/* needed before its prefetch landed: finish it now
				 * (blocked I/O, counted) */
				iload_step(&iloads[k], 1);
				sync_read(i, "info (queued)");
			}
	if (m->info_st == U_RESIDENT)
	{
		m->info_used = frame;
		return m;
	}
	if (m->info_st == U_BAD || model_files(i) || m->size < 64)
		return NULL;
	io_read_sync(&hdr, m->rom, 64, "studio info");
	if (info_header(m, i, &hdr))
		return NULL;
	if (!(m->info_blk = alloc(m->info_size, (void **)&m->info)))
		sys_error("studio: no pool room for model %d info (%lu B)", i,
			(unsigned long)m->info_size);
	io_read_sync(m->info, m->rom + m->info_ofs, m->info_size, "studio info");
	if (info_landed(m, i))
		return NULL;
	sync_read(i, "info");
	return m;
}

/* Prefetch the info of every model with residency interest (res_precache:
 * the records' models of the open maps, the ring) once, asynchronously;
 * sync (New Game, load I/O): read them now. */
static void info_prefetch(int sync)
{
	int n = res_count(RES_K_MODEL), i, k;

	if (n > HL64_STUDIO_MODELS)
		n = HL64_STUDIO_MODELS;
	for (k = 0; k < ILOADS; k++)
		if (iloads[k].idx >= 0)
			iload_step(&iloads[k], sync);
	for (i = 0; i < n; i++)
	{
		smodel_t *m = &models[i];
		res_t r = RES_MAKE(RES_K_MODEL, i);

		if (m->info_st != U_ABSENT || m->info_try || !res_interest(r))
			continue;
		if (sync)
		{
			info_of(r);
			continue;
		}
		for (k = 0; k < ILOADS && iloads[k].idx >= 0; k++)
			;
		if (k == ILOADS)
			return;
		if (model_files(i) || m->size < 64)
		{
			m->info_try = 1;
			continue;
		}
		m->info_try = 1;
		m->info_st = U_LOADING;
		memset(&iloads[k].r, 0, sizeof(iloads[k].r));
		iloads[k].idx = (int16_t)i;
		iloads[k].phase = 1;
		iloads[k].r.rom = m->rom;
		iloads[k].r.dst = (uint8_t *)&iloads[k].hdr;
		iloads[k].r.len = 64;
		iloads[k].r.prio = IO_PRIO_SEAM;
		io_submit(&iloads[k].r);
	}
}

void studio_preload(void)
{
	int e;

	info_prefetch(1);
	/* the live entities' models (the player's has no precache) */
	for (e = ent_next_live(0); e >= 0; e = ent_next_live(e))
		if (RES_KIND(ent_rnd(e)->model) == RES_K_MODEL)
			info_of(ent_rnd(e)->model);
}

int studio_infos_pending(void)
{
	int n = res_count(RES_K_MODEL), i, p = 0;

	if (n > HL64_STUDIO_MODELS)
		n = HL64_STUDIO_MODELS;
	for (i = 0; i < n; i++)
	{
		const smodel_t *m = &models[i];

		if (m->info_st == U_LOADING || (m->info_st == U_ABSENT && !m->info_try
			&& res_interest(RES_MAKE(RES_K_MODEL, i))))
			p++;
	}
	return p;
}

const void *studio_info(res_t model, uint32_t *size)
{
	smodel_t *m = info_of(model);

	if (!m)
		return NULL;
	if (size)
		*size = m->info_size;
	return m->info;
}

/* ---- reads ----------------------------------------------------------------------------- */

static void req_done(io_req_t *r)
{
	sreq_t *q = (sreq_t *)r;

	if (q->kind == 1)
		models[q->idx].mesh_st = U_LANDED;
	else if (q->kind == 2)
		anims[q->idx].st = U_LANDED;
	if (q->kind == 1)
		models[q->idx].mesh_blk->locked = 0;
	else
		anims[q->idx].blk->locked = 0;
	q->kind = 0;
}

static sreq_t *req_get(void)
{
	int i;

	for (i = 0; i < REQS; i++)
		if (!reqs[i].kind)
			return &reqs[i];
	return NULL;
}

static void submit(sreq_t *q, int kind, int idx, uint32_t rom, void *dst, uint32_t len, int prio)
{
	memset(&q->r, 0, sizeof(q->r));
	q->kind = (uint8_t)kind;
	q->idx = (uint16_t)idx;
	q->r.rom = rom;
	q->r.dst = dst;
	q->r.len = len;
	q->r.prio = (uint8_t)prio;
	q->r.cb = req_done;
	io_submit(&q->r);
}

/* ---- animation chunks -------------------------------------------------------------------- */

static sanim_t *anim_slot(int mi, int seq, int create)
{
	uint32_t h = ((uint32_t)mi * 131u + (uint32_t)seq) % HL64_STUDIO_ANIMS;
	int k, free_at = -1;

	for (k = 0; k < HL64_STUDIO_ANIMS; k++)
	{
		sanim_t *a = &anims[(h + k) % HL64_STUDIO_ANIMS];

		if (a->model == mi + 1 && a->seq == seq)
			return a;
		if (!a->model)
		{
			if (free_at < 0)
				free_at = (int)((h + k) % HL64_STUDIO_ANIMS);
			if (a->st != U_BAD)	/* never used: end of the probe run */
				break;
		}
	}
	if (!create || free_at < 0)
		return NULL;
	anims[free_at].model = (uint16_t)(mi + 1);
	anims[free_at].seq = (uint16_t)seq;
	anims[free_at].st = U_ABSENT;
	return &anims[free_at];
}

static void anim_land(sanim_t *a, uint32_t size)
{
	sw16(a->data, size / 2u);
	a->st = U_RESIDENT;
	st.anims++;
}

/* want_only: request, never return data; sync: a chunk not resident is
 * read now (a queued read finished first), counted as a sync load. */
static const void *anim_get(res_t model, int seq, uint32_t *size, int want_only, int sync)
{
	smodel_t *m = info_of(model);
	const sfmt_seq_t *s;
	sanim_t *a;
	sreq_t *q;
	int mi;

	if (!m || seq < 0 || seq >= m->numseq)
		return NULL;
	mi = model_index(model);
	s = &SFMT_SEQS(m->info)[seq];
	if (!s->anim_size || !in(s->anim_ofs, s->anim_size, m->asize))
		return NULL;
	a = anim_slot(mi, seq, 1);
	if (!a)
	{
		st.dropped++;
		return NULL;
	}
	a->used = frame;
	if (sync && a->st == U_LOADING)
	{
		int k;

		for (k = 0; k < REQS; k++)
			if (reqs[k].kind == 2 && reqs[k].idx == a - anims)
			{
				io_finish(&reqs[k].r, "studio anim (queued)");
				sync_read(mi, "anim (queued)");
			}
		io_pump();
	}
	if (sync && a->st == U_ABSENT)
	{
		if (!(a->blk = alloc(s->anim_size, (void **)&a->data)))
			return NULL;
		io_read_sync(a->data, m->arom + s->anim_ofs, s->anim_size, "studio anim");
		sync_read(mi, "anim");
		a->st = U_LANDED;
	}
	if (a->st == U_LANDED)
		anim_land(a, s->anim_size);
	if (a->st == U_RESIDENT)
	{
		if (size)
			*size = s->anim_size;
		return want_only ? NULL : a->data;
	}
	if (a->st == U_ABSENT && (q = req_get()) != NULL)
	{
		if (!(a->blk = alloc(s->anim_size, (void **)&a->data)))
			return NULL;
		a->blk->locked = 1;
		a->st = U_LOADING;
		submit(q, 2, (int)(a - anims), m->arom + s->anim_ofs, a->data, s->anim_size,
			IO_PRIO_PREFETCH);
	}
	return NULL;
}

const void *studio_anim(res_t model, int sequence, uint32_t *size)
{
	return anim_get(model, sequence, size, 0, 0);
}

const void *studio_anim_now(res_t model, int sequence, uint32_t *size)
{
	return anim_get(model, sequence, size, 0, 1);
}

void studio_want(res_t model, int sequence)
{
	anim_get(model, sequence, NULL, 1, 0);
}

static int reqs_free(void)
{
	int i, n = 0;

	for (i = 0; i < REQS; i++)
		n += !reqs[i].kind;
	return n;
}

/* Request every sequence of small model i (info resident) while more than
 * PF_FREE_REQS read slots are free (the visible entities' reads keep
 * theirs); 1 when every sequence was asked for. */
static int want_all(int i)
{
	const smodel_t *m = &models[i];
	int s;

	if (m->info_st != U_RESIDENT || m->asize > SMALL_ANIMS)
		return 1;
	for (s = 0; s < m->numseq; s++)
	{
		if (reqs_free() <= PF_FREE_REQS)
			return 0;
		anim_get(RES_MAKE(RES_K_MODEL, i), s, NULL, 1, 0);
	}
	return 1;
}

/* studio_frame: the small models with residency interest get every
 * sequence requested once their info is resident. */
static void anim_prefetch(void)
{
	int n = res_count(RES_K_MODEL), i;

	if (n > HL64_STUDIO_MODELS)
		n = HL64_STUDIO_MODELS;
	for (i = 0; i < n; i++)
	{
		smodel_t *m = &models[i];

		if (m->anim_pf || m->info_st != U_RESIDENT || !res_interest(RES_MAKE(RES_K_MODEL, i)))
			continue;
		if (!want_all(i))
			return;
		m->anim_pf = 1;
	}
}

/* 1 when sequence seq of model (as the server pose picks it: 0 when out
 * of range) can be posed without a read: info and animation resident, or
 * a sequence without animation data. Otherwise what is missing is
 * requested (an info through residency interest, studio_frame's async
 * prefetch): the render paths skip the draw instead of reading it now. */
static int anim_ready(res_t model, int seq)
{
	int i = model_index(model);
	const smodel_t *m;
	const sanim_t *a;

	if (i < 0)
		return 0;
	m = &models[i];
	if (m->info_st != U_RESIDENT)
	{
		res_precache(model);
		return 0;
	}
	if (seq < 0 || seq >= m->numseq)
		seq = 0;
	if (!m->numseq || !SFMT_SEQS(m->info)[seq].anim_size)
		return 1;
	anim_get(model, seq, NULL, 1, 0);
	a = anim_slot(i, seq, 0);
	return a && a->st == U_RESIDENT;
}

/* ---- mesh -------------------------------------------------------------------------------- */

/* The model's recorded mesh, or NULL after requesting it. */
static smodel_t *mesh_of(res_t r)
{
	smodel_t *m = info_of(r);
	sreq_t *q;
	uint32_t gsz;

	if (!m)
		return NULL;
	m->mesh_used = frame;
	switch (m->mesh_st)
	{
	case U_RESIDENT:
		return m;
	case U_ABSENT:
		if (!(q = req_get()))
			return NULL;
		if (!(m->mesh_blk = alloc(m->mesh_size, (void **)&m->mesh)))
			return NULL;
		/* GPU-visible: never slides; locked for the DMA as well */
		m->mesh_blk->locked = 1;
		m->mesh_st = U_LOADING;
		submit(q, 1, (int)(m - models), m->rom + m->mesh_ofs, m->mesh, m->mesh_size,
			IO_PRIO_OPEN);
		return NULL;
	case U_LANDED:
		m->mesh_blk->locked = 1;
		if (fix_mesh(m->mesh, m->mesh_size, m->info))
		{
			log_printf("studio: model %d: bad mesh block\n", (int)(m - models));
			mem_pool_free(&pool, m->mesh_blk, 0);
			m->mesh = NULL;
			m->mesh_st = U_BAD;
			return NULL;
		}
		gsz = gpu_studio_state_size(m->mesh);
		while (!(m->gstate_blk = mem_pool_alloc(&spool, (gsz + 15u) & ~15u, &m->gstate)))
		{
			st.nofit++;
			return NULL;
		}
		m->gstate_blk->locked = 1;
		if (gpu_studio_build(m->gstate, m->mesh))
		{
			mem_pool_free(&spool, m->gstate_blk, 0);
			return NULL;
		}
		m->mesh_st = U_RESIDENT;
		return m;
	default:
		return NULL;
	}
}

/* ---- boot, frame ------------------------------------------------------------------------- */


static void unpack_init(void);

void studio_init(void)
{
	uint8_t *sc = mem_region(MEM_STUDIO_SCRATCH);
	uint32_t scb = mem_region_bytes(MEM_STUDIO_SCRATCH);
	int k;

	for (k = 0; k < ILOADS; k++)
		iloads[k].idx = -1;
	mem_pool_init(&pool, "studio", mem_region(MEM_POOL_STUDIO),
		mem_region_bytes(MEM_POOL_STUDIO), desc, HL64_STUDIO_BLOCKS);
	if (scb < (MAT_KB + VIEW_KB + 16) * 1024u)
		sys_error("studio: scratch region %lu B too small", (unsigned long)scb);
	gpu_studio_init(sc, MAT_KB * 1024u);
	view_verts[0] = (w64_vert_t *)(sc + MAT_KB * 1024u);
	view_verts[1] = view_verts[0] + VIEW_KB * 1024u / 2u / sizeof(w64_vert_t);
	view_slots = VIEW_KB * 1024u / 2u / sizeof(w64_vert_t) * 2u;
	mem_pool_init(&spool, "studiogpu", sc + (MAT_KB + VIEW_KB) * 1024u,
		scb - (MAT_KB + VIEW_KB) * 1024u, sdesc, 256);
	col_set_hitbox_source(studio_hitboxes);
	unpack_init();
}

static int is_studio(int e)
{
	return ent_is_live(e) && RES_KIND(ent_rnd(e)->model) == RES_K_MODEL;
}

/* Seconds since studio frame f (the time ring; older than it: long ago). */
static float age_s(uint32_t f, double now)
{
	if (frame - f >= TIME_RING)
		return 1e30f;
	return (float)(now - ftime[f % TIME_RING]);
}

/* Soft residency (the old tree's studio eviction classes, stream-arch-v2
 * 6.8 note): pinned = the models of live entities (either map) and the
 * view model (info, mesh, every animation), in-flight reads, the infos
 * (a record's hydration reads its model's info: the records' and the
 * ring's precache keep them; the LRU evicts them under pressure); a
 * small model's prefetched animations while it has residency interest
 * (the temp-entity / view paths never read them synchronously). Soft:
 * the meshes and the other animations of models no live entity uses,
 * neighbour-map precache entries and 5 s-grace prefetches included:
 * freed once unused for SOFT_GRACE_S instead of filling the pool until
 * an allocation fails. The want (pinned and in use: pinned models'
 * info + mesh, animations used this frame or the last, in-flight reads)
 * peaks into st.want_peak. */
static void soft_residency(double now)
{
	uint32_t want = 0;
	int i;

	for (i = 0; i < HL64_STUDIO_ANIMS; i++)
	{
		sanim_t *a = &anims[i];
		const smodel_t *m;

		if (!a->model || !a->blk || a->st < U_LOADING || a->st > U_RESIDENT)
			continue;
		m = &models[a->model - 1];
		if (a->st != U_RESIDENT || a->used + 1 >= frame)
			want += a->blk->size;
		else if (m->pinned != frame && age_s(a->used, now) > SOFT_GRACE_S
			&& !(m->asize <= SMALL_ANIMS && res_interest(RES_MAKE(RES_K_MODEL, a->model - 1))))
		{
			mem_pool_free(&pool, a->blk, 0);
			a->blk = NULL;
			a->model = 0;
			a->st = U_BAD;		/* a used slot: anim_slot probes past it */
			st.soft++;
		}
	}
	for (i = 0; i < HL64_STUDIO_MODELS; i++)
	{
		smodel_t *m = &models[i];

		if (m->mesh_st == U_LOADING || m->mesh_st == U_LANDED)
			want += m->mesh_blk->size;
		if (m->pinned == frame)
		{
			if (m->info_st == U_RESIDENT)
				want += m->info_blk->size;
			if (m->mesh_st == U_RESIDENT)
				want += m->mesh_blk->size;
		}
		else if (m->mesh_st == U_RESIDENT && age_s(m->mesh_used, now) > SOFT_GRACE_S)
		{
			gpu_studio_release(m->gstate);
			mem_pool_free(&spool, m->gstate_blk, 0);
			mem_pool_free(&pool, m->mesh_blk, 0);
			m->mesh = NULL;
			m->mesh_st = U_ABSENT;
			st.soft++;
		}
	}
	for (i = 0; i < ILOADS; i++)
		if (iloads[i].idx >= 0 && iloads[i].phase == 2)
			want += models[iloads[i].idx].info_blk->size;
	if (want > st.want_peak)
		st.want_peak = want;
}

/* Step 9: pins, wanted chunks, STEP placements (after the entity pass),
 * soft residency. */
void studio_frame(void)
{
	double now = eng_time();
	uint32_t live[STEP_WORDS];
	int e, i, w;

	frame++;
	ftime[frame % TIME_RING] = (float)now;
	mem_pool_retire(&pool, frame);
	info_prefetch(0);
	anim_prefetch();
	memset(live, 0, sizeof(live));
	for (e = ent_next_live(0); e >= 0; e = ent_next_live(e))
	{
		const ent_rnd_t *r;
		const col_ent_t *cv;
		sstep_t *s = &steps[e];
		int mi;

		live[e >> 5] |= 1u << (e & 31);
		if (!is_studio(e))
		{
			s->valid = 0;
			continue;
		}
		r = ent_rnd(e);
		mi = model_index(r->model);
		if (mi >= 0)
			models[mi].pinned = frame;
		studio_want(r->model, r->sequence);
		cv = ent_cv(e);
		if (cv->movetype != MOVETYPE_STEP)
		{
			s->valid = 0;
			continue;
		}
		if (!s->valid || s->serial != ent_idc(e)->serial)
		{
			s->valid = 1;
			step_on[e >> 5] |= 1u << (e & 31);
			s->serial = ent_idc(e)->serial;
			for (i = 0; i < 2; i++)
			{
				memcpy(s->org[i], cv->origin, sizeof(s->org[i]));
				memcpy(s->ang[i], cv->angles, sizeof(s->ang[i]));
				s->t[i] = now;
			}
			continue;
		}
		if (memcmp(s->org[1], cv->origin, sizeof(s->org[1]))
			|| memcmp(s->ang[1], cv->angles, sizeof(s->ang[1])))
		{
			memcpy(s->org[0], s->org[1], sizeof(s->org[0]));
			memcpy(s->ang[0], s->ang[1], sizeof(s->ang[0]));
			s->t[0] = s->t[1];
			memcpy(s->org[1], cv->origin, sizeof(s->org[1]));
			memcpy(s->ang[1], cv->angles, sizeof(s->ang[1]));
			s->t[1] = now;
		}
	}
	for (w = 0; w < STEP_WORDS; w++)
	{
		uint32_t b = step_on[w] & ~live[w];

		step_on[w] &= live[w];
		for (i = w << 5; b; b >>= 1, i++)
			if (b & 1)
				steps[i].valid = 0;
	}
	if (view_set && (i = model_index(view.model)) >= 0)
		models[i].pinned = frame;
	soft_residency(now);
}

/* Placement drawn for e (map-local): STEP entities between their last
 * two think steps over the interval between them (<= 0.2 s), a jump over
 * 64 u snaps (physics-movetypes.md 7); others as they are. */
static void placement(int e, double time, float org[3], float ang[3])
{
	const col_ent_t *cv = ent_cv(e);
	const sstep_t *s = &steps[e];
	double span, f;
	float d2 = 0;
	int k;

	memcpy(org, cv->origin, 3 * sizeof(float));
	memcpy(ang, cv->angles, 3 * sizeof(float));
	if (!s->valid || cv->movetype != MOVETYPE_STEP)
		return;
	span = s->t[1] - s->t[0];
	if (span <= 0)
		return;
	if (span > STEP_MAX)
		span = STEP_MAX;
	f = (time - s->t[1]) / span;
	if (f >= 1)
		return;
	if (f < 0)
		f = 0;
	for (k = 0; k < 3; k++)
		d2 += (s->org[1][k] - s->org[0][k]) * (s->org[1][k] - s->org[0][k]);
	if (d2 > STEP_SNAP * STEP_SNAP)
		return;
	for (k = 0; k < 3; k++)
	{
		float d = s->ang[1][k] - s->ang[0][k];

		if (d > 180)
			d -= 360;
		else if (d < -180)
			d += 360;
		org[k] = s->org[0][k] + (float)((s->org[1][k] - s->org[0][k]) * f);
		ang[k] = s->ang[0][k] + (float)(d * f);
	}
}

/* ---- light (lightstyles.md 4, dlights.md 4) -------------------------------------------------- */

/* REND probe value at map-local p of layer L (trilinear over the 64 u
 * lattice of a visible sector containing p); -1 when none is resident. */
static int probe_at(wld_layer_t *L, const uint16_t *list, int cnt, const float p[3])
{
	int i, k;

	for (i = 0; i < cnt; i++)
	{
		const w64_sector_t *s = &L->sects[list[i]];
		const w64_rend_t *r;
		const int16_t *org;
		const uint8_t *v;
		uint8_t *ch;
		float g[3], fr[3], acc = 0;
		int c[3], n[3];

		for (k = 0; k < 3; k++)
			if (p[k] < s->bbox[k] || p[k] > s->bbox[k + 3])
				break;
		if (k < 3)
			continue;
		if (!(ch = wld_chunk_use(L, s->rend_chunk)))
			return -1;
		r = (const w64_rend_t *)ch;
		if (!r->probe_ofs)
			return -1;
		org = (const int16_t *)(ch + r->probe_ofs);
		v = (const uint8_t *)(org + 3);
		for (k = 0; k < 3; k++)
		{
			n[k] = r->probe_dims[k];
			g[k] = (p[k] - org[k]) / 64.0f;
			if (g[k] < 0)
				g[k] = 0;
			if (g[k] > n[k] - 1)
				g[k] = (float)(n[k] - 1);
			c[k] = (int)g[k];
			if (c[k] > n[k] - 2)
				c[k] = n[k] > 1 ? n[k] - 2 : 0;
			fr[k] = n[k] > 1 ? g[k] - c[k] : 0;
		}
		for (k = 0; k < 8; k++)
		{
			int x = c[0] + (k & 1 && n[0] > 1), y = c[1] + ((k >> 1) & 1 && n[1] > 1);
			int z = c[2] + ((k >> 2) & 1 && n[2] > 1);
			float w = ((k & 1) ? fr[0] : 1 - fr[0]) * (((k >> 1) & 1) ? fr[1] : 1 - fr[1])
				* (((k >> 2) & 1) ? fr[2] : 1 - fr[2]);

			acc += w * v[(z * n[1] + y) * n[0] + x];
		}
		return (int)(acc + 0.5f);
	}
	return -1;
}

/* Model light (lightstyles.md 4, dlights.md 4): the probe under the point
 * (128 when its REND chunk is not resident), the style term (none until
 * probe v3) and the live lights, by the renderer's ren_model_light. */
static int light_at(int e, wld_layer_t *L, const uint16_t *list, int cnt, const float local[3],
	const float global[3])
{
	int base = probe_at(L, list, cnt, local);

	return ren_model_light(e, global, base < 0 ? 128.0f : (float)base, -1, 0);
}

/* pfnGetEntityIllum (ent.h): the light a studio entity is drawn with, or
 * the model light at a non-studio entity's origin, from the sectors its
 * leaf sees; an entity whose probe chunk is not resident keeps its last
 * value (128 before the first). */
static uint8_t	illum[HL64_MAX_ENTITIES];

/* New Game (after ent_reset): entity serials restart with the slots, so
 * no STEP placement or probe value of an old entity is kept (boot state) */
void studio_new_game(void)
{
	memset(steps, 0, sizeof(steps));
	memset(step_on, 0, sizeof(step_on));
	memset(illum, 0, sizeof(illum));
}

int ent_illum(int e)
{
	const col_ent_t *cv;
	wld_layer_t *L;
	const uint16_t *list;
	float g[3];
	int cnt, base, probe, k, v;

	if (!ent_is_live(e) || !(L = wld_layer(ent_idc(e)->layer)))
		return 0;
	cv = ent_cv(e);
	list = wld_leaf_sectors(L, wld_point_leaf(L, cv->origin), &cnt);
	base = probe = probe_at(L, list, cnt, cv->origin);
	if (base < 0)
		base = illum[e] ? illum[e] : 128;
	for (k = 0; k < 3; k++)
		g[k] = cv->origin[k] + L->gofs[k];
	v = ren_model_light(e, g, (float)base, -1, 0);
	illum[e] = (uint8_t)(base > 0 ? base : 1);
	if (dbg_trace())
		log_printf("trace %lu illum e=%d probe=%d base=%d v=%d\n",
			(unsigned long)dbg_trace_frame(), e, probe, base, v);
	return v;
}

/* ---- draw ------------------------------------------------------------------------------------ */

typedef struct {
	uint16_t	e;
	uint16_t	trans;
	float		dist;
} cand_t;

static float	bones[MAXBONES][3][4];
static float	slots[MAXBONES][3][4];
static uint16_t	runs_buf[256], tex_buf[256];

/* Model matrix: angles (pitch negated, the studio convention), scale,
 * camera-relative position. */
static void model_matrix(ren_mat_t *mt, const float org_g[3], const float ang[3], float scale,
	const float eye[3])
{
	float a[3], f[3], r[3], u[3];
	int k;

	a[0] = -ang[0];
	a[1] = ang[1];
	a[2] = ang[2];
	ren_angle_axes(a, f, r, u);
	for (k = 0; k < 3; k++)
	{
		mt->m[0][k] = f[k] * scale;
		mt->m[1][k] = -r[k] * scale;
		mt->m[2][k] = u[k] * scale;
		mt->m[3][k] = org_g[k] - eye[k];
	}
	mt->m[0][3] = mt->m[1][3] = mt->m[2][3] = 0;
	mt->m[3][3] = 1;
}

/* The runs of the entity's body parts and their skins. */
static int pick_runs(const smodel_t *m, int body, int skin)
{
	const sfmt_info_t *h = m->info;
	const sfmt_mesh_t *ms = m->mesh;
	const uint8_t *b = (const uint8_t *)ms;
	const sfmt_sub_t *subs = (const sfmt_sub_t *)(b + ms->sub_ofs);
	const sfmt_run_t *runs = (const sfmt_run_t *)(b + ms->run_ofs);
	const uint16_t *skins = (const uint16_t *)(b + ms->skin_ofs);
	int p, n = 0, k;

	if (skin < 0 || skin >= ms->nfamilies)
		skin = 0;
	for (p = 0; p < h->numbodyparts; p++)
	{
		const sfmt_bodypart_t *bp = &SFMT_BODYPARTS(h)[p];
		const sfmt_sub_t *s = &subs[bp->sub_first + (body / bp->base) % bp->nummodels];

		for (k = s->run_first; k < s->run_first + s->run_count && n < 256; k++, n++)
		{
			runs_buf[n] = (uint16_t)k;
			tex_buf[n] = skins[skin * ms->nskinref + runs[k].skinref];
		}
	}
	return n;
}

static void slot_matrices(const smodel_t *m)
{
	const uint8_t *bslot = (const uint8_t *)m->mesh + m->mesh->bslot_ofs;
	float k = 1.0f / (float)(1 << m->mesh->pos_shift);
	int s, r;

	for (s = 0; s < m->mesh->nbslots; s++)
	{
		const float (*b)[4] = bones[bslot[s]];

		for (r = 0; r < 3; r++)
		{
			slots[s][r][0] = b[r][0] * k;
			slots[s][r][1] = b[r][1] * k;
			slots[s][r][2] = b[r][2] * k;
			slots[s][r][3] = b[r][3];
		}
	}
}

/* render-fx.md 2 (studio column), blend = renderamt (fx 0). */
static int blend_mode(int e, uint8_t *alpha)
{
	int mode = ent_cv(e)->rendermode;

	*alpha = 255;
	switch (mode)
	{
	case 1:		/* TransColor: as TransTexture */
	case 2:		/* TransTexture */
		*alpha = ent_rnd(e)->renderamt;
		return GPU_STUDIO_BLEND;
	case 3:		/* Glow: as TransAdd */
	case 5:		/* TransAdd (render-fx.md 2a: the screen-like blend, no cap) */
		*alpha = ent_rnd(e)->renderamt;
		return GPU_STUDIO_ADD;
	default:	/* Normal, TransAlpha */
		return GPU_STUDIO_OPAQUE;
	}
}

/* mode < 0: the entity's own render mode and blend. shift: added to the
 * entity's place to put it in L's frame (a seam ghost, seam_ghost), or
 * NULL. */
static void draw_entity(int e, double time, wld_layer_t *L, const uint16_t *list, int cnt,
	const float eye[3], int mode, uint8_t alpha, const float *shift)
{
	const ent_rnd_t *r = ent_rnd(e);
	gpu_studio_ent_t ge;
	smodel_t *m = mesh_of(r->model);
	float org[3], ang[3], org_g[3];
	uint64_t t0;
	int n, k, lv;

	if (!m)
	{
		st.pending++;
		return;
	}
	t0 = sys_time_us();
	n = game_studio_setup_bones(e, bones, MAXBONES);
	st.bones_us += (uint32_t)(sys_time_us() - t0);
	if (n < m->info->numbones)
		return;
	slot_matrices(m);
	placement(e, time, org, ang);
	for (k = 0; k < 3; k++)
	{
		if (shift)
			org[k] += shift[k];
		org_g[k] = org[k] + L->gofs[k];
	}
	memset(&ge, 0, sizeof(ge));
	model_matrix(&ge.model, org_g, ang, r->scale > 0 ? r->scale : 1.0f, eye);
	ge.bones = (const float (*)[3][4])slots;
	ge.nbones = m->mesh->nbslots;
	ge.nruns = pick_runs(m, r->body, ent_cv(e)->skin);
	ge.runs = runs_buf;
	ge.tex = tex_buf;
	lv = light_at(e, L, list, cnt, org, org_g);
	ge.rgba[0] = ge.rgba[1] = ge.rgba[2] = (uint8_t)lv;
	if (mode < 0)
		ge.mode = (uint8_t)blend_mode(e, &ge.rgba[3]);
	else
	{
		ge.mode = (uint8_t)mode;
		ge.rgba[3] = alpha;
	}
	t0 = sys_time_us();
	n = gpu_studio_draw(m->gstate, m->mesh, &ge);
	st.gpu_us += (uint32_t)(sys_time_us() - t0);
	if (n)
	{
		st.dropped++;
		return;
	}
	st.drawn++;
}

/* Visible: a sector of the eye leaf's list meets the linked box (map-local,
 * + shift when not NULL) and the box is inside the frustum. */
static int visible(const wld_layer_t *L, const uint16_t *list, int cnt, const col_ent_t *cv,
	const float *shift, const float eye[3])
{
	const float (*fr)[3] = ren_frustum();
	float lo[3], hi[3];
	int i, k;

	for (k = 0; k < 3; k++)
	{
		lo[k] = cv->absmin[k] + (shift ? shift[k] : 0);
		hi[k] = cv->absmax[k] + (shift ? shift[k] : 0);
	}
	for (i = 0; i < 4; i++)
	{
		float d = 0;

		for (k = 0; k < 3; k++)
		{
			float v = fr[i][k] >= 0 ? hi[k] : lo[k];

			d += fr[i][k] * (v + L->gofs[k] - eye[k]);
		}
		if (d < 0)
			return 0;
	}
	for (i = 0; i < cnt; i++)
	{
		const int16_t *b = L->sects[list[i]].bbox;

		for (k = 0; k < 3; k++)
			if (lo[k] > b[k + 3] || hi[k] < b[k])
				break;
		if (k == 3)
			return 1;
	}
	return 0;
}

/* The frame's eye-leaf sector list per drawn layer (studio_draw). */
static const uint16_t	*lay_list[HL64_WORLD_LAYERS];
static int		lay_cnt[HL64_WORLD_LAYERS];
static double		draw_time;

static void timed(uint64_t t0)
{
	uint64_t t1 = sys_time_us();

	st.draw_us += (uint32_t)(t1 - t0);
	st.us_sum += (uint32_t)(t1 - t0);
	if (t1 - t0 > st.us_max)
		st.us_max = (uint32_t)(t1 - t0);
}

/* A seam ghost (seam_ghost) seen from home layer L: 1, shift = its place
 * (its layer's frame) -> L's frame. */
static int ghost_shift(int e, const wld_layer_t *L, float shift[3])
{
	const wld_layer_t *B;
	float off[3];
	int k;

	if (!seam_ghost(e, L->map, off) || !(B = wld_layer(ent_idc(e)->layer)))
		return 0;
	for (k = 0; k < 3; k++)
		shift[k] = B->gofs[k] + off[k] - L->gofs[k];
	return 1;
}

/* A non-home layer's studio entities: none draws (counted, ren
 * other_ents), except, through an OPEN seam whose B layer this is
 * (ren_open_layer), the opaque ones whose origin is in the carry box,
 * drawn at B's origin - T: what the eye sees of B through the seam. Seam
 * ghosts draw with the home layer's entities (studio_draw). */
static void draw_other_layer(wld_layer_t *L, double time, const float eye[3])
{
	static wld_layer_t bt;		/* L with its content's draw origin */
	float off[3] = { 0, 0, 0 }, lo[3] = { 0, 0, 0 }, hi[3] = { 0, 0, 0 }, eye_l[3], g[3];
	const uint16_t *list = NULL;
	int e, k, cnt = 0, open = ren_open_layer(L->slot, off, lo, hi);

	if (open)
	{
		bt = *L;
		for (k = 0; k < 3; k++)
		{
			bt.gofs[k] = L->gofs[k] + off[k];
			eye_l[k] = eye[k] - bt.gofs[k];
		}
		list = wld_leaf_sectors(L, wld_point_leaf(L, eye_l), &cnt);
	}
	for (e = ent_next_live(1); e >= 0; e = ent_next_live(e))
	{
		const col_ent_t *cv;
		uint8_t a;

		if (!is_studio(e) || ent_idc(e)->layer != L->slot || (ent_rnd(e)->effects & EF_NODRAW)
			|| seam_ghost(e, ren_cam.map, g))
			continue;
		cv = ent_cv(e);
		for (k = 0; open && k < 3; k++)
			if (cv->origin[k] < lo[k] || cv->origin[k] > hi[k])
				break;
		if (!open || k < 3 || blend_mode(e, &a) != GPU_STUDIO_OPAQUE
			|| !visible(&bt, list, cnt, cv, NULL, eye))
		{
			ren_other_layer_ent(0);
			continue;
		}
		draw_entity(e, time, &bt, list, cnt, eye, -1, 0, NULL);
		ren_other_layer_ent(1);
	}
}

void studio_draw(double time)
{
	static cand_t cand[HL64_STUDIO_DRAWN];
	uint64_t t0 = sys_time_us();
	const float *eye = ren_cam.org;
	int li, e, n = 0, i;

	draw_time = time;
	gpu_studio_frame();
	gpu_studio_begin();
	for (li = 0; li < HL64_WORLD_LAYERS; li++)
	{
		wld_layer_t *L = wld_layer(li);
		const uint16_t *list;
		float eye_l[3];
		int cnt, k;

		lay_list[li] = NULL;
		lay_cnt[li] = 0;
		/* a non-home layer draws nothing, unless through an OPEN seam */
		if (L && L->map != ren_cam.map)
			draw_other_layer(L, time, eye);
		if (!L || L->map != ren_cam.map)
			continue;
		for (k = 0; k < 3; k++)
			eye_l[k] = eye[k] - L->gofs[k];
		list = wld_leaf_sectors(L, wld_point_leaf(L, eye_l), &cnt);
		lay_list[li] = list;
		lay_cnt[li] = cnt;
		n = 0;
		for (e = ent_next_live(1); e >= 0; e = ent_next_live(e))
		{
			const col_ent_t *cv;
			const float *sh = NULL;
			float shift[3];
			uint8_t a;
			float d = 0;

			if (!is_studio(e) || (ent_rnd(e)->effects & EF_NODRAW))
				continue;
			/* the home layer's own, and the seam ghosts it shows */
			if (ent_idc(e)->layer != li)
			{
				if (!ghost_shift(e, L, shift))
					continue;
				sh = shift;
			}
			cv = ent_cv(e);
			if (!visible(L, list, cnt, cv, sh, eye))
			{
				st.culled++;
				continue;
			}
			for (k = 0; k < 3; k++)
			{
				float v = cv->origin[k] + (sh ? sh[k] : 0) - eye_l[k];

				d += v * v;
			}
			if (n < HL64_STUDIO_DRAWN)
				i = n++;
			else
			{
				/* keep the nearest: replace the farthest */
				int far = 0;

				for (i = 1; i < n; i++)
					if (cand[i].dist > cand[far].dist)
						far = i;
				st.dropped++;
				if (cand[far].dist <= d)
					continue;
				i = far;
			}
			cand[i].e = (uint16_t)e;
			cand[i].trans = (uint16_t)(blend_mode(e, &a) != GPU_STUDIO_OPAQUE);
			cand[i].dist = d;
		}
		/* opaque here; translucent ones join the renderer's back-to-front
		 * pass (engine/render/trans.c calls studio_draw_trans) */
		for (i = 0; i < n; i++)
		{
			float shift[3];
			const float *sh = ent_idc(cand[i].e)->layer != li && ghost_shift(cand[i].e, L, shift)
				? shift : NULL;

			if (!cand[i].trans)
				draw_entity(cand[i].e, time, L, list, cnt, eye, -1, 0, sh);
			else
				ren_trans_studio(cand[i].e, cand[i].dist);
		}
	}
	gpu_studio_end();
	timed(t0);
	st.frames++;
}

void studio_draw_trans(int e, int gpu_mode, uint8_t alpha)
{
	uint64_t t0 = sys_time_us();
	float shift[3];
	const float *sh = NULL;
	int li;

	if (!is_studio(e))
		return;
	li = ent_idc(e)->layer;
	if (li < 0 || li >= HL64_WORLD_LAYERS)
		return;
	if (!lay_list[li])
	{
		/* a seam ghost: drawn in the home layer */
		for (li = 0; li < HL64_WORLD_LAYERS; li++)
			if (lay_list[li] && ghost_shift(e, wld_layer(li), shift))
				break;
		if (li == HL64_WORLD_LAYERS)
			return;
		sh = shift;
	}
	gpu_studio_begin();
	draw_entity(e, draw_time, wld_layer(li), lay_list[li], lay_cnt[li], ren_cam.org, gpu_mode,
		alpha, sh);
	gpu_studio_end();
	timed(t0);
}

void studio_draw_temp(res_t model, int body, int skin, const float org_g[3], const float ang[3],
	int gpu_mode, uint8_t alpha)
{
	uint64_t t0 = sys_time_us();
	smodel_t *m;
	gpu_studio_ent_t ge;
	game_pose_t p;
	int li, lv = 128;

	/* never a synchronous read on a draw: a temp model whose info or
	 * sequence 0 is not resident yet is requested and skipped */
	if (!anim_ready(model, 0) || !(m = mesh_of(model)))
		return;
	memset(&p, 0, sizeof(p));
	p.model = model;
	if (game_studio_pose_draw(&p, bones, MAXBONES) < m->info->numbones)
		return;
	slot_matrices(m);
	memset(&ge, 0, sizeof(ge));
	model_matrix(&ge.model, org_g, ang, 1.0f, ren_cam.org);
	ge.bones = (const float (*)[3][4])slots;
	ge.nbones = m->mesh->nbslots;
	ge.nruns = pick_runs(m, body, skin);
	ge.runs = runs_buf;
	ge.tex = tex_buf;
	for (li = 0; li < HL64_WORLD_LAYERS; li++)
	{
		wld_layer_t *L = wld_layer(li);
		float local[3];
		int k;

		if (!L || !lay_list[li])
			continue;
		for (k = 0; k < 3; k++)
			local[k] = org_g[k] - L->gofs[k];
		lv = light_at(-1, L, lay_list[li], lay_cnt[li], local, org_g);
		break;
	}
	ge.rgba[0] = ge.rgba[1] = ge.rgba[2] = (uint8_t)lv;
	ge.rgba[3] = gpu_mode ? alpha : 255;
	ge.mode = (uint8_t)gpu_mode;
	gpu_studio_begin();
	if (gpu_studio_draw(m->gstate, m->mesh, &ge))
		st.dropped++;
	else
		st.drawn++;
	gpu_studio_end();
	timed(t0);
}

static void draw_view(const float eye[3]);

void studio_draw_view(void)
{
	uint64_t t0 = sys_time_us();

	gpu_studio_begin();
	draw_view(ren_cam.org);
	gpu_studio_end();
	timed(t0);
	st.view_us += (uint32_t)(sys_time_us() - t0);
}

/* ---- view model ------------------------------------------------------------------------------ */

void studio_view_model(const studio_view_t *v)
{
	int i;

	view_set = v && v->model != RES_NONE;
	if (!view_set)
		return;
	view = *v;
	/* the view model's sequences stay resident while it is held (a new
	 * weapon animation never reads synchronously) */
	if ((i = model_index(v->model)) >= 0)
		want_all(i);
}

static float	chrome_up[MAXBONES][3], chrome_right[MAXBONES][3];
/* the packed normal components: x / 15.5 (5 bits), x / 31.5 (6 bits), the
 * same divisions done once (studio_init) instead of per vertex */
static float	nrm5[32], nrm6[64];

static void unpack_init(void)
{
	int x;

	for (x = 0; x < 32; x++)
		nrm5[x] = (float)(x >= 16 ? x - 32 : x) / 15.5f;
	for (x = 0; x < 64; x++)
		nrm6[x] = (float)(x >= 32 ? x - 64 : x) / 31.5f;
}

/* One CPU-skinned vertex: bone b's matrix on the bone-local position,
 * chrome texture coordinates from the normal's bone when chrome. k is
 * 2^pos_shift and ik its reciprocal (exact: x * ik = x / k). */
static void skin_vert(float k, float ik, int16_t pos[3], uint16_t nrm, int16_t st[2],
	int b, int nb, int chrome, const w64_texp_t *tex)
{
	float p[3], q;
	int r;

	for (r = 0; r < 3; r++)
		p[r] = pos[r] * ik;
	for (r = 0; r < 3; r++)
	{
		q = (bones[b][r][0] * p[0] + bones[b][r][1] * p[1] + bones[b][r][2] * p[2]
			+ bones[b][r][3]) * k;
		pos[r] = (int16_t)(q < -32768 ? -32768 : q > 32767 ? 32767 : q);
	}
	if (chrome)
	{
		float n[3], c[2];

		n[0] = nrm5[(nrm >> 11) & 31];
		n[1] = nrm6[(nrm >> 5) & 63];
		n[2] = nrm5[nrm & 31];
		game_studio_chrome_st(chrome_up[nb], chrome_right[nb], n, c);
		/* chrome coordinates span 64 source texels */
		st[0] = (int16_t)(c[0] * tex->w / 64.0f * 32.0f);
		st[1] = (int16_t)(c[1] * tex->h / 64.0f * 32.0f);
	}
}

static void draw_view(const float eye[3])
{
	smodel_t *m;
	const sfmt_mesh_t *ms;
	const uint8_t *b, *bslot, *nbone;
	const sfmt_run_t *runs;
	const sfmt_meshlet_t *ml;
	const sfmt_load_t *ld;
	static uint16_t view_crun[256], view_ctex[256];
	const uint32_t *tofs;
	const w64_vert_t *src;
	w64_vert_t *dst;
	gpu_studio_ent_t ge;
	game_pose_t p;
	float f[3], rt[3], u[3], pk, ipk;
	const float *gm;
	int n, ri, i, l, k, layer, nr, np = 0, nc = 0, drop = 0;

	if (!view_set)
		return;
	view_set = 0;
	if (!anim_ready(view.model, view.sequence) || !(m = mesh_of(view.model)))
		return;
	ms = m->mesh;
	if (ms->nslots > view_slots)
	{
		st.dropped++;
		return;
	}
	layer = ent_is_live(1) ? ent_idc(1)->layer : -1;
	gm = eng_layer_origin(layer);
	memset(&p, 0, sizeof(p));
	p.model = view.model;
	p.sequence = view.sequence;
	p.frame = view.frame;
	memcpy(p.angles, view.ang, sizeof(p.angles));
	memcpy(p.controller, view.controller, sizeof(p.controller));
	for (k = 0; k < 3; k++)
		p.origin[k] = view.org[k] + gm[k] - eye[k];
	n = game_studio_pose_draw(&p, bones, MAXBONES);
	if (n < m->info->numbones)
		return;
	b = (const uint8_t *)ms;
	bslot = b + ms->bslot_ofs;
	nbone = ms->nbone_ofs ? b + ms->nbone_ofs : NULL;
	runs = (const sfmt_run_t *)(b + ms->run_ofs);
	ml = (const sfmt_meshlet_t *)(b + ms->meshlet_ofs);
	ld = (const sfmt_load_t *)(b + ms->load_ofs);
	tofs = (const uint32_t *)(b + ms->tex_ofs);
	src = (const w64_vert_t *)(b + ms->vert_ofs);
	/* the drawn runs (body, skin): the chrome ones (texture coordinates
	 * from each vertex's normal) CPU-skinned, the others drawn from their
	 * blocks with the pose's bone matrices, as an entity's (the pose is
	 * already eye-relative: identity model matrix) */
	nr = pick_runs(m, view.body, view.skin);
	for (k = 0; k < nr; k++)
	{
		const w64_texp_t *tex = (const w64_texp_t *)(b + tofs[tex_buf[k]]);

		if (nbone && (tex->pad2 & SFMT_NF_CHROME))
		{
			view_crun[nc] = runs_buf[k];
			view_ctex[nc++] = tex_buf[k];
		}
		else
		{
			runs_buf[np] = runs_buf[k];
			tex_buf[np++] = tex_buf[k];
		}
	}
	memset(&ge, 0, sizeof(ge));
	ge.rgba[0] = ge.rgba[1] = ge.rgba[2] = ge.rgba[3] = 255;
	if (ent_is_live(1))
	{
		float org[3];

		for (k = 0; k < 3; k++)
			org[k] = view.org[k];
		/* the player's light at the eye */
		for (i = 0; i < HL64_WORLD_LAYERS; i++)
		{
			wld_layer_t *L = wld_layer(i);
			const uint16_t *list;
			float g[3];
			int cnt, lv;

			if (!L || L->slot != layer)
				continue;
			list = wld_leaf_sectors(L, wld_point_leaf(L, org), &cnt);
			for (k = 0; k < 3; k++)
				g[k] = org[k] + L->gofs[k];
			lv = light_at(1, L, list, cnt, org, g);
			ge.rgba[0] = ge.rgba[1] = ge.rgba[2] = (uint8_t)lv;
		}
	}
	ge.model.m[3][3] = 1;
	if (np)
	{
		slot_matrices(m);
		ge.model.m[0][0] = ge.model.m[1][1] = ge.model.m[2][2] = 1;
		ge.bones = (const float (*)[3][4])slots;
		ge.nbones = ms->nbslots;
		ge.nruns = np;
		ge.runs = runs_buf;
		ge.tex = tex_buf;
		if (gpu_studio_draw(m->gstate, ms, &ge))
			drop = 1;
	}
	if (nc)
	{
		uint32_t lo = 0xFFFFFFFFu, hi = 0;

		dst = view_verts[view_flip];
		view_flip ^= 1;
		pk = (float)(1 << ms->pos_shift);
		ipk = 1.0f / pk;
		ren_angle_axes(ren_cam.ang, f, rt, u);
		game_studio_chrome_axes((const float (*)[3][4])bones, n, rt, chrome_up, chrome_right);
		for (ri = 0; ri < nc; ri++)
		{
			const sfmt_run_t *ru = &runs[view_crun[ri]];
			const w64_texp_t *tex = (const w64_texp_t *)(b + tofs[view_ctex[ri]]);
			int slot = 0, s0;

			/* the run's first slot: every earlier load's padded count */
			for (i = 0; i < ru->meshlet_first; i++)
				for (l = ml[i].load_first; l < ml[i].load_first + ml[i].nloads; l++)
					slot += ld[l].count + (ld[l].count & 1);
			s0 = slot;
			for (i = ru->meshlet_first; i < ru->meshlet_first + ru->meshlet_count; i++)
				for (l = ml[i].load_first; l < ml[i].load_first + ml[i].nloads; l++)
				{
					int bone = bslot[ld[l].bslot], c = ld[l].count + (ld[l].count & 1);

					memcpy(&dst[slot >> 1], &src[slot >> 1], (uint32_t)c / 2u * sizeof(w64_vert_t));
					for (k = 0; k < c; k++, slot++)
					{
						w64_vert_t *v = &dst[slot >> 1];

						if (slot & 1)
							skin_vert(pk, ipk, v->posB, v->normB, v->stB, bone, nbone[slot],
								1, tex);
						else
							skin_vert(pk, ipk, v->posA, v->normA, v->stA, bone, nbone[slot],
								1, tex);
					}
				}
			if ((uint32_t)s0 < lo)
				lo = (uint32_t)s0;
			if ((uint32_t)slot > hi)
				hi = (uint32_t)slot;
		}
		gpu_writeback(&dst[lo >> 1], (hi - lo) / 2u * sizeof(w64_vert_t));
		ge.model.m[0][0] = ge.model.m[1][1] = ge.model.m[2][2] = ipk;
		ge.bones = NULL;
		ge.nbones = 0;
		ge.nruns = nc;
		ge.runs = view_crun;
		ge.tex = view_ctex;
		if (gpu_studio_draw_cpu(m->gstate, ms, dst, &ge))
			drop = 1;
	}
	if (drop)
		st.dropped++;
	else
		st.drawn++;
}

/* ---- hitbox source (trace-and-hulls.md 4.5) ------------------------------------------------------ */

static col_hitbox_t	hb[255];

/* 1 when the segment a -> b passes farther than r from c */
static int seg_beyond(const float a[3], const float b[3], const float c[3], float r)
{
	float ab[3], ac[3], p[3], t, l2, d2 = 0;
	int k;

	for (k = 0; k < 3; k++)
	{
		ab[k] = b[k] - a[k];
		ac[k] = c[k] - a[k];
	}
	l2 = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2];
	t = l2 > 0 ? (ac[0] * ab[0] + ac[1] * ab[1] + ac[2] * ab[2]) / l2 : 0;
	if (!(t > 0))
		t = 0;
	else if (t > 1)
		t = 1;
	for (k = 0; k < 3; k++)
	{
		p[k] = a[k] + t * ab[k] - c[k];
		d2 += p[k] * p[k];
	}
	return d2 > r * r;
}

int studio_hitboxes(const col_ent_t *e, const float start[3], const float end[3],
	const col_hitbox_t **out)
{
	const ent_rnd_t *r;
	const sfmt_hitbox_t *x;
	const float (*pb)[3][4];
	smodel_t *m;
	game_pose_t p;
	float reach;
	uint8_t keep[255];
	int i, n, ready;

	*out = hb;
	if (!ent_is_live(e->index))
		return 0;
	r = ent_rnd(e->index);
	/* never a synchronous read in a playing frame's trace: while the
	 * current sequence's animation (or the info) is still landing (a
	 * sequence change a frame or two ago; requested here), the box stands
	 * for the hitboxes; load I/O (boot, tools, map loads) reads it */
	ready = anim_ready(r->model, r->sequence);
	if ((!ready && !io_loading()) || !(m = info_of(r->model)) || !m->info->numhitboxes)
		return 0;
	/* a line beyond every hitbox of every pose of the sequence: no pose
	 * (a long trace's box meets many entities' boxes) */
	if (start && ready && (reach = game_studio_hitbox_reach(r->model, r->sequence)) >= 0
		&& seg_beyond(start, end, e->origin, reach))
		return -1;
	memset(&p, 0, sizeof(p));
	p.model = r->model;
	p.sequence = r->sequence;
	p.frame = r->frame;
	memcpy(p.origin, e->origin, sizeof(p.origin));
	memcpy(p.angles, e->angles, sizeof(p.angles));
	memcpy(p.controller, r->controller, sizeof(p.controller));
	memcpy(p.blending, r->blending, sizeof(p.blending));
	/* only the hitboxes the line can meet (their bones' chains posed) */
	n = game_studio_hitbox_pose(&p, ready ? start : NULL, end, &pb, keep);
	if (n <= 0)
		return n;
	x = SFMT_HITBOXES(m->info);
	for (i = n = 0; i < m->info->numhitboxes; i++)
	{
		if (!keep[i])
			continue;
		memcpy(hb[n].bone, pb[x[i].bone], sizeof(hb[n].bone));
		memcpy(hb[n].mins, x[i].bbmin, sizeof(hb[n].mins));
		memcpy(hb[n].maxs, x[i].bbmax, sizeof(hb[n].maxs));
		hb[n].group = x[i].group;
		n++;
	}
	return n;
}

/* ---- queries, stats --------------------------------------------------------------------------- */

int studio_body_count(res_t model)
{
	smodel_t *m = info_of(model);
	int i, n = 1;

	if (!m)
		return 1;
	for (i = 0; i < m->info->numbodyparts; i++)
		n *= SFMT_BODYPARTS(m->info)[i].nummodels;
	return n;
}

void studio_stats(studio_stats_t *s, int reset)
{
	st.pool_used = pool.used;
	if (pool.used > st.pool_peak)
		st.pool_peak = pool.used;
	*s = st;
	if (reset)
	{
		uint32_t models_n = st.models, peak = st.pool_peak, anims_n = st.anims;
		uint32_t play = st.sync_play, want = st.want_peak, soft = st.soft;

		memset(&st, 0, sizeof(st));
		st.models = models_n;
		st.pool_peak = peak;
		st.anims = anims_n;
		st.sync_play = play;
		st.want_peak = want;
		st.soft = soft;
	}
}
