/* SPDX-License-Identifier: MIT */
/*
 * ent.c -- entity components, slots and lifetime, the setters that keep
 * links and active sets current, frames and the entity searches
 * (include/hl64/ent.h, include/hl64/grid.h; docs/architecture.md 6.2, 6.3,
 * 7.1, 8; docs/specs/think-and-time.md 6).
 *
 * Storage: structure of arrays in the boot carve's components region (the
 * six components, then the grid nodes, the membership pool, touch ranks,
 * free times and contents flags), every array HL64_MAX_ENTITIES long.
 * Allocation (K-alloc): the lowest free slot >= 2 freed before time 2.0 or
 * at least 0.5 s ago, else the next never-used slot; freeing bumps the
 * serial and records the free time (f32 frame time).
 */
#include <math.h>
#include <stddef.h>
#include <string.h>

#include "hl64/ent.h"
#include "hl64/rec.h"
#include "hl64/snd.h"
#include "engine/core/log.h"
#include "engine/core/sys.h"
#include "engine/ent/ent.h"
#include "engine/ent/rec_int.h"
#include "engine/mem/carve.h"
#include "engine/phys/player.h"
#include "engine/phys/phys.h"
#include "engine/save/sv.h"
#include "engine/save/svmod.h"
#include "engine/world/wld.h"

ent_comp_t	ent_c;
ent_slots_t	ent_s;

static int frame_layer = -1;
static const float zero3[3];
static uint8_t warned_late_reuse;

/* Class names for messages (classes.def line order = cls_t). */
static const char *const cls_names[] = {
#define CLASS(id, name) name,
#include "game/defs/classes.def"
#undef CLASS
};
#define NCLS ((int)(sizeof(cls_names) / sizeof(cls_names[0])))

const char *ent_class_name(int e)
{
	cls_t c;

	if (e < 0 || e >= HL64_MAX_ENTITIES)
		return "?";
	c = ent_c.id[e].cls;
	return c < NCLS ? cls_names[c] : c == CLS_NONE ? "(no class)" : "?";
}

/* ---- regions ----------------------------------------------------------------- */

static uint8_t *take(uint8_t **p, uint8_t *end, uint32_t n)
{
	uint8_t *r = *p;

	n = (n + 15u) & ~15u;
	if ((uint32_t)(end - r) < n)
		sys_error("ent: components region too small");
	*p = r + n;
	return r;
}

void ent_init(void)
{
	uint8_t *p = mem_region(MEM_COMPONENTS), *start = p;
	uint8_t *end = p + mem_region_bytes(MEM_COMPONENTS);
	const uint32_t n = HL64_MAX_ENTITIES;

	ent_c.cv = (col_ent_t *)take(&p, end, n * sizeof(col_ent_t));
	ent_c.mot = (ent_mot_t *)take(&p, end, n * sizeof(ent_mot_t));
	ent_c.phys = (ent_phys_t *)take(&p, end, n * sizeof(ent_phys_t));
	ent_c.tm = (ent_tm_t *)take(&p, end, n * sizeof(ent_tm_t));
	ent_c.rnd = (ent_rnd_t *)take(&p, end, n * sizeof(ent_rnd_t));
	ent_c.id = (ent_id_t *)take(&p, end, n * sizeof(ent_id_t));
	ent_s.node = (ent_node_t *)take(&p, end, n * sizeof(ent_node_t));
	ent_s.cell = (ent_cell_t *)take(&p, end, ENT_CELLS * sizeof(ent_cell_t));
	ent_s.rank = (uint32_t *)take(&p, end, n * 4);
	ent_s.freetime = (float *)take(&p, end, n * 4);
	ent_s.contents = take(&p, end, n);
	ent_s.region_bytes = (uint32_t)(p - start);
	memset(start, 0, (size_t)(p - start));
	ent_sets_init();
	ent_grid_init();
	ent_priv_init();
#ifdef HL64_TEST
	ent_grid_bench_init();
#endif
	ent_reset();
}

/* Free state of slot i: components zeroed, references none, serial and
 * free time kept. */
