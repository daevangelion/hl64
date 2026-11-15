/* SPDX-License-Identifier: MIT */
/*
 * gamels.c -- host level-state driver for test/m4/gamels.py (slice GameLS,
 * M4): game_dehydrate / game_rehydrate / game_twin_copy / game_carry /
 * game_cut_refs on the real engine and game with cooked content.
 *
 * hl64-gamels --root CONTENT --map MAP [--frames N]
 *   New Game on MAP (player at its start), every physical record woken,
 *   the run-time creation paths fired (monstermakers, rappelling grunts),
 *   N frames (default 90) at a fixed 1/30 s. Then:
 *   close    every live entity is dehydrated as a closing map would be
 *            (per-class and per-map delta bytes, pristine / refused /
 *            transient counts); every delta is rehydrated into a fresh
 *            spawn of its record (or class, for runtime entities) with the
 *            restored hooks off, and must encode to the same bytes (the
 *            whole field list: components + every table level);
 *   mutate   every record entity with a save table: each field of its
 *            delta set to a different value of its type (references to
 *            another live entity, names to another name of the map),
 *            quiescence kept; dehydrate (must not refuse), fresh spawn,
 *            rehydrate, same bytes; then the restored hooks run;
 *   runtime  every class seen on the map that the game creates at run
 *            time (monsters, items, weapons, ammo, cans): CBaseEntity_Create,
 *            the same mutate round trip with the spawn constants included;
 *   refuse   the quiescence rules on the classes present (a door going
 *            up, a pusher moving, a rider, a monster scripted / in combat /
 *            with a live enemy, a bound scripted_sequence, a falling item),
 *            the steady fan accepted, a grenade freed as a transient;
 *   carry    game_carry of every live entity against HLSDK's ObjectCaps
 *            FCAP_ACROSS_TRANSITION table (test/m4/gamels.py);
 *   lines    `gamels class NAME n= pristine= bytes= max= refused=
 *            transient=` and `gamels map MAP entities= bytes= pristine=
 *            refused= transient=`; failures `gamels FAIL ...`.
 *
 * hl64-gamels --root CONTENT --twin A B LA:LB[,LA:LB...]
 *   A New Game on A, B's layer and HLE slot opened next to it; the pairs
 *   aliased (rec_twin_of); A's twin of each pair set moving (door used),
 *   its components copied into B's twin as the seam does (Seam's rule:
 *   brush origin = B's record origin + A's displacement), game_twin_copy,
 *   then both run 60 frames and must stay in step (toggle state, origin
 *   displacement, think); B keeps its own spawn constants and names;
 *   then game_cut_refs(B's slot): A-side references into B cleared,
 *   A-to-A and B-to-anything references kept.
 *
 * The record layer's M4 calls the game uses (rec_runtime_id,
 * rec_ref_entity, rec_twin_of) and seam_changelevel are this driver's
 * (--wrap): canonical references resolve by scanning the live entities.
 * Exit 0 when nothing failed, 1 otherwise, 2 on usage / content errors.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hl64/ent.h"
#include "hl64/game.h"
#include "hl64/rec.h"
#include "engine/core/dbg.h"
#include "engine/core/eng.h"
#include "engine/core/log.h"
#include "engine/core/sys.h"
#include "engine/phys/player.h"
#include "engine/world/wld.h"
#include "game/hl/grenade.h"
#include "game/hl/monster.h"
#include "game/hl/player.h"
#include "plat/host/host.h"

static const char *const names[] = {
#define CLASS(id, name)	name,
#include "game/defs/classes.def"
#undef CLASS
};
#define NCLASS	(sizeof(names) / sizeof(names[0]))

static const char *cname(cls_t c)
{
	return c < NCLASS ? names[c] : "?";
}

static int fails;

#define FAIL(...)	do { log_printf("gamels FAIL " __VA_ARGS__); log_flush(); fails++; } while (0)

/* ---- the record layer's M4 calls (wrapped) ------------------------------------ */

static uint16_t next_rtid = 1;
static gid_t16 twin_a[16], twin_b[16];
static int ntwins;

static int slot_map(int slot)
{
	wld_layer_t *L = slot >= 0 ? wld_layer(slot) : NULL;

	return L ? L->map : -1;
}

uint16_t __wrap_rec_runtime_id(int e);
uint16_t __wrap_rec_runtime_id(int e)
{
	if (!ent_is_live(e) || ent_idc(e)->gid != GID_NONE)
		return 0;
	if (!ent_idc(e)->rtid)
		ent_idc(e)->rtid = next_rtid++;
	return ent_idc(e)->rtid;
}

int __wrap_rec_ref_entity(uint16_t map, uint16_t id, int runtime);
int __wrap_rec_ref_entity(uint16_t map, uint16_t id, int runtime)
{
	int e;

	for (e = ENT_PLAYER + 1; e < HL64_MAX_ENTITIES; e++)
	{
		const ent_id_t *d;

		if (!ent_is_live(e))
			continue;
		d = ent_idc(e);
		if (runtime ? d->gid == GID_NONE && d->rtid == id && slot_map(d->layer) == map
			    : d->gid != GID_NONE && GID_LUMP(d->gid) == id &&
			      slot_map(GID_SLOT(d->gid)) == map)
			return e;
	}
	return -1;
}

gid_t16 __wrap_rec_twin_of(gid_t16 g);
gid_t16 __wrap_rec_twin_of(gid_t16 g)
{
	int i;

	for (i = 0; i < ntwins; i++)
	{
		if (twin_a[i] == g)
			return twin_b[i];
		if (twin_b[i] == g)
			return twin_a[i];
	}
	return GID_NONE;
}

/* rec_slot_closing: the closing pass sets the slot */
static int closing_slot = -1;
int __wrap_rec_slot_closing(int slot);
int __wrap_rec_slot_closing(int slot)
{
	return slot >= 0 && slot == closing_slot;
}

static int changelevels;
int __wrap_seam_changelevel(gid_t16 trigger, int use);
int __wrap_seam_changelevel(gid_t16 trigger, int use)
{
	(void)trigger;
	(void)use;
	changelevels++;
	return 1;
}

/* ---- HLSDK ObjectCaps & FCAP_ACROSS_TRANSITION (dlls/ *.cpp, cbase.h) --------- */

