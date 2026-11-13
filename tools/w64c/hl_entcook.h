/* SPDX-License-Identifier: MIT */
/*
 * hl_entcook.h — GoldSrc (BSP30) entity cook: BSP entity-lump text → one
 * self-contained binary entity chunk ("HLE1",
 * docs/formats/hle-format-v1.md), the .w64 package's ENTS chunk
 * (docs/formats/w64-format.md). Used by the offline cooker
 * (tools/w64c/hlecook.c); the engine's record module needs only the format
 * part (define HLE_FORMAT_ONLY first). Pure C99, no engine dependencies;
 * include from exactly one translation unit per program.
 *
 * The chunk is position independent (offsets/indices only) and is used in
 * place after one endian fix-up pass (hle_fixup): per-map interned strings,
 * records with a class id and typed key values, targetname → record lists,
 * multi_manager fire lists (sorted by delay), multisource input lists.
 *
 * Cook rules:
 *   classes   only hle_classes[] are kept (what the native game spawns)
 *   keys      the HLSDK entvars fields the game fills from entity keys
 *             (hle_fields[]) and the keys HLSDK classes read in their own
 *             KeyValue (hle_xkeys[]) are kept, everything else dropped;
 *             leading '_', and "", "0", "0 0 0" values (GoldSrc zero-inits
 *             entity memory) are dropped; "angle" → "angles" "0 a 0";
 *             "\n" escapes are expanded
 *   rewrites  trigger_changelevel landmark → message; info_landmark
 *             targetname copied to message; multi_manager non-field keys
 *             → fire list (name up to '#', delay), stable-sorted by delay
 *   prune     logic classes that only act when used and have no
 *             targetname; info_target / path_corner /
 *             info_teleport_destination whose name nothing in the map
 *             references (repeated to a fixed point)
 *
 * Hostile input: an entity block without its closing brace, a value without
 * its closing quote or text outside the blocks is rejected (hle_build
 * returns the reason).
 */
#ifndef HL_ENTCOOK_H
#define HL_ENTCOOK_H

#include <stdint.h>
#include <string.h>

#define HLE_MAGIC	0x31454C48u	/* bytes "HLE1" read as a host u32 (LE) */
#define HLE_MAGIC_SW	0x484C4531u	/* the same bytes on the other order */
#define HLE_VERSION	1
#define HLE_F_BIGENDIAN	1		/* header.flags: multi-byte fields BE */
#define HLE_F_IDS	2		/* the id step ran: class / keyname / key ids */

#define HLE_NONE	0xFFFFu		/* u16 "no index" */
#define HLE_MAX_LUMP	1024		/* lump < 1024: gid = slot << 10 | lump */

/* key value kinds (keyname.kind) */
#define HLE_KF		1	/* float: val = IEEE bits */
#define HLE_KV		2	/* vec3: val = index of 3 floats */
#define HLE_KS		3	/* string: val = string offset */
#define HLE_KN		4	/* name: val = name index (string = name's) */

/* class / record flags */
#define HLE_CF_LOGIC	0x0001	/* no entity: logic table record */
#define HLE_CF_POINT	0x0002	/* no entity: point record (origin/angles) */
#define HLE_RF_PIN	0x0004	/* streaming: keep resident (script refs) */
#define HLE_RF_PERSIST	0x0008	/* seam: landmark / changelevel */
#define HLE_RF_AUXFIRE	0x0010	/* aux = fire[] (else lumplist[]) */

/* name flags */
#define HLE_NF_RUNTIME	0x0001	/* also given to runtime entities (netname) */

typedef struct
{
	uint32_t	magic;
	uint16_t	version, flags;
	uint32_t	size;		/* whole chunk, bytes */
	uint32_t	srchash;	/* FNV-1a of the source entity text */
	uint16_t	numlumps;	/* BSP entities (kept + dropped) */
	uint16_t	numrecs;
	uint16_t	numclasses, numkeynames;
	uint16_t	numnames, hashsize;	/* hashsize: power of two */
	uint32_t	numkeys, numlumplist, numfire, numfloats;
	uint32_t	strsize, namestr_end;	/* names region = [0, namestr_end) */
	uint32_t	ofs_recs, ofs_lump2rec, ofs_classes, ofs_keynames;
	uint32_t	ofs_names, ofs_hash, ofs_keys, ofs_lumplist;
	uint32_t	ofs_fire, ofs_floats, ofs_strings;
} hle_header_t;			/* 96 bytes */

typedef struct
{
	uint16_t	lump;		/* BSP entity index */
	uint16_t	cls;		/* class index */
	uint16_t	tname;		/* name index of targetname, HLE_NONE */
	uint16_t	flags;		/* HLE_CF_* of the class | HLE_RF_* */
	uint16_t	key_first, key_num;
	uint16_t	aux_first, aux_count;	/* multi_manager: fire[];
						   multisource: lumplist[] */
	uint16_t	sector;		/* streaming sector, HLE_NONE = global */
	uint16_t	trank;		/* trigger touch rank (tools/w64c/touchrank.py), NONE */
	uint16_t	res_first, res_count;	/* streaming precache refs */
} hle_rec_t;			/* 24 bytes */

typedef struct
{
	uint32_t	str;
	uint16_t	flags;
	uint16_t	id;		/* classes.def id (cls_t), 0xFFFF none */
} hle_class_t;

typedef struct
{
	uint32_t	str;
	uint16_t	kind;
	uint16_t	id;		/* keys.def id (key_t16) */
} hle_keyname_t;

typedef struct
{
	uint32_t	str;		/* in the names region, u16 index before */
	uint16_t	first, count;	/* lumplist[]: records with this name */
	uint16_t	flags, pad;
} hle_name_t;

/* key.id (HLE_F_IDS, hle-format-v1.md "Ids"): HLE_KS values: res_t of a
 * resource value (submodel '*N' as RES_K_SUBMODEL index N), the pattern id
 * of a `pattern` key, the global-state id of a `globalstate` key; HLE_KN
 * values: the classes.def id of a classname value; 0xFFFF none */
typedef struct
{
	uint16_t	kn;		/* keyname index */
	uint16_t	id;		/* typed id, 0xFFFF none */
	uint32_t	val;
} hle_key_t;

typedef struct
{
	uint16_t	name;		/* name index */
	uint16_t	pad;
	float		delay;
} hle_fire_t;

/* names and the runtime hash agree on this */
static inline uint32_t hle_hash (const char *s)
{
	uint32_t	h = 2166136261u;

	while (*s)
		h = (h ^ (uint8_t)*s++) * 16777619u;
	return h;
}

static inline uint16_t hle_sw16 (uint16_t v)
{
	return (uint16_t)((v >> 8) | (v << 8));
}

static inline uint32_t hle_sw32 (uint32_t v)
{
	return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u)
		| (v << 24);
}

static inline int hle_host_big (void)
{
	const uint16_t	one = 1;

	return *(const uint8_t *)&one == 0;
}

static inline void hle_sw16a (void *p, uint32_t n)
{
	uint16_t	*a = p;

	while (n--)
	{
		*a = hle_sw16 (*a);
		a++;
	}
}

static inline void hle_sw32a (void *p, uint32_t n)
{
	uint32_t	*a = p;

	while (n--)
	{
		*a = hle_sw32 (*a);
		a++;
	}
}

