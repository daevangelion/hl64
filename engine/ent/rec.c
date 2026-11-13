/* SPDX-License-Identifier: MIT */
/*
 * rec.c -- map entity records: HLE1 chunks per HLE slot, record states,
 * typed keys, names, HLSDK FireTargets, logic records, logic timers, the
 * hydration pump and pending-record wakes (docs/architecture.md 6.4, 7.1
 * "records"; docs/formats/hle-format-v1.md; docs/specs/think-and-time.md
 * 5-6, area-links-and-touch.md 3 "Pending-record wake").
 *
 * Retargeted from the old tree's source/hl/hl_ents.c/.h (audit entry
 * source/hl/hl_ents.c: CLEAN + QUAKE-INTERFACE): the design is kept (one
 * in-place chunk per map, a dense pending list with a hashed pending grid
 * for box wakes, a bounded timer queue, a budgeted distance-ordered pump
 * with a prefetch ring, capped synchronous wakes with a deferred-use FIFO);
 * every Quake binding is gone: no edict_t / entvars / pr_* / sv.* / cvars,
 * entities are component slots (include/hl64/ent.h), names, classes, keys
 * and resources are cooked ids. The multi_manager think (audit range
 * 2486-2524, HLSDK) is not here: it is the game's (game_logic_timer), the
 * engine only keeps HLSDK's float nextthink and due order (think-and-time.md
 * 5).
 *
 * Memory: the HLE_BLOBS region holds the slot table, timers, deferred uses
 * and name holders, then one block per open slot (its chunk, then the
 * per-record runtime, logic blocks and the pending lists); the DEHYDRATE
 * region holds the store. Nothing is allocated elsewhere.
 */
#include <math.h>
#include <string.h>

#define HLE_FORMAT_ONLY
#include "tools/w64c/hl_entcook.h"

#include "hl64/ent.h"
#include "hl64/game.h"
#include "hl64/limits.h"
#include "hl64/rec.h"
#include "engine/core/log.h"
#include "engine/core/sys.h"
#include "engine/ent/rec_int.h"
#include "engine/io/io.h"
#include "engine/mem/carve.h"
#include "engine/phys/phys.h"
#include "engine/world/wld.h"

/* record runtime flags */
#define RT_LOGIC	0x01
#define RT_POINT	0x02
#define RT_PHYS		0x04
#define RT_HUGE		0x08	/* pending-grid huge list */

/* pending grid (architecture.md 6.4: 512 cells of 512 u per map) */
#define PG_BUCKETS	512
#define PG_CELL		512.0f
#define PG_HALF		256.0f	/* records up to this half-size sit in their centre cell */
#define PG_SCAN_CELLS	64	/* a query over more cells scans the pending list */

typedef struct {
	uint8_t		state;		/* REC_* */
	uint8_t		flags;		/* RT_* */
	uint16_t	aux;		/* logic: block index; physical: pending-list position */
	ent_h		h;		/* live entity */
	int16_t		lo[3], hi[3];	/* cooked bounds, map-local, rounded outwards */
	uint16_t	pnext;		/* pending-grid chain */
	uint16_t	pad;
} rrt_t;			/* 24 */

typedef struct {
	uint8_t			open;
	uint8_t			slot;
	uint16_t		map;		/* campaign map */
	uint32_t		block, block_len;	/* in the HLE region */
	uint8_t			*b;		/* the chunk */
	uint32_t		len;
	const hle_header_t	*h;
	const hle_rec_t		*recs;
	const uint16_t		*l2r;
	const hle_class_t	*cls;
	const hle_keyname_t	*kn;
	const hle_name_t	*names;
	const uint16_t		*hash;
	const hle_key_t		*keys;
	const uint16_t		*ll;
	const hle_fire_t	*fire;
	const float		*fl;
	const char		*str;
	const void		*graph;
	uint32_t		graph_size;
	rrt_t			*rt;
	rec_logic_t		*logic;
	uint16_t		*plist;		/* pending physical records */
	uint16_t		npend, nlogic;
	uint16_t		huge;		/* pending-grid huge chain */
	uint16_t		kn_of_key[RK__N];	/* keys.def id -> keyname index */
	uint16_t		head[PG_BUCKETS];
	float			pmin[3], pmax[3];	/* hull of the pending boxes */
	uint32_t		pgen;		/* pending-set generation */
	/* pump shortlist: the last scan found nothing to spawn here */
	uint32_t		idle_gen;
	float			idle_at[3];
	uint8_t			idle;
	uint8_t			logic_pending;
} rmap_t;

enum { TM_FREE, TM_REC, TM_FIRE };

typedef struct {
	float		t;		/* f32 nextthink */
	uint32_t	seq;		/* insertion order */
	uint8_t		kind, use_type;
	uint16_t	arg;
	gid_t16		gid;		/* TM_REC record; TM_FIRE caller record */
	name_t		target, killtarget;
	uint16_t	hold;		/* re-armed by its own callback in pass `hold`: waits */
	ent_h		act, caller;
	float		value;
} rtimer_t;			/* 32 */

typedef struct {
	gid_t16		gid, caller_gid;
	uint8_t		use_type, plain;	/* plain: a wake without a use */
	uint16_t	pad;
	ent_h		act, caller;
	float		value;
} rdefer_t;			/* 20 */

typedef struct {
	ent_h		h;
	name_t		n;
	uint16_t	pad;
} rhold_t;

/* frozen timers of a closed map (think-and-time.md 5) */
typedef struct {
	uint16_t	map, lump;	/* lump of a TM_REC record / the caller's */
	rtimer_t	tm;		/* t = remaining time */
} rfrozen_t;

static struct {
	rmap_t		*map;		/* [REC_SLOTS] */
	rtimer_t	*tm;		/* [REC_TIMERS] */
	rdefer_t	*defer;		/* [REC_DEFER] FIFO */
	rhold_t		*hold;		/* [REC_HOLDERS] */
	rfrozen_t	*frozen;	/* [REC_TIMERS] */
	uint8_t		*heap;		/* map blocks */
	uint32_t	heap_len;
	uint32_t	ntm, seq, nfrozen;
	uint32_t	defer_head, ndefer;
	uint32_t	sync_wakes;	/* this frame's in-call hydrations */
	uint16_t	pass;		/* rec_run_timers pass number, never 0 */
	gid_t16		running;	/* record whose timer callback runs, GID_NONE */
	gid_t16		caller;		/* rec_caller_gid */
	gid_t16		self;		/* record / entity being dispatched */
	rec_stats_t	st;		/* window counters + current state */
	uint64_t	us_sum;
	uint32_t	us_max, frames, frame_us;
	uint32_t	live_peak;
	uint8_t		*store;		/* DEHYDRATE region */
	uint32_t	store_used, store_cap, store_peak;
} R;

static const float zero3[3] = { 0, 0, 0 };

/* ---- slots and records ------------------------------------------------------- */

static rmap_t *slot_map(int slot)
{
	return slot >= 0 && slot < REC_SLOTS && R.map && R.map[slot].open ? &R.map[slot] : NULL;
}

/* record index of a gid, -1 bad */
static int rec_index(gid_t16 gid, rmap_t **mp)
{
	rmap_t *m;
	uint32_t lump = GID_LUMP(gid), ri;

	if (gid == GID_NONE || !(m = slot_map(GID_SLOT(gid))) || lump >= m->h->numlumps)
		return -1;
	ri = m->l2r[lump];
	if (ri == HLE_NONE)
		return -1;
	*mp = m;
	return (int)ri;
}

static gid_t16 gid_of(const rmap_t *m, int ri)
{
	return GID_MAKE(m->slot, m->recs[ri].lump);
}

static float ftou(uint32_t u)
{
	union { uint32_t u; float f; } v;

	v.u = u;
	return v.f;
}

/* last key of the record with keys.def id k (later keys win), NULL absent */
static const hle_key_t *rkey(const rmap_t *m, int ri, key_t16 k)
{
	const hle_rec_t *r = &m->recs[ri];
	uint32_t kn, i;

	if (k >= RK__N || (kn = m->kn_of_key[k]) == HLE_NONE)
		return NULL;
	for (i = r->key_num; i-- > 0;)
		if (m->keys[r->key_first + i].kn == kn)
			return &m->keys[r->key_first + i];
	return NULL;
}