/* 1 crosses, 0 does not; a class missing here fails (the table must cover
 * every class the maps spawn). CBaseEntity's default is ACROSS; CPointEntity,
 * CBaseTrigger, CBaseDoor, CBaseButton, CBasePlatTrain, CFuncWall, CBreakable,
 * CAmbientGeneric, CSpeaker, CSprite, CGlow, CCineMonster, the chargers,
 * CFuncRotating, CPendulum, CFuncIllusionary, CFuncMonsterClip remove it;
 * FCAP_DONT_SAVE classes (CGib, CShower, CStomp, CPlatTrigger, CCorpse) never
 * cross; the world is not in any PVS list. */
static const struct { const char *name; int carry; } hlsdk_carry[] = {
	{ "ambient_generic", 0 }, { "speaker", 0 }, { "env_sprite", 0 }, { "env_glow", 0 },
	{ "env_beverage", 1 }, { "item_sodacan", 1 }, { "env_explosion", 1 },
	{ "spark_shower", 0 }, { "gib", 0 }, { "garg_stomp", 0 }, { "streak_spiral", 1 },
	{ "env_smoker", 1 }, { "bodyque", 0 }, { "worldspawn", 0 }, { "infodecal", 1 },
	{ "func_breakable", 0 }, { "func_pushable", 0 }, { "func_button", 0 },
	{ "func_rot_button", 0 }, { "momentary_rot_button", 0 }, { "func_door", 0 },
	{ "func_door_rotating", 0 }, { "func_water", 0 }, { "momentary_door", 0 },
	{ "func_guntarget", 0 }, { "func_plat", 0 }, { "func_train", 0 }, { "plat_trigger", 0 },
	{ "func_healthcharger", 0 }, { "func_recharge", 0 }, { "func_illusionary", 0 },
	{ "func_wall", 0 }, { "func_wall_toggle", 0 }, { "func_conveyor", 0 },
	{ "func_ladder", 0 }, { "func_pendulum", 0 }, { "func_rotating", 0 },
	{ "path_corner", 0 }, { "path_track", 0 }, { "trigger_multiple", 0 },
	{ "trigger_once", 0 }, { "trigger_counter", 0 }, { "trigger_hurt", 0 },
	{ "trigger_push", 0 }, { "trigger_teleport", 0 }, { "trigger_transition", 0 },
	{ "trigger_changelevel", 0 }, { "trigger_endsection", 0 }, { "trigger_autosave", 0 },
	{ "target_cdaudio", 0 }, { "trigger_cdaudio", 0 },
	{ "info_target", 0 },
	{ "info_landmark", 0 }, { "info_player_start", 0 }, { "info_teleport_destination", 0 },
	{ "scripted_sequence", 0 }, { "aiscripted_sequence", 0 }, { "grenade", 1 },
	{ "weaponbox", 1 },
	/* CFuncTrackTrain: (CBaseEntity & ~FCAP_ACROSS_TRANSITION) | FCAP_DIRECTIONAL_USE */
	{ "func_tracktrain", 0 },
};

static int expect_carry(const char *n)
{
	unsigned i;

	if (!strncmp(n, "monster_", 8) || !strncmp(n, "item_", 5) || !strncmp(n, "ammo_", 5) ||
	    !strncmp(n, "weapon_", 7))
		return strcmp(n, "monster_furniture") != 0;
	for (i = 0; i < sizeof(hlsdk_carry) / sizeof(hlsdk_carry[0]); i++)
		if (!strcmp(hlsdk_carry[i].name, n))
			return hlsdk_carry[i].carry;
	return -1;
}

/* ---- entity helpers --------------------------------------------------------------- */

static uint8_t d1[2048], d2[2048];

typedef struct {
	cls_t		cls;
	gid_t16		gid;
	uint16_t	rtid;
	int8_t		layer;
	float		org[3], ang[3];
} ident_t;

static void ident_of(int e, ident_t *id)
{
	id->cls = ent_idc(e)->cls;
	id->gid = ent_idc(e)->gid;
	id->rtid = ent_idc(e)->rtid;
	id->layer = ent_idc(e)->layer;
	memcpy(id->org, ent_cv(e)->origin, sizeof id->org);
	memcpy(id->ang, ent_cv(e)->angles, sizeof id->ang);
}

/* A fresh entity of the same record (as rec.c hydrates) or class (as a
 * runtime record respawns), before game_rehydrate. */
static int respawn(const ident_t *id)
{
	int e = ent_create(ENT_ALLOC_GAME), prev, r;

	if (e < 0)
		return -1;
	ent_idc(e)->layer = id->layer;
	ent_idc(e)->cls = id->cls;
	prev = eng_frame_layer();
	eng_set_frame_layer(id->layer);
	if (id->gid != GID_NONE)
	{
		ent_idc(e)->gid = id->gid;
		ent_idc(e)->targetname = rec_targetname(id->gid);
		rec_origin(id->gid, ent_cv(e)->origin);
		rec_angles(id->gid, ent_cv(e)->angles);
		r = game_spawn_record(id->gid, e);
	}
	else
	{
		ent_idc(e)->rtid = id->rtid;
		memcpy(ent_cv(e)->origin, id->org, sizeof id->org);
		memcpy(ent_cv(e)->angles, id->ang, sizeof id->ang);
		r = game_spawn_class(id->cls, e);
	}
	eng_set_frame_layer(prev);
	if (r < 0)
	{
		if (ent_is_live(e))
			ent_destroy(e);
		return -1;
	}
	return e;
}

static int first_diff(const uint8_t *a, int na, const uint8_t *b, int nb)
{
	int i;

	for (i = 0; i < na && i < nb; i++)
		if (a[i] != b[i])
			return i;
	return na == nb ? -1 : (na < nb ? na : nb);
}

/* Rehydrate delta (n bytes) into a fresh spawn of id with the restored
 * hooks off; the fresh entity must encode to the same bytes. Returns the
 * new entity (hooks then run on it), -1 on failure. */