/* swap every multi-byte field after the header (header already host) */
static inline void hle_swap_body (uint8_t *b, const hle_header_t *h, int to_host)
{
	uint32_t	i;
	hle_name_t	*nm = (hle_name_t *)(b + h->ofs_names);

	hle_sw16a (b + h->ofs_recs, (uint32_t)h->numrecs * 12);
	hle_sw16a (b + h->ofs_lump2rec, h->numlumps);
	for (i = 0; i < h->numclasses; i++)
	{
		hle_class_t *c = (hle_class_t *)(b + h->ofs_classes) + i;
		c->str = hle_sw32 (c->str);
		c->flags = hle_sw16 (c->flags);
		c->id = hle_sw16 (c->id);
	}
	for (i = 0; i < h->numkeynames; i++)
	{
		hle_keyname_t *k = (hle_keyname_t *)(b + h->ofs_keynames) + i;
		k->str = hle_sw32 (k->str);
		k->kind = hle_sw16 (k->kind);
		k->id = hle_sw16 (k->id);
	}
	for (i = 0; i < h->numnames; i++)
	{
		/* the u16 index in front of the name string; read the offset
		 * in host order */
		uint32_t	s = to_host ? hle_sw32 (nm[i].str) : nm[i].str;
		uint16_t	*pre = (uint16_t *)(b + h->ofs_strings + s - 2);

		*pre = hle_sw16 (*pre);
		nm[i].str = hle_sw32 (nm[i].str);
		nm[i].first = hle_sw16 (nm[i].first);
		nm[i].count = hle_sw16 (nm[i].count);
		nm[i].flags = hle_sw16 (nm[i].flags);
	}
	hle_sw16a (b + h->ofs_hash, h->hashsize);
	for (i = 0; i < h->numkeys; i++)
	{
		hle_key_t *k = (hle_key_t *)(b + h->ofs_keys) + i;
		k->kn = hle_sw16 (k->kn);
		k->id = hle_sw16 (k->id);
		k->val = hle_sw32 (k->val);
	}
	hle_sw16a (b + h->ofs_lumplist, h->numlumplist);
	for (i = 0; i < h->numfire; i++)
	{
		hle_fire_t *f = (hle_fire_t *)(b + h->ofs_fire) + i;
		f->name = hle_sw16 (f->name);
		hle_sw32a (&f->delay, 1);
	}
	hle_sw32a (b + h->ofs_floats, h->numfloats);
}

static inline void hle_swap_header (hle_header_t *h)
{
	uint16_t	*p16 = &h->version;
	int		i;

	h->magic = hle_sw32 (h->magic);
	p16[0] = hle_sw16 (p16[0]);
	p16[1] = hle_sw16 (p16[1]);
	h->size = hle_sw32 (h->size);
	h->srchash = hle_sw32 (h->srchash);
	p16 = &h->numlumps;
	for (i = 0; i < 6; i++)
		p16[i] = hle_sw16 (p16[i]);
	hle_sw32a (&h->numkeys, 17);
}

static inline int hle_range (const hle_header_t *h, uint32_t ofs, uint32_t n,
	uint32_t sz)
{
	return ofs <= h->size && (ofs & 3) == 0 && n <= (h->size - ofs) / sz;
}

/*
 * Validate a chunk and bring it to host byte order in place (one pass,
 * nothing when it already is). Returns 0 on success, else a reason.
 * Hostile-input safe: every offset, count and index is range checked.
 */
static inline const char *hle_fixup (void *blob, uint32_t len)
{
	uint8_t		*b = blob;
	hle_header_t	*h = blob;
	uint32_t	i, s;
	int		big;

	if (!blob || len < sizeof (*h) || ((uintptr_t)blob & 3))
		return "short or unaligned chunk";
	if (h->magic == HLE_MAGIC_SW)
	{
		hle_swap_header (h);
		big = !hle_host_big ();
	}
	else if (h->magic == HLE_MAGIC)
		big = hle_host_big ();
	else
		return "bad magic";
	if (h->version != HLE_VERSION)
		return "unsupported version";
	if (((h->flags & HLE_F_BIGENDIAN) != 0) != big)
		return "byte-order flag mismatch";
	if (h->size > len)
		return "truncated";
	if (!hle_range (h, h->ofs_recs, h->numrecs, sizeof (hle_rec_t))
		|| !hle_range (h, h->ofs_lump2rec, h->numlumps, 2)
		|| !hle_range (h, h->ofs_classes, h->numclasses, sizeof (hle_class_t))
		|| !hle_range (h, h->ofs_keynames, h->numkeynames,
			sizeof (hle_keyname_t))
		|| !hle_range (h, h->ofs_names, h->numnames, sizeof (hle_name_t))
		|| !hle_range (h, h->ofs_hash, h->hashsize, 2)
		|| !hle_range (h, h->ofs_keys, h->numkeys, sizeof (hle_key_t))
		|| !hle_range (h, h->ofs_lumplist, h->numlumplist, 2)
		|| !hle_range (h, h->ofs_fire, h->numfire, sizeof (hle_fire_t))
		|| !hle_range (h, h->ofs_floats, h->numfloats, 4)
		|| !hle_range (h, h->ofs_strings, h->strsize, 1)
		|| h->namestr_end > h->strsize || !h->strsize
		|| b[h->ofs_strings + h->strsize - 1] != 0
		|| !h->hashsize || (h->hashsize & (h->hashsize - 1))
		|| h->numlumps > HLE_MAX_LUMP)
		return "section out of range";
	/* name strings: index prefix inside the names region, 2-aligned */
	for (i = 0; i < h->numnames; i++)
	{
		const hle_name_t *n = (const hle_name_t *)(b + h->ofs_names) + i;
		uint32_t ns = h->magic == HLE_MAGIC && big != hle_host_big ()
			? hle_sw32 (n->str) : n->str;
		if (ns < 2 || ns >= h->namestr_end || (ns & 1))
			return "name string out of range";
	}
	if (big != hle_host_big ())
		hle_swap_body (b, h, 1);
	h->flags = (uint16_t)((h->flags & ~HLE_F_BIGENDIAN)
		| (hle_host_big () ? HLE_F_BIGENDIAN : 0));
	/* indices */
	for (i = 0; i < h->numrecs; i++)
	{
		const hle_rec_t *r = (const hle_rec_t *)(b + h->ofs_recs) + i;
		if (r->lump >= h->numlumps || r->cls >= h->numclasses
			|| (r->tname != HLE_NONE && r->tname >= h->numnames)
			|| (uint32_t)r->key_first + r->key_num > h->numkeys)
			return "record index out of range";
		if ((uint32_t)r->aux_first + r->aux_count
			> ((r->flags & HLE_RF_AUXFIRE) ? h->numfire : h->numlumplist))
			return "record aux out of range";
	}
	for (i = 0; i < h->numlumps; i++)
	{
		uint16_t v = ((const uint16_t *)(b + h->ofs_lump2rec))[i];
		if (v != HLE_NONE && v >= h->numrecs)
			return "lump map out of range";
	}
	for (i = 0; i < h->numclasses; i++)
		if (((const hle_class_t *)(b + h->ofs_classes))[i].str >= h->strsize)
			return "class string out of range";
	for (i = 0; i < h->numkeynames; i++)
	{
		const hle_keyname_t *k = (const hle_keyname_t *)(b + h->ofs_keynames) + i;
		if (k->str >= h->strsize || k->kind < HLE_KF || k->kind > HLE_KN)
			return "keyname out of range";
	}
	for (i = 0; i < h->numnames; i++)
	{
		const hle_name_t *n = (const hle_name_t *)(b + h->ofs_names) + i;
		if ((uint32_t)n->first + n->count > h->numlumplist
			|| ((const uint16_t *)(b + h->ofs_strings + n->str))[-1] != i)
			return "name list out of range";
	}
	for (i = 0; i < h->hashsize; i++)
	{
		uint16_t v = ((const uint16_t *)(b + h->ofs_hash))[i];
		if (v != HLE_NONE && v >= h->numnames)
			return "hash out of range";
	}
	for (i = 0; i < h->numkeys; i++)
	{
		const hle_key_t *k = (const hle_key_t *)(b + h->ofs_keys) + i;
		uint16_t kind;
		if (k->kn >= h->numkeynames)
			return "key name out of range";
		kind = ((const hle_keyname_t *)(b + h->ofs_keynames))[k->kn].kind;
		s = k->val;
		if ((kind == HLE_KS && s >= h->strsize)
			|| (kind == HLE_KN && s >= h->numnames)
			|| (kind == HLE_KV && (s > h->numfloats || h->numfloats - s < 3)))
			return "key value out of range";
	}
	for (i = 0; i < h->numlumplist; i++)
		if (((const uint16_t *)(b + h->ofs_lumplist))[i] >= h->numlumps)
			return "lump list out of range";
	for (i = 0; i < h->numfire; i++)
		if (((const hle_fire_t *)(b + h->ofs_fire))[i].name >= h->numnames)
			return "fire list out of range";
	return 0;
}