static uint16_t rkind(const rmap_t *m, const hle_key_t *k)
{
	return m->kn[k->kn].kind;
}

/* ---- the pending set ---------------------------------------------------------- */

static int ifloor(float x)
{
	return (int)floorf(x);
}

static uint32_t pg_bucket(int x, int y, int z)
{
	return ((uint32_t)x * 73856093u ^ (uint32_t)y * 19349663u ^ (uint32_t)z * 83492791u)
		& (PG_BUCKETS - 1);
}

static void pend_add(rmap_t *m, int ri)
{
	rrt_t *t = &m->rt[ri];
	int k;

	t->aux = m->npend;
	m->plist[m->npend++] = (uint16_t)ri;
	for (k = 0; k < 3; k++)
	{
		float c = 0.5f * ((float)t->lo[k] + (float)t->hi[k]);

		if (m->npend == 1 || t->lo[k] < m->pmin[k])
			m->pmin[k] = t->lo[k];
		if (m->npend == 1 || t->hi[k] > m->pmax[k])
			m->pmax[k] = t->hi[k];
		(void)c;
	}
	if ((float)(t->hi[0] - t->lo[0]) > 2 * PG_HALF || (float)(t->hi[1] - t->lo[1]) > 2 * PG_HALF
		|| (float)(t->hi[2] - t->lo[2]) > 2 * PG_HALF)
	{
		t->flags |= RT_HUGE;
		t->pnext = m->huge;
		m->huge = (uint16_t)ri;
	}
	else
	{
		uint32_t b = pg_bucket(ifloor(0.5f * (t->lo[0] + t->hi[0]) / PG_CELL),
			ifloor(0.5f * (t->lo[1] + t->hi[1]) / PG_CELL),
			ifloor(0.5f * (t->lo[2] + t->hi[2]) / PG_CELL));

		t->flags &= (uint8_t)~RT_HUGE;
		t->pnext = m->head[b];
		m->head[b] = (uint16_t)ri;
	}
	m->pgen++;
}

static void chain_remove(rmap_t *m, uint16_t *head, int ri)
{
	uint16_t *p = head;

	while (*p != HLE_NONE)
	{
		if (*p == ri)
		{
			*p = m->rt[ri].pnext;
			return;
		}
		p = &m->rt[*p].pnext;
	}
}

static void pend_remove(rmap_t *m, int ri)
{
	rrt_t *t = &m->rt[ri];
	uint16_t last;

	if (t->state != REC_PENDING && t->state != REC_DRY)
		return;
	if (!(t->flags & RT_PHYS))
		return;
	last = m->plist[--m->npend];
	m->plist[t->aux] = last;
	m->rt[last].aux = t->aux;
	if (t->flags & RT_HUGE)
		chain_remove(m, &m->huge, ri);
	else
		chain_remove(m, &m->head[pg_bucket(ifloor(0.5f * (t->lo[0] + t->hi[0]) / PG_CELL),
			ifloor(0.5f * (t->lo[1] + t->hi[1]) / PG_CELL),
			ifloor(0.5f * (t->lo[2] + t->hi[2]) / PG_CELL))], ri);
	m->pgen++;
}

/* ---- entities ------------------------------------------------------------------ */

/* live entity of a LIVE physical record; a slot the game freed without
 * rec_kill means the entity is gone for good (HLSDK: removed entities never
 * respawn) */
static int rt_entity(rrt_t *t)
{
	int e;

	if (t->state != REC_LIVE || !(t->flags & RT_PHYS))
		return -1;
	e = ent_of(t->h);
	if (e < 0)
		t->state = REC_KILLED;
	return e;
}

static int hydrate(rmap_t *m, int ri, int reserve)
{
	rrt_t *t = &m->rt[ri];
	const hle_rec_t *r = &m->recs[ri];
	gid_t16 gid = gid_of(m, ri);
	ent_id_t *id;
	col_ent_t *cv;
	int e, prev, ok;
	uint64_t t0 = sys_time_us();

	if (!(t->flags & RT_PHYS) || (t->state != REC_PENDING && t->state != REC_DRY))
		return -1;
	e = ent_create(reserve);
	if (e < 0)
		return -1;
	id = ent_idc(e);
	id->layer = (int8_t)m->slot;
	id->gid = gid;
	id->cls = m->cls[r->cls].id;
	id->targetname = r->tname == HLE_NONE ? NAME_NONE : NAME_MAKE(m->slot, r->tname);
	cv = ent_cv(e);
	rec_origin(gid, cv->origin);
	rec_angles(gid, cv->angles);
	if (r->trank != HLE_NONE)
		ent_set_rank(cv, m->slot, r->trank);
	pend_remove(m, ri);
	t->state = REC_LIVE;
	t->h = ent_handle_of(e);
	prev = eng_frame_layer();
	eng_set_frame_layer(m->slot);
	ok = game_spawn_record(gid, e);
	eng_set_frame_layer(prev);
	if (ok < 0)
	{
		if (ent_is_live(e))
			ent_destroy(e);
		t->state = REC_KILLED;
		return -1;
	}
	R.st.hydrated++;
	(void)t0;
	return e;
}

static void spawn_logic(rmap_t *m, int ri)
{
	rrt_t *t = &m->rt[ri];
	int prev;

	if (!(t->flags & RT_LOGIC) || t->state != REC_PENDING)
		return;
	memset(&m->logic[t->aux], 0, sizeof(rec_logic_t));
	t->state = REC_LIVE;
	prev = eng_frame_layer();
	eng_set_frame_layer(m->slot);
	game_spawn_logic(gid_of(m, ri));
	eng_set_frame_layer(prev);
}

/* ---- record queries --------------------------------------------------------------- */

int rec_kind(gid_t16 gid)
{
	rmap_t *m;
	int ri = rec_index(gid, &m);

	if (ri < 0)
		return -1;
	return m->rt[ri].flags & RT_LOGIC ? REC_KIND_LOGIC
		: m->rt[ri].flags & RT_POINT ? REC_KIND_POINT : REC_KIND_ENTITY;
}

int rec_state(gid_t16 gid)
{
	rmap_t *m;
	int ri = rec_index(gid, &m);

	if (ri < 0)
		return -1;
	rt_entity(&m->rt[ri]);
	return m->rt[ri].state;
}

cls_t rec_class(gid_t16 gid)
{
	rmap_t *m;
	int ri = rec_index(gid, &m);

	return ri < 0 ? CLS_NONE : m->cls[m->recs[ri].cls].id;
}

name_t rec_targetname(gid_t16 gid)
{
	rmap_t *m;
	int ri = rec_index(gid, &m);

	return ri < 0 || m->recs[ri].tname == HLE_NONE ? NAME_NONE
		: NAME_MAKE(m->slot, m->recs[ri].tname);
}

uint32_t rec_flags(gid_t16 gid)
{
	rmap_t *m;
	int ri = rec_index(gid, &m);

	return ri < 0 ? 0 : m->recs[ri].flags;
}

int rec_key_v(gid_t16 gid, key_t16 k, float out[3])
{
	rmap_t *m;
	int ri = rec_index(gid, &m);
	const hle_key_t *key;

	if (ri < 0 || !(key = rkey(m, ri, k)) || rkind(m, key) != HLE_KV)
		return 0;
	memcpy(out, m->fl + key->val, 3 * sizeof(float));
	return 1;
}

void rec_origin(gid_t16 gid, float out[3])
{
	if (!rec_key_v(gid, RK_ORIGIN, out))
		memcpy(out, zero3, sizeof(zero3));
}

void rec_angles(gid_t16 gid, float out[3])
{
	if (!rec_key_v(gid, RK_ANGLES, out))
		memcpy(out, zero3, sizeof(zero3));
}

int rec_key_f(gid_t16 gid, key_t16 k, float *out)
{
	rmap_t *m;
	int ri = rec_index(gid, &m);
	const hle_key_t *key;

	if (ri < 0 || !(key = rkey(m, ri, k)) || rkind(m, key) != HLE_KF)
		return 0;
	*out = ftou(key->val);
	return 1;
}