static void clear_slot(int i)
{
	col_ent_t *v = &ent_c.cv[i];
	ent_id_t *id = &ent_c.id[i];
	uint16_t serial = id->serial;
	int k;

	memset(v, 0, sizeof(*v));
	v->index = (uint16_t)i;
	v->owner = COL_NONE;
	v->model = COL_NONE;
	v->studio = COL_NONE;
	memset(&ent_c.mot[i], 0, sizeof(ent_mot_t));
	memset(&ent_c.phys[i], 0, sizeof(ent_phys_t));
	ent_c.phys[i].groundentity = ENT_NONE;
	ent_c.phys[i].aiment = ENT_NONE;
	memset(&ent_c.tm[i], 0, sizeof(ent_tm_t));
	memset(&ent_c.rnd[i], 0, sizeof(ent_rnd_t));
	for (k = 0; k < 4; k++)
		ent_c.rnd[i].sectors[k] = ENT_NONE;
	memset(id, 0, sizeof(*id));
	id->serial = serial;
	id->state = ENT_ST_FREE;
	id->layer = -1;
	id->cls = CLS_NONE;
	id->gid = GID_NONE;
	id->targetname = NAME_NONE;
	ent_s.node[i].kind = 0;
	ent_s.node[i].cells = ENT_UNLINKED;
	ent_s.rank[i] = ENT_RANK_RUNTIME;
	ent_s.contents[i] = 0;
}

void ent_reset(void)
{
	int i;

	for (i = 0; i < HL64_MAX_ENTITIES; i++)
	{
		if (ent_s.contents[i])
			col_contents_remove(&ent_c.cv[i]);
		clear_slot(i);
		ent_s.freetime[i] = 0;
	}
	ent_grid_clear();
	ent_sets_clear();
	ent_priv_reset();
	ent_s.seq = 0;
	ent_s.top = 0;
	ent_s.live = 0;
	ent_s.live_peak = 0;
	frame_layer = -1;
}

/* ---- lifetime ------------------------------------------------------------------ */

static int create_at(int i)
{
	clear_slot(i);
	ent_c.id[i].state = ENT_ST_LIVE;
	ent_c.id[i].layer = (int8_t)frame_layer;
	ent_s.rank[i] = ENT_RANK_RUNTIME | (ent_s.seq++ & 0x7FFFFFFFu);
	if (i >= ent_s.top)
		ent_s.top = (uint16_t)(i + 1);
	if (++ent_s.live > ent_s.live_peak)
		ent_s.live_peak = ent_s.live;
	ent_sets_update(i);
	return i;
}

/* K-alloc slot rule (think-and-time.md 6) at the frame's time. */
static int pick_slot(void)
{
	double t = eng_clock_time();
	int i;

	for (i = ENT_PLAYER + 1; i < ent_s.top; i++)
		if (ent_c.id[i].state == ENT_ST_FREE
			&& (ent_s.freetime[i] < 2.0f || t - (double)ent_s.freetime[i] >= 0.5))
			return i;
	if (ent_s.top < HL64_MAX_ENTITIES)
		return ent_s.top < ENT_PLAYER + 1 ? ENT_PLAYER + 1 : ent_s.top;
	/* every slot used once: a recently freed one rather than none */
	for (i = ENT_PLAYER + 1; i < HL64_MAX_ENTITIES; i++)
		if (ent_c.id[i].state == ENT_ST_FREE)
		{
			if (!warned_late_reuse)
				log_printf("ent: slot %d reused within 0.5 s (all %d slots used)\n",
					i, HL64_MAX_ENTITIES);
			warned_late_reuse = 1;
			return i;
		}
	return -1;
}

static void census(void)
{
	uint16_t count[NCLS + 1];
	int i, shown;

	memset(count, 0, sizeof(count));
	for (i = 0; i < HL64_MAX_ENTITIES; i++)
		if (ent_is_live(i))
			count[ent_c.id[i].cls < NCLS ? ent_c.id[i].cls : NCLS]++;
	log_printf("ent: %d slots live; by class:\n", (int)ent_s.live);
	for (shown = 0; shown < 12; shown++)
	{
		int best = -1;

		for (i = 0; i <= NCLS; i++)
			if (count[i] && (best < 0 || count[i] > count[best]))
				best = i;
		if (best < 0)
			break;
		log_printf("  %4u %s\n", count[best], best < NCLS ? cls_names[best] : "(other)");
		count[best] = 0;
	}
}

static int create(int reserve, int fatal)
{
	int nfree = HL64_MAX_ENTITIES - ent_s.live, i;

	if ((reserve == ENT_ALLOC_OPTIONAL && nfree < 48)
		|| (reserve == ENT_ALLOC_TRANSIENT && nfree < 32))
		return -1;
	i = pick_slot();
	if (i < 0)
	{
		if (!fatal)
			return -1;
		census();
		sys_error("ent: all %d entity slots in use", HL64_MAX_ENTITIES);
	}
	return create_at(i);
}

