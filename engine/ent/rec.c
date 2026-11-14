/* SPDX-License-Identifier: MIT */
/*
 * rec.c -- map entity records: HLE1 chunks per HLE slot, record states,
 * typed keys, names, HLSDK FireTargets, logic records, logic timers, the
 * hydration pump and pending-record wakes, and the M4 level state: async
 * map open, release by distance (dehydrate into the store), map close
 * into a frozen runtime part, runtime entities by (map, runtime id), twin
 * aliases, the canonical deferred-use FIFO (docs/architecture.md 4.1
 * "HLE slots for M4", 6.4, 7.1 "records", 14; docs/formats/
 * hle-format-v1.md; docs/specs/think-and-time.md 5-6,
 * area-links-and-touch.md 3 "Pending-record wake").
 *
 * Retargeted from the old tree's source/hl/hl_ents.c/.h (audit entry
 * source/hl/hl_ents.c: CLEAN + QUAKE-INTERFACE) and, for the level state,
 * source/hl/hl_levelstate.c/.h (audit entry source/hl/hl_levelstate.c:
 * CLEAN + QUAKE-INTERFACE): the design is kept (one in-place chunk per
 * map, a dense pending list with a hashed pending grid for box wakes, a
 * bounded timer queue frozen by remaining time, a budgeted distance-ordered
 * pump with a prefetch ring, capped synchronous wakes with a deferred-use
 * FIFO keyed by (map, lump) / runtime id, timed release batches with
 * refusal back-off, twin aliases resolving to the live copy, durable
 * runtime ids); every Quake binding is gone: no edict_t / entvars / func_t /
 * pr_* / sv.* / svs.* / cvars / Con_* / Sys_* / Host_*, entities are
 * component slots (include/hl64/ent.h), names, classes, keys and
 * resources are cooked ids, the per-class delta is the game's
 * (game_dehydrate / game_rehydrate). The multi_manager think (audit range
 * 2486-2524, HLSDK) is not here: it is the game's (game_logic_timer), the
 * engine only keeps HLSDK's float nextthink and due order (think-and-time.md
 * 5).
 *
 * Memory: the HLE_BLOBS region holds the slot table, timers, deferred uses,
 * name holders, runtime ids, twins and frozen-map table, then one block per
 * open (or opening) slot (its chunk, then the per-record runtime, logic
 * blocks and the pending lists) and one per frozen map (record states +
 * logic blocks); the DEHYDRATE region holds the store. Nothing is
 * allocated elsewhere.
 */
#include <math.h>
#include <string.h>

#define HLE_FORMAT_ONLY
#include "tools/w64c/hl_entcook.h"

#include "hl64/ent.h"
#include "hl64/game.h"
#include "hl64/limits.h"
#include "hl64/rec.h"
#include "hl64/studio.h"
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

/* async open phases (rec_map_open .. rec_map_ready) */
enum { PH_CLOSED, PH_LAYER, PH_HEADER, PH_CHUNK, PH_READY };

typedef struct {
	uint8_t			hdr[96] __attribute__((aligned(16)));	/* HLE1 header read */
	uint8_t			decl[16] __attribute__((aligned(16)));	/* DECL header read */
	io_req_t		req[3];		/* header, DECL header, chunk */
	uint8_t			open;		/* PH_READY: records valid */
	uint8_t			slot;
	uint16_t		map;		/* campaign map */
	uint8_t			phase;		/* PH_* */
	uint8_t			idle;
	uint8_t			logic_pending;
	uint8_t			pre_wait;	/* precache_held: frames held + 1, 0 not held */
	uint32_t		ndecals;	/* rec_limits_t.ndecals of the layer */
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
	float			rel_next;	/* next release batch (game clock, f32 s) */
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

/* A canonical entity reference (6.4: never slots or entity pointers past
 * a frame): the player, a map record (map, lump), a runtime entity (map,
 * rtid), or, for a runtime entity without a durable id, its handle (gone
 * once its map closes: "resolves absent"). */
enum { RR_NONE, RR_PLAYER, RR_REC, RR_RT, RR_HANDLE };
typedef struct {
	uint8_t		kind;		/* RR_* */
	uint8_t		pad;
	uint16_t	map, id;
	uint16_t	pad2;
	ent_h		h;
} rref_t;			/* 12 */

/* Deferred use (6.4): target (map, lump) or (map, rtid); FIFO by array
 * order, delivered in per-map order when the target map is open. */
typedef struct {
	uint16_t	map, id;
	uint8_t		runtime, use_type, plain, pad;	/* plain: a wake without a use */
	uint16_t	caller_map, caller_lump;	/* logic caller record, 0xFFFF none */
	rref_t		act, caller;
	float		value;
} rdefer_t;			/* 40 */

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

/* runtime entity with a durable id (monstermaker child, created pickup) */
enum { RUN_FREE, RUN_LIVE, RUN_DRY };
typedef struct {
	uint16_t	map, rtid;
	cls_t		cls;
	uint8_t		state;		/* RUN_* */
	uint8_t		pad;
	uint16_t	name;		/* held map name index while dry, 0xFFFF none */
	uint16_t	pad2;
	ent_h		h;		/* RUN_LIVE */
	float		org[3], ang[3];	/* RUN_DRY: map-local, where it was released */
} rrun_t;			/* 40 */

typedef struct {
	gid_t16		a, b;
	uint8_t		cur;		/* the copy live last: 0 a, 1 b */
	uint8_t		pad;
} rtwin_t;

/* a closed map's runtime part (architecture.md 4.1 "Frozen") */
typedef struct {
	uint16_t	map;		/* WLD_NO_MAP: free */
	uint16_t	nrecs, nlogic;
	uint16_t	pad;
	uint32_t	block, block_len;
	uint32_t	age;		/* freeze order: the oldest is dropped first */
} rfmap_t;			/* 20 */

_Static_assert(sizeof(rrt_t) == REC_RT_BYTES, "rrt_t");
_Static_assert(sizeof(rec_logic_t) == REC_LOGIC_BYTES, "rec_logic_t");
#define TABLE_BYTES ((((REC_SLOTS * sizeof(rmap_t) + 15) & ~(size_t)15) \
	+ REC_TIMERS * (sizeof(rtimer_t) + sizeof(rfrozen_t)) + REC_DEFER * sizeof(rdefer_t) \
	+ REC_HOLDERS * sizeof(rhold_t) + REC_RUNTIME * sizeof(rrun_t) \
	+ REC_TWINS * sizeof(rtwin_t) + REC_FROZEN_MAPS * sizeof(rfmap_t) \
	+ HL64_MAX_ENTITIES * sizeof(float) + 15) & ~(size_t)15)
_Static_assert(TABLE_BYTES == REC_TABLE_BYTES, "rec_init tables");

