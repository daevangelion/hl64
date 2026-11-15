/* SPDX-License-Identifier: MIT */
/* save_stub.c -- PLACEHOLDER for m6-savegame's backend (include/hl64/save.h
 * is its interface, byte-identical): RAM slots, nothing restored, no
 * Controller Pak. The m6-savegame merge deletes this file and its Makefile
 * line. */
#include <string.h>

#include "hl64/save.h"
#include "engine/core/eng.h"
#include "engine/core/log.h"
#include "engine/phys/player.h"
#include "engine/world/wld.h"

#define STUB_SLOTS	(1 + SAVE_FLASH_MANUAL)	/* the autosave, the Flash slots */

static save_info_t slots[STUB_SLOTS];
static int busy, result;
static uint32_t seq;

void save_init(void)
{
	memset(slots, 0, sizeof(slots));
	slots[0].flags = SAVE_F_AUTO;
}

int save_frame(void)
{
	if (busy > 0)
	{
		busy--;
		return 1;
	}
	return 0;
}

int save_slots(save_info_t *out, int max)
{
	int n = max < STUB_SLOTS ? max : STUB_SLOTS;

	memcpy(out, slots, (size_t)n * sizeof(*out));
	return n;
}

int save_write(int slot)
{
	if (slot < 1 || slot >= STUB_SLOTS || busy)
		return -1;
	slots[slot].state = SAVE_OK;
	slots[slot].seq = ++seq;
	slots[slot].game_time = (float)eng_frame_count() / 30.0f;
	strncpy(slots[slot].map, wld_map_name(phy_player_map()), sizeof(slots[slot].map) - 1);
	strncpy(slots[slot].title, "Uplink", sizeof(slots[slot].title) - 1);
	busy = 2;
	result = 0;
	log_printf("save stub: write %d\n", slot);
	return 0;
}

int save_load(int slot)
{
	if (slot < 0 || slot >= STUB_SLOTS || slots[slot].state != SAVE_OK || busy)
		return -1;
	busy = 2;
	result = 0;
	log_printf("save stub: load %d\n", slot);
	return 0;
}

int save_busy(void)
{
	return busy > 0;
}

int save_result(void)
{
	return result;
}

int save_pak_status(void)
{
	return SAVE_PAK_ABSENT;
}

void save_autosave(void)
{
}

int settings_load(settings_t *s)
{
	(void)s;
	return -1;
}

int settings_store(const settings_t *s)
{
	(void)s;
	log_printf("save stub: settings store\n");
	return 0;
}