int ent_create(int reserve)
{
	return create(reserve, 1);
}

void ent_destroy(int e)
{
	if (!ent_is_live(e))
		return;
	ent_unlink(&ent_c.cv[e]);
	ent_sets_remove(e);
	ent_sets_ground_lost(e);
	ent_priv_free(e);
	ent_s.freetime[e] = (float)eng_clock_time();
	ent_c.id[e].serial++;
	snd_entity_freed(e);
	clear_slot(e);
	ent_s.live--;
}

void ent_remove(int e)
{
	if (!ent_is_live(e))
		return;
	ent_c.cv[e].flags |= FL_KILLME;
	ent_sets_update(e);
}

ent_h ent_handle_of(int e)
{
	return (ent_h)(uint16_t)e | (ent_h)ent_c.id[e].serial << 16;
}

int ent_of(ent_h h)
{
	int i = (int)(h & 0xFFFFu);

	return ent_is_live(i) && ent_c.id[i].serial == (uint16_t)(h >> 16) ? i : -1;
}

/* ---- the M2 slot API (grid.h) ---------------------------------------------------- */

col_ent_t *ent_alloc(void)
{
	int i = create(ENT_ALLOC_GAME, 0);

	return i < 0 ? NULL : &ent_c.cv[i];
}

col_ent_t *ent_alloc_at(int i)
{
	if (i < 0 || i >= HL64_MAX_ENTITIES || ent_c.id[i].state != ENT_ST_FREE)
		return NULL;
	return &ent_c.cv[create_at(i)];
}

void ent_free(col_ent_t *e)
{
	ent_destroy(e->index);
}

col_ent_t *ent_get(int i)
{
	return ent_is_live(i) ? &ent_c.cv[i] : NULL;
}

ent_h ent_handle(const col_ent_t *e)
{
	return ent_handle_of(e->index);
}

col_ent_t *ent_resolve(ent_h h)
{
	int i = ent_of(h);

	return i < 0 ? NULL : &ent_c.cv[i];
}

void ent_set_touch(col_ent_t *e, uint16_t fn)
{
	ent_c.tm[e->index].touch = fn;
}

uint16_t ent_touch_fn(const col_ent_t *e)
{
	return ent_c.tm[e->index].touch;
}

void ent_set_rank(col_ent_t *e, int map_slot, uint16_t rank)
{
	ent_s.rank[e->index] = (uint32_t)map_slot << 16 | rank;
}

/* ---- frames (architecture.md 8) ----------------------------------------------------- */

int eng_frame_layer(void)
{
	return frame_layer;
}

void eng_set_frame_layer(int layer)
{
	frame_layer = layer;
}

const float *eng_layer_origin(int layer)
{
	const wld_layer_t *L;

	if (layer < 0 || layer >= HL64_WORLD_LAYERS || !(L = wld_layer(layer)))
		return zero3;
	return L->gofs;
}

void ent_layer_delta(int layer, int frame, float d[3])
{
	const float *a = eng_layer_origin(layer), *b = eng_layer_origin(frame);
	int k;

	for (k = 0; k < 3; k++)
		d[k] = a[k] - b[k];
}

int ent_frame_delta(int e, float d[3])
{
	int layer = ent_c.id[e].layer;

	if (layer == frame_layer)
	{
		d[0] = d[1] = d[2] = 0;
		return 0;
	}
	ent_layer_delta(layer, frame_layer, d);
	return d[0] != 0 || d[1] != 0 || d[2] != 0;
}

/* ---- setters (6.3) -------------------------------------------------------------- */

static void check_pair(int e)
{
	const col_ent_t *v = &ent_c.cv[e];

	if (v->solid == COL_SOLID_BSP && v->movetype != MOVETYPE_PUSH)
		sys_error("ent %d (%s): SOLID_BSP without MOVETYPE_PUSH (movetype %d)", e,
			ent_class_name(e), v->movetype);
}

void ent_set_origin(int e, const float org[3])
{
	memcpy(ent_c.cv[e].origin, org, sizeof(ent_c.cv[e].origin));
	ent_link(&ent_c.cv[e]);
	ent_sets_update(e);
}

void ent_set_size(int e, const float mins[3], const float maxs[3])
{
	memcpy(ent_c.cv[e].mins, mins, sizeof(ent_c.cv[e].mins));
	memcpy(ent_c.cv[e].maxs, maxs, sizeof(ent_c.cv[e].maxs));
	ent_link(&ent_c.cv[e]);
}