/* HLE hash lookup of a name string in a map (spawn time) */
static int name_lookup(const rmap_t *m, const char *s)
{
	uint32_t mask = (uint32_t)m->h->hashsize - 1, x = hle_hash(s) & mask, n;

	for (n = 0; n <= mask; n++, x = (x + 1) & mask)
	{
		uint16_t v = m->hash[x];

		if (v == HLE_NONE)
			return -1;
		if (!strcmp(m->str + m->names[v].str, s))
			return v;
	}
	return -1;
}

name_t rec_key_name(gid_t16 gid, key_t16 k)
{
	rmap_t *m;
	int ri = rec_index(gid, &m), v;
	const hle_key_t *key;

	if (ri < 0 || !(key = rkey(m, ri, k)))
		return NAME_NONE;
	if (rkind(m, key) == HLE_KN)
		return NAME_MAKE(m->slot, key->val);
	if (rkind(m, key) == HLE_KS && (v = name_lookup(m, m->str + key->val)) >= 0)
		return NAME_MAKE(m->slot, v);
	return NAME_NONE;
}

static int typed_id(gid_t16 gid, key_t16 k, uint16_t want_kind, const hle_key_t **out)
{
	rmap_t *m;
	int ri = rec_index(gid, &m);
	const hle_key_t *key;

	if (ri < 0 || !(key = rkey(m, ri, k)) || rkind(m, key) != want_kind || key->id == HLE_NONE)
		return -1;
	*out = key;
	return ri;
}

res_t rec_key_res(gid_t16 gid, key_t16 k)
{
	const hle_key_t *key;
	cls_t c = rec_class(gid);

	if (k == RK_PATTERN || k == RK_GLOBALSTATE || k == RK_MONSTERTYPE
		|| (k == RK_MESSAGE && c == RC_ENV_MESSAGE) || (k == RK_CHAPTERTITLE && c == RC_WORLDSPAWN)
		|| typed_id(gid, k, HLE_KS, &key) < 0)
		return RES_NONE;
	if (RES_KIND(key->id) == RES_K_SUBMODEL)
		return RES_SUBMODEL(GID_SLOT(gid), RES_SUB_NUM(key->id));
	return key->id;
}

int rec_key_global(gid_t16 gid, key_t16 k)
{
	const hle_key_t *key;

	return k == RK_GLOBALSTATE && typed_id(gid, k, HLE_KS, &key) >= 0 ? key->id : -1;
}

int rec_key_pattern(gid_t16 gid, key_t16 k)
{
	const hle_key_t *key;

	return k == RK_PATTERN && typed_id(gid, k, HLE_KS, &key) >= 0 ? key->id : -1;
}

int rec_key_title(gid_t16 gid, key_t16 k)
{
	const hle_key_t *key;
	cls_t c = rec_class(gid);

	if (!((k == RK_MESSAGE && c == RC_ENV_MESSAGE) || (k == RK_CHAPTERTITLE && c == RC_WORLDSPAWN)))
		return -1;
	return typed_id(gid, k, HLE_KS, &key) >= 0 ? key->id : -1;
}

cls_t rec_key_cls(gid_t16 gid, key_t16 k)
{
	const hle_key_t *key;

	if (typed_id(gid, k, HLE_KN, &key) >= 0
		|| (k == RK_MONSTERTYPE && typed_id(gid, k, HLE_KS, &key) >= 0))
		return key->id;
	return CLS_NONE;
}

const char *rec_key_str(gid_t16 gid, key_t16 k)
{
	rmap_t *m;
	int ri = rec_index(gid, &m);
	const hle_key_t *key;

	if (ri < 0 || !(key = rkey(m, ri, k)))
		return NULL;
	if (rkind(m, key) == HLE_KS)
		return m->str + key->val;
	if (rkind(m, key) == HLE_KN)
		return m->str + m->names[key->val].str;
	return NULL;
}

int rec_key_count(gid_t16 gid)
{
	rmap_t *m;
	int ri = rec_index(gid, &m);

	return ri < 0 ? 0 : m->recs[ri].key_num;
}

key_t16 rec_key_id(gid_t16 gid, int i)
{
	rmap_t *m;
	int ri = rec_index(gid, &m);

	if (ri < 0 || i < 0 || i >= m->recs[ri].key_num)
		return 0xFFFF;
	return m->kn[m->keys[m->recs[ri].key_first + i].kn].id;
}

int rec_aux_count(gid_t16 gid)
{
	rmap_t *m;
	int ri = rec_index(gid, &m);

	return ri < 0 ? 0 : m->recs[ri].aux_count;
}

gid_t16 rec_aux_gid(gid_t16 gid, int i)
{
	rmap_t *m;
	int ri = rec_index(gid, &m);
	const hle_rec_t *r;

	if (ri < 0)
		return GID_NONE;
	r = &m->recs[ri];
	if ((r->flags & HLE_RF_AUXFIRE) || i < 0 || i >= r->aux_count)
		return GID_NONE;
	return GID_MAKE(m->slot, m->ll[r->aux_first + i]);
}

void rec_fire_entry(gid_t16 gid, int i, name_t *name, float *delay)
{
	rmap_t *m;
	int ri = rec_index(gid, &m);
	const hle_rec_t *r;

	*name = NAME_NONE;
	*delay = 0;
	if (ri < 0)
		return;
	r = &m->recs[ri];
	if (!(r->flags & HLE_RF_AUXFIRE) || i < 0 || i >= r->aux_count)
		return;
	*name = NAME_MAKE(m->slot, m->fire[r->aux_first + i].name);
	*delay = m->fire[r->aux_first + i].delay;
}

int rec_entity(gid_t16 gid)
{
	rmap_t *m;
	int ri = rec_index(gid, &m);

	return ri < 0 ? -1 : rt_entity(&m->rt[ri]);
}

int rec_wake(gid_t16 gid)
{
	rmap_t *m;
	int ri = rec_index(gid, &m), e;

	if (ri < 0)
		return -1;
	if ((e = rt_entity(&m->rt[ri])) >= 0)
		return e;
	e = hydrate(m, ri, ENT_ALLOC_GAME);
	if (e >= 0)
		R.st.woken++;
	return e;
}

static void cancel_timers(gid_t16 gid);

void rec_kill(gid_t16 gid)
{
	rmap_t *m;
	int ri = rec_index(gid, &m);

	if (ri < 0)
		return;
	pend_remove(m, ri);
	m->rt[ri].state = REC_KILLED;
	if (m->rt[ri].flags & RT_LOGIC)
		cancel_timers(gid);
}

rec_logic_t *rec_logic(gid_t16 gid)
{
	rmap_t *m;
	int ri = rec_index(gid, &m);

	if (ri < 0 || !(m->rt[ri].flags & RT_LOGIC))
		return NULL;
	return &m->logic[m->rt[ri].aux];
}

/* ---- names ---------------------------------------------------------------------- */

static const hle_name_t *name_of(name_t n, rmap_t **mp)
{
	rmap_t *m;

	if (n == NAME_NONE || !(m = slot_map(NAME_SLOT(n))) || NAME_INDEX(n) >= m->h->numnames)
		return NULL;
	*mp = m;
	return &m->names[NAME_INDEX(n)];
}

int rec_name_count(name_t n)
{
	rmap_t *m;
	const hle_name_t *nm = name_of(n, &m);

	return nm ? nm->count : 0;
}

gid_t16 rec_name_gid(name_t n, int i)
{
	rmap_t *m;
	const hle_name_t *nm = name_of(n, &m);

	if (!nm || i < 0 || i >= nm->count)
		return GID_NONE;
	return GID_MAKE(m->slot, m->ll[nm->first + i]);
}

name_t rec_name_in(name_t n, int slot)
{
	rmap_t *m, *o = slot_map(slot);
	const hle_name_t *nm = name_of(n, &m);
	int v;

	if (!nm || !o)
		return NAME_NONE;
	if (o == m)
		return n;
	v = name_lookup(o, m->str + nm->str);
	return v < 0 ? NAME_NONE : NAME_MAKE(slot, v);
}

name_t rec_name_find(int slot, const char *s)
{
	rmap_t *m = slot_map(slot);
	int v;

	if (!m || !s)
		return NAME_NONE;
	v = name_lookup(m, s);
	return v < 0 ? NAME_NONE : NAME_MAKE(slot, v);
}