#ifndef HLE_FORMAT_ONLY
/*================================================================ cook */

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

/* classes the native game spawns (case-insensitive) and their kind */
static const struct { const char *name; uint16_t flags; } hle_classes[] = {
	/* world / spawn / portals */
	{"worldspawn", 0}, {"info_player_start", HLE_CF_POINT},
	{"info_player_deathmatch", HLE_CF_POINT},
	{"info_player_coop", HLE_CF_POINT},
	{"info_teleport_destination", HLE_CF_POINT},
	{"info_landmark", HLE_CF_POINT | HLE_RF_PERSIST},
	{"info_target", HLE_CF_POINT}, {"path_corner", 0}, {"path_track", 0},
	{"trigger_changelevel", HLE_RF_PERSIST}, {"trigger_transition", 0},
	{"light", HLE_CF_LOGIC}, {"light_spot", HLE_CF_LOGIC},
	{"light_fluoro", HLE_CF_LOGIC}, {"light_fluorospark", HLE_CF_LOGIC},
	/* brushes */
	{"func_door", 0}, {"func_door_rotating", 0}, {"func_wall", 0},
	{"func_wall_toggle", 0}, {"func_button", 0}, {"func_rot_button", 0},
	{"momentary_door", 0}, {"momentary_rot_button", 0}, {"func_train", 0},
	{"func_plat", 0}, {"func_illusionary", 0}, {"func_water", 0},
	{"func_breakable", 0}, {"func_pushable", 0}, {"func_rotating", 0},
	{"func_pendulum", 0}, {"func_healthcharger", 0}, {"func_recharge", 0},
	{"func_ladder", 0}, {"func_guntarget", 0},
	/* triggers / logic */
	{"trigger_teleport", 0}, {"trigger_once", 0}, {"trigger_multiple", 0},
	{"trigger_relay", HLE_CF_LOGIC}, {"trigger_hurt", 0},
	{"trigger_push", 0}, {"trigger_counter", 0},
	{"trigger_auto", HLE_CF_LOGIC}, {"multi_manager", HLE_CF_LOGIC},
	{"multisource", HLE_CF_LOGIC}, {"env_global", HLE_CF_LOGIC},
	{"trigger_endsection", 0},
	/* scripting */
	{"scripted_sequence", HLE_RF_PIN}, {"aiscripted_sequence", HLE_RF_PIN},
	{"scripted_sentence", HLE_CF_LOGIC}, {"monstermaker", HLE_CF_LOGIC | HLE_RF_PIN},
	/* monsters */
	{"monster_cockroach", 0}, {"monster_scientist", 0},
	{"monster_scientist_dead", 0}, {"monster_headcrab", 0},
	{"monster_zombie", 0}, {"monster_houndeye", 0}, {"monster_barney", 0},
	{"monster_gman", 0}, {"monster_gargantua", 0}, {"monster_barnacle", 0},
	{"monster_human_grunt", 0}, {"monster_hgrunt_dead", 0},
	{"monster_grunt_repel", 0}, {"monster_tripmine", 0},
	{"monster_generic", 0}, {"monster_rat", 0}, {"monster_miniturret", 0},
	/* items / ammo / weapons */
	{"item_suit", 0}, {"item_healthkit", 0}, {"item_battery", 0},
	{"item_longjump", 0}, {"world_items", 0},
	{"ammo_9mmAR", 0}, {"ammo_mp5clip", 0}, {"ammo_9mmclip", 0},
	{"ammo_9mmbox", 0}, {"ammo_buckshot", 0}, {"ammo_357", 0},
	{"ammo_ARgrenades", 0}, {"ammo_mp5grenades", 0},
	{"weapon_crowbar", 0}, {"weapon_9mmhandgun", 0}, {"weapon_glock", 0},
	{"weapon_357", 0}, {"weapon_shotgun", 0}, {"weapon_9mmAR", 0},
	{"weapon_mp5", 0}, {"weapon_handgrenade", 0}, {"weapon_tripmine", 0},
	{"weaponbox", 0},
	{"player_weaponstrip", HLE_CF_LOGIC},
	/* sound / fx (env_beam, env_laser are dropped: no N64 beam renderer);
	 * GoldSrc shows placed decals and glows (HLSDK CDecal, CGlow) */
	{"ambient_generic", 0}, {"speaker", 0}, {"env_beverage", 0},
	{"env_sprite", 0}, {"env_glow", 0}, {"infodecal", 0},
	{"env_explosion", HLE_CF_LOGIC}, {"env_shake", HLE_CF_LOGIC},
	{"env_fade", HLE_CF_LOGIC}, {"env_message", HLE_CF_LOGIC},
	{"env_render", HLE_CF_LOGIC}, {"env_spark", HLE_CF_LOGIC},
	{"env_shooter", HLE_CF_LOGIC}, {"gibshooter", HLE_CF_LOGIC},
	{NULL, 0}
};

/* logic classes that do nothing unless used: no targetname → pruned */
static const char * const hle_needs_name[] = {
	"multi_manager", "trigger_relay", "multisource", "env_render",
	"env_shake", "env_fade", "env_message", "player_weaponstrip",
	"env_explosion", "env_shooter", "gibshooter", "light", "light_spot",
	"env_beverage", NULL
};

/* point classes that only matter when a key in the map names them */
static const char * const hle_needs_ref[] = {
	"info_target", "path_corner", "path_track", "info_teleport_destination",
	NULL
};

/*
 * entvars fields: the HLSDK entvars_t members the game fills from entity
 * keys before any class sees them (dlls/util.cpp gEntvarsDescription, via
 * cbase.cpp DispatchKeyValue → EntvarsKeyvalue), in that table's order;
 * the FIELD_EDICT members are left out (EntvarsKeyvalue rejects them).
 * FIELD_INTEGER / FLOAT / TIME → HLE_KF, VECTOR / POSITION_VECTOR → HLE_KV,
 * STRING / MODELNAME / SOUNDNAME → HLE_KS; target / targetname → HLE_KN.
 */
static const struct { const char *name; uint16_t kind; } hle_fields[] = {
	{"classname",HLE_KS}, {"globalname",HLE_KS}, {"origin",HLE_KV},
	{"oldorigin",HLE_KV}, {"velocity",HLE_KV}, {"basevelocity",HLE_KV},
	{"movedir",HLE_KV}, {"angles",HLE_KV}, {"avelocity",HLE_KV},
	{"punchangle",HLE_KV}, {"v_angle",HLE_KV}, {"fixangle",HLE_KF},
	{"idealpitch",HLE_KF}, {"pitch_speed",HLE_KF}, {"ideal_yaw",HLE_KF},
	{"yaw_speed",HLE_KF}, {"modelindex",HLE_KF}, {"model",HLE_KS},
	{"viewmodel",HLE_KS}, {"weaponmodel",HLE_KS}, {"absmin",HLE_KV},
	{"absmax",HLE_KV}, {"mins",HLE_KV}, {"maxs",HLE_KV}, {"size",HLE_KV},
	{"ltime",HLE_KF}, {"nextthink",HLE_KF}, {"solid",HLE_KF},
	{"movetype",HLE_KF}, {"skin",HLE_KF}, {"body",HLE_KF},
	{"effects",HLE_KF}, {"gravity",HLE_KF}, {"friction",HLE_KF},
	{"light_level",HLE_KF}, {"frame",HLE_KF}, {"scale",HLE_KF},
	{"sequence",HLE_KF}, {"animtime",HLE_KF}, {"framerate",HLE_KF},
	{"controller",HLE_KF}, {"blending",HLE_KF}, {"rendermode",HLE_KF},
	{"renderamt",HLE_KF}, {"rendercolor",HLE_KV}, {"renderfx",HLE_KF},
	{"health",HLE_KF}, {"frags",HLE_KF}, {"weapons",HLE_KF},
	{"takedamage",HLE_KF}, {"deadflag",HLE_KF}, {"view_ofs",HLE_KV},
	{"button",HLE_KF}, {"impulse",HLE_KF}, {"spawnflags",HLE_KF},
	{"flags",HLE_KF}, {"colormap",HLE_KF}, {"team",HLE_KF},
	{"max_health",HLE_KF}, {"teleport_time",HLE_KF}, {"armortype",HLE_KF},
	{"armorvalue",HLE_KF}, {"waterlevel",HLE_KF}, {"watertype",HLE_KF},
	{"target",HLE_KN}, {"targetname",HLE_KN}, {"netname",HLE_KS},
	{"message",HLE_KS}, {"dmg_take",HLE_KF}, {"dmg_save",HLE_KF},
	{"dmg",HLE_KF}, {"dmgtime",HLE_KF}, {"noise",HLE_KS},
	{"noise1",HLE_KS}, {"noise2",HLE_KS}, {"noise3",HLE_KS},
	{"speed",HLE_KF}, {"air_finished",HLE_KF}, {"pain_finished",HLE_KF},
	{"radsuit_finished",HLE_KF},
	{NULL, 0}
};