void ent_set_model(int e, res_t m)
{
	col_ent_t *v = &ent_c.cv[e];

	ent_c.rnd[e].model = m;
	v->model = COL_NONE;
	v->studio = COL_NONE;
	if (RES_KIND(m) == RES_K_SUBMODEL)
	{
		float mins[3], maxs[3];

		v->model = COL_MODEL(RES_SUB_SLOT(m), RES_SUB_NUM(m));
		if (!col_model_bounds(v->model, mins, maxs))
			sys_error("ent %d (%s): brush model %d:%d not open", e, ent_class_name(e),
				RES_SUB_SLOT(m), RES_SUB_NUM(m));
		ent_set_size(e, mins, maxs);
		return;
	}
	if (RES_KIND(m) == RES_K_MODEL)
		v->studio = m;
	/* studio models, sprites and none: a point until the game sets the
	 * size (HLSDK spawns follow SET_MODEL with UTIL_SetSize) */
	ent_set_size(e, zero3, zero3);
}

void ent_set_velocity(int e, const float v[3])
{
	memcpy(ent_c.mot[e].velocity, v, sizeof(ent_c.mot[e].velocity));
	ent_sets_update(e);
}

void ent_set_avelocity(int e, const float v[3])
{
	memcpy(ent_c.mot[e].avelocity, v, sizeof(ent_c.mot[e].avelocity));
	ent_sets_update(e);
}

void ent_set_basevelocity(int e, const float v[3])
{
	memcpy(ent_c.mot[e].basevelocity, v, sizeof(ent_c.mot[e].basevelocity));
	ent_sets_update(e);
}

void ent_set_flags(int e, uint32_t flags)
{
	ent_c.cv[e].flags = flags;
	ent_sets_update(e);
}

static int valid_movetype(int mt)
{
	return mt == MOVETYPE_NONE || (mt >= MOVETYPE_WALK && mt <= MOVETYPE_PUSHSTEP);
}

/* Movetypes whose GoldSrc step runs the non-player water check
 * (physics-movetypes.md 5). */
static int water_checked(int mt)
{
	return mt == MOVETYPE_TOSS || mt == MOVETYPE_BOUNCE || mt == MOVETYPE_FLY
		|| mt == MOVETYPE_FLYMISSILE || mt == MOVETYPE_BOUNCEMISSILE
		|| mt == MOVETYPE_STEP || mt == MOVETYPE_PUSHSTEP;
}

void ent_set_movetype(int e, int movetype)
{
	if (!valid_movetype(movetype))
		sys_error("ent %d (%s): bad movetype %d", e, ent_class_name(e), movetype);
	/* GoldSrc's first step of such an entity sets watertype EMPTY even
	 * when it rests, and a resting entity is never stepped here: start it
	 * EMPTY [probe:measured P-push, P-rotate, K-pusher: resting crates 0 ->
	 * -1 at frame 1]; other movetypes keep 0 [probe:measured K-think] */
	if (water_checked(movetype) && !water_checked(ent_c.cv[e].movetype)
		&& ent_c.phys[e].watertype == 0)
		ent_c.phys[e].watertype = COL_EMPTY;
	ent_c.cv[e].movetype = (uint8_t)movetype;
	check_pair(e);
	ent_sets_update(e);
}

void ent_set_solid(int e, int solid)
{
	ent_c.cv[e].solid = (uint8_t)solid;
	check_pair(e);
	ent_link(&ent_c.cv[e]);
}

void ent_set_ground(int e, int ground)
{
	ent_c.phys[e].groundentity = ground < 0 ? ENT_NONE : (uint16_t)ground;
	ent_sets_update(e);
}

void ent_set_aiment(int e, int aiment)
{
	ent_c.phys[e].aiment = aiment < 0 ? ENT_NONE : (uint16_t)aiment;
	ent_sets_update(e);
}

void ent_think_at(int e, float t)
{
	ent_c.tm[e].nextthink = t > 0 ? t : 0;
	ent_sets_update(e);
}

void ent_motion_changed(int e)
{
	ent_sets_update(e);
}

void ent_relink(int e, int touch)
{
	if (touch)
		phy_link_touch(&ent_c.cv[e]);
	else
		ent_link(&ent_c.cv[e]);
}

/* ---- searches (7.1) ------------------------------------------------------------- */