static int round_trip(const ident_t *id, const uint8_t *d, int n, const char *pass)
{
	int e = respawn(id), m, k;

	if (e < 0)
	{
		FAIL("%s %s: respawn refused\n", pass, cname(id->cls));
		return -1;
	}
	sv_test_restore_hooks(0);
	game_rehydrate(e, d, (uint32_t)n);
	sv_test_restore_hooks(1);
	if (n == 1 && d[0] == 0)
		return e;	/* pristine: the fresh spawn is the state */
	m = sv_test_encode(e, d2, sizeof d2);
	if ((k = first_diff(d, n, d2, m)) >= 0)
	{
		int lv = -1, of = -1, fi = sv_test_diff(e, d, n, d2, m, &lv, &of);

		FAIL("%s %s lump %d: rehydrated state differs at delta byte %d (%d / %d B): "
			"level %d field %d offset %d\n", pass, cname(id->cls),
			id->gid == GID_NONE ? -1 : (int)GID_LUMP(id->gid), k, n, m, lv, fi, of);
		{
			char ha[64], hb[64];
			int j, o = k > 4 ? k - 4 : 0;

			for (j = 0; j < 12; j++)
			{
				snprintf(ha + 3 * j, 4, "%02x ", o + j < n ? d[o + j] : 0);
				snprintf(hb + 3 * j, 4, "%02x ", o + j < m ? d2[o + j] : 0);
			}
			log_printf("gamels   at %d: %s| %s\n", o, ha, hb);
		}
	}
	log_flush();
	sv_restored(e);
	return e;
}

/* ---- per-class accounting ----------------------------------------------------------- */

static struct {
	uint32_t	n, pristine, bytes, max, refused, transient;
} cs[NCLASS];

/* ---- close pass --------------------------------------------------------------------- */

static void close_pass(const char *map, int slot)
{
	static uint16_t list[HL64_MAX_ENTITIES];
	int e, i, n = 0, rt = 0;
	uint32_t tot = 0, ent = 0, pr = 0, ref = 0, tr = 0;

	for (e = ENT_PLAYER + 1; e < HL64_MAX_ENTITIES; e++)
		if (ent_is_live(e) && ent_idc(e)->layer == slot)
			list[n++] = (uint16_t)e;
	for (i = 0; i < n; i++)
	{
		ident_t id;
		int r;
		uint32_t k;
		cls_t c;

		e = list[i];
		if (!ent_is_live(e))
			continue;	/* a helper freed with its owner (plat trigger) */
		ident_of(e, &id);
		c = id.cls;
		if (!sv_table_of(e))
		{
			FAIL("close %s (%d): no save table\n", cname(c), e);
			continue;
		}
		k = game_dehydrate(e, d1, sizeof d1);
		ent++;
		if (!ent_is_live(e) || ent_idc(e)->cls != c)
		{
			cs[c].transient++;
			tr++;
			continue;
		}
		if (k == 0)
		{
			cs[c].refused++;
			ref++;
			continue;
		}
		cs[c].n++;
		cs[c].bytes += k;
		tot += k;
		if (k > cs[c].max)
			cs[c].max = k;
		if (k == 1 && d1[0] == 0)
		{
			cs[c].pristine++;
			pr++;
		}
		if (id.gid == GID_NONE)
		{
			id.rtid = ent_idc(e)->rtid;
			rt++;
		}
		ent_destroy(e);
		r = round_trip(&id, d1, (int)k, "close");
		(void)r;
	}
	log_printf("gamels map %s entities=%u bytes=%u pristine=%u refused=%u transient=%u "
		"runtime=%d\n", map, ent, tot, pr, ref, tr, rt);
	log_flush();
}

/* ---- closing pass: the whole map dehydrates as rec_map_close does (no
 * refusals: monsters mid-script, idling headcrabs, movers mid-travel keep
 * their state), every record respawns, then every delta rehydrates; the
 * state must encode back to the same closed delta, and scripted monsters
 * keep their scripted_sequence (and it its monster) --------------------------- */

#define CLOSE_MAX	HL64_MAX_ENTITIES
static uint8_t cl_delta[CLOSE_MAX][640];
static uint16_t cl_len[CLOSE_MAX];
static ident_t cl_id[CLOSE_MAX];
static int8_t cl_script[CLOSE_MAX];	/* was in a script: its cine's lump */
static int16_t cl_cine[CLOSE_MAX];

static void closing_pass(const char *map, int slot)
{
	static uint16_t list[HL64_MAX_ENTITIES], neu[CLOSE_MAX];
	int e, i, n = 0, k = 0, dropped = 0, scripted = 0, kept = 0;

	for (e = ENT_PLAYER + 1; e < HL64_MAX_ENTITIES; e++)
		if (ent_is_live(e) && ent_idc(e)->layer == slot && ent_idc(e)->gid != GID_NONE)
			list[n++] = (uint16_t)e;
	closing_slot = slot;
	for (i = 0; i < n; i++)
	{
		const sv_table_t *t;
		uint32_t len;
		int cine;

		e = list[i];
		if (!ent_is_live(e))
			continue;
		cl_script[k] = 0;
		for (t = sv_table_of(e); t && t != &sv_CBaseMonster; t = t->base)
			;
		if (t && (cine = EHGet(MON(e)->m_pCine)) >= 0 && ent_idc(cine)->gid != GID_NONE)
		{
			cl_script[k] = 1;
			cl_cine[k] = (int16_t)GID_LUMP(ent_idc(cine)->gid);
		}
		ident_of(e, &cl_id[k]);
		len = game_dehydrate(e, cl_delta[k], sizeof cl_delta[k]);
		if (!ent_is_live(e))
			continue;	/* a transient the game freed */
		if (len == 0)
		{
			FAIL("closing %s (%d): refused at close\n", cname(ent_idc(e)->cls), e);
			dropped++;
			ent_destroy(e);
			continue;
		}
		cl_len[k++] = (uint16_t)len;
		ent_destroy(e);
	}
	/* the map reopens: every record first, then the deltas (HLSDK restores
	 * the whole level before the references are used) */
	for (i = 0; i < k; i++)
		neu[i] = (uint16_t)respawn(&cl_id[i]);
	sv_test_restore_hooks(0);
	for (i = 0; i < k; i++)
		if ((int16_t)neu[i] >= 0)
			game_rehydrate(neu[i], cl_delta[i], cl_len[i]);
	sv_test_restore_hooks(1);
	for (i = 0; i < k; i++)
	{
		int m, d;

		e = (int16_t)neu[i];
		if (e < 0)
		{
			FAIL("closing %s: respawn refused\n", cname(cl_id[i].cls));
			continue;
		}
		if (cl_len[i] == 1 && cl_delta[i][0] == 0)
			continue;
		m = sv_test_encode(e, d2, sizeof d2);
		d2[0] = cl_delta[i][0];	/* the closed-delta marker */
		if ((d = first_diff(cl_delta[i], cl_len[i], d2, m)) >= 0)
		{
			int lv = -1, of = -1, fi = sv_test_diff(e, cl_delta[i], cl_len[i], d2, m, &lv,
				&of);

			FAIL("closing %s lump %d: state differs after reopen at byte %d: level %d "
				"field %d offset %d\n", cname(cl_id[i].cls), (int)GID_LUMP(cl_id[i].gid),
				d, lv, fi, of);
		}
		if (cl_script[i])
		{
			int cine = EHGet(MON(e)->m_pCine);

			scripted++;
			if (cine >= 0 && (int)GID_LUMP(ent_idc(cine)->gid) == cl_cine[i] &&
			    MON(e)->m_MonsterState == MONSTERSTATE_SCRIPT)
				kept++;
			else
				FAIL("closing %s lump %d: its script (cine lump %d) was not kept\n",
					cname(cl_id[i].cls), (int)GID_LUMP(cl_id[i].gid), cl_cine[i]);
		}
	}
	for (i = 0; i < k; i++)
		if ((int16_t)neu[i] >= 0)
			sv_restored(neu[i]);
	closing_slot = -1;
	log_printf("gamels closing %s entities=%d dropped=%d scripted=%d kept=%d\n", map, k,
		dropped, scripted, kept);
	log_flush();
}
/* ---- mutation ----------------------------------------------------------------------- */