static struct {
	rmap_t		*map;		/* [REC_SLOTS] */
	rtimer_t	*tm;		/* [REC_TIMERS] */
	rdefer_t	*defer;		/* [REC_DEFER] FIFO */
	rhold_t		*hold;		/* [REC_HOLDERS] */
	rfrozen_t	*frozen;	/* [REC_TIMERS] */
	rrun_t		*run;		/* [REC_RUNTIME] */
	rtwin_t		*twin;		/* [REC_TWINS] */
	rfmap_t		*fmap;		/* [REC_FROZEN_MAPS] */
	float		*retry;		/* [HL64_MAX_ENTITIES] release refused: ask again at */
	uint8_t		*heap;		/* map blocks */
	uint32_t	heap_len;
	uint32_t	ntm, seq, nfrozen;
	uint32_t	ndefer, ntwin, fage;
	uint16_t	next_rtid;
	uint32_t	sync_wakes;	/* this frame's in-call hydrations */
	uint16_t	pass;		/* rec_run_timers pass number, never 0 */
	gid_t16		running;	/* record whose timer callback runs, GID_NONE */
	gid_t16		caller;		/* rec_caller_gid */
	gid_t16		self;		/* record / entity being dispatched */
	rec_stats_t	st;		/* window counters + current state */
	uint64_t	us_sum;
	uint32_t	us_max, frames, frame_us;
	uint32_t	rel_us_max, rel_frames;	/* frames with a release batch */
	uint32_t	late, close_dropped, store_full;
	uint8_t		sweep;		/* HL64_TEST class sweep: slot + 1, 0 off */
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

/* the ready slot of a campaign map, -1 when it is not open */
static int slot_of_map(uint16_t map)
{
	int s;

	for (s = 0; R.map && s < REC_SLOTS; s++)
		if (R.map[s].open && R.map[s].map == map)
			return s;
	return -1;
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

/* dehydrate store entry (DEHYDRATE region), the delta after it */
typedef struct {
	uint16_t	map, lump, rtid, len;	/* lump 0xFFFF: runtime entity rtid */
	int16_t		lo[3], hi[3];	/* map-local box where it was released */
} rstore_t;
_Static_assert(sizeof(rstore_t) == REC_STORE_HDR, "rstore_t");
#define LUMP_RT		0xFFFF

static uint8_t *store_find(uint16_t map, uint16_t lump, uint16_t rtid);
static void store_del(uint8_t *p);
static void store_drop(uint16_t map, uint16_t lump, uint16_t rtid);

/* the released delta of (map, lump / rtid) into the fresh entity e, then
 * the store entry goes (game_rehydrate never touches the store) */
static void rehydrate_from_store(int e, uint16_t map, uint16_t lump, uint16_t rtid)
{
	uint8_t *p = store_find(map, lump, rtid);
	int prev;

	if (!p)
		return;
	prev = eng_frame_layer();
	eng_set_frame_layer(ent_idc(e)->layer);
	game_rehydrate(e, p + sizeof(rstore_t), ((rstore_t *)p)->len);
	eng_set_frame_layer(prev);
	if ((p = store_find(map, lump, rtid)))
		store_del(p);
}

static void twin_mark_live(gid_t16 g);
static int hydrate(rmap_t *m, int ri, int reserve)
{
	rrt_t *t = &m->rt[ri];
	const hle_rec_t *r = &m->recs[ri];
	gid_t16 gid = gid_of(m, ri);
	ent_id_t *id;
	col_ent_t *cv;
	int e, prev, ok, dry;

	if (!(t->flags & RT_PHYS) || (t->state != REC_PENDING && t->state != REC_DRY))
		return -1;
	e = ent_create(reserve);
	if (e < 0)
		return -1;
	dry = t->state == REC_DRY;
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
	R.retry[e] = 0;
	prev = eng_frame_layer();
	eng_set_frame_layer(m->slot);
	ok = game_spawn_record(gid, e);
	eng_set_frame_layer(prev);
	if (ok < 0)
	{
		if (ent_is_live(e))
			ent_destroy(e);
		t->state = REC_KILLED;
		if (dry)
			store_drop(m->map, r->lump, 0);
		return -1;
	}
	/* 6.4: a released record respawns, then takes its delta */
	if (dry && ent_is_live(e))
		rehydrate_from_store(e, m->map, r->lump, 0);
	twin_mark_live(gid);
	R.st.hydrated++;
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
		|| (k == RK_TEXTURE && c == RC_INFODECAL) || typed_id(gid, k, HLE_KS, &key) < 0)
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

int rec_key_decal(gid_t16 gid, key_t16 k)
{
	const hle_key_t *key;

	if (k != RK_TEXTURE || rec_class(gid) != RC_INFODECAL)
		return -1;
	return typed_id(gid, k, HLE_KS, &key) >= 0 ? key->id : -1;
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

/* ---- twins (cwg-format.md twins, architecture.md 5.4 req. 3) ------------------------- */

static rtwin_t *twin_find(gid_t16 g)
{
	uint32_t i;

	for (i = 0; i < R.ntwin; i++)
		if (R.twin[i].a == g || R.twin[i].b == g)
			return &R.twin[i];
	return NULL;
}

/* The copy a fire, the pump or a box wake of record g reaches: the pair's
 * current copy (the one live last); g when it has no twin. The other copy
 * hydrates only through rec_wake (the seam's handoff back). */
static gid_t16 twin_current(gid_t16 g)
{
	const rtwin_t *tw = twin_find(g);

	return tw ? (tw->cur ? tw->b : tw->a) : g;
}

static int twin_blocked(gid_t16 g)
{
	return twin_current(g) != g;
}

static void twin_mark_live(gid_t16 g)
{
	rtwin_t *tw = twin_find(g);

	if (tw)
		tw->cur = tw->b == g;
}

/* ---- canonical references (6.4) ------------------------------------------------- */

static void ref_make(rref_t *r, int e)
{
	const ent_id_t *id;
	const rmap_t *m;

	memset(r, 0, sizeof(*r));
	if (e < 1 || !ent_is_live(e))
		return;
	if (e == 1)
	{
		r->kind = RR_PLAYER;
		return;
	}
	id = ent_idc(e);
	if (id->gid != GID_NONE && (m = slot_map(GID_SLOT(id->gid))))
	{
		r->kind = RR_REC;
		r->map = m->map;
		r->id = (uint16_t)GID_LUMP(id->gid);
	}
	else if (id->rtid && (m = slot_map(id->layer)))
	{
		r->kind = RR_RT;
		r->map = m->map;
		r->id = id->rtid;
	}
	else
	{
		r->kind = RR_HANDLE;
		r->h = ent_handle_of(e);
	}
}

/* the live entity, -1 absent (its map closed, dry, gone); never hydrates */
static int ref_get(const rref_t *r)
{
	switch (r->kind)
	{
	case RR_PLAYER:
		return ent_is_live(1) ? 1 : -1;
	case RR_REC:
		return rec_ref_entity(r->map, r->id, 0);
	case RR_RT:
		return rec_ref_entity(r->map, r->id, 1);
	case RR_HANDLE:
		return ent_of(r->h);
	}
	return -1;
}

/* ---- runtime entities (6.4: by (map, runtime id)) -------------------------------- */

static int run_find(uint16_t map, uint16_t rtid)
{
	int i;

	for (i = 0; rtid && i < REC_RUNTIME; i++)
		if (R.run[i].state != RUN_FREE && R.run[i].map == map && R.run[i].rtid == rtid)
			return i;
	return -1;
}

/* live entity of a RUN_LIVE entry; one the game freed is gone for good */
static int run_entity(rrun_t *r)
{
	int e;

	if (r->state != RUN_LIVE)
		return -1;
	e = ent_of(r->h);
	if (e < 0)
		r->state = RUN_FREE;
	return e;
}

static void run_free(rrun_t *r)
{
	if (r->state == RUN_DRY)
		store_drop(r->map, LUMP_RT, r->rtid);
	r->state = RUN_FREE;
}

uint16_t rec_runtime_id(int e)
{
	ent_id_t *id;
	const rmap_t *m;
	rrun_t *r;
	int i;

	if (e < 2 || !ent_is_live(e))
		return 0;
	id = ent_idc(e);
	if (id->gid != GID_NONE || !(m = slot_map(id->layer)))
		return 0;
	if (id->rtid)
		return id->rtid;
	for (i = 0; i < REC_RUNTIME; i++)
	{
		run_entity(&R.run[i]);
		if (R.run[i].state == RUN_FREE)
			break;
	}
	if (i == REC_RUNTIME)
	{
		log_printf("rec: more than %d runtime entities with ids: %s #%d not persistent\n",
			REC_RUNTIME, wld_map_name(m->map), e);
		return 0;
	}
	if (++R.next_rtid == 0)
		R.next_rtid = 1;
	r = &R.run[i];
	memset(r, 0, sizeof(*r));
	r->map = m->map;
	r->rtid = R.next_rtid;
	r->cls = id->cls;
	r->state = RUN_LIVE;
	r->name = 0xFFFF;
	r->h = ent_handle_of(e);
	id->rtid = r->rtid;
	return id->rtid;
}

/* a released runtime entity back (game_spawn_class + its delta) */
static int hydrate_rt(int i, int reserve)
{
	rrun_t *r = &R.run[i];
	int slot = slot_of_map(r->map), e, prev, ok;
	ent_id_t *id;
	col_ent_t *cv;

	if (r->state != RUN_DRY || slot < 0)
		return -1;
	prev = eng_frame_layer();
	eng_set_frame_layer(slot);
	e = ent_create(reserve);
	if (e < 0)
	{
		eng_set_frame_layer(prev);
		return -1;
	}
	id = ent_idc(e);
	id->layer = (int8_t)slot;
	id->gid = GID_NONE;
	id->rtid = r->rtid;
	id->cls = r->cls;
	cv = ent_cv(e);
	memcpy(cv->origin, r->org, sizeof(cv->origin));
	memcpy(cv->angles, r->ang, sizeof(cv->angles));
	R.retry[e] = 0;
	r->state = RUN_LIVE;
	r->h = ent_handle_of(e);
	ok = game_spawn_class(r->cls, e);
	eng_set_frame_layer(prev);
	if (ok < 0 || !ent_is_live(e))
	{
		if (ent_is_live(e))
			ent_destroy(e);
		store_drop(r->map, LUMP_RT, r->rtid);
		r->state = RUN_FREE;
		return -1;
	}
	rehydrate_from_store(e, r->map, LUMP_RT, r->rtid);
	if (r->name != 0xFFFF)
	{
		rec_name_hold(e, NAME_MAKE(slot, r->name));
		r->name = 0xFFFF;
	}
	R.map[slot].pgen++;
	R.st.hydrated++;
	return e;
}

int rec_ref_entity(uint16_t map, uint16_t id, int runtime)
{
	int slot = slot_of_map(map), i;

	if (slot < 0)
		return -1;
	if (!runtime)
		return id < 1024 ? rec_entity(GID_MAKE(slot, id)) : -1;
	i = run_find(map, id);
	return i < 0 ? -1 : run_entity(&R.run[i]);
}

/* ---- record lifetime ----------------------------------------------------------- */

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
	if (m->rt[ri].state == REC_DRY)
		store_drop(m->map, (uint16_t)GID_LUMP(gid), 0);
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
	rmap_t *m = NULL;
	const hle_name_t *nm = name_of(n, &m);
	int count = nm ? nm->count : 0, j;

	while (*it < count)
	{
		int i = (*it)++, ri, e;
		gid_t16 g = twin_current(GID_MAKE(m->slot, m->ll[nm->first + i]));
		rmap_t *tm;
		rrt_t *t;

		if ((ri = rec_index(g, &tm)) < 0)
			continue;
		t = &tm->rt[ri];
		if (!(t->flags & RT_PHYS))
			continue;
		if ((e = rt_entity(t)) >= 0)
			return e;
		if (t->state == REC_PENDING || t->state == REC_DRY)
		{
			e = hydrate(tm, ri, ENT_ALLOC_GAME);
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
	/* released runtime holders of the name in its map */
	for (j = *it - count - REC_HOLDERS; nm && j < REC_RUNTIME; j++)
	{
		const rrun_t *r = &R.run[j];
		int e;

		(*it)++;
		if (r->state == RUN_DRY && r->map == m->map && r->name == NAME_INDEX(n)
			&& (e = hydrate_rt(j, ENT_ALLOC_GAME)) >= 0)
		{
			R.st.woken++;
			return e;
		}
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

/* a use waiting for its target: canonical (map, lump) / (map, rtid), the
 * activator and caller as canonical references (callers keep the FIFO
 * from overflowing: full = they hydrate in the call) */
static void defer(uint16_t map, uint16_t id, int runtime, int act, int caller, gid_t16 caller_gid,
	int use_type, float value)
{
	rdefer_t *d = &R.defer[R.ndefer++];
	const rmap_t *cm;

	memset(d, 0, sizeof(*d));
	d->map = map;
	d->id = id;
	d->runtime = (uint8_t)runtime;
	d->use_type = (uint8_t)use_type;
	d->value = value;
	d->caller_map = d->caller_lump = 0xFFFF;
	if (caller_gid != GID_NONE && (cm = slot_map(GID_SLOT(caller_gid))))
	{
		d->caller_map = cm->map;
		d->caller_lump = (uint16_t)GID_LUMP(caller_gid);
	}
	ref_make(&d->act, act);
	ref_make(&d->caller, caller);
}

void rec_fire(name_t n, int act, int caller, gid_t16 caller_gid, int use_type, float value)
{
	rmap_t *m = NULL;
	const hle_name_t *nm = name_of(n, &m);
	int i, j, count = nm ? nm->count : 0;

	for (i = 0; i < count; i++)
	{
		int ri = m->l2r[m->ll[nm->first + i]], e, ti;
		rrt_t *t = &m->rt[ri];
		gid_t16 gid = gid_of(m, ri);
		rmap_t *tm;

		if (t->flags & RT_LOGIC)
		{
			if (t->state == REC_LIVE)
				use_logic(gid, act, caller, caller_gid, use_type, value);
			continue;
		}
		if (t->flags & RT_POINT)
			continue;
		/* a twin resolves to the live copy (either slot) */
		gid = twin_current(gid);
		if ((ti = rec_index(gid, &tm)) < 0)
			continue;
		t = &tm->rt[ti];
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
			if ((e = hydrate(tm, ti, ENT_ALLOC_GAME)) >= 0)
			{
				R.st.woken++;
				use_ent(e, act, caller, caller_gid, use_type, value);
			}
		}
		else
			defer(tm->map, (uint16_t)GID_LUMP(gid), 0, act, caller, caller_gid, use_type, value);
	}
	if (n == NAME_NONE)
		return;
	for (j = 0; j < REC_HOLDERS; j++)
	{
		int e;

		if (R.hold[j].n == n && (e = ent_of(R.hold[j].h)) >= 0)
			use_ent(e, act, caller, caller_gid, use_type, value);
	}
	/* released runtime holders of the name (monstermaker children) */
	for (j = 0; nm && j < REC_RUNTIME; j++)
	{
		const rrun_t *r = &R.run[j];
		int e;

		if (r->state != RUN_DRY || r->map != m->map || r->name != NAME_INDEX(n))
			continue;
		if (R.sync_wakes < REC_SYNC_WAKES || R.ndefer >= REC_DEFER)
		{
			R.sync_wakes++;
			if ((e = hydrate_rt(j, ENT_ALLOC_GAME)) >= 0)
			{
				R.st.woken++;
				use_ent(e, act, caller, caller_gid, use_type, value);
			}
		}
		else
			defer(r->map, r->rtid, 1, act, caller, caller_gid, use_type, value);
	}
}

/* HLSDK killtarget: every entity with the name removed (UTIL_Remove), its
 * record (and its twin) never respawns; logic records stop */
static void kill_name(name_t n)
{
	rmap_t *m = NULL;
	const hle_name_t *nm = name_of(n, &m);
	int i, j, k;

	for (i = 0; nm && i < nm->count; i++)
	{
		int ri = m->l2r[m->ll[nm->first + i]];
		gid_t16 g[2];

		if (m->rt[ri].flags & RT_POINT)
			continue;
		g[0] = gid_of(m, ri);
		g[1] = rec_twin_of(g[0]);
		for (k = 0; k < 2; k++)
		{
			int e = g[k] == GID_NONE ? -1 : rec_entity(g[k]);

			if (g[k] == GID_NONE)
				continue;
			if (e >= 0)
				ent_remove(e);
			rec_kill(g[k]);
		}
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
	for (j = 0; nm && j < REC_RUNTIME; j++)
		if (R.run[j].state == RUN_DRY && R.run[j].map == m->map && R.run[j].name == NAME_INDEX(n))
			run_free(&R.run[j]);
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

/* first-fit block in the HLE heap among the opening / open slots' blocks
 * and the frozen maps' blocks */
static uint32_t heap_find(uint32_t need)
{
	uint32_t at = 0;
	int moved = 1, s;

	while (moved)
	{
		moved = 0;
		for (s = 0; s < REC_SLOTS + REC_FROZEN_MAPS; s++)
		{
			uint32_t b, l;

			if (s < REC_SLOTS)
			{
				if (R.map[s].phase < PH_CHUNK)
					continue;
				b = R.map[s].block;
				l = R.map[s].block_len;
			}
			else
			{
				if (R.fmap[s - REC_SLOTS].map == WLD_NO_MAP)
					continue;
				b = R.fmap[s - REC_SLOTS].block;
				l = R.fmap[s - REC_SLOTS].block_len;
			}
			if (at < b + l && b < at + need)
			{
				at = (b + l + 15) & ~15u;
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

/* a record's cooked bounds: its origin, plus its brush model's box */
static void cooked_bounds(const rmap_t *m, int ri, int16_t lo[3], int16_t hi[3])
{
	const wld_layer_t *L = wld_layer(m->slot);
	const hle_key_t *key = rkey(m, ri, RK_ORIGIN);
	const hle_key_t *mk = rkey(m, ri, RK_MODEL);
	float org[3] = { 0, 0, 0 }, l[3], h[3];
	int k;

	if (key && rkind(m, key) == HLE_KV)
		memcpy(org, m->fl + key->val, sizeof(org));
	memcpy(l, org, sizeof(l));
	memcpy(h, org, sizeof(h));
	if (L && mk && rkind(m, mk) == HLE_KS && RES_KIND(mk->id) == RES_K_SUBMODEL)
		for (k = 0; k < 3; k++)
		{
			l[k] += L->models[RES_SUB_NUM(mk->id)].mins[k];
			h[k] += L->models[RES_SUB_NUM(mk->id)].maxs[k];
		}
	for (k = 0; k < 3; k++)
	{
		lo[k] = s16_lo(l[k]);
		hi[k] = s16_hi(h[k]);
	}
}

/* the layer's DECL chunk (w64-format.md), -1 when none */
static int decl_chunk(const wld_layer_t *L)
{
	uint32_t c;

	for (c = 0; c < L->dir->nchunks; c++)
		if (L->chunks[c].fourcc == REC_DECL_FOURCC && L->chunks[c].size >= 8)
			return (int)c;
	return -1;
}

static void submit(io_req_t *r, uint32_t rom, uint8_t *dst, uint32_t len)
{
	memset(r, 0, sizeof(*r));
	r->rom = rom;
	r->dst = dst;
	r->len = len;
	r->prio = IO_PRIO_OPEN;
	io_submit(r);
}

static int req_busy(const io_req_t *r)
{
	return r->len && r->state != IO_DONE;
}

static void open_abort(rmap_t *m)
{
	int i;

	for (i = 0; i < 3; i++)
		if (m->req[i].state == IO_QUEUED || m->req[i].state == IO_ACTIVE)
			io_cancel(&m->req[i]);
	m->open = 0;
	m->phase = PH_CLOSED;
}

/* 1: the layer is ready: queue the HLE1 header and the DECL header reads */
static void start_header(rmap_t *m, const wld_layer_t *L)
{
	const w64_chunk_t *ck;
	int c;

	if (L->dir->ents_chunk == W64_NONE)
		sys_error("%s: no entity chunk", wld_map_name(m->map));
	ck = &L->chunks[L->dir->ents_chunk];
	if (ck->size < sizeof(m->hdr) || ck->size > R.heap_len)
		sys_error("%s: ENTS chunk size %lu", wld_map_name(m->map), (unsigned long)ck->size);
	submit(&m->req[0], L->rom + ck->ofs, m->hdr, sizeof(m->hdr));
	if ((c = decl_chunk(L)) >= 0)
		submit(&m->req[1], L->rom + L->chunks[c].ofs, m->decl, 8);
	m->phase = PH_HEADER;
}

/* 2: headers landed: the block (chunk, runtime, logic blocks bounded by
 * records, pending list; the cook's region proof mirrors this and
 * block_len below, tools/w64c/hleids.py slot_bytes), the chunk read */
static void header_done(rmap_t *m, const wld_layer_t *L)
{
	const w64_chunk_t *ck = &L->chunks[L->dir->ents_chunk];
	const hle_header_t *h = (const hle_header_t *)m->hdr;
	const char *file = wld_map_name(m->map);
	uint32_t len = ck->size, nrecs, need, at, n;
	const uint8_t *d = m->decl;

	nrecs = h->magic == HLE_MAGIC ? h->numrecs : hle_sw16(h->numrecs);
	if (nrecs > HLE_MAX_LUMP)
		sys_error("%s: ENTS: %lu records", file, (unsigned long)nrecs);
	/* infodecal ids of the layer: the global sheet's, then the DECL's */
	m->ndecals = REC_DECALS_GLOBAL;
	if (m->req[1].len)
	{
		n = (uint32_t)d[4] << 8 | d[5];
		if (((uint32_t)d[0] << 24 | (uint32_t)d[1] << 16 | (uint32_t)d[2] << 8 | d[3])
			== REC_DECL_MAGIC && n <= REC_DECALS_MAP_MAX)
			m->ndecals += n;
	}
	need = ((len + 15) & ~15u) + nrecs * (uint32_t)sizeof(rrt_t)
		+ nrecs * (uint32_t)sizeof(rec_logic_t) + 2 * nrecs + 16;
	if ((at = heap_find(need)) == ~0u)
		sys_error("%s: ENTS: %lu B do not fit the HLE region (%u KB)", file,
			(unsigned long)need, (unsigned)HL64_HLE_BLOBS_KB);
	m->block = at;
	m->block_len = need;
	m->len = len;
	submit(&m->req[2], L->rom + ck->ofs, R.heap + at, (len + 1) & ~1u);
	m->phase = PH_CHUNK;
}

static void thaw_runtime(rmap_t *m);

/* 3: the chunk landed: validate, records, the frozen runtime part back,
 * logic and timers live */
static void chunk_done(rmap_t *m, const wld_layer_t *L)
{
	const char *file = wld_map_name(m->map), *why;
	uint8_t *p = R.heap + m->block;
	uint32_t i, k, nlog = 0;
	hle_header_t *h;
	rec_limits_t lim;
	int prev;

	memset(&lim, 0, sizeof(lim));
	lim.classes = (uint32_t)res_classes();
	lim.patterns = (uint32_t)res_patterns();
	lim.globals = (uint32_t)res_globals();
	lim.titles = (uint32_t)res_titles();
	for (k = 0; k < 8; k++)
		lim.kcount[k] = (uint32_t)res_count((int)k);
	lim.nsub = L->dir->nsub;
	lim.ndecals = m->ndecals;
	if ((why = rec_hle_check(p, m->len, &lim, &m->graph, &m->graph_size)))
		sys_error("%s: ENTS chunk: %s", file, why);
	m->b = p;
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
	p += (m->len + 15) & ~15u;
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
	/* the slot is readable from here (cooked_bounds, rec_* queries) */
	m->open = 1;
	/* records: kinds, bounds, pending */
	for (i = 0; i < h->numrecs; i++)
	{
		rrt_t *t = &m->rt[i];
		const hle_rec_t *r = &m->recs[i];

		memset(t, 0, sizeof(*t));
		t->state = REC_PENDING;
		cooked_bounds(m, (int)i, t->lo, t->hi);
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
	/* logic blocks only for the logic records, the pending list after */
	{
		uint8_t *end = (uint8_t *)(m->logic + nlog);
		uint16_t *pl = (uint16_t *)(((uintptr_t)end + 3) & ~(uintptr_t)3);

		memmove(pl, m->plist, 2u * m->npend);
		m->plist = pl;
		m->block_len = (uint32_t)((uint8_t *)(pl + h->numrecs) - (R.heap + m->block));
	}
	m->idle = 0;
	m->phase = PH_READY;
	thaw_runtime(m);
	for (i = 0, m->logic_pending = 0; i < h->numrecs; i++)
	{
		if ((m->rt[i].flags & RT_LOGIC) && m->rt[i].state == REC_PENDING)
			m->logic_pending = 1;
		/* GoldSrc precaches a map's models at load: interest for every
		 * physical record's model key (the residency owners stream them) */
		if (m->rt[i].flags & RT_PHYS)
		{
			res_t r = rec_key_res(gid_of(m, (int)i), RK_MODEL);

			if (r != RES_NONE && RES_KIND(r) != RES_K_SUBMODEL)
				res_precache(r);
		}
	}
	prev = eng_frame_layer();
	eng_set_frame_layer(m->slot);
	game_map_opened(m->slot);
	m->pre_wait = 1;	/* its precache lands before its records hydrate */
	thaw(m);
	/* a reopened map's live logic records (frozen state thawed): the
	 * class's Precache on restore (HLSDK DispatchRestore) */
	for (i = 0; i < h->numrecs; i++)
		if ((m->rt[i].flags & RT_LOGIC) && m->rt[i].state == REC_LIVE)
			game_logic_restored(gid_of(m, (int)i));
	eng_set_frame_layer(prev);
	log_printf("rec: %s open in slot %d: %u records (%u logic, %u pending), chunk %lu B, "
		"block %lu B\n", file, m->slot, m->h->numrecs, m->nlogic, m->npend,
		(unsigned long)m->len, (unsigned long)m->block_len);
}

/* the async open's next steps, as far as the reads allow */
static void advance(rmap_t *m)
{
	const wld_layer_t *L;

	if (m->phase == PH_CLOSED || m->phase == PH_READY)
		return;
	if (!(L = wld_layer_of_map(m->map)) || L->slot != m->slot || wld_layer_ready(L) < 0)
	{
		log_printf("rec: %s: layer closed or failed while its HLE slot opened\n",
			wld_map_name(m->map));
		open_abort(m);
		return;
	}
	if (m->phase == PH_LAYER)
	{
		if (wld_layer_ready(L) != 1)
			return;
		start_header(m, L);
	}
	if (m->phase == PH_HEADER)
	{
		if (req_busy(&m->req[0]) || req_busy(&m->req[1]))
			return;
		header_done(m, L);
	}
	if (m->phase == PH_CHUNK && !req_busy(&m->req[2]))
		chunk_done(m, L);
}

int rec_map_open(int layer)
{
	const wld_layer_t *L;
	rmap_t *m;
	int map;

	if (layer < 0 || layer >= REC_SLOTS || !R.map)
		return -1;
	/* wld_layer is NULL while the layer's own async open is in flight */
	if (!(L = wld_layer(layer)))
		for (map = 0; map < wld_map_count() && !L; map++)
			if ((L = wld_layer_of_map(map)) && L->slot != layer)
				L = NULL;
	if (!L)
		return -1;
	m = &R.map[layer];
	if (m->phase != PH_CLOSED && m->map == L->map)
		return layer;
	if (m->phase != PH_CLOSED)
		rec_map_close(layer);
	memset(m, 0, sizeof(*m));
	m->slot = (uint8_t)layer;
	m->map = L->map;
	m->phase = PH_LAYER;
	advance(m);
	return layer;
}

int rec_map_ready(int slot)
{
	if (slot < 0 || slot >= REC_SLOTS || !R.map)
		return 0;
	advance(&R.map[slot]);
	return R.map[slot].open && !R.map[slot].pre_wait;
}

void rec_map_finish(int slot)
{
	rmap_t *m;
	int i;

	if (slot < 0 || slot >= REC_SLOTS || !R.map)
		return;
	m = &R.map[slot];
	io_set_load(1);
	while (m->phase != PH_CLOSED && m->phase != PH_READY)
	{
		int ph = m->phase;

		for (i = 0; i < 3; i++)
			if (m->req[i].state == IO_QUEUED || m->req[i].state == IO_ACTIVE)
				io_finish(&m->req[i], "hle open");
		advance(m);
		if (m->phase == ph)
			sys_error("rec: %s: HLE open waits for its layer", wld_map_name(m->map));
	}
	/* a map load: its precache's infos are read now too (load I/O) */
	if (m->pre_wait)
	{
		studio_preload();
		m->pre_wait = 0;
	}
	io_set_load(0);
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

/* ---- frozen runtime part (architecture.md 4.1 "Frozen") ------------------------------ */

enum { FS_PENDING, FS_LIVE, FS_KILLED, FS_DRY };	/* 2-bit record states */

static uint32_t frozen_bytes(uint32_t nrecs, uint32_t nlogic)
{
	return (((nrecs + 3) / 4 + 3) & ~3u) + nlogic * (uint32_t)sizeof(rec_logic_t);
}

static rfmap_t *frozen_of(uint16_t map)
{
	int i;

	for (i = 0; i < REC_FROZEN_MAPS; i++)
		if (R.fmap[i].map == map)
			return &R.fmap[i];
	return NULL;
}

static void store_purge_map(uint16_t map);

/* A frozen map is forgotten (a third map froze): its record states, store
 * deltas, frozen timers, queued uses and runtime entities go; it reopens
 * as cooked. */
static void frozen_drop(rfmap_t *f)
{
	uint32_t i, j;

	log_printf("rec: frozen %s dropped (%d frozen maps kept)\n", wld_map_name(f->map),
		REC_FROZEN_MAPS);
	store_purge_map(f->map);
	for (i = j = 0; i < R.nfrozen; i++)
		if (R.frozen[i].map != f->map)
			R.frozen[j++] = R.frozen[i];
	R.nfrozen = j;
	for (i = j = 0; i < R.ndefer; i++)
		if (R.defer[i].map != f->map)
			R.defer[j++] = R.defer[i];
	R.ndefer = j;
	for (i = 0; i < REC_RUNTIME; i++)
		if (R.run[i].state != RUN_FREE && R.run[i].map == f->map)
			R.run[i].state = RUN_FREE;
	f->map = WLD_NO_MAP;
}

static rfmap_t *frozen_oldest(void)
{
	rfmap_t *o = NULL;
	int i;

	for (i = 0; i < REC_FROZEN_MAPS; i++)
		if (R.fmap[i].map != WLD_NO_MAP && (!o || R.fmap[i].age < o->age))
			o = &R.fmap[i];
	return o;
}

/* close: record states (2 bits) and logic blocks into a frozen block */
static void freeze_runtime(rmap_t *m)
{
	uint32_t n = m->h->numrecs, need = frozen_bytes(n, m->nlogic), at, i;
	rfmap_t *f;
	uint8_t *p;

	if ((f = frozen_of(m->map)))
		frozen_drop(f);
	while (!(f = frozen_of(WLD_NO_MAP)))
		frozen_drop(frozen_oldest());
	while ((at = heap_find(need)) == ~0u)
	{
		rfmap_t *o = frozen_oldest();

		if (!o)
			sys_error("rec: %s: frozen runtime %lu B does not fit the HLE region",
				wld_map_name(m->map), (unsigned long)need);
		frozen_drop(o);
	}
	p = R.heap + at;
	memset(p, 0, need);
	for (i = 0; i < n; i++)
	{
		const rrt_t *t = &m->rt[i];
		uint32_t s = t->state == REC_KILLED ? FS_KILLED : t->state == REC_DRY ? FS_DRY
			: t->state == REC_LIVE && (t->flags & RT_LOGIC) ? FS_LIVE : FS_PENDING;

		p[i >> 2] |= (uint8_t)(s << ((i & 3) * 2));
	}
	memcpy(p + (((n + 3) / 4 + 3) & ~3u), m->logic, m->nlogic * sizeof(rec_logic_t));
	f->map = m->map;
	f->nrecs = (uint16_t)n;
	f->nlogic = m->nlogic;
	f->block = at;
	f->block_len = need;
	f->age = ++R.fage;
}

/* reopen: the frozen states and logic blocks over the fresh records */
static void thaw_runtime(rmap_t *m)
{
	rfmap_t *f = frozen_of(m->map);
	const uint8_t *p;
	uint32_t i;

	if (!f)
		return;
	if (f->nrecs != m->h->numrecs || f->nlogic != m->nlogic)
	{
		log_printf("rec: %s: frozen part for %u records, chunk has %u: dropped\n",
			wld_map_name(m->map), f->nrecs, m->h->numrecs);
		frozen_drop(f);
		return;
	}
	p = R.heap + f->block;
	for (i = 0; i < f->nrecs; i++)
	{
		rrt_t *t = &m->rt[i];
		uint32_t s = (p[i >> 2] >> ((i & 3) * 2)) & 3;
		const uint8_t *e;

		if (s == FS_KILLED)
		{
			pend_remove(m, (int)i);
			t->state = REC_KILLED;
		}
		else if (s == FS_LIVE && (t->flags & RT_LOGIC))
			t->state = REC_LIVE;
		else if (s == FS_DRY && (t->flags & RT_PHYS)
			&& (e = store_find(m->map, m->recs[i].lump, 0)))
		{
			const rstore_t *st = (const rstore_t *)e;

			pend_remove(m, (int)i);
			t->state = REC_DRY;
			memcpy(t->lo, st->lo, sizeof(t->lo));
			memcpy(t->hi, st->hi, sizeof(t->hi));
			pend_add(m, (int)i);
		}
	}
	memcpy(m->logic, p + (((f->nrecs + 3) / 4 + 3) & ~3u), f->nlogic * sizeof(rec_logic_t));
	f->map = WLD_NO_MAP;
}

/* ---- release (dehydrate, 6.4) ------------------------------------------------------- */

static uint32_t store_need(uint32_t n)
{
	return (uint32_t)sizeof(rstore_t) + ((n + 3u) & ~3u);
}

/* The slot whose closing release is dehydrating an entity (rec_slot_closing:
 * the game stores a closing map's entities whatever their state). */
static int closing_slot = -1;

int rec_slot_closing(int slot)
{
	return slot >= 0 && slot == closing_slot;
}

/* Dehydrate entity e of an open slot into the store: record DRY / runtime
 * entry dry, entity freed. 1 when e is gone (released, or freed by the
 * game: transients), 0 refused (retry stamped). `closing`: the game does
 * not refuse (rec_slot_closing); a refusal anyway (store full) frees the
 * entity (its record respawns fresh; a runtime entity is lost). */
static int release_one(int e, int closing)
{
	ent_id_t *id = ent_idc(e);
	const col_ent_t *cv = ent_cv(e);
	rmap_t *m = slot_map(id->layer), *rm;
	rrt_t *t = NULL;
	int ri = -1, run = -1, prev, j, k;
	uint16_t lump = LUMP_RT, rtid = 0;
	uint32_t room, n = 0;
	rstore_t hdr;
	uint8_t *at;

	if (e < 2 || !ent_is_live(e) || !m)
		return 0;
	if (id->gid != GID_NONE)
	{
		if ((ri = rec_index(id->gid, &rm)) < 0 || rm != m)
		{
			if (!closing)
				return 0;
			ent_destroy(e);
			R.close_dropped++;
			return 1;
		}
		t = &m->rt[ri];
		lump = (uint16_t)GID_LUMP(id->gid);
	}
	else if (!(rtid = rec_runtime_id(e)) || (run = run_find(m->map, rtid)) < 0)
	{
		if (!closing)
		{
			R.retry[e] = (float)(eng_time() + REC_RELEASE_RETRY);
			return 0;
		}
		ent_destroy(e);
		R.close_dropped++;
		return 1;
	}
	memset(&hdr, 0, sizeof(hdr));
	hdr.map = m->map;
	hdr.lump = lump;
	hdr.rtid = rtid;
	for (k = 0; k < 3; k++)
	{
		int box = cv->absmin[k] != 0 || cv->absmax[k] != 0;

		hdr.lo[k] = s16_lo(box ? cv->absmin[k] : cv->origin[k]);
		hdr.hi[k] = s16_hi(box ? cv->absmax[k] : cv->origin[k]);
	}
	store_drop(m->map, lump, rtid);
	room = R.store_cap - R.store_used;
	at = R.store + R.store_used + sizeof(rstore_t);
	if (room > sizeof(rstore_t) + 4)
	{
		prev = eng_frame_layer();
		eng_set_frame_layer(id->layer);
		closing_slot = closing ? id->layer : -1;
		n = game_dehydrate(e, at, room - (uint32_t)sizeof(rstore_t));
		closing_slot = -1;
		eng_set_frame_layer(prev);
	}
	else if (!R.store_full++)
		log_printf("rec: dehydrate store full (%lu B): releases refused\n",
			(unsigned long)R.store_cap);
	if (!ent_is_live(e))
	{
		/* the game freed it (a transient effect): gone, nothing stored */
		if (run >= 0)
			R.run[run].state = RUN_FREE;
		if (t)
			rt_entity(t);
		return 1;
	}
	if (n > room - (uint32_t)sizeof(rstore_t))
		sys_error("rec: game_dehydrate wrote %lu B of %lu", (unsigned long)n,
			(unsigned long)(room - sizeof(rstore_t)));
	if (!n || store_need(n) > room)
	{
		if (!closing)
		{
			R.retry[e] = (float)(eng_time() + REC_RELEASE_RETRY);
			return 0;
		}
		/* closing: the entity goes anyway; its record respawns fresh */
		log_printf("rec: %s #%d refused dehydration at close: dropped\n",
			wld_map_name(m->map), e);
		R.close_dropped++;
		n = 0;
	}
	else if (t && n == 1 && at[0] == 0)
		n = 0;	/* pristine (spawn state): respawn from its keys is exact */
	if (t)
	{
		t->h = 0;
		if (n)
		{
			t->state = REC_DRY;
			memcpy(t->lo, hdr.lo, sizeof(t->lo));
			memcpy(t->hi, hdr.hi, sizeof(t->hi));
		}
		else
		{
			t->state = REC_PENDING;
			cooked_bounds(m, ri, t->lo, t->hi);
		}
		pend_add(m, ri);
	}
	else if (!n)
		R.run[run].state = RUN_FREE;
	else
	{
		rrun_t *r = &R.run[run];

		r->state = RUN_DRY;
		r->cls = id->cls;
		memcpy(r->org, cv->origin, sizeof(r->org));
		memcpy(r->ang, cv->angles, sizeof(r->ang));
		r->name = 0xFFFF;
		for (j = 0; j < REC_HOLDERS; j++)
			if (R.hold[j].n != NAME_NONE && ent_of(R.hold[j].h) == e)
			{
				if (NAME_SLOT(R.hold[j].n) == m->slot)
					r->name = NAME_INDEX(R.hold[j].n);
				R.hold[j].n = NAME_NONE;
			}
		m->pgen++;
	}
	if (n)
	{
		hdr.len = (uint16_t)n;
		memcpy(R.store + R.store_used, &hdr, sizeof(hdr));
		R.store_used += store_need(n);
		if (R.store_used > R.store_peak)
			R.store_peak = R.store_used;
	}
	ent_destroy(e);
	R.st.released++;
	R.st.released_total++;
	return 1;
}

int rec_release(int e)
{
	return e >= 2 && ent_is_live(e) ? release_one(e, 0) : 0;
}

int rec_slot_live(int slot)
{
	int e, j, n = 0;

	if (slot < 0 || slot >= REC_SLOTS)
		return 0;
	for (e = 2; e < HL64_MAX_ENTITIES; e++)
		if (ent_is_live(e) && ent_idc(e)->layer == slot)
			n++;
	for (j = 0; j < REC_HOLDERS; j++)
		if (R.hold[j].n != NAME_NONE && NAME_SLOT(R.hold[j].n) == slot
			&& (e = ent_of(R.hold[j].h)) >= 0 && ent_idc(e)->layer != slot)
			n++;
	return n;
}

void rec_map_close(int slot)
{
	rmap_t *m;
	uint32_t rel0, drop0;
	int e, j, prev;

	if (slot < 0 || slot >= REC_SLOTS || !R.map || (m = &R.map[slot])->phase == PH_CLOSED)
		return;
	if (!m->open)
	{
		open_abort(m);
		return;
	}
	prev = eng_frame_layer();
	eng_set_frame_layer(slot);
	game_cut_refs(slot);
	game_map_closing(slot);
	eng_set_frame_layer(prev);
	rec_twin_clear(slot);
	/* every live entity of the map dehydrates (the closing release) */
	rel0 = R.st.released_total;
	drop0 = R.close_dropped;
	for (e = 2; e < HL64_MAX_ENTITIES; e++)
		if (ent_is_live(e) && ent_idc(e)->layer == slot)
			release_one(e, 1);
	for (j = 0; j < REC_HOLDERS; j++)
		if (R.hold[j].n != NAME_NONE && NAME_SLOT(R.hold[j].n) == slot)
			R.hold[j].n = NAME_NONE;
	freeze(m);
	freeze_runtime(m);
	m->open = 0;
	m->phase = PH_CLOSED;
	log_printf("rec: %s closed from slot %d: %lu released, %lu dropped, frozen %lu B, "
		"store %lu B\n", wld_map_name(m->map), slot,
		(unsigned long)(R.st.released_total - rel0), (unsigned long)(R.close_dropped - drop0),
		(unsigned long)frozen_bytes(m->h->numrecs, m->nlogic), (unsigned long)R.store_used);
}

/* ---- twins, re-homing ------------------------------------------------------------------ */

/* queued uses and use contexts of record (map, lump) follow a new owner */
static void defer_retarget(uint16_t map, uint16_t id, int runtime, uint16_t nmap, uint16_t nid,
	int nruntime)
{
	uint32_t i;
	int k;

	for (i = 0; i < R.ndefer; i++)
	{
		rdefer_t *d = &R.defer[i];
		rref_t *refs[2] = { &d->act, &d->caller };

		if (d->runtime == runtime && d->map == map && d->id == id)
		{
			d->map = nmap;
			d->id = nid;
			d->runtime = (uint8_t)nruntime;
		}
		for (k = 0; k < 2; k++)
			if (refs[k]->kind == (runtime ? RR_RT : RR_REC) && refs[k]->map == map
				&& refs[k]->id == id)
			{
				refs[k]->kind = nruntime ? RR_RT : RR_REC;
				refs[k]->map = nmap;
				refs[k]->id = nid;
			}
	}
}

void rec_twin_alias(gid_t16 a, gid_t16 b)
{
	rmap_t *ma, *mb;
	int ra = rec_index(a, &ma), rb = rec_index(b, &mb), ea, eb;
	rtwin_t *tw;

	if (ra < 0 || rb < 0 || ma == mb)
		return;
	if (!(tw = twin_find(a)) && !(tw = twin_find(b)))
	{
		if (R.ntwin >= REC_TWINS)
			sys_error("rec: more than %d twin pairs", REC_TWINS);
		tw = &R.twin[R.ntwin++];
	}
	tw->a = a;
	tw->b = b;
	/* the live copy. a is the home copy (seam.c aliases the approached seam
	 * home -> B): live, it stays the one names reach, and a B copy the
	 * reopened map's pump hydrated before the alias (its stale state from
	 * its own close) retires silently, as the crossing retires A's copy;
	 * GoldSrc carries the transition volume's (global) entities over the
	 * copies the level file holds (HLSDK CGlobalState / globalname). Else
	 * b, the copy taking over. */
	ea = rt_entity(&ma->rt[ra]);
	eb = rt_entity(&mb->rt[rb]);
	tw->cur = ea < 0;
	if (ea >= 0 && eb >= 0 && !rec_release(eb))
		log_printf("rec: twin %u:%u: stale B copy not released (game refused)\n",
			(unsigned)GID_SLOT(b), (unsigned)GID_LUMP(b));
}

void rec_twin_clear(int slot)
{
	uint32_t i, j;

	for (i = j = 0; i < R.ntwin; i++)
	{
		rtwin_t tw = R.twin[i];
		gid_t16 cur, old;
		rmap_t *mc, *mo;

		if (GID_SLOT(tw.a) != (uint32_t)slot && GID_SLOT(tw.b) != (uint32_t)slot)
		{
			R.twin[j++] = tw;
			continue;
		}
		/* uses queued for the retired copy go to the live one */
		cur = tw.cur ? tw.b : tw.a;
		old = tw.cur ? tw.a : tw.b;
		if (rec_index(cur, &mc) >= 0 && rec_index(old, &mo) >= 0)
			defer_retarget(mo->map, (uint16_t)GID_LUMP(old), 0, mc->map,
				(uint16_t)GID_LUMP(cur), 0);
	}
	R.ntwin = j;
}

gid_t16 rec_twin_of(gid_t16 g)
{
	const rtwin_t *tw = g == GID_NONE ? NULL : twin_find(g);

	return !tw ? GID_NONE : tw->a == g ? tw->b : tw->a;
}

int rec_rehome(int e, int slot)
{
	rmap_t *mb = slot_map(slot), *ma = NULL;
	ent_id_t *id;
	uint16_t omap = WLD_NO_MAP, olump = LUMP_RT, ortid, rtid;
	name_t on;
	int ri = -1, i, j;

	if (e < 2 || !ent_is_live(e) || !mb)
		return -1;
	id = ent_idc(e);
	ortid = id->rtid;
	if (id->gid != GID_NONE)
		ri = rec_index(id->gid, &ma);
	else
		ma = slot_map(id->layer);
	if (ma == mb)
		return (rtid = rec_runtime_id(e)) ? rtid : -1;
	/* room for the durable id first: nothing changes on failure */
	if (!ortid)
	{
		for (i = 0; i < REC_RUNTIME; i++)
		{
			run_entity(&R.run[i]);
			if (R.run[i].state == RUN_FREE)
				break;
		}
		if (i == REC_RUNTIME)
			return -1;
	}
	if (ma)
		omap = ma->map;
	if (ri >= 0)
	{
		/* moved: its record never respawns in its map */
		olump = (uint16_t)GID_LUMP(id->gid);
		ma->rt[ri].state = REC_KILLED;
		ma->rt[ri].h = 0;
	}
	on = id->targetname;
	id->gid = GID_NONE;
	id->layer = (int8_t)slot;
	if (ortid && (i = run_find(omap, ortid)) >= 0)
		R.run[i].map = mb->map;
	else
		id->rtid = 0;
	rtid = rec_runtime_id(e);
	/* its names as names of the new map (fires reach it as a holder) */
	id->targetname = on == NAME_NONE ? NAME_NONE : rec_name_in(on, slot);
	for (j = 0; j < REC_HOLDERS; j++)
		if (R.hold[j].n != NAME_NONE && ent_of(R.hold[j].h) == e)
			R.hold[j].n = rec_name_in(R.hold[j].n, slot);
	if (id->targetname != NAME_NONE)
		rec_name_hold(e, id->targetname);
	if (ri >= 0)
		defer_retarget(omap, olump, 0, mb->map, rtid, 1);
	else if (ortid)
		defer_retarget(omap, ortid, 1, mb->map, rtid, 1);
	return rtid;
}

/* ---- hydration pump and release batches (step 8) ----------------------------------- */

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

static float abs_d2(const float lo[3], const float hi[3], const float p[3])
{
	float d2 = 0;
	int k;

	for (k = 0; k < 3; k++)
	{
		float d = p[k] < lo[k] ? lo[k] - p[k] : p[k] > hi[k] ? p[k] - hi[k] : 0;

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

/* Deferred uses whose target map is open, in FIFO order per map (a use
 * that cannot be delivered yet holds back the later uses of its map);
 * a closed map's uses wait for its reopen. */
static void deliver_deferred(uint64_t t0, uint32_t *done)
{
	uint32_t i = 0, held = 0;

	while (i < R.ndefer && budget_left(t0, *done))
	{
		rdefer_t d = R.defer[i];
		int slot = slot_of_map(d.map), e = -1, ri, k, wait = 0;
		gid_t16 cg = GID_NONE;
		rmap_t *m;

		if (slot < 0 || (held & 1u << slot))
		{
			i++;
			continue;
		}
		if (d.runtime)
		{
			if ((k = run_find(d.map, d.id)) >= 0 && (e = run_entity(&R.run[k])) < 0
				&& R.run[k].state == RUN_DRY)
			{
				if ((e = hydrate_rt(k, ENT_ALLOC_GAME)) >= 0)
				{
					R.st.woken++;
					(*done)++;
				}
				else
					wait = R.run[k].state == RUN_DRY;
			}
		}
		else if ((ri = rec_index(twin_current(GID_MAKE(slot, d.id)), &m)) >= 0
			&& (e = rt_entity(&m->rt[ri])) < 0
			&& (m->rt[ri].state == REC_PENDING || m->rt[ri].state == REC_DRY))
		{
			if ((e = hydrate(m, ri, ENT_ALLOC_GAME)) >= 0)
			{
				R.st.woken++;
				(*done)++;
			}
			else
				wait = m->rt[ri].state != REC_KILLED;
		}
		if (wait)
		{
			held |= 1u << slot;
			i++;
			continue;
		}
		memmove(&R.defer[i], &R.defer[i + 1], (R.ndefer - i - 1) * sizeof(rdefer_t));
		R.ndefer--;
		if (e < 0)
			continue;	/* the target is gone (killed, removed) */
		if (d.caller_map != 0xFFFF && (k = slot_of_map(d.caller_map)) >= 0)
			cg = GID_MAKE(k, d.caller_lump);
		use_ent(e, ref_get(&d.act), ref_get(&d.caller), cg, d.use_type, d.value);
	}
}

#define CAND		16
#define CAND_RT		0x8000	/* candidate is runtime entry (index | CAND_RT) */
#define REC_SPAWNERS	4	/* pending spawner thinks a slot's spawn windows name */
#define REC_SPAWN_LEAD	1.0	/* s before its think a spawner's box is named */

/* the spawner set pump last scanned with, per slot (spawners); the
 * player's home slot of this rec_frame (-1 none) */
static uint32_t	spawn_sig[REC_SLOTS];
static int	spawn_home = -1;

/* A neighbour slot's logic records with a think due within
 * REC_SPAWN_LEAD that will create an entity (game_spawner_box: a
 * monstermaker), soonest first, at most REC_SPAWNERS: their children's
 * boxes, and a signature of the set. A child spawns, falls and moves in
 * its box the frame its maker thinks; a map opened beside the player runs
 * its makers 3000 u away, where no window named the collision (a sync
 * COLL read the frame the child fell). Prefetch only: no game state
 * changes. None in the home slot (its windows fill the collision pool
 * around the player; standing spawner boxes of the hldemo2 fight timed
 * out hldemo3's open gate on the m4 route) and none while a map's open
 * gate waits (precache_held): its candidates' windows come first. */
static int spawners(const rmap_t *m, float (*box)[6], uint32_t *sig)
{
	const rtimer_t *at[REC_SPAWNERS];
	const double due = eng_time() + REC_SPAWN_LEAD;
	float b[6];
	uint32_t h = 2166136261u;
	int n = 0, i, j, k;

	for (i = 0; i < REC_SLOTS && !R.map[i].pre_wait; i++)
		;
	for (i = i < REC_SLOTS || m->slot == spawn_home ? REC_TIMERS : 0; i < REC_TIMERS && R.ntm; i++)
	{
		const rtimer_t *tm = &R.tm[i];

		if (tm->kind != TM_REC || (int)GID_SLOT(tm->gid) != m->slot || (double)tm->t > due)
			continue;
		for (j = n; j > 0 && (at[j - 1]->t > tm->t
			|| (at[j - 1]->t == tm->t && at[j - 1]->seq > tm->seq)); j--)
			;
		if (j == REC_SPAWNERS || !game_spawner_box(tm->gid, b))
			continue;
		if (n == REC_SPAWNERS)
			n--;
		memmove(&at[j + 1], &at[j], (size_t)(n - j) * sizeof(at[0]));
		memmove(&box[j + 1], &box[j], (size_t)(n - j) * sizeof(box[0]));
		at[j] = tm;
		memcpy(box[j], b, sizeof(b));
		n++;
	}
	for (i = 0; i < n; i++)
		for (k = 0; k < 6; k++)
		{
			union { float f; uint32_t u; } v;

			v.f = box[i][k];
			h = (h ^ v.u) * 16777619u;
		}
	*sig = h ^ (uint32_t)n;
	return n;
}

static void cand_add(uint16_t *c, float *cd, int *nc, int max, uint16_t v, float d)
{
	int j;

	for (j = *nc; j > 0 && cd[j - 1] > d; j--)
		if (j < max)
		{
			c[j] = c[j - 1];
			cd[j] = cd[j - 1];
		}
	if (j < max)
	{
		c[j] = v;
		cd[j] = d;
		if (*nc < max)
			(*nc)++;
	}
}

#define REC_DROP	256.0f	/* DROP_TO_FLOOR's trace below a spawn */

/* Candidate c's spawn box (map-local): the record's bounds or a released
 * runtime entity's origin, down to where a DROP_TO_FLOOR can reach; its
 * own brush submodel (a trigger, a door), -1 none. */
static int spawn_box(const rmap_t *m, uint16_t c, float b[6])
{
	res_t r;
	int k;

	for (k = 0; k < 3; k++)
		if (c & CAND_RT)
		{
			b[k] = R.run[c & ~CAND_RT].org[k] - 16;
			b[k + 3] = R.run[c & ~CAND_RT].org[k] + 16;
		}
		else
		{
			b[k] = m->rt[c].lo[k];
			b[k + 3] = m->rt[c].hi[k];
		}
	b[2] -= REC_DROP;
	if (c & CAND_RT)
		return -1;
	r = rec_key_res(gid_of(m, c), RK_MODEL);
	return r != RES_NONE && RES_KIND(r) == RES_K_SUBMODEL && (int)RES_SUB_SLOT(r) == m->slot
		? (int)RES_SUB_NUM(r) : -1;
}

/* The slot's hydration candidates within the spawn radius of an anchor
 * (nearest first; runtime entries | CAND_RT) and its prefetch ring; their
 * spawn boxes, and the pending spawners' (sp), go to the stream, which
 * prefetches their collision cells (a Spawn traces: drop_to_floor, a
 * monster's first step). */
static void scan(rmap_t *m, const float (*loc)[3], int n, uint16_t *cand, float *cd, int *nc,
	uint16_t *ring, float *rd, int *nr, const float (*sp)[6], int nsp)
{
	const float r2 = REC_SPAWN_R * REC_SPAWN_R;
	const float o2 = (REC_SPAWN_R + REC_PREFETCH_R) * (REC_SPAWN_R + REC_PREFETCH_R);
	float box[CAND + REC_RING + REC_SPAWNERS][6];
	int sub[CAND + REC_RING + REC_SPAWNERS], a, i, nb = 0;

	*nc = *nr = 0;
	for (i = 0; i < m->npend; i++)
	{
		int ri = m->plist[i];
		float d = 1e30f;

		if (R.ntwin && twin_blocked(gid_of(m, ri)))
			continue;
		for (a = 0; a < n; a++)
		{
			float x = box_d2(&m->rt[ri], loc[a]);

			if (x < d)
				d = x;
		}
		if (d <= r2)
			cand_add(cand, cd, nc, CAND, (uint16_t)ri, d);
		else if (d <= o2)
			cand_add(ring, rd, nr, REC_RING, (uint16_t)ri, d);
	}
	/* released runtime entities of the map, by where they were released */
	for (i = 0; i < REC_RUNTIME; i++)
	{
		const rrun_t *r = &R.run[i];
		float d = 1e30f;

		if (r->state != RUN_DRY || r->map != m->map)
			continue;
		for (a = 0; a < n; a++)
		{
			float x = abs_d2(r->org, r->org, loc[a]);

			if (x < d)
				d = x;
		}
		if (d <= r2)
			cand_add(cand, cd, nc, CAND, (uint16_t)(i | CAND_RT), d);
	}
	for (i = 0; i < *nc; i++, nb++)
		sub[nb] = spawn_box(m, cand[i], box[nb]);
	for (i = 0; i < *nr; i++, nb++)
		sub[nb] = spawn_box(m, ring[i], box[nb]);
	for (i = 0; i < nsp; i++, nb++)
	{
		memcpy(box[nb], sp[i], sizeof(box[nb]));
		sub[nb] = -1;
	}
	wld_stream_spawns(m->slot, (const float (*)[6])box, sub, nb);
}

static void pump(rmap_t *m, const float (*loc)[3], int n, uint64_t t0, uint32_t *done)
{
	uint16_t cand[CAND], ring[REC_RING];
	float cd[CAND], rd[REC_RING], sp[REC_SPAWNERS][6];
	uint32_t sig;
	int nc, nr, i, nsp = spawners(m, sp, &sig);

	/* nothing changed (pending records, spawners) and the anchor stayed
	 * put since a scan that found nothing to spawn: skip the scan */
	if (m->idle && m->idle_gen == m->pgen && spawn_sig[m->slot] == sig
		&& fabsf(loc[0][0] - m->idle_at[0]) < 16 && fabsf(loc[0][1] - m->idle_at[1]) < 16
		&& fabsf(loc[0][2] - m->idle_at[2]) < 16)
		return;
	spawn_sig[m->slot] = sig;
	scan(m, loc, n, cand, cd, &nc, ring, rd, &nr, (const float (*)[6])sp, nsp);
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
		int e;

		if (cand[i] & CAND_RT)
			e = hydrate_rt(cand[i] & ~CAND_RT, ENT_ALLOC_OPTIONAL);
		else
			e = hydrate(m, cand[i], ENT_ALLOC_OPTIONAL);
		if (e < 0 && ((cand[i] & CAND_RT) ? R.run[cand[i] & ~CAND_RT].state == RUN_DRY
			: m->rt[cand[i]].state != REC_KILLED))
			break;	/* the optional reserve: stop here */
		if (e >= 0 && cd[i] <= REC_READY_R * REC_READY_R)
			R.late++;
		(*done)++;
	}
}

/* e stands on a pusher that is moving: the pusher carries it this frame
 * and wakes it again if released (push.c gather), so it stays (hldemo3's
 * func_pushable on the dish momentary_doors, 1300 u from the player). */
static int rides_moving_pusher(int e)
{
	int g = ent_phys(e)->groundentity;
	const ent_mot_t *mo;

	if (!(ent_cv(e)->flags & FL_ONGROUND) || g == ENT_NONE || !ent_is_live(g)
		|| ent_cv(g)->movetype != MOVETYPE_PUSH)
		return 0;
	mo = ent_mot(g);
	return memcmp(mo->velocity, zero3, sizeof(zero3)) || memcmp(mo->avelocity, zero3, sizeof(zero3));
}

/* One release batch of a slot every 0.1 s: live entities whose box is
 * beyond 1024 u of every anchor dehydrate, at least REC_RELEASE_BATCH
 * tries, more while the budget lasts; a refusal waits 0.5 s. 1 when a
 * batch ran. */
static int release_pass(rmap_t *m, const float (*loc)[3], int n, uint64_t t0, uint32_t *done)
{
	const float r2 = REC_RELEASE_R * REC_RELEASE_R;
	double now = eng_time();
	int e, a, tried = 0;

	if (R.sweep
		|| (now < (double)m->rel_next && (double)m->rel_next <= now + REC_RELEASE_PERIOD))
		return 0;
	m->rel_next = (float)(now + REC_RELEASE_PERIOD);
	for (e = 2; e < HL64_MAX_ENTITIES; e++)
	{
		const col_ent_t *cv;
		const float *lo, *hi;

		if (!ent_is_live(e) || ent_idc(e)->layer != m->slot)
			continue;
		cv = ent_cv(e);
		/* the linked box; an unlinked entity's origin */
		lo = cv->absmin;
		hi = cv->absmax;
		if (!memcmp(lo, zero3, sizeof(zero3)) && !memcmp(hi, zero3, sizeof(zero3)))
			lo = hi = cv->origin;
		for (a = 0; a < n; a++)
			if (abs_d2(lo, hi, loc[a]) <= r2)
				break;
		if (a < n)
			continue;
		if ((double)R.retry[e] > now && (double)R.retry[e] <= now + REC_RELEASE_RETRY)
			continue;
		if (rides_moving_pusher(e))
			continue;
		if (tried >= REC_RELEASE_BATCH && !budget_left(t0, *done))
			break;
		tried++;
		if (release_one(e, 0))
			(*done)++;
	}
	return 1;
}

/* the last rec_frame's anchors (global): rec_slot_held */
static float	held_anc[8][3];
static int	held_nanc;

int rec_slot_held(int slot)
{
	const float r2 = REC_RELEASE_R * REC_RELEASE_R;
	const wld_layer_t *L = wld_layer(slot);
	float loc[8][3];
	int e, a, k, j, n = 0;

	if (slot < 0 || slot >= REC_SLOTS)
		return 0;
	for (a = 0; a < held_nanc; a++)
		for (k = 0; k < 3; k++)
			loc[a][k] = held_anc[a][k] - (L ? L->gofs[k] : 0);
	for (e = 2; e < HL64_MAX_ENTITIES; e++)
	{
		const col_ent_t *cv;
		const float *lo, *hi;

		if (!ent_is_live(e) || ent_idc(e)->layer != slot)
			continue;
		cv = ent_cv(e);
		lo = cv->absmin;
		hi = cv->absmax;
		if (!memcmp(lo, zero3, sizeof(zero3)) && !memcmp(hi, zero3, sizeof(zero3)))
			lo = hi = cv->origin;
		for (a = 0; a < held_nanc; a++)
			if (abs_d2(lo, hi, loc[a]) <= r2)
				break;
		if (a < held_nanc)
			n++;
	}
	for (j = 0; j < REC_HOLDERS; j++)
		if (R.hold[j].n != NAME_NONE && NAME_SLOT(R.hold[j].n) == slot
			&& (e = ent_of(R.hold[j].h)) >= 0 && ent_idc(e)->layer != slot)
			n++;
	return n;
}

#ifdef HL64_TEST
void rec_test_class_sweep(int slot)
{
	R.sweep = (uint8_t)(slot >= 0 && slot < REC_SLOTS ? slot + 1 : 0);
	log_printf("rec: class sweep %s: no distance release, pump only in slot %d (test)\n",
		R.sweep ? "on" : "off", slot);
}
#endif

/* the open slot whose world bounds hold the global point, -1 none */
static int home_slot(const float g[3])
{
	int s, k;

	for (s = 0; s < REC_SLOTS; s++)
	{
		const wld_layer_t *L;

		if (!R.map[s].open || !(L = wld_layer(s)) || !L->dir->nsub)
			continue;
		for (k = 0; k < 3; k++)
			if (g[k] < L->models[0].mins[k] + L->gofs[k] || g[k] > L->models[0].maxs[k] + L->gofs[k])
				break;
		if (k == 3)
			return s;
	}
	return -1;
}

/* A newly opened map's records wait for its level-load precache (the
 * interest game_map_opened declared; studio infos land asynchronously),
 * as GoldSrc precaches before the first spawn: a Spawn would otherwise
 * read its model's info synchronously; and for the collision cells of
 * the candidates in the spawn radius (scan names them), whose Spawn
 * traces would read them synchronously. This is the map's one open
 * gate (after its layer and HLE reads): released on residency, bounded;
 * an expiry is logged and counted (stats `rec: expired=`, the M4 gate
 * fails on it). Test builds publish I/O completions at frame boundaries
 * (engine/io, architecture.md 15), so the release frame is the same on
 * the host and the cart. */
#define REC_PRECACHE_FRAMES	60
static int precache_held(rmap_t *m, const float (*loc)[3], int n)
{
	uint16_t cand[CAND], ring[REC_RING];
	float cd[CAND], rd[REC_RING], b[6];
	int ni, nc = 0, nr, i, nw = 0;

	if (!m->pre_wait)
		return 0;
	ni = studio_infos_pending();
	if (n > 0)
	{
		scan(m, loc, n, cand, cd, &nc, ring, rd, &nr, NULL, 0);
		for (i = 0; i < nc; i++)
		{
			int sub = spawn_box(m, cand[i], b);

			nw += !wld_spawn_resident(m->slot, b, sub);
		}
	}
	if ((ni || nw) && m->pre_wait < REC_PRECACHE_FRAMES)
	{
		m->pre_wait++;
		wld_stream_spawn_gate(m->slot, nw != 0);
		return 1;
	}
	wld_stream_spawn_gate(m->slot, 0);
	if (ni || nw)
		R.st.open_expired++;
	log_printf("rec: %s precache %s after %u frames (%d infos, %d spawn windows pending)\n",
		wld_map_name(m->map), ni || nw ? "timed out" : "landed", (unsigned)(m->pre_wait - 1),
		ni, nw);
	m->pre_wait = 0;
	return 0;
}

void rec_frame(const float (*anchors)[3], int nanchors)
{
	uint64_t t0 = sys_time_us();
	uint32_t done = 0, us;
	float loc[REC_SLOTS][8][3];
	int s, a, k, home = -1, batch = 0, n = nanchors > 8 ? 8 : nanchors;
	uint32_t live = 0;	/* open slots with their layer (pumped) */

	if (!R.map)
		return;
	held_nanc = n > 0 ? n : 0;
	if (held_nanc)
		memcpy(held_anc, anchors, (size_t)held_nanc * sizeof(held_anc[0]));
	for (s = 0; s < REC_SLOTS; s++)
	{
		rmap_t *m = &R.map[s];
		const wld_layer_t *L;
		int i;

		advance(m);
		if (!m->open || !(L = wld_layer(s)))
			continue;
		for (a = 0; a < n; a++)
			for (k = 0; k < 3; k++)
				loc[s][a][k] = anchors[a][k] - L->gofs[k];
		if (precache_held(m, (const float (*)[3])loc[s], n))
			continue;
		/* a newly opened map's logic records: all at once (no entities) */
		if (m->logic_pending)
		{
			for (i = 0; i < m->h->numrecs; i++)
				spawn_logic(m, i);
			m->logic_pending = 0;
		}
		live |= 1u << s;
	}
	if (R.sweep)
		live &= 1u << (R.sweep - 1);	/* test class sweep: one map */
	deliver_deferred(t0, &done);
	if (n > 0)
	{
		home = home_slot(anchors[0]);
		spawn_home = home;
		/* 6.4 budget order: neighbour release, arrival pump, home pump,
		 * then the home map's own far entities (lowest) */
		for (s = 0; s < REC_SLOTS; s++)
			if (s != home && (live & 1u << s))
				batch |= release_pass(&R.map[s], (const float (*)[3])loc[s], n, t0, &done);
		for (s = 0; s < REC_SLOTS; s++)
			if (s != home && (live & 1u << s))
				pump(&R.map[s], (const float (*)[3])loc[s], n, t0, &done);
		if (home >= 0 && (live & 1u << home))
		{
			pump(&R.map[home], (const float (*)[3])loc[home], n, t0, &done);
			batch |= release_pass(&R.map[home], (const float (*)[3])loc[home], n, t0, &done);
		}
	}
	us = (uint32_t)(sys_time_us() - t0) + R.frame_us;
	R.us_sum += us;
	if (us > R.us_max)
		R.us_max = us;
	if (batch)
	{
		R.rel_frames++;
		if (us > R.rel_us_max)
			R.rel_us_max = us;
	}
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
	if ((R.ntwin && twin_blocked(gid_of(m, ri))) || hydrate(m, ri, ENT_ALLOC_OPTIONAL) < 0)
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

		if (!m->open || m->pre_wait || !m->npend || !wld_layer(s))
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
	rec_map_finish(slot);
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

/* ---- level state store (6.4) ------------------------------------------------------- */

static uint8_t *store_find(uint16_t map, uint16_t lump, uint16_t rtid)
{
	uint32_t at = 0;

	while (at < R.store_used)
	{
		rstore_t *e = (rstore_t *)(R.store + at);

		if (e->map == map && e->lump == lump && e->rtid == rtid)
			return R.store + at;
		at += store_need(e->len);
	}
	return NULL;
}

static void store_del(uint8_t *p)
{
	uint32_t n = store_need(((rstore_t *)p)->len);

	memmove(p, p + n, R.store_used - (uint32_t)(p - R.store) - n);
	R.store_used -= n;
}

static void store_drop(uint16_t map, uint16_t lump, uint16_t rtid)
{
	uint8_t *p = store_find(map, lump, rtid);

	if (p)
		store_del(p);
}

static void store_purge_map(uint16_t map)
{
	uint32_t at = 0;

	while (at < R.store_used)
	{
		rstore_t *e = (rstore_t *)(R.store + at);

		if (e->map == map)
			store_del((uint8_t *)e);
		else
			at += store_need(e->len);
	}
}

int rec_store_put(gid_t16 gid, uint16_t rtid, const void *delta, uint32_t n)
{
	rmap_t *m;
	int ri = rec_index(gid, &m);
	rstore_t e;

	if (ri < 0 || n > 0xFFFF)
		return -1;
	store_drop(m->map, (uint16_t)GID_LUMP(gid), rtid);
	if (store_need(n) > R.store_cap - R.store_used)
		return -1;
	memset(&e, 0, sizeof(e));
	e.map = m->map;
	e.lump = (uint16_t)GID_LUMP(gid);
	e.rtid = rtid;
	e.len = (uint16_t)n;
	memcpy(e.lo, m->rt[ri].lo, sizeof(e.lo));
	memcpy(e.hi, m->rt[ri].hi, sizeof(e.hi));
	memcpy(R.store + R.store_used, &e, sizeof(e));
	memcpy(R.store + R.store_used + sizeof(e), delta, n);
	R.store_used += store_need(n);
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
	R.run = (rrun_t *)p;
	p += REC_RUNTIME * sizeof(rrun_t);
	R.twin = (rtwin_t *)p;
	p += REC_TWINS * sizeof(rtwin_t);
	R.fmap = (rfmap_t *)p;
	p += REC_FROZEN_MAPS * sizeof(rfmap_t);
	R.retry = (float *)p;
	p += HL64_MAX_ENTITIES * sizeof(float);
	p = (uint8_t *)(((uintptr_t)p + 15) & ~(uintptr_t)15);
	fixed = (uint32_t)(p - (uint8_t *)mem_region(MEM_HLE_BLOBS));
	if (fixed != REC_TABLE_BYTES || fixed >= total)
		sys_error("rec: HLE region tables %lu B (region %lu B)", (unsigned long)fixed,
			(unsigned long)total);
	memset(R.map, 0, REC_SLOTS * sizeof(rmap_t));
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
	{
		open_abort(&R.map[s]);
		R.map[s].rel_next = 0;
	}
	for (j = 0; j < REC_FROZEN_MAPS; j++)
		R.fmap[j].map = WLD_NO_MAP;
	memset(R.tm, 0, REC_TIMERS * sizeof(rtimer_t));
	memset(R.run, 0, REC_RUNTIME * sizeof(rrun_t));
	memset(R.retry, 0, HL64_MAX_ENTITIES * sizeof(float));
	for (j = 0; j < REC_HOLDERS; j++)
		R.hold[j].n = NAME_NONE;
	R.ntm = R.nfrozen = R.ndefer = R.ntwin = R.sync_wakes = R.fage = 0;
	R.next_rtid = 0;
	R.seq = 0;
	R.caller = R.self = R.running = GID_NONE;
	R.store_used = 0;
	memset(&R.st, 0, sizeof(R.st));
	R.us_sum = 0;
	R.us_max = R.frames = R.frame_us = R.rel_us_max = R.rel_frames = 0;
	R.late = R.close_dropped = R.store_full = 0;
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
	for (i = 0; R.run && i < REC_RUNTIME; i++)
		R.st.dry += R.run[i].state == RUN_DRY;
	if (R.st.live > R.live_peak)
		R.live_peak = R.st.live;
	R.st.timers = R.ntm;
	R.st.store_used = R.store_used;
	R.st.store_peak = R.store_peak;
	*s = R.st;
	if (reset)
	{
		R.st.hydrated = R.st.released = R.st.woken = 0;
		R.st.budget_us = R.st.budget_max_us = 0;
	}
}

void rec_mem_use(uint32_t *hle_bytes, uint32_t *store_bytes)
{
	uint32_t n = REC_TABLE_BYTES;
	int s;

	for (s = 0; R.map && s < REC_SLOTS; s++)
		if (R.map[s].phase >= PH_CHUNK)
			n += R.map[s].block_len;
	for (s = 0; R.fmap && s < REC_FROZEN_MAPS; s++)
		if (R.fmap[s].map != WLD_NO_MAP)
			n += R.fmap[s].block_len;
	if (hle_bytes)
		*hle_bytes = n;
	if (store_bytes)
		*store_bytes = R.store_used;
}

void rec_prof_line(void)
{
	rec_stats_t s;
	unsigned long n = R.frames ? R.frames : 1;

	rec_stats(&s, 0);
	log_printf("prof rec_us avg=%lu max=%lu rel_max=%lu rel_frames=%lu live=%lu pending=%lu "
		"dry=%lu hydrated=%lu released=%lu woken=%lu late=%lu timers=%lu timers_peak=%lu "
		"deferred=%lu\n",
		(unsigned long)(R.us_sum / n), (unsigned long)R.us_max, (unsigned long)R.rel_us_max,
		(unsigned long)R.rel_frames, (unsigned long)s.live, (unsigned long)s.pending,
		(unsigned long)s.dry, (unsigned long)s.hydrated, (unsigned long)s.released,
		(unsigned long)s.woken, (unsigned long)R.late, (unsigned long)s.timers,
		(unsigned long)s.timers_peak, (unsigned long)R.ndefer);
	rec_stats(&s, 1);
	R.us_sum = 0;
	R.us_max = 0;
	R.rel_us_max = 0;
	R.rel_frames = 0;
	R.frames = 0;
}

void rec_mem_line(void)
{
	uint32_t blobs = 0, runtime = 0, frozen = 0;
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
	for (s = 0; R.fmap && s < REC_FROZEN_MAPS; s++)
		if (R.fmap[s].map != WLD_NO_MAP)
		{
			frozen += R.fmap[s].block_len;
			log_printf("mem rec: frozen %s runtime=%lu records=%u logic=%u\n",
				wld_map_name(R.fmap[s].map), (unsigned long)R.fmap[s].block_len,
				R.fmap[s].nrecs, R.fmap[s].nlogic);
		}
	log_printf("mem rec: blobs=%lu runtime=%lu frozen=%lu heap=%lu tables=%lu timers_peak=%lu "
		"store=%lu/%lu peak=%lu live_peak=%lu released=%lu close_dropped=%lu store_full=%lu\n",
		(unsigned long)blobs, (unsigned long)runtime, (unsigned long)frozen,
		(unsigned long)R.heap_len,
		(unsigned long)(mem_region_bytes(MEM_HLE_BLOBS) - R.heap_len),
		(unsigned long)R.st.timers_peak, (unsigned long)R.store_used,
		(unsigned long)R.store_cap, (unsigned long)R.store_peak, (unsigned long)R.live_peak,
		(unsigned long)R.st.released_total, (unsigned long)R.close_dropped,
		(unsigned long)R.store_full);
}