/* Squared distance from p to [lo + d, hi + d]. */
static float box_dist2(const float lo[3], const float hi[3], const float d[3], const float p[3])
{
	float s = 0;
	int k;

	for (k = 0; k < 3; k++)
	{
		float a = lo[k] + d[k], b = hi[k] + d[k];
		float x = p[k] < a ? a - p[k] : p[k] > b ? p[k] - b : 0;

		s += x * x;
	}
	return s;
}

int ent_next_in_sphere(int after, const float org[3], float r)
{
	int i = after < 0 ? 1 : after + 1;

	for (; i < ent_s.top; i++)
	{
		float d[3];

		if (!ent_is_live(i))
			continue;
		ent_frame_delta(i, d);
		if (box_dist2(ent_c.cv[i].absmin, ent_c.cv[i].absmax, d, org) <= r * r)
			return i;
	}
	return -1;
}

int ent_in_box(const float mins[3], const float maxs[3], uint32_t flagmask, uint16_t *out, int max)
{
	int i, n = 0, k;

	/* the linked slots in index order (a word of the set at a time) */
	for (i = ent_sets_next_linked(1); i >= 0 && i < ent_s.top && n < max;
		i = ent_sets_next_linked(i + 1))
	{
		const col_ent_t *v = &ent_c.cv[i];
		float d[3];

		if (flagmask && !(v->flags & flagmask))
			continue;
		if (!ent_is_live(i))
			continue;
		ent_frame_delta(i, d);
		for (k = 0; k < 3; k++)
			if (v->absmin[k] + d[k] > maxs[k] || v->absmax[k] + d[k] < mins[k])
				break;
		if (k == 3)
			out[n++] = (uint16_t)i;
	}
	return n;
}

int ent_next_class(int after, cls_t cls, int slot)
{
	int i = after < 0 ? 0 : after + 1;

	for (; i < ent_s.top; i++)
	{
		const ent_id_t *id = &ent_c.id[i];

		if (id->state != ENT_ST_LIVE || id->cls != cls)
			continue;
		if (slot >= 0 && (id->gid != GID_NONE ? GID_SLOT(id->gid) : id->layer) != slot)
			continue;
		return i;
	}
	return -1;
}

/* ---- PVS (architecture.md 5.5: the exact leaf row, engine/world's cache) -------- */

/* Is map-local point p (layer's frame) in the PVS of map-local eye? */
static int layer_pvs(int layer, const float eye[3], const float p[3])
{
	wld_layer_t *L;
	const uint8_t *row;
	int le, lp, visleafs;

	if (layer < 0 || layer >= HL64_WORLD_LAYERS || !(L = wld_layer(layer)))
		return 1;
	le = wld_point_leaf(L, eye);
	lp = wld_point_leaf(L, p);
	if (lp <= 0)
		return 0;
	row = wld_pvs_row(L, le, &visleafs);
	if (!row || lp > visleafs)
		return 1;
	return (row[(lp - 1) >> 3] >> ((lp - 1) & 7)) & 1;
}

int ent_client_in_pvs(int e)
{
	ent_player_t *pl;
	float eye[3], p[3], d[3];
	int k, layer;

	if (!ent_is_live(ENT_PLAYER) || !ent_is_live(e))
		return 0;
	pl = phy_player();
	layer = ent_c.id[ENT_PLAYER].layer;
	/* e's origin in the player's frame */
	ent_layer_delta(ent_c.id[e].layer, layer, d);
	for (k = 0; k < 3; k++)
	{
		eye[k] = pl->origin[k] + pl->view_ofs[k];
		p[k] = ent_c.cv[e].origin[k] + d[k];
	}
	return layer_pvs(layer, eye, p) ? ENT_PLAYER : 0;
}

int ent_point_in_pvs(int viewer, const float p[3])
{
	const float *o;
	float eye[3];
	int k;

	if (!ent_is_live(viewer))
		return 0;
	if (viewer == ENT_PLAYER)
	{
		ent_player_t *pl = phy_player();

		for (k = 0; k < 3; k++)
			eye[k] = pl->origin[k] + pl->view_ofs[k];
		return layer_pvs(ent_c.id[viewer].layer, eye, p);
	}
	/* monsters: the origin (their view_ofs is game state) */
	o = ent_c.cv[viewer].origin;
	return layer_pvs(ent_c.id[viewer].layer, o, p);
}

/* ---- memory report ------------------------------------------------------------- */

