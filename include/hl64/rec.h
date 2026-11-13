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
/* Cooked typed ids (hle-format-v1.md "Ids"): global-state id of a
 * `globalstate` key, lightstyle pattern id of a `pattern` key (letters:
 * res_pattern), -1 when absent; classes.def id of a name key whose value
 * is a classname (m_iszEntity "monster_barney"), CLS_NONE otherwise.
 * rec_key_name also resolves a string key whose value is a name of the
 * record's map (changelevel / landmark message, netname): spawn only. */
int		rec_key_global(gid_t16 gid, key_t16 k);
/* env_message `message` / worldspawn `chaptertitle`: title id
 * (res_title), -1 when absent or not in titles.txt (rec_key_str then has
 * the text GoldSrc would show). */
int		rec_key_title(gid_t16 gid, key_t16 k);
int		rec_key_pattern(gid_t16 gid, key_t16 k);
/* infodecal `texture`: decal id, -1 when absent or not in decals.wad
 * (HLSDK CDecal removes the entity). Below 42 the global sheet's entry
 * (gsfx/decals.i4, HLSDK decals.h order), 42 + i entry i of the record's
 * own map's DECL sheet (w64-format.md "DECL"). */
int		rec_key_decal(gid_t16 gid, key_t16 k);
cls_t		rec_key_cls(gid_t16 gid, key_t16 k);
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
/* A name string of HLE slot `slot` (HLE hash), NAME_NONE when the map has
 * no such name; spawn only (monstermaker netname). */
name_t		rec_name_find(int slot, const char *s);
/* The slot's HLN1 node graph (hle-format-v1.md "Node graph"), validated
 * and in host byte order (tools/w64c/hl_nodecook.h HLN_FORMAT_ONLY
 * structs); NULL when the map has none. */
const void	*rec_node_graph(int slot, uint32_t *size);

/* Pending-record wake (area-links-and-touch.md 3 "Pending-record wake",
 * architecture.md 6.4): hydrate the pending records of every open slot
 * whose cooked box meets the GLOBAL box (links with touch, moves);
 * returns the number hydrated. */
int		rec_wake_box(const float gmins[3], const float gmaxs[3]);

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
 * (ties: insertion order); one per record (replaces). Timers armed by a
 * callback that are due run in the same pass, except the record re-arming
 * itself, which runs on the next frame (one think per frame). 128
 * entries; full = sys_error naming the map and record. */
void		rec_timer(gid_t16 gid, float t, uint16_t arg, int activator);
void		rec_timer_cancel(gid_t16 gid);
/* FireAndDie / CBaseDelay::SUB_UseTargets delay from a runtime entity or
 * a record: fire target (and kill killtarget) at t. */
void		rec_fire_at(float t, name_t target, name_t killtarget, int activator,
			int caller, int use_type, float value);

/* ---- per-map, per-frame (engine) ----------------------------------------- */

/* Open the HLE1 chunk of an open world layer's map into an HLE slot (the
 * same slot number as the world layer); records start pending. M4: the
 * chunk read is queued at OPEN priority (never blocks; New Game finishes
 * it under load I/O); a frozen map's runtime part (record states, frozen
 * timers, deferred uses, runtime ids) is restored into the slot. */
int		rec_map_open(int layer);
/* 1 once the slot's chunk is resident and its logic / thawed timers are
 * live (records may hydrate). */
int		rec_map_ready(int slot);
/* Close (M4): live entities dehydrate (game_cut_refs first), timers
 * freeze by remaining time, the chunk is dropped and the runtime part is
 * kept frozen by map (architecture.md 4.1 "Frozen"). */
void		rec_map_close(int slot);
/* Live entities and runtime name holders of a slot (0: it may close). */
int		rec_slot_live(int slot);
/* Of those, the ones that keep the slot open now: live entities within
 * the release radius of the last frame's anchors, and runtime name
 * holders. The release has offered everything else and it refused
 * (quiescence: a monster idling in a script for good); the closing
 * release takes those (0: it may close when its layer is needed). */
int		rec_slot_held(int slot);
/* Release one live entity now (dehydrate into the store, record DRY;
 * runtime entities by (map, runtime id)); 0 when the game refused it. */
int		rec_release(int e);
/* 1 while the closing release of `slot` dehydrates its entities (map close:
 * game_dehydrate stores any state, as GoldSrc saves a level it leaves). */