static int other_entity(int e, int slot)
{
	int r;

	for (r = HL64_MAX_ENTITIES - 1; r > ENT_PLAYER; r--)
		if (r != e && ent_is_live(r) && ent_idc(r)->layer == slot &&
		    ent_idc(r)->gid != GID_NONE && sv_table_of(r) &&
		    ent_idc(r)->cls != CLS_PLAT_TRIGGER)
			return r;
	return ENT_WORLD;
}

static name_t other_name(name_t n, int slot)
{
	int l;

	for (l = 0; l < 1024; l++)
	{
		gid_t16 g = GID_MAKE(slot, l);
		name_t t;

		if (rec_kind(g) < 0)
			continue;
		t = rec_targetname(g);
		if (t != NAME_NONE && t != n)
			return t;
	}
	return n;
}

static void mutate_elem(int ty, uint8_t *v, int e, int slot)
{
	float f, vec[3];
	int32_t i;
	uint16_t s;
	uint32_t u;

	switch (ty)
	{
	case SV_F32:
		memcpy(&f, v, 4);
		f += 1.5f;
		memcpy(v, &f, 4);
		break;
	case SV_TIME:
		memcpy(&f, v, 4);
		f = f != 0 ? f + 0.25f : (float)eng_time() + 0.75f;
		memcpy(v, &f, 4);
		break;
	case SV_VEC: case SV_POS:
		memcpy(vec, v, 12);
		vec[0] += 1;
		vec[1] += 2;
		vec[2] += 3;
		memcpy(v, vec, 12);
		break;
	case SV_I32: case SV_U32:
		memcpy(&i, v, 4);
		i += 1;
		memcpy(v, &i, 4);
		break;
	case SV_I16: case SV_U16:
		memcpy(&s, v, 2);
		s = (uint16_t)(s + 1);
		memcpy(v, &s, 2);
		break;
	case SV_U8:
		v[0] ^= 1;
		break;
	case SV_FN:
		memcpy(&s, v, 2);
		s = s ? (uint16_t)(s ^ 1) : 1;
		memcpy(v, &s, 2);
		break;
	case SV_NAME:
		memcpy(&s, v, 2);
		s = other_name(s, slot);
		memcpy(v, &s, 2);
		break;
	case SV_EH:
		u = EHANDLE(other_entity(e, slot));
		memcpy(v, &u, 4);
		break;
	case SV_IX16:
		s = (uint16_t)other_entity(e, slot);
		memcpy(v, &s, 2);
		break;
	case SV_IX32:
		i = other_entity(e, slot);
		memcpy(v, &i, 4);
		break;
	default:
		break;		/* RES, GID: kept (cooked ids) */
	}
}

/* Every field a delta of e carries, set to another value of its type. */
static int mutate(int e, int with_constants)
{
	const sv_table_t *t = sv_table_of(e), *ch[8], *x;
	uint8_t *p = (uint8_t *)ent_priv(e);
	int slot = ent_idc(e)->layer, n = 0, i, j, k, nf = 0;
	col_ent_t *cv = ent_cv(e);
	ent_rnd_t *r = ent_rnd(e);
	ent_phys_t *ph = ent_phys(e);
	float v[3];

	for (x = t; x; x = x->base)
		ch[n++] = x;
	for (i = 0; i < n; i++)
		for (j = 0; j < ch[i]->nf; j++)
		{
			const sv_field_t *f = &ch[i]->f[j];
			int ty = f->type;

			if ((f->flags & SVK) && !with_constants)
				continue;
			if (ch[i] == &sv_CBaseEntity && f->ofs == offsetof(CBaseEntity, message))
				continue;	/* a res (sounds) or a name by class */
			for (k = 0; k < f->n; k++)
				mutate_elem(ty, p + f->ofs + k * sv_elem_size(ty), e, slot);
			nf++;
		}
	/* components */
	VecCopy(cv->origin, v);
	v[0] += 4;
	v[1] += 5;
	v[2] += 6;
	ent_set_origin(e, v);
	cv->angles[1] += 15;
	if (cv->solid != SOLID_BSP)
	{
		float mn[3], mx[3];

		VecCopy(cv->mins, mn);
		VecCopy(cv->maxs, mx);
		mn[0] -= 1;
		mx[2] += 1;
		ent_set_size(e, mn, mx);
	}
	if (cv->movetype != MOVETYPE_PUSH && cv->movetype != MOVETYPE_NONE)
	{
		VecSet(v, 10, -5, 0);
		ent_set_velocity(e, v);
		ent_set_flags(e, cv->flags | FL_ONGROUND);
	}
	cv->skin = (int16_t)(cv->skin + 1);
	cv->rendermode = (uint8_t)(cv->rendermode ^ 1);
	r->frame += 2;
	r->framerate += 0.5f;
	r->scale += 0.25f;
	r->renderamt ^= 0x10;
	r->renderfx ^= 1;
	r->rendercolor[1] ^= 0x20;
	r->body ^= 1;
	r->controller[0] ^= 3;
	r->blending[1] ^= 5;
	r->effects ^= EF_DIMLIGHT;
	r->animtime = (float)eng_time() + 0.125f;
	ph->gravity += 0.5f;
	ph->friction += 0.25f;
	ph->ideal_yaw += 10;
	ph->yaw_speed += 3;
	ph->view_ofs_z = (int16_t)(ph->view_ofs_z + 1);
	cv->owner = (uint16_t)other_entity(e, slot);
	if (cv->movetype == MOVETYPE_PUSH)
	{
		ent_tm(e)->ltime += 0.25f;
		ent_think_at(e, ent_tm(e)->ltime + 0.5f);
	}
	else
		ent_think_at(e, (float)eng_time() + 0.5f);
	return nf;
}