void ent_mem_log(void)
{
	ent_priv_stats_t ps;
	const uint32_t per = sizeof(col_ent_t) + sizeof(ent_mot_t) + sizeof(ent_phys_t)
		+ sizeof(ent_tm_t) + sizeof(ent_rnd_t) + sizeof(ent_id_t);

	ent_priv_stats(&ps);
	log_printf("mem: ent components %lu B of %lu (%d slots x %lu B + grid / ranks %lu B) "
		"live %u peak %u\n",
		(unsigned long)ent_s.region_bytes, (unsigned long)mem_region_bytes(MEM_COMPONENTS),
		HL64_MAX_ENTITIES, (unsigned long)per,
		(unsigned long)(ent_s.region_bytes - per * HL64_MAX_ENTITIES),
		(unsigned)ent_s.live, (unsigned)ent_s.live_peak);
	log_printf("mem: ent private %lu B peak %lu B, pages %lu peak %lu of %lu, refused %lu; "
		"blocks 32:%lu/%lu 64:%lu/%lu 128:%lu/%lu 256:%lu/%lu 512:%lu/%lu 1024:%lu/%lu\n",
		(unsigned long)ps.bytes_live, (unsigned long)ps.bytes_peak,
		(unsigned long)ps.pages, (unsigned long)ps.pages_peak, (unsigned long)ps.pages_total,
		(unsigned long)ps.refused,
		(unsigned long)ps.live[0], (unsigned long)ps.peak[0], (unsigned long)ps.live[1],
		(unsigned long)ps.peak[1], (unsigned long)ps.live[2], (unsigned long)ps.peak[2],
		(unsigned long)ps.live[3], (unsigned long)ps.peak[3], (unsigned long)ps.live[4],
		(unsigned long)ps.peak[4], (unsigned long)ps.live[5], (unsigned long)ps.peak[5]);
	log_printf("mem: ent sets heap %d movers %d (think/grid region %lu B)\n",
		ent_heap_count(), ent_movers_count(), (unsigned long)ent_sets_region_bytes());
}

/* ---- saved games (architecture.md 14) ------------------------------------------- */

/* A slot as the save sees it: the components, the touch rank and free
 * time, the contents flag and the grid node without its query stamp (a
 * load zeroes the stamps: every stamp a run holds is below the next
 * query's, so a query visits the same nodes either way; the box only
 * when the node is in the grid -- ent_link writes it before it is read).
 * A live slot's free time is not kept (create reads it only for free
 * slots, ent_free writes it): 0. Its memberships go with the grid
 * (ent_grid_save). */
typedef struct {
	col_ent_t	cv;
	ent_mot_t	mot;
	ent_phys_t	phys;
	ent_tm_t	tm;
	ent_rnd_t	rnd;
	ent_id_t	id;
	uint32_t	rank;
	float		freetime;
	uint8_t		kind, contents;
	uint16_t	cells;
	int16_t		lo[3], hi[3];
} srow_t;
_Static_assert(sizeof(srow_t) == sizeof(col_ent_t) + sizeof(ent_mot_t) + sizeof(ent_phys_t)
	+ sizeof(ent_tm_t) + sizeof(ent_rnd_t) + sizeof(ent_id_t) + 24, "srow_t has no padding");
#define SROW_WORDS	(sizeof(srow_t) / 4)
#define SROW_ABS	((offsetof(srow_t, cv) + offsetof(col_ent_t, absmin)) / 4)	/* 6 words */
#define SROW_BOX	(offsetof(srow_t, lo) / 4)					/* 3 words */
_Static_assert(offsetof(col_ent_t, absmax) == offsetof(col_ent_t, absmin) + 12
	&& offsetof(srow_t, lo) % 4 == 0 && sizeof(srow_t) == offsetof(srow_t, lo) + 12,
	"srow_t derived words");

/* Per slot the map record whose record row names its entity (rec.c
 * rec_slot_records), for the rows' references. */
static uint32_t slot_rec[HL64_MAX_ENTITIES];

/* The reference a slot's row is saved against: slot i never used
 * (clear_slot's state, serial and free time 0); a slot a map record's row
 * names: the entity hydrate made of that record (id, rank, origin,
 * angles) with the record's model set (ent_set_model). */