int rec_next_named(name_t n, int *it)
{
	rmap_t *m;
	const hle_name_t *nm = name_of(n, &m);
	int count = nm ? nm->count : 0, j;

	while (*it < count)
	{
		int i = (*it)++, ri = m->l2r[m->ll[nm->first + i]], e;
		rrt_t *t = &m->rt[ri];

		if (!(t->flags & RT_PHYS))
			continue;
		if ((e = rt_entity(t)) >= 0)
			return e;
		if (t->state == REC_PENDING || t->state == REC_DRY)
		{
			e = hydrate(m, ri, ENT_ALLOC_GAME);
			if (e >= 0)
			{
				R.st.woken++;
				return e;
			}
		}
	}
	if (n == NAME_NONE)
		return -1;
	for (j = *it - count; j < REC_HOLDERS; j++)
	{
		int e;

		(*it)++;
		if (R.hold[j].n == n && (e = ent_of(R.hold[j].h)) >= 0)
			return e;
	}
	return -1;
}

void rec_name_hold(int e, name_t n)
{
	int j, freej = -1;

	if (!ent_is_live(e) || n == NAME_NONE)
		return;
	for (j = 0; j < REC_HOLDERS; j++)
	{
		if (R.hold[j].n == NAME_NONE || ent_of(R.hold[j].h) < 0)
		{
			if (freej < 0)
				freej = j;
		}
		else if (ent_of(R.hold[j].h) == e)
		{
			R.hold[j].n = n;
			return;
		}
	}
	if (freej < 0)
		sys_error("rec: more than %d runtime entities hold map names", REC_HOLDERS);
	R.hold[freej].h = ent_handle_of(e);
	R.hold[freej].n = n;
}

const void *rec_node_graph(int slot, uint32_t *size)
{
	rmap_t *m = slot_map(slot);

	if (!m || !m->graph)
		return NULL;
	if (size)
		*size = m->graph_size;
	return m->graph;
}

/* ---- uses ------------------------------------------------------------------------ */

gid_t16 rec_caller_gid(void)
{
	return R.caller;
}

static void use_ent(int e, int act, int caller, gid_t16 caller_gid, int use_type, float value)
{
	gid_t16 pc = R.caller, ps = R.self;
	int prev = eng_frame_layer();

	R.caller = caller_gid != GID_NONE ? caller_gid : caller >= 0 ? ent_idc(caller)->gid : GID_NONE;
	R.self = ent_idc(e)->gid;
	eng_set_frame_layer(ent_idc(e)->layer);
	game_use(e, act, caller, use_type, value);
	eng_set_frame_layer(prev);
	R.caller = pc;
	R.self = ps;
}

static void use_logic(gid_t16 gid, int act, int caller, gid_t16 caller_gid, int use_type, float value)
{
	gid_t16 pc = R.caller, ps = R.self;
	int prev = eng_frame_layer();

	R.caller = caller_gid != GID_NONE ? caller_gid : caller >= 0 ? ent_idc(caller)->gid : GID_NONE;
	R.self = gid;
	eng_set_frame_layer(GID_SLOT(gid));
	game_logic_use(gid, act, caller, use_type, value);
	eng_set_frame_layer(prev);
	R.caller = pc;
	R.self = ps;
}

static ent_h handle_or_none(int e)
{
	return ent_is_live(e) ? ent_handle_of(e) : 0;
}

static int slot_or_none(ent_h h, int had)
{
	return had ? ent_of(h) : -1;
}

static void defer(gid_t16 gid, int act, int caller, gid_t16 caller_gid, int use_type, float value,
	int plain)
{
	rdefer_t *d = &R.defer[(R.defer_head + R.ndefer) % REC_DEFER];

	d->gid = gid;
	d->caller_gid = caller_gid;
	d->use_type = (uint8_t)use_type;
	d->plain = (uint8_t)(plain | (ent_is_live(act) ? 2 : 0) | (ent_is_live(caller) ? 4 : 0));
	d->act = handle_or_none(act);
	d->caller = handle_or_none(caller);
	d->value = value;
	R.ndefer++;
}

void rec_fire(name_t n, int act, int caller, gid_t16 caller_gid, int use_type, float value)
{
	rmap_t *m;
	const hle_name_t *nm = name_of(n, &m);
	int i, j, count = nm ? nm->count : 0;

	for (i = 0; i < count; i++)
	{
		int ri = m->l2r[m->ll[nm->first + i]], e;
		rrt_t *t = &m->rt[ri];
		gid_t16 gid = gid_of(m, ri);

		if (t->flags & RT_LOGIC)
		{
			if (t->state == REC_LIVE)
				use_logic(gid, act, caller, caller_gid, use_type, value);
			continue;
		}
		if (t->flags & RT_POINT)
			continue;
		if ((e = rt_entity(t)) >= 0)
		{
			use_ent(e, act, caller, caller_gid, use_type, value);
			continue;
		}
		if (t->state != REC_PENDING && t->state != REC_DRY)
			continue;
		if (R.sync_wakes < REC_SYNC_WAKES || R.ndefer >= REC_DEFER)
		{
			R.sync_wakes++;
			if ((e = hydrate(m, ri, ENT_ALLOC_GAME)) >= 0)
			{
				R.st.woken++;
				use_ent(e, act, caller, caller_gid, use_type, value);
			}
		}
		else
			defer(gid, act, caller, caller_gid, use_type, value, 0);
	}
	if (n == NAME_NONE)
		return;
	for (j = 0; j < REC_HOLDERS; j++)
	{
		int e;

		if (R.hold[j].n == n && (e = ent_of(R.hold[j].h)) >= 0)
			use_ent(e, act, caller, caller_gid, use_type, value);
	}
}

/* HLSDK killtarget: every entity with the name removed (UTIL_Remove), its
 * record never respawns; logic records stop */
static void kill_name(name_t n)
{
	rmap_t *m;
	const hle_name_t *nm = name_of(n, &m);
	int i, j;

	for (i = 0; nm && i < nm->count; i++)
	{
		int ri = m->l2r[m->ll[nm->first + i]], e = rt_entity(&m->rt[ri]);

		if (m->rt[ri].flags & RT_POINT)
			continue;
		if (e >= 0)
			ent_remove(e);
		rec_kill(gid_of(m, ri));
	}
	for (j = 0; j < REC_HOLDERS; j++)
	{
		int e;

		if (R.hold[j].n == n && (e = ent_of(R.hold[j].h)) >= 0)
		{
			ent_remove(e);
			R.hold[j].n = NAME_NONE;
		}
	}
}

/* ---- logic timers (think-and-time.md 5) ------------------------------------------ */

static rtimer_t *timer_new(float t)
{
	uint32_t i;

	for (i = 0; i < REC_TIMERS; i++)
		if (R.tm[i].kind == TM_FREE)
		{
			memset(&R.tm[i], 0, sizeof(R.tm[i]));
			R.tm[i].t = t;
			R.tm[i].seq = R.seq++;
			R.ntm++;
			if (R.ntm > R.st.timers_peak)
				R.st.timers_peak = R.ntm;
			return &R.tm[i];
		}
	return NULL;
}

static void timer_free(rtimer_t *tm)
{
	tm->kind = TM_FREE;
	R.ntm--;
}

static void cancel_timers(gid_t16 gid)
{
	uint32_t i;

	for (i = 0; i < REC_TIMERS; i++)
		if (R.tm[i].kind == TM_REC && R.tm[i].gid == gid)
			timer_free(&R.tm[i]);
}

void rec_timer(gid_t16 gid, float t, uint16_t arg, int activator)
{
	rmap_t *m;
	int ri = rec_index(gid, &m);
	rtimer_t *tm;

	if (ri < 0)
		return;
	/* one nextthink per record (HLSDK pev->nextthink); <= 0 clears it */
	cancel_timers(gid);
	if (!(t > 0.0f))
		return;
	if (!(tm = timer_new(t)))
		sys_error("rec: logic timer queue full (%d): map %s record %u", REC_TIMERS,
			wld_map_name(m->map), (unsigned)GID_LUMP(gid));
	tm->kind = TM_REC;
	tm->gid = gid;
	tm->arg = arg;
	/* one think per frame (think-and-time.md 2, 5): a record re-arming
	 * itself from its own callback runs on a later pass even when due */
	tm->hold = gid == R.running ? R.pass : 0;
	tm->act = handle_or_none(activator);
	tm->use_type = ent_is_live(activator);	/* had an activator */
}