/* Undo what the mutation may have made non-quiescent (the refuse cases
 * test those) and clear riders. */
static void quiesce(int e)
{
	const sv_table_t *t;
	int r;

	for (t = sv_table_of(e); t; t = t->base)
	{
		if (t == &sv_CBaseToggle)
		{
			CBaseToggle *p = PRIV(CBaseToggle, e);

			if (p->m_toggle_state == TS_GOING_UP || p->m_toggle_state == TS_GOING_DOWN)
				p->m_toggle_state = TS_AT_TOP;
		}
		if (t == &sv_CBaseMonster)
		{
			CBaseMonster *m = MON(e);

			m->m_MonsterState = MONSTERSTATE_IDLE;
			m->m_IdealMonsterState = MONSTERSTATE_ALERT;
			m->m_pCine = EH_NONE;
			m->m_hEnemy = EH_NONE;
			if (m->deadflag == DEAD_DYING)
				m->deadflag = DEAD_DEAD;
		}
	}
	if (ent_idc(e)->cls == CLS_SCRIPTED_SEQUENCE || ent_idc(e)->cls == CLS_AISCRIPTED_SEQUENCE)
	{
		/* m_hTargetEnt is the first member after CBaseToggle */
		ent_h *h = (ent_h *)((uint8_t *)ent_priv(e) + sizeof(CBaseToggle));

		*h = EH_NONE;
	}
	if (ent_idc(e)->cls == CLS_MONSTER_TRIPMINE)
		ent_tm(e)->think = 0;
	if (ent_idc(e)->cls == CLS_FUNC_HEALTHCHARGER || ent_idc(e)->cls == CLS_FUNC_RECHARGE)
		*(int32_t *)((uint8_t *)ent_priv(e) + sv_table_of(e)->f[3].ofs) = 0;	/* m_iOn */
	if (ent_cv(e)->movetype == MOVETYPE_FOLLOW)
		ent_set_movetype(e, MOVETYPE_NONE);
	if (ent_idc(e)->cls == CLS_PATH_TRACK)
	{
		/* a path_track waits for its map's track trains to stop
		 * (trains.c CPathTrack_quiet) */
		static const float z[3];

		for (r = -1; (r = ent_next_class(r, CLS_FUNC_TRACKTRAIN, ent_idc(e)->layer)) >= 0;)
		{
			PRIV(CBaseEntity, r)->speed = 0;
			ent_set_velocity(r, z);
		}
	}
	for (r = ENT_PLAYER; r < HL64_MAX_ENTITIES; r++)
		if (r != e && ent_is_live(r))
		{
			if (ent_phys(r)->groundentity == e)
				ent_set_ground(r, ENT_WORLD);
			if (ent_phys(r)->aiment == e)
				ent_set_aiment(r, ENT_NONE);
		}
	if (ent_cv(e)->movetype == MOVETYPE_PUSH && !(ent_idc(e)->cls == CLS_FUNC_ROTATING ||
	    ent_idc(e)->cls == CLS_FUNC_PENDULUM))
	{
		static const float z[3];

		ent_set_velocity(e, z);
		ent_set_avelocity(e, z);
	}
}

static int is_transient_class(cls_t c)
{
	return c == CLS_GIB || c == CLS_GRENADE || c == CLS_SPARK_SHOWER || c == CLS_GARG_STOMP ||
		c == CLS_STREAK_SPIRAL || c == CLS_ENV_SMOKER || c == CLS_ENV_EXPLOSION ||
		c == CLS_BODYQUE || c == CLS_PLAT_TRIGGER || c == CLS_WORLDSPAWN;
}

static void mutate_pass(int slot, int runtime)
{
	static uint16_t list[HL64_MAX_ENTITIES];
	static uint8_t done[NCLASS];
	int e, i, n = 0, ok = 0;

	memset(done, 0, sizeof done);
	for (e = ENT_PLAYER + 1; e < HL64_MAX_ENTITIES; e++)
		if (ent_is_live(e) && ent_idc(e)->layer == slot && sv_table_of(e) &&
		    !is_transient_class(ent_idc(e)->cls))
			list[n++] = (uint16_t)e;
	for (i = 0; i < n; i++)
	{
		ident_t id;
		uint32_t k;
		int nf, c, rt = -1;

		e = list[i];
		if (!ent_is_live(e))
			continue;
		c = ent_idc(e)->cls;
		if (runtime)
		{
			/* one runtime incarnation per class seen here: CBaseEntity::Create */
			const char *cn = cname((cls_t)c);

			if (done[c] || ent_idc(e)->gid == GID_NONE ||
			    !(!strncmp(cn, "monster_", 8) || !strncmp(cn, "item_", 5) ||
			      !strncmp(cn, "ammo_", 5) || !strncmp(cn, "weapon_", 7)) ||
			    !strcmp(cn, "monster_grunt_repel"))
				continue;
			done[c] = 1;
			eng_set_frame_layer(slot);	/* as the game's dispatch does */
			rt = CBaseEntity_Create((cls_t)c, ent_cv(e)->origin, ent_cv(e)->angles, -1);
			if (rt < 0)
			{
				FAIL("runtime %s: CBaseEntity_Create refused\n", cn);
				continue;
			}
			e = rt;
		}
		else if (ent_idc(e)->gid == GID_NONE)
			continue;
		nf = mutate(e, runtime);
		quiesce(e);
		ident_of(e, &id);
		k = game_dehydrate(e, d1, sizeof d1);
		if (k == 0 || !ent_is_live(e))
		{
			FAIL("%s %s (%d): dehydrate refused a quiescent mutated entity\n",
				runtime ? "runtime" : "mutate", cname((cls_t)c), e);
			continue;
		}
		if (k == 1 && d1[0] == 0)
		{
			FAIL("%s %s (%d): a mutated entity came out pristine\n",
				runtime ? "runtime" : "mutate", cname((cls_t)c), e);
			continue;
		}
		if (runtime)
			id.rtid = ent_idc(e)->rtid;
		ent_destroy(e);
		if (round_trip(&id, d1, (int)k, runtime ? "runtime" : "mutate") >= 0)
			ok++;
		log_printf("gamels %s %s fields=%d bytes=%u\n", runtime ? "runtime" : "mutate",
			cname((cls_t)c), nf, k);
		log_flush();
	}
	log_printf("gamels %s done %d\n", runtime ? "runtime" : "mutate", ok);
}

