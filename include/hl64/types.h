/* SPDX-License-Identifier: MIT */
/*
 * types.h -- the id types shared by the engine and game/ (docs/architecture.md
 * 6.2, 6.5, 7): entity handles, cooked resource ids, cooked entity names,
 * function ids, class ids and key ids. Every id is a small integer; nothing
 * stored, saved or cooked is a pointer.
 */
#ifndef HL64_TYPES_H
#define HL64_TYPES_H

#include <stdint.h>

/* Entity handle ent_h (slot index | serial << 16): include/hl64/grid.h. */

/* Resource id (docs/formats/rtb-format.md "res_t"): kind << 13 | index;
 * 0 = none. */
typedef uint16_t	res_t;
#define RES_NONE		0
#define RES_K_MODEL		1
#define RES_K_SPRITE		2
#define RES_K_SOUND		3
#define RES_K_ANIM		4
#define RES_K_SENTENCE		5
#define RES_K_GROUP		6
#define RES_K_SUBMODEL		7
#define RES_KIND(r)		((r) >> 13)
#define RES_INDEX(r)		((r) & 0x1FFF)
#define RES_MAKE(k, i)		((res_t)((k) << 13 | (i)))
/* Brush submodel of an open HLE/world slot (architecture.md 6.5): index =
 * slot << 10 | submodel. */
#define RES_SUBMODEL(slot, sub)	RES_MAKE(RES_K_SUBMODEL, (slot) << 10 | (sub))
#define RES_SUB_SLOT(r)		(RES_INDEX(r) >> 10)
#define RES_SUB_NUM(r)		(RES_INDEX(r) & 1023)

/* Entity name (targetname, target, ...): global form slot << 12 | the
 * HLE1 per-map name index; 0xFFFF none (architecture.md 6.2). */
typedef uint16_t	name_t;
#define NAME_NONE		0xFFFF
#define NAME_MAKE(slot, idx)	((name_t)((slot) << 12 | (idx)))
#define NAME_SLOT(n)		((n) >> 12)
#define NAME_INDEX(n)		((n) & 0xFFF)

/* Game function id (game/defs/funcs.def line order + 1; 0 = none):
 * think / touch / use / blocked handlers the engine stores and the game
 * dispatches. */
typedef uint16_t	fn_t;
/* Class id (game/defs/classes.def line order); CLS_NONE = not spawnable. */
typedef uint16_t	cls_t;
#define CLS_NONE		0xFFFF
/* Record key id (game/defs/keys.def line order). */
typedef uint16_t	key_t16;

/* Global record id of a map entity: HLE slot << 10 | BSP entity lump
 * (docs/formats/hle-format-v1.md); GID_NONE for runtime entities. */
typedef uint16_t	gid_t16;
#define GID_NONE		0xFFFF
#define GID_MAKE(slot, lump)	((gid_t16)((slot) << 10 | (lump)))
#define GID_SLOT(g)		((g) >> 10)
#define GID_LUMP(g)		((g) & 1023)

/* No entity (slot references in components). */
#define ENT_NONE		0xFFFF

#endif
