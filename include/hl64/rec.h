/* SPDX-License-Identifier: MIT */
/*
 * rec.h -- map entity records (HLE1), hydration, names, firing, logic
 * records, logic timers, level state, and the cooked resource table
 * (docs/architecture.md 6.4, 6.5, 7.1 "records" / "resources";
 * docs/formats/hle-format-v1.md, rtb-format.md; docs/specs/
 * think-and-time.md 5, precache-and-resources.md).
 *
 * A record is a BSP entity kept by the cook: physical (becomes an entity
 * when hydrated), logic (no entity: a 16-byte state block + the game's
 * class handlers) or point (origin / angles queried by name or class).
 * Records are addressed by gid (types.h). Keys are typed and addressed by
 * key id (game/defs/keys.def); resource-valued keys arrive as res_t,
 * names as name_t, classes as cls_t (cooked; no string compare on the
 * cart's steady path). rec_key_str exists for spawn-time strings the game
 * still parses (sequence names, titles), never per frame.
 */
#ifndef HL64_REC_H
#define HL64_REC_H

#include <stdint.h>

#include "hl64/types.h"

/* HLSDK USE_TYPE */
#define USE_OFF			0
#define USE_ON			1
#define USE_SET			2
#define USE_TOGGLE		3

/* ---- records -------------------------------------------------------------- */

#define REC_PENDING		0	/* physical, not hydrated yet */
#define REC_LIVE		1	/* has an entity / logic block active */
#define REC_KILLED		2	/* removed by the game: never respawns */
#define REC_DRY			3	/* dehydrated: respawns from its delta */

#define REC_KIND_ENTITY		0
#define REC_KIND_LOGIC		1
#define REC_KIND_POINT		2

int		rec_kind(gid_t16 gid);		/* -1 bad gid */
int		rec_state(gid_t16 gid);
cls_t		rec_class(gid_t16 gid);
name_t		rec_targetname(gid_t16 gid);
uint32_t	rec_flags(gid_t16 gid);		/* HLE_RF_* */
/* origin / angles keys (map-local; 0 when absent) */
void		rec_origin(gid_t16 gid, float out[3]);
void		rec_angles(gid_t16 gid, float out[3]);

/* Typed keys: 1 present (out written), 0 absent. */
int		rec_key_f(gid_t16 gid, key_t16 k, float *out);
int		rec_key_v(gid_t16 gid, key_t16 k, float out[3]);
name_t		rec_key_name(gid_t16 gid, key_t16 k);	/* NAME_NONE absent */
res_t		rec_key_res(gid_t16 gid, key_t16 k);	/* RES_NONE absent */
const char	*rec_key_str(gid_t16 gid, key_t16 k);	/* NULL absent; spawn only */
/* Key iteration for the game's KeyValue dispatch (multi_manager targets
 * arrive as the fire list, not keys): i < rec_key_count(gid). */
int		rec_key_count(gid_t16 gid);
key_t16		rec_key_id(gid_t16 gid, int i);

/* Multisource inputs (lumplist) and multi_manager fire list. */
int		rec_aux_count(gid_t16 gid);
gid_t16		rec_aux_gid(gid_t16 gid, int i);
void		rec_fire_entry(gid_t16 gid, int i, name_t *name, float *delay);

/* Live entity of a record, -1 when none (never hydrates). */
int		rec_entity(gid_t16 gid);
/* Hydrate a pending / dry record now (named fires, searches); returns the
 * entity, -1 for logic / point / killed records or a refused spawn. */
int		rec_wake(gid_t16 gid);
/* The game removed the record's entity for good (fire-once, killtarget). */
void		rec_kill(gid_t16 gid);

/* ---- names ------------------------------------------------------------------ */

/* Records carrying the name in its map. */
int		rec_name_count(name_t n);
gid_t16		rec_name_gid(name_t n, int i);
/* The same name in another HLE slot (changetarget across a seam), or
 * NAME_NONE. */
name_t		rec_name_in(name_t n, int slot);
/* Live entities with targetname n: map records (pending ones woken),
 * then runtime holders (monstermaker children); *it starts at 0;
 * returns -1 at the end. */
int		rec_next_named(name_t n, int *it);
/* A runtime entity takes a map name (monstermaker netname). */
void		rec_name_hold(int e, name_t n);