int		rec_slot_closing(int slot);
/* Twin pair (cwg-format.md twins): names fired in either slot resolve to
 * the live copy; cleared with rec_twin_clear when either slot closes. */
void		rec_twin_alias(gid_t16 a, gid_t16 b);
void		rec_twin_clear(int slot);
/* The other gid of a rec_twin_alias pair, GID_NONE when g has no twin. */
gid_t16		rec_twin_of(gid_t16 g);
/* A carried entity (seam crossing) leaves its record (moved: never
 * respawns in its map) and becomes a runtime entity of slot `slot` with a
 * durable runtime id; the caller has re-based its origin. */
int		rec_rehome(int e, int slot);
/* Durable runtime id of a live entity without a record (monstermaker
 * child, created pickup): assigned on first call or first release, never
 * reused before New Game, kept across rec_rehome; 0 for a map record's
 * entity or a dead slot. */
uint16_t	rec_runtime_id(int e);
/* Live entity of a canonical reference: (CWG map, lump) of a record or
 * (CWG map, runtime id); -1 when absent (map closed, entity dry or gone).
 * Never hydrates. */
int		rec_ref_entity(uint16_t map, uint16_t id, int runtime);
/* New Game: open the start map, spawn its logic and its records within
 * the spawn radius of the player start, in entity order, at time 1.0; the
 * loop's next two frames are the settle frames (think-and-time.md 6,
 * eng_clock_new_game). */
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
	uint32_t	released_total;		/* since New Game */
	uint32_t	store_used, store_peak;	/* dehydrate store bytes */
	uint32_t	open_expired;		/* open gates (precache hold) expired with data pending */
} rec_stats_t;
void		rec_stats(rec_stats_t *s, int reset);
/* O(REC_SLOTS), no record walk (per-frame profilers): HLE region bytes in
 * use (tables + open slot blocks + frozen runtime) and dehydrate store
 * bytes in use. */
void		rec_mem_use(uint32_t *hle_bytes, uint32_t *store_bytes);

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
/* PI address and size of a cart file of the table (resolved once at
 * res_init); 0 for a bad index. */
uint32_t	res_file_rom(int file, uint32_t *size);
uint8_t		res_flags(res_t r);		/* MODEL: EXTTEX, sequence groups */
/* game/defs/res.def line -> res_t (DEFS section); RES_NONE when absent. */
res_t		res_def(int line);
/* pfnModelFrames: sprite frame count (cooked .sc4 header), studio model
 * body combinations (studio_body_count), 1 for brush models / none. */
int		res_model_frames(res_t r);
/* Residency interest (precache-and-resources.md 2): never fails;
 * res_interest tells the residency owners (studio, sound) it was declared. */
void		res_precache(res_t r);
int		res_interest(res_t r);
/* Sentence words and groups (rtb SENT / WORD / SGRP). */
typedef struct {
	res_t		res;		/* SOUND */
	uint8_t		pitch, volume, start, end, time, pad;
} res_word_t;			/* 8; sound.md 4 modifiers */
int		res_sentence_words(uint16_t sentence, int *first);
const res_word_t *res_word(int i);	/* NULL out of range */
int		res_group(uint16_t group, uint16_t *first_sentence);
/* Lightstyle pattern letters (0..25), length; NULL bad id. */
const uint8_t	*res_pattern(uint16_t pattern, int *len);
int		res_patterns(void);		/* PATN count */
int		res_globals(void);		/* global-state ids (GLOB count) */
int		res_classes(void);		/* classes.def lines (CLAS count) */
/* titles.txt messages (rtb TITL / TTXT; HLSDK client_textmessage_t):
 * title id = staged titles.txt order; env_message `message` and worldspawn
 * `chaptertitle` keys carry it (rec_key_title). */
typedef struct {
	float		x, y;		/* -1 = centred */
	float		fadein, fadeout, holdtime, fxtime;
	uint32_t	text;		/* offset in the text pool (res_title_text) */
	uint16_t	text_len;	/* bytes, NUL not counted */
	uint8_t		effect;		/* 0 fade, 1 flicker, 2 write-out */
	uint8_t		pad;
	uint8_t		color1[4], color2[4];	/* r g b a */
} res_title_t;			/* 40 */
const res_title_t *res_title(uint16_t id);	/* NULL bad id */
const char	*res_title_text(uint16_t id);	/* NUL-terminated, NULL bad id */
int		res_titles(void);
/* Host / debug builds: the name of an id ("" on the cart). */
const char	*res_name(res_t r);

#endif