/* keys HLSDK classes read in their own KeyValue (the dlls/ sources), for
 * the classes the native game spawns */
static const struct { const char *name; uint16_t kind; } hle_xkeys[] = {
	/* strings */
	{"changetarget",HLE_KS}, {"monstertype",HLE_KS}, {"globalstate",HLE_KS},
	{"section",HLE_KS}, {"m_iszPlay",HLE_KS}, {"m_iszIdle",HLE_KS},
	{"spawnorigin",HLE_KS}, {"messagesound",HLE_KS},
	{"shootmodel",HLE_KS}, {"sentence",HLE_KS},
	{"UseSentence",HLE_KS}, {"UnUseSentence",HLE_KS}, {"map",HLE_KS},
	/* names (targetname or classname references) */
	{"master",HLE_KN}, {"m_iszEntity",HLE_KN}, {"TriggerTarget",HLE_KN},
	{"entity",HLE_KN}, {"listener",HLE_KN}, {"killtarget",HLE_KN},
	/* numbers */
	{"wait",HLE_KF}, {"delay",HLE_KF}, {"lip",HLE_KF}, {"sounds",HLE_KF},
	{"style",HLE_KF}, {"height",HLE_KF}, {"distance",HLE_KF},
	{"volume",HLE_KF}, {"count",HLE_KF},
	{"changedelay",HLE_KF}, {"damage",HLE_KF}, {"material",HLE_KF},
	{"explodemagnitude",HLE_KF}, {"explosion",HLE_KF}, {"spawnobject",HLE_KF},
	{"iMagnitude",HLE_KF}, {"amplitude",HLE_KF}, {"frequency",HLE_KF},
	{"duration",HLE_KF}, {"radius",HLE_KF}, {"holdtime",HLE_KF},
	{"messagevolume",HLE_KF}, {"messageattenuation",HLE_KF},
	{"MaxDelay",HLE_KF}, {"m_iGibs",HLE_KF}, {"m_flVelocity",HLE_KF},
	{"m_flVariance",HLE_KF}, {"m_flGibLife",HLE_KF},
	{"pose",HLE_KF}, {"monstercount",HLE_KF}, {"m_imaxlivechildren",HLE_KF},
	{"TriggerCondition",HLE_KF}, {"movesnd",HLE_KF}, {"stopsnd",HLE_KF},
	{"returnspeed",HLE_KF}, {"fanfriction",HLE_KF}, {"damp",HLE_KF},
	{"m_fMoveTo",HLE_KF}, {"m_flRadius",HLE_KF}, {"refire",HLE_KF},
	{"attenuation",HLE_KF}, {"preset",HLE_KF}, {"triggermode",HLE_KF},
	{"initialstate",HLE_KF}, {"triggerstate",HLE_KF}, {"pitch",HLE_KF},
	{"pitchstart",HLE_KF}, {"spinup",HLE_KF}, {"spindown",HLE_KF},
	{"fadein",HLE_KF}, {"fadeout",HLE_KF}, {"volstart",HLE_KF},
	{"lfotype",HLE_KF}, {"type",HLE_KF}, {"damagetype",HLE_KF},
	/* M3 (game/defs/keys.def appends): the switchable lightstyle pattern,
	 * door / button sound-set indices, breakable gibs, env_shooter,
	 * pushables, ambient_generic LFO / spin-up, worldspawn */
	{"pattern",HLE_KS}, {"locked_sound",HLE_KF}, {"unlocked_sound",HLE_KF},
	{"locked_sentence",HLE_KF}, {"unlocked_sentence",HLE_KF},
	{"gibmodel",HLE_KS}, {"shootsounds",HLE_KF}, {"healthvalue",HLE_KF},
	{"buoyancy",HLE_KF}, {"lforate",HLE_KF}, {"lfomodpitch",HLE_KF},
	{"lfomodvol",HLE_KF}, {"cspinup",HLE_KF}, {"startdark",HLE_KF},
	{"WaveHeight",HLE_KF}, {"MaxRange",HLE_KF}, {"chaptertitle",HLE_KS},
	{"gametitle",HLE_KF}, {"m_flRepeat",HLE_KF}, {"m_iFinishSchedule",HLE_KF},
	/* monster_miniturret (CBaseTurret::KeyValue) */
	{"orientation",HLE_KF}, {"maxsleep",HLE_KF}, {"searchspeed",HLE_KF},
	{"turnrate",HLE_KF},
	/* weaponbox (CWeaponBox::KeyValue: every key an ammo name, PackAmmo
	 * of its count): the HLSDK ammo names of weapons.cpp / GetItemInfo */
	{"9mm",HLE_KF}, {"357",HLE_KF}, {"buckshot",HLE_KF}, {"bolts",HLE_KF},
	{"rockets",HLE_KF}, {"uranium",HLE_KF}, {"Hornets",HLE_KF},
	{"Hand Grenade",HLE_KF}, {"Satchel Charge",HLE_KF}, {"Trip Mine",HLE_KF},
	{"Snarks",HLE_KF}, {"ARgrenades",HLE_KF},
	/* infodecal (CDecal::KeyValue): the decals.wad name; the id step
	 * writes its decal id (hle-format-v1.md "Ids") */
	{"texture",HLE_KS},
	{NULL, 0}
};

/* multi_manager keys that stay real fields (everything else is a target) */
static const char * const hle_mm_fields[] = {
	"classname", "origin", "angles", "angle", "targetname",
	"spawnflags", "wait", "target", "killtarget", "message", NULL
};

typedef struct { const char *k, *v; uint16_t kind; } hlec_kv_t;
typedef struct { const char *name; float delay; size_t dofs; } hlec_fire_t;

typedef struct
{
	int		lump, cls, keep;
	const char	*tname, *cname;
	hlec_kv_t	*kv;
	int		nkv;
	hlec_fire_t	*fire;
	int		nfire;
} hlec_ent_t;

typedef struct
{
	char		**v;
	uint32_t	*ofs;
	int		n, cap;
	uint32_t	*hash;	/* open addressing: index + 1 */
	int		hcap;
} hlec_pool_t;

typedef struct
{
	uint8_t	*d;
	size_t	len, cap;
	int	fail;
} hlec_buf_t;

typedef struct
{
	hlec_ent_t	*ents;
	int		nents, cap;
	char		*text;		/* copied values, NUL-separated */
	size_t		tlen, tcap;
	int		fail;
} hlec_t;

static void *hlec_grow (void *p, int *cap, int need, size_t elt, int *fail)
{
	int	n = *cap ? *cap : 16;
	void	*q;

	if (need <= *cap)
		return p;
	while (n < need)
		n *= 2;
	q = realloc (p, (size_t)n * elt);
	if (!q)
	{
		*fail = 1;
		return p;
	}
	*cap = n;
	return q;
}

static int hlec_streqi (const char *a, const char *b)
{
	while (*a && *b && tolower ((unsigned char)*a)
		== tolower ((unsigned char)*b))
	{
		a++;
		b++;
	}
	return !*a && !*b;
}

static int hlec_class (const char *cn)
{
	int	i;

	for (i = 0; hle_classes[i].name; i++)
		if (hlec_streqi (cn, hle_classes[i].name))
			return i;
	return -1;
}