/* ---- refuse cases ------------------------------------------------------------------- */

static int find_class(cls_t c, int slot)
{
	return ent_next_class(-1, c, slot);
}

static void expect(int e, int want_refuse, const char *what)
{
	ident_t id;
	uint32_t k;
	cls_t c;

	if (e < 0)
		return;
	c = ent_idc(e)->cls;
	ident_of(e, &id);
	k = game_dehydrate(e, d1, sizeof d1);
	if (want_refuse && (k != 0 || !ent_is_live(e)))
		FAIL("refuse %s (%s): accepted\n", what, cname(c));
	else if (!want_refuse && k == 0)
		FAIL("refuse %s (%s): refused\n", what, cname(c));
	else
		log_printf("gamels refuse %s %s ok\n", what, cname(c));
	if (k)
	{
		ent_destroy(e);
		round_trip(&id, d1, (int)k, "refuse");
	}
}

static void refuse_pass(int slot)
{
	static const float up[3] = { 0, 0, 10 }, z[3];
	int e, r, m;

	if ((e = find_class(CLS_FUNC_DOOR, slot)) >= 0)
	{
		quiesce(e);
		PRIV(CBaseToggle, e)->m_toggle_state = TS_GOING_UP;
		expect(e, 1, "door-going-up");
		PRIV(CBaseToggle, e)->m_toggle_state = TS_AT_BOTTOM;
		ent_set_velocity(e, up);
		expect(e, 1, "pusher-moving");
		ent_set_velocity(e, z);
		r = ENT_PLAYER;
		ent_set_ground(r, e);
		expect(e, 1, "rider");
		ent_set_ground(r, ENT_WORLD);
		expect(e, 0, "door-at-rest");
	}
	if ((e = find_class(CLS_FUNC_ROTATING, slot)) >= 0)
	{
		static const float spin[3] = { 0, 90, 0 };

		quiesce(e);
		ent_set_avelocity(e, spin);
		expect(e, 0, "fan-steady");
	}
	for (m = ENT_PLAYER + 1; m < HL64_MAX_ENTITIES; m++)
	{
		const sv_table_t *t;

		if (!ent_is_live(m) || ent_idc(m)->layer != slot || ent_idc(m)->gid == GID_NONE)
			continue;
		for (t = sv_table_of(m); t && t != &sv_CBaseMonster; t = t->base)
			;
		if (t && ent_idc(m)->cls != CLS_PLAYER && IsAlive(m))
			break;
	}
	if (m < HL64_MAX_ENTITIES)
	{
		CBaseMonster *p = MON(m);

		quiesce(m);
		p->m_MonsterState = MONSTERSTATE_SCRIPT;
		expect(m, 1, "monster-script");
		quiesce(m);
		p->m_MonsterState = MONSTERSTATE_COMBAT;
		expect(m, 1, "monster-combat");
		quiesce(m);
		p->m_hEnemy = EHANDLE(ENT_PLAYER);
		expect(m, 1, "monster-live-enemy");
		quiesce(m);
		if ((e = find_class(CLS_SCRIPTED_SEQUENCE, slot)) >= 0)
		{
			p->m_pCine = EHANDLE(e);
			expect(m, 1, "monster-cine");
			quiesce(m);
			*(ent_h *)((uint8_t *)ent_priv(e) + sizeof(CBaseToggle)) = EHANDLE(m);
			expect(e, 1, "cine-bound");
			quiesce(e);
		}
		expect(m, 0, "monster-idle");
	}
	if ((e = find_class(CLS_ITEM_HEALTHKIT, slot)) >= 0 || (e = find_class(CLS_ITEM_BATTERY,
	    slot)) >= 0)
	{
		quiesce(e);
		ent_set_flags(e, ent_cv(e)->flags & ~FL_ONGROUND);
		ent_set_velocity(e, up);
		expect(e, 1, "item-falling");
	}
	{
		float org[3];

		VecCopy(phy_player()->origin, org);
		org[2] += 64;
		e = CGrenade_ShootTimed(ENT_PLAYER, org, up, 3);
		if (e >= 0)
		{
			uint32_t k = game_dehydrate(e, d1, sizeof d1);

			if (k != 0 || ent_is_live(e))
				FAIL("refuse grenade: not freed as a transient\n");
			else
				log_printf("gamels refuse grenade-transient ok\n");
		}
	}
}

/* ---- carry ------------------------------------------------------------------------- */

static void carry_pass(int slot)
{
	static uint8_t seen[NCLASS];
	int e;

	memset(seen, 0, sizeof seen);
	for (e = ENT_PLAYER + 1; e < HL64_MAX_ENTITIES; e++)
	{
		cls_t c;
		int want, got;

		if (!ent_is_live(e) || ent_idc(e)->layer != slot)
			continue;
		c = ent_idc(e)->cls;
		if (seen[c])
			continue;
		seen[c] = 1;
		want = expect_carry(cname(c));
		got = game_carry(e);
		if (want < 0)
			FAIL("carry %s: no HLSDK entry\n", cname(c));
		else if (want != got)
			FAIL("carry %s: game %d HLSDK %d\n", cname(c), got, want);
		else
			log_printf("gamels carry %s %d\n", cname(c), got);
	}
}

/* ---- boot -------------------------------------------------------------------------------- */