void rec_timer_cancel(gid_t16 gid)
{
	cancel_timers(gid);
}

void rec_fire_at(float t, name_t target, name_t killtarget, int activator, int caller,
	int use_type, float value)
{
	rtimer_t *tm = timer_new(t);

	if (!tm)
	{
		rmap_t *m;
		const hle_name_t *nm = name_of(target, &m);

		sys_error("rec: logic timer queue full (%d): map %s fire %s", REC_TIMERS,
			nm ? wld_map_name(m->map) : "?", nm ? m->str + nm->str : "?");
	}
	tm->kind = TM_FIRE;
	tm->target = target;
	tm->killtarget = killtarget;
	tm->gid = caller >= 0 ? ent_idc(caller)->gid : R.self;
	tm->act = handle_or_none(activator);
	tm->caller = handle_or_none(caller);
	tm->arg = (uint16_t)(ent_is_live(activator) | (ent_is_live(caller) ? 2 : 0));
	tm->use_type = (uint8_t)use_type;
	tm->value = value;
}

static void run_timer(rtimer_t *tm)
{
	rtimer_t c = *tm;
	int prev;

	timer_free(tm);
	if (c.kind == TM_REC)
	{
		rmap_t *m;
		int ri = rec_index(c.gid, &m);
		gid_t16 ps = R.self;

		if (ri < 0 || m->rt[ri].state == REC_KILLED)
			return;
		prev = eng_frame_layer();
		eng_set_frame_layer(GID_SLOT(c.gid));
		R.self = c.gid;
		R.running = c.gid;
		game_logic_timer(c.gid, c.arg, slot_or_none(c.act, c.use_type));
		R.running = GID_NONE;
		R.self = ps;
		eng_set_frame_layer(prev);
		return;
	}
	/* fire-and-die: killtarget first, then the target (HLSDK SUB_UseTargets) */
	if (c.killtarget != NAME_NONE)
		kill_name(c.killtarget);
	if (c.target != NAME_NONE)
		rec_fire(c.target, slot_or_none(c.act, c.arg & 1), slot_or_none(c.caller, c.arg & 2),
			c.gid, c.use_type, c.value);
}

void rec_run_timers(double time, double dt)
{
	uint32_t n = 0;
	uint64_t t0 = sys_time_us();

	R.sync_wakes = 0;
	if (++R.pass == 0)
		R.pass = 1;
	while (R.ntm)
	{
		rtimer_t *best = NULL;
		uint32_t i;

		for (i = 0; i < REC_TIMERS; i++)
		{
			rtimer_t *tm = &R.tm[i];

			if (tm->kind == TM_FREE || (double)tm->t > time + dt || tm->hold == R.pass)
				continue;
			if (!best || tm->t < best->t || (tm->t == best->t && tm->seq < best->seq))
				best = tm;
		}
		if (!best)
			break;
		if (n++ == REC_CASCADE)
		{
			log_printf("rec: %d timer callbacks in one frame: the rest wait\n", REC_CASCADE);
			break;
		}
		/* the callback sees (f32) max(due, time) */
		eng_set_clock((double)(float)((double)best->t > time ? (double)best->t : time), dt);
		run_timer(best);
	}
	eng_set_clock(time, dt);
	R.frame_us = (uint32_t)(sys_time_us() - t0);
}

/* ---- map open / close -------------------------------------------------------------- */

/* first-fit block in the HLE heap among the open slots' blocks */
static uint32_t heap_find(uint32_t need)
{
	uint32_t at = 0;
	int moved = 1, s;

	while (moved)
	{
		moved = 0;
		for (s = 0; s < REC_SLOTS; s++)
		{
			const rmap_t *m = &R.map[s];

			if (m->open && at < m->block + m->block_len && m->block < at + need)
			{
				at = (m->block + m->block_len + 15) & ~15u;
				moved = 1;
			}
		}
	}
	if (at > R.heap_len || need > R.heap_len - at)
		return ~0u;
	return at;
}

static int16_t s16_lo(float v)
{
	float f = floorf(v);

	return (int16_t)(f < -32768.0f ? -32768 : f > 32767.0f ? 32767 : f);
}

static int16_t s16_hi(float v)
{
	float f = ceilf(v);

	return (int16_t)(f < -32768.0f ? -32768 : f > 32767.0f ? 32767 : f);
}

static void thaw(rmap_t *m);

static int open_chunk(rmap_t *m, const wld_layer_t *L)
{
	const w64_chunk_t *ck = &L->chunks[L->dir->ents_chunk];
	const char *file = wld_map_name(L->map), *why;
	uint8_t hdr[96] __attribute__((aligned(8)));
	hle_header_t *h = (hle_header_t *)hdr;
	uint32_t len = ck->size, nrecs, need, at, i, k, nlog = 0;
	rec_limits_t lim;
	uint8_t *p;

	if (len < sizeof(hdr) || len > R.heap_len)
		sys_error("%s: ENTS chunk size %lu", file, (unsigned long)len);
	io_read_sync(hdr, L->rom + ck->ofs, sizeof(hdr));
	nrecs = h->magic == HLE_MAGIC ? h->numrecs : hle_sw16(h->numrecs);
	if (nrecs > HLE_MAX_LUMP)
		sys_error("%s: ENTS: %lu records", file, (unsigned long)nrecs);
	/* chunk, runtime, logic blocks (bounded by records), pending list */
	need = ((len + 15) & ~15u) + nrecs * (uint32_t)sizeof(rrt_t)
		+ nrecs * (uint32_t)sizeof(rec_logic_t) + 2 * nrecs + 16;
	if ((at = heap_find(need)) == ~0u)
		sys_error("%s: ENTS: %lu B do not fit the HLE region (%u KB)", file,
			(unsigned long)need, (unsigned)HL64_HLE_BLOBS_KB);
	p = R.heap + at;
	io_read_sync(p, L->rom + ck->ofs, (len + 1) & ~1u);
	memset(&lim, 0, sizeof(lim));
	lim.classes = (uint32_t)res_classes();
	lim.patterns = (uint32_t)res_patterns();
	lim.globals = (uint32_t)res_globals();
	lim.titles = (uint32_t)res_titles();
	for (k = 0; k < 8; k++)
		lim.kcount[k] = (uint32_t)res_count((int)k);
	lim.nsub = L->dir->nsub;
	if ((why = rec_hle_check(p, len, &lim, &m->graph, &m->graph_size)))
		sys_error("%s: ENTS chunk: %s", file, why);
	m->block = at;
	m->block_len = need;
	m->b = p;
	m->len = len;
	m->h = (const hle_header_t *)p;
	h = (hle_header_t *)p;
	m->recs = (const hle_rec_t *)(p + h->ofs_recs);
	m->l2r = (const uint16_t *)(p + h->ofs_lump2rec);
	m->cls = (const hle_class_t *)(p + h->ofs_classes);
	m->kn = (const hle_keyname_t *)(p + h->ofs_keynames);
	m->names = (const hle_name_t *)(p + h->ofs_names);
	m->hash = (const uint16_t *)(p + h->ofs_hash);
	m->keys = (const hle_key_t *)(p + h->ofs_keys);
	m->ll = (const uint16_t *)(p + h->ofs_lumplist);
	m->fire = (const hle_fire_t *)(p + h->ofs_fire);
	m->fl = (const float *)(p + h->ofs_floats);
	m->str = (const char *)(p + h->ofs_strings);
	p += (len + 15) & ~15u;
	m->rt = (rrt_t *)p;
	p += h->numrecs * sizeof(rrt_t);
	m->logic = (rec_logic_t *)p;
	p += h->numrecs * sizeof(rec_logic_t);
	m->plist = (uint16_t *)p;
	for (k = 0; k < RK__N; k++)
		m->kn_of_key[k] = HLE_NONE;
	for (k = 0; k < h->numkeynames; k++)
		if (m->kn_of_key[m->kn[k].id] == HLE_NONE)
			m->kn_of_key[m->kn[k].id] = (uint16_t)k;
	for (k = 0; k < PG_BUCKETS; k++)
		m->head[k] = HLE_NONE;
	m->huge = HLE_NONE;
	m->npend = 0;
	/* records: kinds, bounds, pending */
	for (i = 0; i < h->numrecs; i++)
	{
		rrt_t *t = &m->rt[i];
		const hle_rec_t *r = &m->recs[i];
		float org[3] = { 0, 0, 0 }, lo[3], hi[3];
		const hle_key_t *key = rkey(m, (int)i, RK_ORIGIN);
		const hle_key_t *mk = rkey(m, (int)i, RK_MODEL);

		memset(t, 0, sizeof(*t));
		t->state = REC_PENDING;
		if (key && rkind(m, key) == HLE_KV)
			memcpy(org, m->fl + key->val, sizeof(org));
		memcpy(lo, org, sizeof(lo));
		memcpy(hi, org, sizeof(hi));
		if (mk && rkind(m, mk) == HLE_KS && RES_KIND(mk->id) == RES_K_SUBMODEL)
			for (k = 0; k < 3; k++)
			{
				lo[k] += L->models[RES_SUB_NUM(mk->id)].mins[k];
				hi[k] += L->models[RES_SUB_NUM(mk->id)].maxs[k];
			}
		for (k = 0; k < 3; k++)
		{
			t->lo[k] = s16_lo(lo[k]);
			t->hi[k] = s16_hi(hi[k]);
		}
		if (r->flags & HLE_CF_LOGIC)
		{
			t->flags = RT_LOGIC;
			t->aux = (uint16_t)nlog++;
		}
		else if (r->flags & HLE_CF_POINT)
			t->flags = RT_POINT;
		else
		{
			t->flags = RT_PHYS;
			pend_add(m, (int)i);
		}
	}
	m->nlogic = (uint16_t)nlog;
	m->logic_pending = nlog != 0;
	/* logic blocks only for the logic records, the pending list after */
	{
		uint8_t *end = (uint8_t *)(m->logic + nlog);
		uint16_t *pl = (uint16_t *)(((uintptr_t)end + 3) & ~(uintptr_t)3);

		memmove(pl, m->plist, 2u * m->npend);
		m->plist = pl;
		m->block_len = (uint32_t)((uint8_t *)(pl + h->numrecs) - (R.heap + at));
	}
	m->idle = 0;
	return 0;
}