static int hlec_in (const char * const *list, const char *s)
{
	int	i;

	for (i = 0; list[i]; i++)
		if (!strcmp (list[i], s))
			return 1;
	return 0;
}

/* copy n bytes (expanding "\n") into the text store; returns offset+1 */
static size_t hlec_text (hlec_t *c, const char *s, int n)
{
	size_t	at;
	int	i;

	if (c->tlen + (size_t)n + 1 > c->tcap)
	{
		size_t	cap = c->tcap ? c->tcap : 4096;
		char	*q;

		while (cap < c->tlen + (size_t)n + 1)
			cap *= 2;
		q = realloc (c->text, cap);
		if (!q)
		{
			c->fail = 1;
			return 0;
		}
		c->text = q;
		c->tcap = cap;
	}
	at = c->tlen;
	for (i = 0; i < n; i++)
	{
		if (s[i] == '\\' && i + 1 < n && s[i + 1] == 'n')
		{
			c->text[c->tlen++] = '\n';
			i++;
		}
		else
			c->text[c->tlen++] = s[i];
	}
	c->text[c->tlen++] = 0;
	return at + 1;
}

static uint16_t hlec_keykind (const char *k)
{
	int	i;

	for (i = 0; hle_fields[i].name; i++)
		if (!strcmp (hle_fields[i].name, k))
			return hle_fields[i].kind;
	for (i = 0; hle_xkeys[i].name; i++)
		if (!strcmp (hle_xkeys[i].name, k))
			return hle_xkeys[i].kind;
	return 0;
}

static int hlec_zero (const char *v, int vl)
{
	return vl == 0 || (vl == 1 && v[0] == '0')
		|| (vl == 5 && !strncmp (v, "0 0 0", 5));
}

/*
 * Parse one "{...}" block. Strings are stored as text offsets (+1) until
 * the text store stops moving; hlec_finish_ptrs turns them into pointers.
 */
typedef struct { size_t k, v; uint16_t kind; } hlec_rkv_t;

static int hlec_parse_block (hlec_t *c, const char *p, const char *end,
	hlec_rkv_t **rkv, int *rkvcap)
{
	char		cn[64];
	const char	*q;
	int		ci, n = 0, is_mm, is_cl, is_lm;

	/* classname first */
	cn[0] = 0;
	for (q = p; q + 12 < end; q++)
	{
		if (q[0] == '"' && !strncmp (q + 1, "classname\"", 10))
		{
			int	l = 0;

			q += 11;
			while (q < end && isspace ((unsigned char)*q))
				q++;
			if (q >= end || *q != '"')
				return -1;
			q++;
			while (q < end && *q != '"' && l < (int)sizeof (cn) - 1)
				cn[l++] = *q++;
			cn[l] = 0;
			break;
		}
	}
	ci = cn[0] ? hlec_class (cn) : -1;
	if (ci < 0)
		return -1;
	is_mm = !strcmp (cn, "multi_manager");
	is_cl = !strcmp (cn, "trigger_changelevel");
	is_lm = !strcmp (cn, "info_landmark");
	q = p;
	if (q < end && *q == '{')
		q++;
	for (;;)
	{
		const char	*ks, *vs;
		int		kl, vl, i;
		char		key[64], val[64];
		uint16_t	kind;

		while (q < end && isspace ((unsigned char)*q))
			q++;
		if (q >= end || *q != '"')
			break;
		ks = ++q;
		while (q < end && *q != '"')
			q++;
		if (q >= end)
			break;
		kl = (int)(q - ks);
		q++;
		while (q < end && isspace ((unsigned char)*q))
			q++;
		if (q >= end || *q != '"')
			break;
		vs = ++q;
		while (q < end && *q != '"')
			q++;
		if (q >= end)
			break;
		vl = (int)(q - vs);
		q++;
		/* trailing spaces in key names are not part of the name */
		while (kl > 0 && ks[kl - 1] == ' ')
			kl--;
		if (kl <= 0 || ks[0] == '_' || kl >= (int)sizeof (key))
			continue;
		memcpy (key, ks, (size_t)kl);
		key[kl] = 0;

		if (is_mm && !hlec_in (hle_mm_fields, key))
		{
			/* fire target: name (strip #suffix) = delay; kind 0 */
			for (i = 0; i < kl && key[i] != '#'; i++)
				;
			if (i == 0)
				continue;
			*rkv = hlec_grow (*rkv, rkvcap, n + 1, sizeof (**rkv), &c->fail);
			if (c->fail)
				return -1;
			(*rkv)[n].k = hlec_text (c, key, i);
			(*rkv)[n].v = hlec_text (c, vs, vl);
			(*rkv)[n].kind = 0;
			n++;
			continue;
		}
		if (is_mm && !strcmp (key, "message"))
			continue;	/* GoldSrc multi_manager has no message */
		if (strcmp (key, "classname") && hlec_zero (vs, vl))
			continue;
		if (is_cl && !strcmp (key, "landmark"))
			strcpy (key, "message");
		if (!strcmp (key, "angle"))
		{
			int l = vl < 40 ? vl : 40;

			strcpy (key, "angles");
			memcpy (val, "0 ", 2);
			memcpy (val + 2, vs, (size_t)l);
			memcpy (val + 2 + l, " 0", 3);
			vs = val;
			vl = (int)strlen (val);
		}
		kind = hlec_keykind (key);
		if (!kind)
			continue;	/* nothing reads it */
		for (i = 0; i < 2; i++)
		{
			*rkv = hlec_grow (*rkv, rkvcap, n + 1, sizeof (**rkv), &c->fail);
			if (c->fail)
				return -1;
			(*rkv)[n].k = hlec_text (c, key, (int)strlen (key));
			(*rkv)[n].v = hlec_text (c, vs, vl);
			(*rkv)[n].kind = kind;
			n++;
			/* info_landmark: targetname also copied to message */
			if (!(is_lm && !strcmp (key, "targetname")))
				break;
			strcpy (key, "message");
			kind = HLE_KS;
		}
	}
	return n;
}

static void hlec_put (hlec_buf_t *b, const void *p, size_t n)
{
	if (b->fail)
		return;
	if (b->len + n > b->cap)
	{
		size_t	cap = b->cap ? b->cap : 8192;
		uint8_t	*q;

		while (cap < b->len + n)
			cap *= 2;
		q = realloc (b->d, cap);
		if (!q)
		{
			b->fail = 1;
			return;
		}
		b->d = q;
		b->cap = cap;
	}
	if (p)
		memcpy (b->d + b->len, p, n);
	else
		memset (b->d + b->len, 0, n);
	b->len += n;
}

static void hlec_align (hlec_buf_t *b, size_t a)
{
	while (!b->fail && (b->len & (a - 1)))
		hlec_put (b, NULL, 1);
}

/* string pool with dedupe; names get a 2-byte index prefix */
static int hlec_pool_find (hlec_pool_t *p, const char *s)
{
	uint32_t	h;
	int		i;

	if (!p->hcap)
		return -1;
	h = hle_hash (s) & (uint32_t)(p->hcap - 1);
	for (i = 0; i < p->hcap; i++)
	{
		uint32_t	v = p->hash[(h + (uint32_t)i) & (uint32_t)(p->hcap - 1)];

		if (!v)
			return -1;
		if (!strcmp (p->v[v - 1], s))
			return (int)v - 1;
	}
	return -1;
}

static int hlec_pool_add (hlec_pool_t *p, const char *s, int *fail)
{
	int		i = hlec_pool_find (p, s);
	uint32_t	h;

	if (i >= 0)
		return i;
	if ((p->n + 1) * 2 > p->hcap)
	{
		int		cap = p->hcap ? p->hcap * 2 : 256, j;
		uint32_t	*nh = calloc ((size_t)cap, sizeof (*nh));

		if (!nh)
		{
			*fail = 1;
			return 0;
		}
		for (j = 0; j < p->n; j++)
		{
			h = hle_hash (p->v[j]) & (uint32_t)(cap - 1);
			while (nh[h])
				h = (h + 1) & (uint32_t)(cap - 1);
			nh[h] = (uint32_t)j + 1;
		}
		free (p->hash);
		p->hash = nh;
		p->hcap = cap;
	}
	p->v = hlec_grow (p->v, &p->cap, p->n + 1, sizeof (*p->v), fail);
	if (*fail)
		return 0;
	p->v[p->n] = (char *)s;
	h = hle_hash (s) & (uint32_t)(p->hcap - 1);
	while (p->hash[h])
		h = (h + 1) & (uint32_t)(p->hcap - 1);
	p->hash[h] = (uint32_t)p->n + 1;
	return p->n++;
}