static int boot(const char *root, const char *mapname, int *slot)
{
	dbg_cmd_t dt;
	int map, lump;

	host_io_init(root);
	eng_boot("host");
	map = wld_map_find(mapname);
	if (map < 0)
	{
		fprintf(stderr, "hl64-gamels: no map %s\n", mapname);
		return -1;
	}
	if (map != wld_start_map())
		wld_close(wld_open(wld_start_map()));
	memset(&dt, 0, sizeof(dt));
	dt.op = DBG_FIXED_DT;
	dt.nargs = 1;
	dt.arg[0].u = 33333;
	if (dbg_push(&dt))
		return -1;
	game_new_campaign();
	rec_new_campaign(map);
	if (phy_player_spawn_start(map))
	{
		fprintf(stderr, "hl64-gamels: no player start on %s\n", mapname);
		return -1;
	}
	*slot = wld_open(map)->slot;
	for (lump = 0; lump < 1024; lump++)
	{
		gid_t16 gid = GID_MAKE(*slot, lump);

		if (rec_kind(gid) != REC_KIND_ENTITY || rec_entity(gid) >= 0)
			continue;
		if (rec_state(gid) == REC_PENDING || rec_state(gid) == REC_DRY)
			rec_wake(gid);
	}
	return map;
}

static void fire_makers(int slot)
{
	name_t fired[256];
	int lump, k, nfired = 0;

	for (lump = 0; lump < 1024; lump++)
	{
		gid_t16 gid = GID_MAKE(slot, lump);
		const char *cn;
		name_t n;

		if (rec_kind(gid) < 0)
			continue;
		cn = cname(rec_class(gid));
		if (strcmp(cn, "monstermaker") && strcmp(cn, "monster_grunt_repel"))
			continue;
		n = rec_targetname(gid);
		if (n == NAME_NONE)
			continue;
		for (k = 0; k < nfired && fired[k] != n; k++)
			;
		if (k < nfired)
			continue;
		if (nfired < 256)
			fired[nfired++] = n;
		rec_fire(n, ENT_PLAYER, ENT_PLAYER, GID_NONE, USE_ON, 0);
	}
}

static int map_mode(const char *root, const char *mapname, int frames)
{
	int slot, f;
	cls_t c;

	if (boot(root, mapname, &slot) < 0)
		return 2;
	fire_makers(slot);
	for (f = 0; f < frames; f++)
	{
		eng_frame();
		log_flush();
	}
	carry_pass(slot);
	log_flush();
	sv_stats_reset();
	closing_pass(mapname, slot);
	close_pass(mapname, slot);
	for (c = 0; c < NCLASS; c++)
		if (cs[c].n || cs[c].refused || cs[c].transient)
		{
			log_printf("gamels class %s n=%u pristine=%u bytes=%u max=%u refused=%u "
				"transient=%u\n", cname(c), cs[c].n, cs[c].pristine, cs[c].bytes,
				cs[c].max, cs[c].refused, cs[c].transient);
			log_flush();
		}
	mutate_pass(slot, 0);
	mutate_pass(slot, 1);
	refuse_pass(slot);
	log_printf("gamels done %s fails=%d\n", mapname, fails);
	log_flush();
	return fails ? 1 : 0;
}

/* ---- twins --------------------------------------------------------------------------------- */

/* The record layer releases far records (B is 2048 u away): wake them
 * back, from their deltas, when the test needs them. */
static int live_of(gid_t16 g)
{
	int e = rec_entity(g);

	return e >= 0 ? e : rec_wake(g);
}