static void srow_ref(int i, srow_t *r)
{
	gid_t16 gid = (gid_t16)(slot_rec[i] & 0xFFFFu);
	int k;

	memset(r, 0, sizeof(*r));
	r->cv.index = (uint16_t)i;
	r->cv.owner = COL_NONE;
	r->cv.model = COL_NONE;
	r->cv.studio = COL_NONE;
	r->phys.groundentity = ENT_NONE;
	r->phys.aiment = ENT_NONE;
	for (k = 0; k < 4; k++)
		r->rnd.sectors[k] = ENT_NONE;
	r->id.state = ENT_ST_FREE;
	r->id.layer = -1;
	r->id.cls = CLS_NONE;
	r->id.gid = GID_NONE;
	r->id.targetname = NAME_NONE;
	r->rank = ENT_RANK_RUNTIME;
	r->cells = ENT_UNLINKED;
	if (gid != GID_NONE && !rec_entity_ref(gid, &r->id, &r->rank, r->cv.origin, r->cv.angles))
	{
		res_t m = rec_key_res(gid, RK_MODEL);

		r->id.serial = (uint16_t)(slot_rec[i] >> 16);
		r->id.state = ENT_ST_LIVE;
		r->rnd.model = m;
		if (m != RES_NONE && RES_KIND(m) == RES_K_SUBMODEL)
		{
			r->cv.model = COL_MODEL(RES_SUB_SLOT(m), RES_SUB_NUM(m));
			if (!col_model_bounds(r->cv.model, r->cv.mins, r->cv.maxs))
				memset(r->cv.mins, 0, sizeof(r->cv.mins) + sizeof(r->cv.maxs));
		}
		else if (m != RES_NONE && RES_KIND(m) == RES_K_MODEL)
			r->cv.studio = m;
	}
}

/* The words a row's other words predict: a live slot's absmin / absmax
 * as ent_link would set them now (grid.c linked_box), a grid node's box
 * as ent_link rounds that box (+ g_M of the layer). Computed from row
 * into the derived words of d, absmin / absmax first (the box reads d's). */
static int32_t srow_coord(float x)
{
	if (!(x >= -16777216.0f))
		x = -16777216.0f;
	if (x > 16777216.0f)
		x = 16777216.0f;
	return (int32_t)x;
}