static void hlec_pool_free (hlec_pool_t *p)
{
	free (p->v);
	free (p->ofs);
	free (p->hash);
	memset (p, 0, sizeof (*p));
}

typedef struct
{
	int	kept, dropped, pruned, keys, keysdropped;
} hle_stats_t;

/*
 * Cook a BSP entity lump (text, len bytes) into a chunk. big_endian picks
 * the byte order of the output. On success *out is a malloc'd chunk of
 * *outlen bytes (8-aligned size) and 0 is returned; otherwise an error
 * string. stats is optional.
 */
static const char *hle_build (const char *src, int len, int big_endian,
	uint8_t **out, int *outlen, hle_stats_t *stats)
{
	hlec_t		c;
	hlec_rkv_t	*rkv = NULL;
	int		rkvcap = 0, lump = 0, i, j, k, changed;
	const char	*p = src, *end = src + len, *err = 0;
	hlec_pool_t	names, strs, keynames, classes;
	hlec_buf_t	b;
	hle_header_t	h;
	hle_stats_t	st;
	uint16_t	*lump2rec = NULL, *namefirst = NULL, *namecnt = NULL;
	uint16_t	*nameflags = NULL;
	uint32_t	*namelist = NULL;
	int		nnamelist = 0, namelistcap = 0;
	uint16_t	*clsflags = NULL;
	int		clscap = 0;

	memset (&c, 0, sizeof (c));
	memset (&names, 0, sizeof (names));
	memset (&strs, 0, sizeof (strs));
	memset (&keynames, 0, sizeof (keynames));
	memset (&classes, 0, sizeof (classes));
	memset (&b, 0, sizeof (b));
	memset (&st, 0, sizeof (st));
	*out = NULL;
	*outlen = 0;

	/* ---- parse */
	while (p < end)
	{
		const char	*brace;
		int		n;

		while (p < end && (isspace ((unsigned char)*p) || !*p))
			p++;
		if (p >= end)
			break;
		if (*p != '{')
		{
			err = "entity text: expected '{'";
			goto done;
		}
		brace = p++;
		while (p < end && *p != '}')
		{
			if (*p == '{')
			{
				err = "entity text: '{' inside an entity block";
				goto done;
			}
			if (*p == '"')	/* braces inside values */
			{
				p++;
				while (p < end && *p != '"')
					p++;
			}
			if (p < end)
				p++;
		}
		if (p >= end)
		{
			err = "entity text: unterminated entity block";
			goto done;
		}
		p++;
		if (lump >= HLE_MAX_LUMP)
		{
			err = "more than 1024 entities";
			goto done;
		}
		n = hlec_parse_block (&c, brace, p, &rkv, &rkvcap);
		if (c.fail)
		{
			err = "out of memory";
			goto done;
		}
		if (n >= 0)
		{
			hlec_ent_t	*e;
			int		nf = 0, nk = 0;

			c.ents = hlec_grow (c.ents, &c.cap, c.nents + 1,
				sizeof (*c.ents), &c.fail);
			if (c.fail)
			{
				err = "out of memory";
				goto done;
			}
			e = &c.ents[c.nents++];
			memset (e, 0, sizeof (*e));
			e->lump = lump;
			e->keep = 1;
			for (i = 0; i < n; i++)
				nf += rkv[i].kind == 0;
			e->kv = malloc ((size_t)(n - nf + 1) * sizeof (*e->kv));
			e->fire = malloc ((size_t)(nf + 1) * sizeof (*e->fire));
			if (!e->kv || !e->fire)
			{
				err = "out of memory";
				goto done;
			}
			/* text offsets for now: the text store may still move */
			for (i = 0; i < n; i++)
			{
				if (rkv[i].kind)
				{
					e->kv[nk].k = (const char *)rkv[i].k;
					e->kv[nk].v = (const char *)rkv[i].v;
					e->kv[nk].kind = rkv[i].kind;
					nk++;
				}
				else
				{
					e->fire[e->nfire].name = (const char *)rkv[i].k;
					e->fire[e->nfire].dofs = rkv[i].v;
					e->nfire++;
				}
			}
			e->nkv = nk;
		}
		else
			st.dropped++;
		lump++;
	}

	/* ---- text offsets → pointers; class, targetname, sorted fire lists */
	for (i = 0; i < c.nents; i++)
	{
		hlec_ent_t	*e = &c.ents[i];

		for (j = 0; j < e->nkv; j++)
		{
			e->kv[j].k = c.text + ((size_t)e->kv[j].k - 1);
			e->kv[j].v = c.text + ((size_t)e->kv[j].v - 1);
		}
		for (j = 0; j < e->nfire; j++)
		{
			e->fire[j].name = c.text + ((size_t)e->fire[j].name - 1);
			e->fire[j].delay = (float)atof (c.text + e->fire[j].dofs - 1);
		}
		e->cls = -1;
		for (j = 0; j < e->nkv; j++)
		{
			if (!strcmp (e->kv[j].k, "classname"))
			{
				e->cname = e->kv[j].v;
				e->cls = hlec_class (e->kv[j].v);
			}
			else if (!strcmp (e->kv[j].k, "targetname"))
				e->tname = e->kv[j].v;
		}
		if (e->cls < 0)
		{
			e->keep = 0;
			continue;
		}
		/* classname / targetname live in the record (cls, tname) */
		for (j = k = 0; j < e->nkv; j++)
			if (strcmp (e->kv[j].k, "classname")
				&& strcmp (e->kv[j].k, "targetname"))
				e->kv[k++] = e->kv[j];
		e->nkv = k;
		/* stable insertion sort by delay (HLSDK bubble sort order) */
		for (j = 1; j < e->nfire; j++)
		{
			hlec_fire_t	t = e->fire[j];

			for (k = j; k > 0 && e->fire[k - 1].delay > t.delay; k--)
				e->fire[k] = e->fire[k - 1];
			e->fire[k] = t;
		}
	}

	/* ---- prune to a fixed point */
	do
	{
		changed = 0;
		for (i = 0; i < c.nents; i++)
		{
			hlec_ent_t	*e = &c.ents[i];
			const char	*cn;
			int		ref = 0;

			if (!e->keep)
				continue;
			cn = hle_classes[e->cls].name;
			if (hlec_in (hle_needs_name, cn) && !e->tname)
			{
				e->keep = 0;
				changed = 1;
				continue;
			}
			if (!hlec_in (hle_needs_ref, cn))
				continue;
			if (e->tname)
			{
				for (j = 0; j < c.nents && !ref; j++)
				{
					hlec_ent_t *o = &c.ents[j];

					if (!o->keep || o == e)
						continue;
					for (k = 0; k < o->nkv && !ref; k++)
						if (o->kv[k].kind == HLE_KN
							&& strcmp (o->kv[k].k, "targetname")
							&& !strcmp (o->kv[k].v, e->tname))
							ref = 1;
					for (k = 0; k < o->nfire && !ref; k++)
						if (!strcmp (o->fire[k].name, e->tname))
							ref = 1;
				}
			}
			if (!ref)
			{
				e->keep = 0;
				changed = 1;
			}
		}
	} while (changed);
	/* env_global without targetname still sets its initial state */

	/* ---- intern */
	for (i = 0; i < c.nents; i++)
	{
		hlec_ent_t	*e = &c.ents[i];

		if (!e->keep)
		{
			st.pruned++;
			continue;
		}
		st.kept++;
		for (j = 0; j < e->nkv; j++)
		{
			hlec_pool_add (&keynames, e->kv[j].k, &c.fail);
			if (e->kv[j].kind == HLE_KN)
				hlec_pool_add (&names, e->kv[j].v, &c.fail);
			else if (e->kv[j].kind == HLE_KS)
				hlec_pool_add (&strs, e->kv[j].v, &c.fail);
		}
		for (j = 0; j < e->nfire; j++)
			hlec_pool_add (&names, e->fire[j].name, &c.fail);
		if (e->tname)
			hlec_pool_add (&names, e->tname, &c.fail);
		j = hlec_pool_add (&classes, e->cname, &c.fail);
		clsflags = hlec_grow (clsflags, &clscap, classes.n, sizeof (*clsflags),
			&c.fail);
		if (!c.fail)
			clsflags[j] = hle_classes[e->cls].flags;
		st.keys += e->nkv;
	}
	if (c.fail)
	{
		err = "out of memory";
		goto done;
	}
	if (names.n >= (int)HLE_NONE || st.kept >= (int)HLE_NONE)
	{
		err = "too many names or records";
		goto done;
	}

	/* ---- name → record lists (lump order) */
	namefirst = calloc ((size_t)names.n + 1, sizeof (*namefirst));
	namecnt = calloc ((size_t)names.n + 1, sizeof (*namecnt));
	nameflags = calloc ((size_t)names.n + 1, sizeof (*nameflags));
	if (!namefirst || !namecnt || !nameflags)
	{
		err = "out of memory";
		goto done;
	}
	for (i = 0; i < names.n; i++)
	{
		namefirst[i] = (uint16_t)nnamelist;
		for (j = 0; j < c.nents; j++)
		{
			if (!c.ents[j].keep || !c.ents[j].tname
				|| strcmp (c.ents[j].tname, names.v[i]))
				continue;
			namelist = hlec_grow (namelist, &namelistcap, nnamelist + 1,
				sizeof (*namelist), &c.fail);
			if (c.fail)
			{
				err = "out of memory";
				goto done;
			}
			namelist[nnamelist++] = (uint32_t)c.ents[j].lump;
			namecnt[i]++;
		}
	}
	/* monstermaker netname: children carry it at runtime */
	for (j = 0; j < c.nents; j++)
	{
		hlec_ent_t *e = &c.ents[j];

		if (!e->keep || strcmp (hle_classes[e->cls].name, "monstermaker"))
			continue;
		for (k = 0; k < e->nkv; k++)
			if (!strcmp (e->kv[k].k, "netname"))
			{
				int n = hlec_pool_find (&names, e->kv[k].v);
				if (n >= 0)
					nameflags[n] |= HLE_NF_RUNTIME;
			}
	}

	/* ---- emit: header placeholder, then sections */
	memset (&h, 0, sizeof (h));
	hlec_put (&b, &h, sizeof (h));

	/* records */
	lump2rec = malloc ((size_t)(lump ? lump : 1) * sizeof (*lump2rec));
	if (!lump2rec)
	{
		err = "out of memory";
		goto done;
	}
	for (i = 0; i < lump; i++)
		lump2rec[i] = HLE_NONE;
	h.ofs_recs = (uint32_t)b.len;
	{
		uint32_t	keyi = 0, auxl = (uint32_t)nnamelist, auxf = 0;
		int		r = 0;

		for (i = 0; i < c.nents; i++)
		{
			hlec_ent_t	*e = &c.ents[i];
			hle_rec_t	rec;
			const char	*cn;

			if (!e->keep)
				continue;
			cn = hle_classes[e->cls].name;
			memset (&rec, 0, sizeof (rec));
			rec.lump = (uint16_t)e->lump;
			rec.cls = (uint16_t)hlec_pool_find (&classes, e->cname);
			rec.tname = e->tname
				? (uint16_t)hlec_pool_find (&names, e->tname) : HLE_NONE;
			rec.flags = hle_classes[e->cls].flags;
			if (e->nfire)
				rec.flags |= HLE_RF_AUXFIRE;
			rec.key_first = (uint16_t)keyi;
			rec.key_num = (uint16_t)e->nkv;
			rec.sector = HLE_NONE;
			rec.trank = HLE_NONE;	/* touchrank fills it after the cook */
			if (e->nfire)
			{
				rec.aux_first = (uint16_t)auxf;
				rec.aux_count = (uint16_t)e->nfire;
				auxf += (uint32_t)e->nfire;
			}
			else if (!strcmp (cn, "multisource") && e->tname)
			{
				/* CMultiSource::Register: entities targeting it, then
				 * multi_managers listing it */
				int n0 = nnamelist;

				for (j = 0; j < c.nents; j++)
				{
					hlec_ent_t *o = &c.ents[j];
					if (!o->keep)
						continue;
					for (k = 0; k < o->nkv; k++)
						if (!strcmp (o->kv[k].k, "target")
							&& !strcmp (o->kv[k].v, e->tname))
							break;
					if (k == o->nkv)
						continue;
					namelist = hlec_grow (namelist, &namelistcap,
						nnamelist + 1, sizeof (*namelist), &c.fail);
					if (c.fail)
						break;
					namelist[nnamelist++] = (uint32_t)o->lump;
				}
				for (j = 0; j < c.nents && !c.fail; j++)
				{
					hlec_ent_t *o = &c.ents[j];
					if (!o->keep)
						continue;
					for (k = 0; k < o->nfire; k++)
						if (!strcmp (o->fire[k].name, e->tname))
							break;
					if (k == o->nfire)
						continue;
					namelist = hlec_grow (namelist, &namelistcap,
						nnamelist + 1, sizeof (*namelist), &c.fail);
					if (c.fail)
						break;
					namelist[nnamelist++] = (uint32_t)o->lump;
				}
				rec.aux_first = (uint16_t)auxl;
				rec.aux_count = (uint16_t)(nnamelist - n0);
				auxl = (uint32_t)nnamelist;
			}
			keyi += (uint32_t)e->nkv;
			if (keyi >= HLE_NONE || auxl >= HLE_NONE || auxf >= HLE_NONE)
			{
				err = "too many keys";
				goto done;
			}
			/* scripted_sequence m_iszEntity names: keep resident */
			for (j = 0; e->tname && j < c.nents
				&& !(rec.flags & HLE_RF_PIN); j++)
			{
				hlec_ent_t *o = &c.ents[j];
				if (!o->keep)
					continue;
				for (k = 0; k < o->nkv; k++)
					if (!strcmp (o->kv[k].k, "m_iszEntity")
						&& !strcmp (o->kv[k].v, e->tname))
						rec.flags |= HLE_RF_PIN;
			}
			lump2rec[e->lump] = (uint16_t)r++;
			hlec_put (&b, &rec, sizeof (rec));
		}
		if (c.fail)
		{
			err = "out of memory";
			goto done;
		}
		h.numrecs = (uint16_t)r;
		h.numkeys = keyi;
		h.numfire = auxf;
	}
	hlec_align (&b, 8);
	h.ofs_lump2rec = (uint32_t)b.len;
	h.numlumps = (uint16_t)lump;
	hlec_put (&b, lump2rec, (size_t)lump * 2);
	hlec_align (&b, 8);

	/* strings: names region (u16 index + string, 2-aligned), then others */
	{
		hlec_buf_t	s;
		uint32_t	*sofs, *kofs, *cofs;

		memset (&s, 0, sizeof (s));
		names.ofs = malloc ((size_t)(names.n + 1) * sizeof (uint32_t));
		sofs = malloc ((size_t)(strs.n + 1) * sizeof (uint32_t));
		kofs = malloc ((size_t)(keynames.n + 1) * sizeof (uint32_t));
		cofs = malloc ((size_t)(classes.n + 1) * sizeof (uint32_t));
		strs.ofs = sofs;
		keynames.ofs = kofs;
		classes.ofs = cofs;
		if (!names.ofs || !sofs || !kofs || !cofs)
		{
			err = "out of memory";
			goto done;
		}
		hlec_put (&s, NULL, 2);	/* offset 0 never a name */
		for (i = 0; i < names.n; i++)
		{
			uint16_t	ix = (uint16_t)i;

			hlec_align (&s, 2);
			if (big_endian != hle_host_big ())
				ix = hle_sw16 (ix);
			hlec_put (&s, &ix, 2);
			names.ofs[i] = (uint32_t)s.len;
			hlec_put (&s, names.v[i], strlen (names.v[i]) + 1);
		}
		h.namestr_end = (uint32_t)s.len;
		for (i = 0; i < strs.n; i++)
		{
			j = hlec_pool_find (&names, strs.v[i]);
			if (j >= 0)
				sofs[i] = names.ofs[j];	/* share the name's bytes */
			else
			{
				sofs[i] = (uint32_t)s.len;
				hlec_put (&s, strs.v[i], strlen (strs.v[i]) + 1);
			}
		}
		for (i = 0; i < keynames.n; i++)
		{
			kofs[i] = (uint32_t)s.len;
			hlec_put (&s, keynames.v[i], strlen (keynames.v[i]) + 1);
		}
		for (i = 0; i < classes.n; i++)
		{
			j = hlec_pool_find (&strs, classes.v[i]);
			if (j >= 0)
				cofs[i] = sofs[j];
			else
			{
				cofs[i] = (uint32_t)s.len;
				hlec_put (&s, classes.v[i], strlen (classes.v[i]) + 1);
			}
		}
		if (s.fail)
		{
			free (s.d);
			err = "out of memory";
			goto done;
		}
		h.strsize = (uint32_t)s.len;

		/* classes */
		h.ofs_classes = (uint32_t)b.len;
		h.numclasses = (uint16_t)classes.n;
		for (i = 0; i < classes.n; i++)
		{
			hle_class_t	cl;

			cl.str = cofs[i];
			cl.flags = clsflags[i];
			cl.id = HLE_NONE;	/* the id step (tools/w64c/hleids.py) */
			hlec_put (&b, &cl, sizeof (cl));
		}
		/* keynames */
		h.ofs_keynames = (uint32_t)b.len;
		h.numkeynames = (uint16_t)keynames.n;
		for (i = 0; i < keynames.n; i++)
		{
			hle_keyname_t	kn;

			kn.str = kofs[i];
			kn.kind = hlec_keykind (keynames.v[i]);
			kn.id = HLE_NONE;
			hlec_put (&b, &kn, sizeof (kn));
		}
		/* names */
		h.ofs_names = (uint32_t)b.len;
		h.numnames = (uint16_t)names.n;
		for (i = 0; i < names.n; i++)
		{
			hle_name_t	nm;

			nm.str = names.ofs[i];
			nm.first = namefirst[i];
			nm.count = namecnt[i];
			nm.flags = nameflags[i];
			nm.pad = 0;
			hlec_put (&b, &nm, sizeof (nm));
		}
		/* hash: power of two >= 2 * names */
		{
			uint16_t	*ht;
			int		hs = 16;

			while (hs < names.n * 2)
				hs *= 2;
			ht = malloc ((size_t)hs * 2);
			if (!ht)
			{
				free (s.d);
				err = "out of memory";
				goto done;
			}
			for (i = 0; i < hs; i++)
				ht[i] = HLE_NONE;
			for (i = 0; i < names.n; i++)
			{
				uint32_t x = hle_hash (names.v[i]) & (uint32_t)(hs - 1);
				while (ht[x] != HLE_NONE)
					x = (x + 1) & (uint32_t)(hs - 1);
				ht[x] = (uint16_t)i;
			}
			hlec_align (&b, 4);
			h.ofs_hash = (uint32_t)b.len;
			h.hashsize = (uint16_t)hs;
			hlec_put (&b, ht, (size_t)hs * 2);
			free (ht);
			hlec_align (&b, 8);
		}
		/* keys + vector floats */
		{
			hlec_buf_t	fl;

			memset (&fl, 0, sizeof (fl));
			h.ofs_keys = (uint32_t)b.len;
			for (i = 0; i < c.nents; i++)
			{
				hlec_ent_t	*e = &c.ents[i];

				if (!e->keep)
					continue;
				for (j = 0; j < e->nkv; j++)
				{
					hle_key_t	key;
					float		f[3];

					key.kn = (uint16_t)hlec_pool_find (&keynames, e->kv[j].k);
					key.id = HLE_NONE;
					switch (e->kv[j].kind)
					{
					case HLE_KF:
						f[0] = (float)atof (e->kv[j].v);
						memcpy (&key.val, f, 4);
						break;
					case HLE_KV:
						f[0] = f[1] = f[2] = 0;
						sscanf (e->kv[j].v, "%f %f %f", &f[0], &f[1], &f[2]);
						key.val = (uint32_t)(fl.len / 4);
						hlec_put (&fl, f, sizeof (f));
						break;
					case HLE_KN:
						key.val = (uint32_t)hlec_pool_find (&names, e->kv[j].v);
						break;
					default:
						key.val = sofs[hlec_pool_find (&strs, e->kv[j].v)];
						break;
					}
					hlec_put (&b, &key, sizeof (key));
				}
			}
			hlec_align (&b, 8);
			/* lump lists (name lists, then multisource inputs) */
			h.ofs_lumplist = (uint32_t)b.len;
			h.numlumplist = (uint32_t)nnamelist;
			for (i = 0; i < nnamelist; i++)
			{
				uint16_t	v = (uint16_t)namelist[i];
				hlec_put (&b, &v, 2);
			}
			hlec_align (&b, 8);
			/* multi_manager fire lists */
			h.ofs_fire = (uint32_t)b.len;
			for (i = 0; i < c.nents; i++)
			{
				hlec_ent_t	*e = &c.ents[i];

				if (!e->keep)
					continue;
				for (j = 0; j < e->nfire; j++)
				{
					hle_fire_t	fr;

					fr.name = (uint16_t)hlec_pool_find (&names, e->fire[j].name);
					fr.pad = 0;
					fr.delay = e->fire[j].delay;
					hlec_put (&b, &fr, sizeof (fr));
				}
			}
			hlec_align (&b, 8);
			h.ofs_floats = (uint32_t)b.len;
			h.numfloats = (uint32_t)(fl.len / 4);
			hlec_put (&b, fl.d, fl.len);
			free (fl.d);
			if (fl.fail)
				b.fail = 1;
			hlec_align (&b, 8);
		}
		h.ofs_strings = (uint32_t)b.len;
		hlec_put (&b, s.d, s.len);
		free (s.d);
		hlec_align (&b, 8);
	}
	if (b.fail)
	{
		err = "out of memory";
		goto done;
	}

	/* ---- header, byte order */
	h.magic = HLE_MAGIC;
	h.version = HLE_VERSION;
	h.flags = (uint16_t)(hle_host_big () ? HLE_F_BIGENDIAN : 0);
	h.size = (uint32_t)b.len;
	{
		uint32_t	x = 2166136261u;
		for (i = 0; i < len; i++)
			x = (x ^ (uint8_t)src[i]) * 16777619u;
		h.srchash = x;
	}
	memcpy (b.d, &h, sizeof (h));
	if (big_endian != hle_host_big ())
	{
		hle_header_t	*hp = (hle_header_t *)b.d;

		/* name prefixes were written in the target order already:
		 * swap them back so hle_swap_body flips everything once */
		for (i = 0; i < names.n; i++)
		{
			uint16_t *pre = (uint16_t *)(b.d + h.ofs_strings
				+ names.ofs[i] - 2);
			*pre = hle_sw16 (*pre);
		}
		hle_swap_body (b.d, hp, 0);
		hp->flags = big_endian ? HLE_F_BIGENDIAN : 0;
		hle_swap_header (hp);
	}
	*out = b.d;
	*outlen = (int)b.len;
	b.d = NULL;
	st.keysdropped = 0;

done:
	for (i = 0; i < c.nents; i++)
	{
		free (c.ents[i].kv);
		free (c.ents[i].fire);
	}
	free (c.ents);
	free (c.text);
	free (rkv);
	free (lump2rec);
	free (namefirst);
	free (namecnt);
	free (nameflags);
	free (namelist);
	free (clsflags);
	hlec_pool_free (&names);
	hlec_pool_free (&strs);
	hlec_pool_free (&keynames);
	hlec_pool_free (&classes);
	free (b.d);
	if (stats)
		*stats = st;
	return err;
}
#endif /* !HLE_FORMAT_ONLY */

#endif /* HL_ENTCOOK_H */