int rec_map_open(int layer)
{
	const wld_layer_t *L = wld_layer(layer);
	rmap_t *m;
	int prev;

	if (!L || layer < 0 || layer >= REC_SLOTS || !R.map)
		return -1;
	m = &R.map[layer];
	if (m->open && m->map == L->map)
		return layer;
	if (m->open)
		rec_map_close(layer);
	if (L->dir->ents_chunk == W64_NONE)
		return -1;
	memset(m, 0, sizeof(*m));
	m->slot = (uint8_t)layer;
	m->map = L->map;
	open_chunk(m, L);
	m->open = 1;
	prev = eng_frame_layer();
	eng_set_frame_layer(layer);
	game_map_opened(layer);
	eng_set_frame_layer(prev);
	thaw(m);
	log_printf("rec: %s open in slot %d: %u records (%u logic, %u pending), chunk %lu B, "
		"block %lu B\n", wld_map_name(m->map), layer, m->h->numrecs, m->nlogic, m->npend,
		(unsigned long)m->len, (unsigned long)m->block_len);
	return layer;
}

/* closing: the map's timers freeze by remaining time, activator dropped */
static void freeze(rmap_t *m)
{
	double now = eng_time();
	uint32_t i;

	for (i = 0; i < REC_TIMERS; i++)
	{
		rtimer_t *tm = &R.tm[i];
		int mine = tm->kind == TM_REC ? GID_SLOT(tm->gid) == m->slot
			: tm->kind == TM_FIRE && NAME_SLOT(tm->target != NAME_NONE ? tm->target
				: tm->killtarget) == m->slot;

		if (tm->kind == TM_FREE || !mine)
			continue;
		if (R.nfrozen < REC_TIMERS)
		{
			rfrozen_t *f = &R.frozen[R.nfrozen++];

			f->map = m->map;
			f->lump = (uint16_t)GID_LUMP(tm->gid);
			f->tm = *tm;
			f->tm.t = (float)((double)tm->t - now);
			f->tm.act = f->tm.caller = 0;
			f->tm.arg = tm->kind == TM_REC ? tm->arg : 0;
			f->tm.use_type = tm->kind == TM_REC ? 0 : tm->use_type;
		}
		timer_free(tm);
	}
}

/* reopening: re-queue at time + remaining, in their old order */
static void thaw(rmap_t *m)
{
	double now = eng_time();
	uint32_t i, j = 0;

	for (i = 0; i < R.nfrozen; i++)
	{
		rfrozen_t f = R.frozen[i];
		rtimer_t *tm;

		if (f.map != m->map)
		{
			R.frozen[j++] = f;
			continue;
		}
		if (!(tm = timer_new((float)(now + (double)f.tm.t))))
			sys_error("rec: logic timer queue full (%d) thawing %s", REC_TIMERS,
				wld_map_name(m->map));
		f.tm.t = tm->t;
		f.tm.seq = tm->seq;
		if (f.tm.gid != GID_NONE)
			f.tm.gid = GID_MAKE(m->slot, f.lump);
		if (f.tm.kind == TM_FIRE)
		{
			if (f.tm.target != NAME_NONE)
				f.tm.target = NAME_MAKE(m->slot, NAME_INDEX(f.tm.target));
			if (f.tm.killtarget != NAME_NONE)
				f.tm.killtarget = NAME_MAKE(m->slot, NAME_INDEX(f.tm.killtarget));
		}
		*tm = f.tm;
	}
	R.nfrozen = j;
}

void rec_map_close(int slot)
{
	rmap_t *m = slot_map(slot);
	uint32_t i, j;
	int prev;

	if (!m)
		return;
	prev = eng_frame_layer();
	eng_set_frame_layer(slot);
	game_map_closing(slot);
	eng_set_frame_layer(prev);
	freeze(m);
	/* the map's live entities go with it (M4 dehydrates them first) */
	for (i = 0; i < m->h->numrecs; i++)
	{
		int e = rt_entity(&m->rt[i]);

		if (e >= 0)
			ent_destroy(e);
	}
	for (i = 0, j = 0; i < R.ndefer; i++)
	{
		rdefer_t d = R.defer[(R.defer_head + i) % REC_DEFER];

		if (GID_SLOT(d.gid) != slot)
			R.defer[(R.defer_head + j++) % REC_DEFER] = d;
	}
	R.ndefer = j;
	for (i = 0; i < REC_HOLDERS; i++)
		if (R.hold[i].n != NAME_NONE && NAME_SLOT(R.hold[i].n) == slot)
			R.hold[i].n = NAME_NONE;
	m->open = 0;
}

/* ---- hydration pump (step 8) ------------------------------------------------------ */

static float box_d2(const rrt_t *t, const float p[3])
{
	float d2 = 0;
	int k;

	for (k = 0; k < 3; k++)
	{
		float d = p[k] < t->lo[k] ? t->lo[k] - p[k] : p[k] > t->hi[k] ? p[k] - t->hi[k] : 0;

		d2 += d * d;
	}
	return d2;
}

/* Test builds (host and cart) count records so the entity state hashes
 * of host and ares runs agree (K10); playable builds spend 2 ms of wall
 * time (architecture.md 6.4). */
static int budget_left(uint64_t t0, uint32_t done)
{
#ifdef HL64_TEST
	(void)t0;
	return done < REC_BUDGET_HOST;
#else
	return !done || sys_time_us() - t0 < REC_BUDGET_US;
#endif
}

static void deliver_deferred(uint64_t t0, uint32_t *done)
{
	while (R.ndefer && budget_left(t0, *done))
	{
		rdefer_t d = R.defer[R.defer_head];
		rmap_t *m;
		int ri = rec_index(d.gid, &m), e = -1;

		R.defer_head = (R.defer_head + 1) % REC_DEFER;
		R.ndefer--;
		if (ri < 0)
			continue;
		if ((e = rt_entity(&m->rt[ri])) < 0 && (m->rt[ri].state == REC_PENDING
			|| m->rt[ri].state == REC_DRY))
		{
			e = hydrate(m, ri, ENT_ALLOC_GAME);
			if (e >= 0)
			{
				R.st.woken++;
				(*done)++;
			}
		}
		if (e >= 0 && !(d.plain & 1))
			use_ent(e, slot_or_none(d.act, d.plain & 2), slot_or_none(d.caller, d.plain & 4),
				d.caller_gid, d.use_type, d.value);
	}
}

