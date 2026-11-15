/* SPDX-License-Identifier: MIT */
/*
 * save.h -- saved games on the cart's FlashRAM and on a Controller Pak in
 * controller 1, and the settings block (docs/architecture.md 14, decision
 * D4); the host keeps the same images in files. The menu (engine/ui) is
 * the caller; autosaves come from the game (trigger_autosave) and the
 * engine (after a seam crossing).
 *
 * Every call is bounded: a snapshot holds one frame, then flash / Pak
 * writes run a little per frame from save_frame(), which eng_frame calls
 * first every frame (also while the game is paused or at the title). A
 * slot whose checksum or version does not match shows as SAVE_CORRUPT and
 * is never loaded. A save never destroys the save it replaces before the
 * new one is complete (each one goes to free space and is committed by its
 * checksummed header; the old copy is free after that).
 */
#ifndef HL64_SAVE_H
#define HL64_SAVE_H

#include <stdint.h>

#include "hl64/settings.h"

/* FlashRAM: 128 KB in 16 KB sectors; sector 0 the settings block, sectors
 * 1..7 a pool the saves take whole sectors of. */
#define SAVE_FLASH_BYTES	(128u * 1024u)
#define SAVE_SECTOR_BYTES	(16u * 1024u)
#define SAVE_POOL_SECTORS	7
#define SAVE_IMAGE_MAX		(30u * 1024u)	/* a save's image at most (a Pak note fits) */
#define SAVE_FLASH_MANUAL	2		/* manual Flash slots beside the autosave */
#define SAVE_PAK_SLOTS		4		/* Pak entries listed at most */
#define SAVE_SLOTS_MAX		(1 + SAVE_FLASH_MANUAL + SAVE_PAK_SLOTS)

enum { SAVE_EMPTY, SAVE_OK, SAVE_CORRUPT };
enum { SAVE_SRC_FLASH, SAVE_SRC_PAK };
#define SAVE_F_AUTO		0x01	/* the autosave (load only) */

typedef struct {
	uint8_t		state;		/* SAVE_EMPTY / SAVE_OK / SAVE_CORRUPT */
	uint8_t		skill;		/* 1..3 */
	uint8_t		source;		/* SAVE_SRC_FLASH / SAVE_SRC_PAK */
	uint8_t		flags;		/* SAVE_F_AUTO */
	uint32_t	seq;		/* larger = newer */
	float		game_time;	/* s of game clock */
	char		map[16];	/* home map name, e.g. "hldemo2" */
	char		title[24];	/* display title */
} save_info_t;

/* Controller Pak in controller 1 (save_pak_status). */
enum {
	SAVE_PAK_OK,		/* Pak entries listed (existing notes; an empty one if a save fits) */
	SAVE_PAK_ABSENT,	/* nothing in the controller (or no controller) */
	SAVE_PAK_FULL,		/* a Controller Pak without room for a save (notes or pages) */
	SAVE_PAK_RUMBLE,	/* a Rumble Pak or another accessory, not a Controller Pak */
	SAVE_PAK_DAMAGED	/* unformatted, or its file system does not check out */
};

/* The slot list, fixed order: the autosave (index 0, SAVE_F_AUTO), the
 * SAVE_FLASH_MANUAL Flash slots, then the Pak entries (only with
 * SAVE_PAK_OK: the hl64 notes on the Pak, then one SAVE_EMPTY entry if a
 * new note fits). min(max, count) entries; returns that count. The index
 * is what save_write / save_load take; the list changes only when
 * save_busy() goes 0 or the Pak is plugged / unplugged (list again then).
 * Never blocks. */
int	save_slots(save_info_t *out, int max);
/* Snapshot the running game now (a frame boundary: called between frames
 * or while paused) and start writing it to slot; the write spreads over
 * the following frames. 0 started; -1 busy / no game / bad slot / the
 * autosave index. */
int	save_write(int slot);
/* Validate slot and schedule its restore at the next frame boundary
 * (works from the title too: the running session, if any, is dropped).
 * 0 scheduled, -1 empty / corrupt / busy. */
int	save_load(int slot);
/* 1 while a write or a load is in flight (draw "Saving..." / "Loading..."
 * and keep running frames), else 0. */
int	save_busy(void);
/* Result of the last finished operation: 0 ok, -1 failed. */
int	save_result(void);
/* SAVE_PAK_* for controller 1, polled by save_frame. */
int	save_pak_status(void);

/* Autosave (game: trigger_autosave; engine: a finished seam crossing):
 * asked now, taken at the next frame boundary where nothing is in flight
 * (not in a seam window or crossing); dropped if a save or load is busy.
 * Writes the autosave slot; the previous autosave stays until the new one
 * is committed. */
void	save_autosave(void);

/* The persistent settings (settings_desc persist = 1), by field id. */
int	settings_load(settings_t *s);		/* 0 ok, -1 none / corrupt (s untouched) */
int	settings_store(const settings_t *s);	/* async (save_busy); 0 started, -1 busy */

/* Boot: probe the flash and the Pak, read the directory (eng_boot). */
void	save_init(void);
/* eng_frame, first thing every frame: steps the writers, applies a
 * scheduled load or autosave, polls the Pak. Returns 1 while a snapshot or
 * a load holds the frame (the caller runs no game step, no route, no debug
 * commands that frame), else 0. */
int	save_frame(void);

#endif