static int twin_mode(const char *root, const char *ma, const char *mb, const char *pairs)
{
	int sa, sb, mapb, i, f, ok = 0, a, b, ra, rb;
	wld_layer_t *L;
	const char *p = pairs;
	int door = -1, doorb = -1, di = 0;
	float d[3];

	if (boot(root, ma, &sa) < 0)
		return 2;
	mapb = wld_map_find(mb);
	if (mapb < 0)
		return 2;
	L = wld_open(mapb);
	sb = L->slot;
	rec_map_open(sb);
	for (f = 0; f < 600 && rec_kind(GID_MAKE(sb, 0)) < 0; f++)
		eng_frame();
	while (*p && ntwins < 16)
	{
		int la = atoi(p), lb;

		p = strchr(p, ':');
		if (!p)
			return 2;
		lb = atoi(++p);
		twin_a[ntwins] = GID_MAKE(sa, la);
		twin_b[ntwins] = GID_MAKE(sb, lb);
		ntwins++;
		while (*p && *p != ',')
			p++;
		if (*p == ',')
			p++;
	}
	for (i = 0; i < ntwins; i++)
	{
		a = live_of(twin_a[i]);
		b = live_of(twin_b[i]);
		if (a < 0 || b < 0 || ent_idc(a)->cls != ent_idc(b)->cls)
		{
			FAIL("twin %d:%d not both live / same class\n", GID_LUMP(twin_a[i]),
				GID_LUMP(twin_b[i]));
			return 1;
		}
		if (ent_idc(a)->cls == CLS_FUNC_DOOR && door < 0)
		{
			door = a;
			doorb = b;
			di = i;
		}
	}
	if (door < 0)
	{
		FAIL("twin: no func_door pair\n");
		return 1;
	}
	/* A's door moves: Use, a few frames */
	game_use(door, ENT_PLAYER, ENT_PLAYER, USE_TOGGLE, 0);
	for (f = 0; f < 5; f++)
		eng_frame();
	log_printf("gamels twin A door %d toggle=%d vel=%.1f %.1f %.1f\n", door,
		PRIV(CBaseToggle, door)->m_toggle_state, (double)ent_mot(door)->velocity[0],
		(double)ent_mot(door)->velocity[1], (double)ent_mot(door)->velocity[2]);
	door = live_of(twin_a[di]);
	/* the seam's component copy, then the game's, for every pair */
	for (i = 0; i < ntwins; i++)
	{
		float oa[3], ob[3], o[3];

		a = live_of(twin_a[i]);
		b = live_of(twin_b[i]);
		rec_origin(twin_a[i], oa);
		rec_origin(twin_b[i], ob);
		VecSub(ent_cv(a)->origin, oa, o);
		VecAdd(ob, o, o);
		ent_set_origin(b, o);
		VecCopy(ent_cv(a)->angles, ent_cv(b)->angles);
		ent_set_velocity(b, ent_mot(a)->velocity);
		ent_set_avelocity(b, ent_mot(a)->avelocity);
		ent_tm(b)->ltime = ent_tm(a)->ltime;
		ent_think_at(b, ent_tm(a)->nextthink);
		ent_rnd(b)->frame = ent_rnd(a)->frame;
		{
			/* B's own spawn constants and names, before the copy */
			CBaseToggle *pb = PRIV(CBaseToggle, b);
			float p1[3];
			name_t tb = pb->target;

			VecCopy(pb->m_vecPosition1, p1);
			game_twin_copy(a, b);
			if (!VecEq(p1, pb->m_vecPosition1))
				FAIL("twin %s: B's m_vecPosition1 overwritten\n", cname(ent_idc(b)->cls));
			if (pb->target != NAME_NONE && NAME_SLOT(pb->target) != (unsigned)sb)
				FAIL("twin %s: target not a name of B's map\n", cname(ent_idc(b)->cls));
			if (tb != NAME_NONE && pb->target == NAME_NONE)
				FAIL("twin %s: B lost its target\n", cname(ent_idc(b)->cls));
		}
	}
	{
		CBaseToggle *pa, *pb;

		door = live_of(twin_a[di]);
		doorb = live_of(twin_b[di]);
		pa = PRIV(CBaseToggle, door);
		pb = PRIV(CBaseToggle, doorb);
		float da[3];

		VecSub(ent_cv(doorb)->origin, ent_cv(door)->origin, d);
		VecAdd(pa->m_vecFinalDest, d, da);
		if (pa->m_toggle_state != pb->m_toggle_state)
			FAIL("twin door: toggle %d vs %d\n", pa->m_toggle_state, pb->m_toggle_state);
		if (!VecEq(da, pb->m_vecFinalDest))
			FAIL("twin door: final dest not A's + d\n");
		if (ent_tm(door)->think != ent_tm(doorb)->think ||
		    pa->m_pfnCallWhenMoveDone != pb->m_pfnCallWhenMoveDone)
			FAIL("twin door: think / move-done not copied\n");
		if (EHGet(pb->m_hActivator) != EHGet(pa->m_hActivator))
			FAIL("twin door: activator not copied\n");
	}
	/* both run: they stay in step (displacement from their record origins) */
	for (f = 0; f < 60; f++)
	{
		eng_frame();
		for (i = 0; i < ntwins; i++)
		{
			float oa[3], ob[3], da[3], db[3];

			a = live_of(twin_a[i]);
			b = live_of(twin_b[i]);
			rec_origin(twin_a[i], oa);
			rec_origin(twin_b[i], ob);
			VecSub(ent_cv(a)->origin, oa, da);
			VecSub(ent_cv(b)->origin, ob, db);
			if (fabsf(da[0] - db[0]) > 0.01f || fabsf(da[1] - db[1]) > 0.01f ||
			    fabsf(da[2] - db[2]) > 0.01f)
			{
				FAIL("twin %s lumps %d:%d out of step at frame %d\n",
					cname(ent_idc(a)->cls), GID_LUMP(twin_a[i]), GID_LUMP(twin_b[i]), f);
				f = 60;
				break;
			}
		}
	}
	for (i = 0; i < ntwins; i++)
	{
		a = live_of(twin_a[i]);
		b = live_of(twin_b[i]);
		if (PRIV(CBaseToggle, a)->m_toggle_state == PRIV(CBaseToggle, b)->m_toggle_state)
			ok++;
		else
			FAIL("twin %s: toggle %d vs %d after 60 frames\n", cname(ent_idc(a)->cls),
				PRIV(CBaseToggle, a)->m_toggle_state, PRIV(CBaseToggle, b)->m_toggle_state);
	}
	log_printf("gamels twin pairs=%d in-step=%d\n", ntwins, ok);

	/* cut refs: A -> B references go, A -> A and B -> A stay */
	ra = live_of(twin_a[0]);
	rb = live_of(twin_b[0]);
	{
		int monst = -1, e;
		CBaseToggle *pa, *pb;

		door = live_of(twin_a[di]);
		doorb = live_of(twin_b[di]);
		pa = PRIV(CBaseToggle, door);
		pb = PRIV(CBaseToggle, doorb);

		for (e = ENT_PLAYER + 1; e < HL64_MAX_ENTITIES && monst < 0; e++)
		{
			const sv_table_t *t;

			if (!ent_is_live(e) || ent_idc(e)->layer != sa)
				continue;
			for (t = sv_table_of(e); t && t != &sv_CBaseMonster; t = t->base)
				;
			if (t)
				monst = e;
		}
		pa->m_hActivator = EHANDLE(rb);	/* A -> B */
		pa->enemy = EHANDLE(ra);	/* A -> A */
		pb->m_hActivator = EHANDLE(ra);	/* B -> A */
		ent_cv(door)->owner = (uint16_t)rb;
		Player()->dmg_inflictor = EHANDLE(rb);
		if (monst >= 0)
			MON(monst)->m_hEnemy = EHANDLE(rb);
		game_cut_refs(sb);
		if (pa->m_hActivator != EH_NONE || ent_cv(door)->owner != COL_NONE ||
		    Player()->dmg_inflictor != EH_NONE || (monst >= 0 && MON(monst)->m_hEnemy != EH_NONE))
			FAIL("cut: an A -> B reference survived\n");
		else if (EHGet(pa->enemy) != ra || EHGet(pb->m_hActivator) != ra)
			FAIL("cut: an A -> A or B -> A reference was cut\n");
		else
			log_printf("gamels cut ok (monster %s)\n", monst >= 0 ? cname(ent_idc(monst)->cls) :
				"none");
	}
	log_printf("gamels done twin fails=%d\n", fails);
	log_flush();
	return fails ? 1 : 0;
}

int main(int argc, char **argv)
{
	int frames = 90;

	if (argc >= 5 && !strcmp(argv[1], "--root") && !strcmp(argv[3], "--map"))
	{
		if (argc == 7 && !strcmp(argv[5], "--frames"))
			frames = atoi(argv[6]);
		return map_mode(argv[2], argv[4], frames);
	}
	if (argc == 7 && !strcmp(argv[1], "--root") && !strcmp(argv[3], "--twin"))
		return twin_mode(argv[2], argv[4], argv[5], argv[6]);
	fprintf(stderr, "usage: hl64-gamels --root CONTENT --map MAP [--frames N]\n"
		"       hl64-gamels --root CONTENT --twin A B LA:LB[,LA:LB...]\n");
	return 2;
}