static void pump(rmap_t *m, const float (*anchors)[3], int n, uint64_t t0, uint32_t *done)
{
	const float r2 = REC_SPAWN_R * REC_SPAWN_R;
	const float o2 = (REC_SPAWN_R + REC_PREFETCH_R) * (REC_SPAWN_R + REC_PREFETCH_R);
	const float *g = wld_layer(m->slot)->gofs;
	float loc[8][3];
	uint16_t cand[16], ring[REC_RING];
	float cd[16], rd[REC_RING];
	int nc = 0, nr = 0, a, i, k;

	if (!m->npend)
		return;
	if (n > 8)
		n = 8;
	for (a = 0; a < n; a++)
		for (k = 0; k < 3; k++)
			loc[a][k] = anchors[a][k] - g[k];
	/* nothing changed and the anchor stayed put since a scan that found
	 * nothing to spawn: skip the scan */
	if (m->idle && m->idle_gen == m->pgen && n >= 1
		&& fabsf(loc[0][0] - m->idle_at[0]) < 16 && fabsf(loc[0][1] - m->idle_at[1]) < 16
		&& fabsf(loc[0][2] - m->idle_at[2]) < 16)
		return;
	for (i = 0; i < m->npend; i++)
	{
		int ri = m->plist[i], j;
		float d = 1e30f;

		for (a = 0; a < n; a++)
		{
			float x = box_d2(&m->rt[ri], loc[a]);

			if (x < d)
				d = x;
		}
		if (d <= r2)
		{
			for (j = nc; j > 0 && cd[j - 1] > d; j--)
				if (j < 16)
				{
					cand[j] = cand[j - 1];
					cd[j] = cd[j - 1];
				}
			if (j < 16)
			{
				cand[j] = (uint16_t)ri;
				cd[j] = d;
				if (nc < 16)
					nc++;
			}
		}
		else if (d <= o2)
		{
			for (j = nr; j > 0 && rd[j - 1] > d; j--)
				if (j < REC_RING)
				{
					ring[j] = ring[j - 1];
					rd[j] = rd[j - 1];
				}
			if (j < REC_RING)
			{
				ring[j] = (uint16_t)ri;
				rd[j] = d;
				if (nr < REC_RING)
					nr++;
			}
		}
	}
	/* prefetch ring: the nearest pending records' models */
	for (i = 0; i < nr; i++)
	{
		res_t r = rec_key_res(gid_of(m, ring[i]), RK_MODEL);

		if (r != RES_NONE && RES_KIND(r) != RES_K_SUBMODEL)
			res_precache(r);
	}
	m->idle = nc == 0;
	m->idle_gen = m->pgen;
	memcpy(m->idle_at, loc[0], sizeof(m->idle_at));
	for (i = 0; i < nc && budget_left(t0, *done); i++)
	{
		if (hydrate(m, cand[i], ENT_ALLOC_OPTIONAL) < 0 && m->rt[cand[i]].state == REC_PENDING)
			break;	/* the optional reserve: stop here */
		(*done)++;
	}
}

void rec_frame(const float (*anchors)[3], int nanchors)
{
	uint64_t t0 = sys_time_us();
	uint32_t done = 0, us;
	int s;

	if (!R.map)
		return;
	deliver_deferred(t0, &done);
	for (s = 0; s < REC_SLOTS; s++)
	{
		rmap_t *m = &R.map[s];
		int i;

		if (!m->open)
			continue;
		/* a newly opened map's logic records: all at once (no entities) */
		if (m->logic_pending)
		{
			for (i = 0; i < m->h->numrecs; i++)
				spawn_logic(m, i);
			m->logic_pending = 0;
		}
		if (nanchors > 0)
			pump(m, anchors, nanchors, t0, &done);
	}
	us = (uint32_t)(sys_time_us() - t0) + R.frame_us;
	R.us_sum += us;
	if (us > R.us_max)
		R.us_max = us;
	R.frames++;
	if (us > R.st.budget_max_us)
		R.st.budget_max_us = us;
	R.st.budget_us += us;
}

/* ---- pending-record wakes ---------------------------------------------------------- */

static int wake_rec(rmap_t *m, int ri, const float lo[3], const float hi[3])
{
	const rrt_t *t = &m->rt[ri];
	int k;

	for (k = 0; k < 3; k++)
		if (t->hi[k] < lo[k] || t->lo[k] > hi[k])
			return 0;
	if (hydrate(m, ri, ENT_ALLOC_OPTIONAL) < 0)
		return 0;
	R.st.woken++;
	return 1;
}

int rec_wake_box(const float gmins[3], const float gmaxs[3])
{
	int s, n = 0;

	if (!R.map)
		return 0;
	for (s = 0; s < REC_SLOTS; s++)
	{
		rmap_t *m = &R.map[s];
		const float *g;
		float lo[3], hi[3];
		int c0[3], c1[3], k, x, y, z, cells = 1, i;
		uint16_t hit[64];
		int nh = 0;

		if (!m->open || !m->npend)
			continue;
		g = wld_layer(s)->gofs;
		for (k = 0; k < 3; k++)
		{
			lo[k] = gmins[k] - g[k];
			hi[k] = gmaxs[k] - g[k];
			if (hi[k] < m->pmin[k] || lo[k] > m->pmax[k])
				break;
			c0[k] = ifloor((lo[k] - PG_HALF) / PG_CELL);
			c1[k] = ifloor((hi[k] + PG_HALF) / PG_CELL);
			cells *= c1[k] - c0[k] + 1;
		}
		if (k < 3)
			continue;
		if (cells > PG_SCAN_CELLS)
		{
			for (i = 0; i < m->npend;)
			{
				int ri = m->plist[i];

				if (wake_rec(m, ri, lo, hi))
					n++;	/* plist[i] now holds another record */
				else
					i++;
			}
			continue;
		}
		/* candidates first (hydration edits the chains) */
		for (i = m->huge; i != HLE_NONE && nh < 64; i = m->rt[i].pnext)
			hit[nh++] = (uint16_t)i;
		for (x = c0[0]; x <= c1[0]; x++)
			for (y = c0[1]; y <= c1[1]; y++)
				for (z = c0[2]; z <= c1[2]; z++)
					for (i = m->head[pg_bucket(x, y, z)]; i != HLE_NONE && nh < 64;
						i = m->rt[i].pnext)
						hit[nh++] = (uint16_t)i;
		for (i = 0; i < nh; i++)
			if (m->rt[hit[i]].state == REC_PENDING || m->rt[hit[i]].state == REC_DRY)
				n += wake_rec(m, hit[i], lo, hi);
	}
	return n;
}

/* ---- New Game ---------------------------------------------------------------------- */

int rec_player_start(int slot, float org[3], float ang[3])
{
	rmap_t *m = slot_map(slot);
	int i;

	if (!m)
		return 0;
	/* HLSDK EntSelectSpawnPoint (single player, no startspot): the first
	 * info_player_start in entity order */
	for (i = 0; i < m->h->numrecs; i++)
		if (m->cls[m->recs[i].cls].id == RC_INFO_PLAYER_START)
		{
			rec_origin(gid_of(m, i), org);
			rec_angles(gid_of(m, i), ang);
			return 1;
		}
	return 0;
}

void rec_new_campaign(int map)
{
	wld_layer_t *L = wld_open(map);
	float org[3] = { 0, 0, 0 }, ang[3];
	rmap_t *m;
	int slot, i;

	rec_reset();
	/* time 1.0; the loop's next two frames are the settle frames
	 * (think-and-time.md 6, K8: engine/ent/pass.c) */
	eng_clock_new_game();
	slot = rec_map_open(L->slot);
	if (slot < 0 || !(m = slot_map(slot)))
		sys_error("%s: no entity chunk", wld_map_name(map));
	rec_player_start(slot, org, ang);
	/* spawn order: entity (lump) order, logic records and the physical
	 * records within the spawn radius of the start */
	for (i = 0; i < m->h->numrecs; i++)
	{
		if (m->rt[i].flags & RT_LOGIC)
			spawn_logic(m, i);
		else if ((m->rt[i].flags & RT_PHYS) && m->rt[i].state == REC_PENDING
			&& box_d2(&m->rt[i], org) <= REC_SPAWN_R * REC_SPAWN_R)
			hydrate(m, i, ENT_ALLOC_GAME);
	}
	m->logic_pending = 0;
	log_printf("rec: new campaign %s: %lu spawned, %u pending, %u logic, %lu timers\n",
		wld_map_name(map), (unsigned long)R.st.hydrated, m->npend, m->nlogic,
		(unsigned long)R.ntm);
}