/* HLSDK FireTargets: every live / woken entity with the name gets
 * game_use; logic records get game_logic_use; caller is an entity slot or
 * -1 with caller_gid set for a logic caller. */
void		rec_fire(name_t n, int activator, int caller, gid_t16 caller_gid,
			int use_type, float value);
/* The record or entity whose use is in progress (HLE caller), GID_NONE. */
gid_t16		rec_caller_gid(void);

/* ---- logic records ---------------------------------------------------------- */

typedef struct {
	uint8_t		flags;		/* game-defined (on / busy / init) */
	uint8_t		pad;
	uint16_t	a16;
	int32_t		a, b;
	float		f;
} rec_logic_t;			/* 16 */
rec_logic_t	*rec_logic(gid_t16 gid);	/* NULL unless logic */

/* ---- logic timers (think-and-time.md 5) ------------------------------------- */

/* Timer for a record: due at the f32 nextthink t by rule 2, callback
 * game_logic_timer(gid, arg, activator) at game_start_frame in due order
 * (ties: insertion order). 128 entries; full = sys_error naming the map
 * and record. */
void		rec_timer(gid_t16 gid, float t, uint16_t arg, int activator);
void		rec_timer_cancel(gid_t16 gid);
/* FireAndDie / CBaseDelay::SUB_UseTargets delay from a runtime entity or
 * a record: fire target (and kill killtarget) at t. */
void		rec_fire_at(float t, name_t target, name_t killtarget, int activator,
			int caller, int use_type, float value);

/* ---- per-map, per-frame (engine) ----------------------------------------- */

/* Open the HLE1 chunk of an open world layer's map into an HLE slot (the
 * same slot number as the world layer); records start pending. */
int		rec_map_open(int layer);
void		rec_map_close(int slot);
/* New Game: open the start map, spawn its logic and its records within
 * the spawn radius of the player start, run the settle frames
 * (think-and-time.md 6). */
void		rec_new_campaign(int map);
/* Step 8: hydration pump / release under the entity budget around the
 * anchors (global positions). */
void		rec_frame(const float (*anchors)[3], int nanchors);
/* Step 4: due logic timers (think-and-time.md 5). */
void		rec_run_timers(double time, double dt);
/* Player start: the start map's info_player_start (or the CWG start
 * spot), map-local; 0 when none. */
int		rec_player_start(int slot, float org[3], float ang[3]);

typedef struct {
	uint32_t	live, pending, logic, killed, dry;
	uint32_t	hydrated, released, woken;	/* in the window */
	uint32_t	timers, timers_peak;
	uint32_t	budget_us, budget_max_us;
	uint32_t	priv_bytes, priv_peak;
} rec_stats_t;
void		rec_stats(rec_stats_t *s, int reset);

/* ---- level state (dehydrate store, 6.4) ------------------------------------ */

/* A released entity's per-class delta (game_dehydrate) and its record
 * state; the store survives map close (64 runtime pickups + per-map
 * deltas). */
int		rec_store_put(gid_t16 gid, uint16_t rtid, const void *delta, uint32_t n);
int		rec_store_get(gid_t16 gid, uint16_t rtid, void *delta, uint32_t max);

/* ---- resources (6.5, campaign.rtb) ------------------------------------------ */

/* Boot: load the rtb cart part into its region (validated). */
void		res_init(void);
int		res_count(int kind);
/* Cart file and byte range of a file-kind id; 0 bad id. */
int		res_file(res_t r, int *file, uint32_t *ofs, uint32_t *size);
uint8_t		res_flags(res_t r);		/* MODEL: EXTTEX, sequence groups */
/* game/defs/res.def line -> res_t (DEFS section); RES_NONE when absent. */
res_t		res_def(int line);
/* Residency interest (precache-and-resources.md 2): never fails. */
void		res_precache(res_t r);
/* Sentence words and groups (rtb SENT / WORD / SGRP). */
int		res_sentence_words(uint16_t sentence, int *first);
int		res_group(uint16_t group, uint16_t *first_sentence);
/* Lightstyle pattern letters (0..25), length. */
const uint8_t	*res_pattern(uint16_t pattern, int *len);
/* Host / debug builds: the name of an id ("" on the cart). */
const char	*res_name(res_t r);

#endif