static int16_t srow_s16(int32_t v)
{
	return (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
}

static void srow_abs(const srow_t *row, srow_t *d)
{
	const col_ent_t *e = &row->cv;
	float *mn = d->cv.absmin, *mx = d->cv.absmax;
	int i;

	if (row->id.state != ENT_ST_LIVE)
		return;
	if (e->solid == COL_SOLID_BSP
		&& (e->angles[0] != 0.0f || e->angles[1] != 0.0f || e->angles[2] != 0.0f))
	{
		float m = 0.0f;

		for (i = 0; i < 3; i++)
		{
			float a = fabsf(e->mins[i]), b = fabsf(e->maxs[i]);

			if (a > m)
				m = a;
			if (b > m)
				m = b;
		}
		for (i = 0; i < 3; i++)
		{
			mn[i] = e->origin[i] - m;
			mx[i] = e->origin[i] + m;
		}
	}
	else
		for (i = 0; i < 3; i++)
		{
			mn[i] = e->origin[i] + e->mins[i];
			mx[i] = e->origin[i] + e->maxs[i];
		}
	for (i = 0; i < 3; i++)
	{
		mn[i] -= 1.0f;
		mx[i] += 1.0f;
	}
}

static void srow_box(const srow_t *row, srow_t *d)
{
	const float *gm = eng_layer_origin(row->id.layer);
	int a;

	if (!row->kind)
		return;
	for (a = 0; a < 3; a++)
	{
		d->lo[a] = srow_s16(srow_coord(row->cv.absmin[a] + gm[a]) - 1);
		d->hi[a] = srow_s16(srow_coord(row->cv.absmax[a] + gm[a]) + 1);
	}
}

static void srow_get(int i, srow_t *r)
{
	const ent_node_t *n = &ent_s.node[i];

	memset(r, 0, sizeof(*r));
	r->cv = ent_c.cv[i];
	r->mot = ent_c.mot[i];
	r->phys = ent_c.phys[i];
	r->tm = ent_c.tm[i];
	r->rnd = ent_c.rnd[i];
	r->id = ent_c.id[i];
	r->rank = ent_s.rank[i];
	if (r->id.state != ENT_ST_LIVE)
		r->freetime = ent_s.freetime[i];
	r->kind = n->kind;
	r->contents = ent_s.contents[i];
	r->cells = n->cells;
	if (n->kind)
	{
		memcpy(r->lo, n->lo, sizeof(r->lo));
		memcpy(r->hi, n->hi, sizeof(r->hi));
	}
}

static void srow_set(int i, const srow_t *r)
{
	ent_node_t *n = &ent_s.node[i];

	ent_c.cv[i] = r->cv;
	ent_c.mot[i] = r->mot;
	ent_c.phys[i] = r->phys;
	ent_c.tm[i] = r->tm;
	ent_c.rnd[i] = r->rnd;
	ent_c.id[i] = r->id;
	ent_s.rank[i] = r->rank;
	ent_s.freetime[i] = r->freetime;
	ent_s.contents[i] = r->contents;
	n->kind = r->kind;
	n->stamp = 0;
	n->cells = r->cells;
	memcpy(n->lo, r->lo, sizeof(n->lo));
	memcpy(n->hi, r->hi, sizeof(n->hi));
}

/* The words of from whose mask bit is clear go to row (words [w, w + n)). */
static void srow_fill(srow_t *row, const srow_t *from, const uint8_t *mask, uint32_t w, uint32_t n)
{
	for (; n--; w++)
		if (!(mask[w >> 3] & (1u << (w & 7))))
			memcpy((uint8_t *)row + 4 * w, (const uint8_t *)from + 4 * w, 4);
}

/* The slots up to the last one not in its never-used state, each as a
 * delta row against srow_ref with the derived words (srow_abs, srow_box)
 * predicted from the row itself: slots, serials, links and the
 * slot-reuse rule's inputs come back exactly. */
void ent_save(void)
{
	srow_t row, ref;
	int i, n = 0;

	sv_part("ent slots");
	sv_tag(SV_TAG('E', 'N', 'T', '2'));
	sv_u32(ent_s.seq);
	sv_u16(ent_s.top);
	sv_u16(ent_s.live);
	sv_u16((uint16_t)(int16_t)frame_layer);
	rec_slot_records(slot_rec);
	for (i = 0; i < HL64_MAX_ENTITIES; i++)
	{
		srow_get(i, &row);
		srow_ref(i, &ref);
		if (memcmp(&row, &ref, sizeof(row)))
			n = i + 1;
	}
	sv_u16((uint32_t)n);
	for (i = 0; i < n; i++)
	{
		srow_get(i, &row);
		srow_ref(i, &ref);
		srow_abs(&row, &ref);
		srow_box(&row, &ref);
		sv_put_delta(&row, &ref, sizeof(row));
	}
	sv_part("ent sets");
	ent_sets_save();
	sv_part("ent grid");
	ent_grid_save();
	sv_part("ent priv");
	ent_priv_save_all();
}

int ent_load(void)
{
	uint8_t mask[(SROW_WORDS + 7) / 8];
	srow_t row, d;
	uint32_t n, live = 0;
	int i;

	if (sv_expect(SV_TAG('E', 'N', 'T', '2')))
		return -1;
	ent_s.seq = sv_get_u32();
	ent_s.top = (uint16_t)sv_get_u16();
	ent_s.live = (uint16_t)sv_get_u16();
	frame_layer = (int16_t)sv_get_u16();
	n = sv_get_u16();
	if (ent_s.top > HL64_MAX_ENTITIES || ent_s.live > ent_s.top || n > HL64_MAX_ENTITIES
		|| frame_layer < -1 || frame_layer >= HL64_WORLD_LAYERS)
		sv_fail("ent: slot counts");
	rec_slot_records(slot_rec);
	for (i = 0; i < HL64_MAX_ENTITIES && !sv_failed(); i++)
	{
		srow_ref(i, &row);
		if ((uint32_t)i < n)
		{
			sv_get_delta(&row, &row, sizeof(row), mask);
			d = row;
			srow_abs(&row, &d);
			srow_fill(&row, &d, mask, SROW_ABS, 6);
			srow_box(&row, &d);
			srow_fill(&row, &d, mask, SROW_BOX, 3);
		}
		if (row.id.state > ENT_ST_LIVE || row.cv.index != i || row.contents > 1
			|| row.id.layer < -1 || row.id.layer >= HL64_WORLD_LAYERS
			|| (row.kind >> 4) > 3 || (row.kind ? row.cells >= ENT_CELLS : row.cells != ENT_UNLINKED)
			|| (row.id.state == ENT_ST_LIVE && i >= ent_s.top))
		{
			sv_fail("ent: slot");
			break;
		}
		live += row.id.state == ENT_ST_LIVE;
		srow_set(i, &row);
	}
	if (!sv_failed() && live != ent_s.live)
		sv_fail("ent: live count");
	if (ent_s.live > ent_s.live_peak)
		ent_s.live_peak = ent_s.live;
	if (sv_failed() || ent_sets_load() || ent_grid_load() || ent_priv_load_all())
		return -1;
	return 0;
}