/* ---- level state store (6.4; the dehydrate side is M4) ---------------------------- */

typedef struct {
	uint16_t	map, lump, rtid, len;
} rstore_t;

static uint8_t *store_find(uint16_t map, uint16_t lump, uint16_t rtid)
{
	uint32_t at = 0;

	while (at < R.store_used)
	{
		rstore_t *e = (rstore_t *)(R.store + at);

		if (e->map == map && e->lump == lump && e->rtid == rtid)
			return R.store + at;
		at += (uint32_t)sizeof(*e) + ((e->len + 3u) & ~3u);
	}
	return NULL;
}

static void store_del(uint8_t *p)
{
	rstore_t *e = (rstore_t *)p;
	uint32_t n = (uint32_t)sizeof(*e) + ((e->len + 3u) & ~3u);

	memmove(p, p + n, R.store_used - (uint32_t)(p - R.store) - n);
	R.store_used -= n;
}

int rec_store_put(gid_t16 gid, uint16_t rtid, const void *delta, uint32_t n)
{
	rmap_t *m;
	int ri = rec_index(gid, &m);
	uint8_t *p;
	rstore_t e;

	if (ri < 0 || n > 0xFFFF)
		return -1;
	if ((p = store_find(m->map, (uint16_t)GID_LUMP(gid), rtid)))
		store_del(p);
	if ((uint32_t)sizeof(e) + ((n + 3u) & ~3u) > R.store_cap - R.store_used)
		return -1;
	e.map = m->map;
	e.lump = (uint16_t)GID_LUMP(gid);
	e.rtid = rtid;
	e.len = (uint16_t)n;
	memcpy(R.store + R.store_used, &e, sizeof(e));
	memcpy(R.store + R.store_used + sizeof(e), delta, n);
	R.store_used += (uint32_t)sizeof(e) + ((n + 3u) & ~3u);
	if (R.store_used > R.store_peak)
		R.store_peak = R.store_used;
	return 0;
}

int rec_store_get(gid_t16 gid, uint16_t rtid, void *delta, uint32_t max)
{
	rmap_t *m;
	int ri = rec_index(gid, &m);
	uint8_t *p;
	uint32_t n;

	if (ri < 0 || !(p = store_find(m->map, (uint16_t)GID_LUMP(gid), rtid)))
		return -1;
	n = ((rstore_t *)p)->len;
	memcpy(delta, p + sizeof(rstore_t), n < max ? n : max);
	return (int)n;
}

/* ---- boot, stats --------------------------------------------------------------------- */

void rec_init(void)
{
	uint8_t *p = mem_region(MEM_HLE_BLOBS);
	uint32_t total = mem_region_bytes(MEM_HLE_BLOBS), fixed;

	memset(&R, 0, sizeof(R));
	R.map = (rmap_t *)p;
	p += (REC_SLOTS * sizeof(rmap_t) + 15) & ~(size_t)15;
	R.tm = (rtimer_t *)p;
	p += REC_TIMERS * sizeof(rtimer_t);
	R.frozen = (rfrozen_t *)p;
	p += REC_TIMERS * sizeof(rfrozen_t);
	R.defer = (rdefer_t *)p;
	p += REC_DEFER * sizeof(rdefer_t);
	R.hold = (rhold_t *)p;
	p += REC_HOLDERS * sizeof(rhold_t);
	p = (uint8_t *)(((uintptr_t)p + 15) & ~(uintptr_t)15);
	fixed = (uint32_t)(p - (uint8_t *)mem_region(MEM_HLE_BLOBS));
	if (fixed >= total)
		sys_error("rec: HLE region smaller than its tables (%lu B)", (unsigned long)fixed);
	R.heap = p;
	R.heap_len = total - fixed;
	R.store = mem_region(MEM_DEHYDRATE);
	R.store_cap = mem_region_bytes(MEM_DEHYDRATE);
	rec_reset();
}

void rec_reset(void)
{
	int s, j;

	if (!R.map)
		return;
	for (s = 0; s < REC_SLOTS; s++)
		R.map[s].open = 0;
	memset(R.tm, 0, REC_TIMERS * sizeof(rtimer_t));
	for (j = 0; j < REC_HOLDERS; j++)
		R.hold[j].n = NAME_NONE;
	R.ntm = R.nfrozen = R.ndefer = R.defer_head = R.sync_wakes = 0;
	R.seq = 0;
	R.caller = R.self = R.running = GID_NONE;
	R.store_used = 0;
	memset(&R.st, 0, sizeof(R.st));
	R.us_sum = 0;
	R.us_max = R.frames = R.frame_us = 0;
}

void rec_stats(rec_stats_t *s, int reset)
{
	int i, k;

	R.st.live = R.st.pending = R.st.logic = R.st.killed = R.st.dry = 0;
	for (i = 0; R.map && i < REC_SLOTS; i++)
	{
		rmap_t *m = &R.map[i];

		if (!m->open)
			continue;
		for (k = 0; k < m->h->numrecs; k++)
		{
			rrt_t *t = &m->rt[k];

			rt_entity(t);
			if (t->flags & RT_LOGIC)
				R.st.logic++;
			else if (t->state == REC_LIVE)
				R.st.live++;
			else if (t->state == REC_PENDING && (t->flags & RT_PHYS))
				R.st.pending++;
			else if (t->state == REC_KILLED)
				R.st.killed++;
			else if (t->state == REC_DRY)
				R.st.dry++;
		}
	}
	if (R.st.live > R.live_peak)
		R.live_peak = R.st.live;
	R.st.timers = R.ntm;
	*s = R.st;
	if (reset)
	{
		R.st.hydrated = R.st.released = R.st.woken = 0;
		R.st.budget_us = R.st.budget_max_us = 0;
	}
}

void rec_prof_line(void)
{
	rec_stats_t s;
	unsigned long n = R.frames ? R.frames : 1;

	rec_stats(&s, 0);
	log_printf("prof rec_us avg=%lu max=%lu live=%lu pending=%lu hydrated=%lu released=%lu "
		"woken=%lu timers=%lu timers_peak=%lu deferred=%lu\n",
		(unsigned long)(R.us_sum / n), (unsigned long)R.us_max, (unsigned long)s.live,
		(unsigned long)s.pending, (unsigned long)s.hydrated, (unsigned long)s.released,
		(unsigned long)s.woken, (unsigned long)s.timers, (unsigned long)s.timers_peak,
		(unsigned long)R.ndefer);
	rec_stats(&s, 1);
	R.us_sum = 0;
	R.us_max = 0;
	R.frames = 0;
}

void rec_mem_line(void)
{
	uint32_t blobs = 0, runtime = 0;
	int s;

	for (s = 0; R.map && s < REC_SLOTS; s++)
		if (R.map[s].open)
		{
			blobs += R.map[s].len;
			runtime += R.map[s].block_len - R.map[s].len;
			log_printf("mem rec: slot %d %s chunk=%lu runtime=%lu records=%u\n", s,
				wld_map_name(R.map[s].map), (unsigned long)R.map[s].len,
				(unsigned long)(R.map[s].block_len - R.map[s].len), R.map[s].h->numrecs);
		}
	log_printf("mem rec: blobs=%lu runtime=%lu heap=%lu tables=%lu timers_peak=%lu "
		"store=%lu/%lu peak=%lu live_peak=%lu\n", (unsigned long)blobs, (unsigned long)runtime,
		(unsigned long)R.heap_len,
		(unsigned long)(mem_region_bytes(MEM_HLE_BLOBS) - R.heap_len),
		(unsigned long)R.st.timers_peak, (unsigned long)R.store_used,
		(unsigned long)R.store_cap, (unsigned long)R.store_peak, (unsigned long)R.live_peak);
}
