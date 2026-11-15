/* SPDX-License-Identifier: MIT */
/*
 * save.h -- saved games and the settings block on the cart's FlashRAM
 * (docs/architecture.md 14, decision D4); the host keeps the same image
 * in a file. The menu (engine/ui) is the caller.
 *
 * Every call is bounded: flash erase / program runs a little per frame
 * from save_frame(), which eng_frame calls first every frame (also while
 * the game is paused or at the title). A slot whose checksum or version
 * does not match shows as SAVE_CORRUPT and is never loaded.
 */
#ifndef HL64_SAVE_H
#define HL64_SAVE_H

#include <stdint.h>

#include "hl64/settings.h"

/* FlashRAM layout: 128 KB in 16 KB sectors; sector 0 holds the settings
 * block, each slot SAVE_SLOT_SECTORS sectors after it. */
#define SAVE_FLASH_BYTES	(128u * 1024u)
#define SAVE_SECTOR_BYTES	(16u * 1024u)
#define SAVE_SLOT_SECTORS	7
#define SAVE_SLOT_BYTES		(SAVE_SLOT_SECTORS * SAVE_SECTOR_BYTES)
#define SAVE_SLOTS		1

enum { SAVE_EMPTY, SAVE_OK, SAVE_CORRUPT };

typedef struct {
	uint8_t		state;		/* SAVE_EMPTY / SAVE_OK / SAVE_CORRUPT */
	uint8_t		skill;		/* 1..3 */
	uint16_t	pad;
	uint32_t	seq;		/* larger = newer */
	float		game_time;	/* s of game clock */
	char		map[16];	/* home map name, e.g. "hldemo2" */
	char		title[24];	/* display title */
} save_info_t;

/* The slot directory (cached at boot, refreshed after every write):
 * min(max, SAVE_SLOTS) entries; returns that count. Never blocks. */
int	save_slots(save_info_t *out, int max);
/* Snapshot the running game now (a frame boundary: called between frames
 * or while paused) and start writing it to slot; the flash write spreads
 * over the following frames. 0 started, -1 busy / no game / bad slot. */
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

/* The persistent settings (settings_desc persist = 1), by field id. */
int	settings_load(settings_t *s);		/* 0 ok, -1 none / corrupt (s untouched) */
int	settings_store(const settings_t *s);	/* async (save_busy); 0 started, -1 busy */

/* Boot: probe the flash and read the directory (eng_boot). */
void	save_init(void);
/* eng_frame, first thing every frame: steps the flash writer, applies a
 * scheduled load. Returns 1 while a load holds the frame (the caller runs
 * no game step, no route, no debug commands that frame), else 0. */
int	save_frame(void);

#endif
