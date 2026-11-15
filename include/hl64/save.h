/* SPDX-License-Identifier: MIT */
/* save.h -- PLACEHOLDER copy of SaveGame's proposed interface (replaced by
 * the m6-savegame merge). */
#ifndef HL64_SAVE_H
#define HL64_SAVE_H

#include <stdint.h>

#include "hl64/settings.h"

#define SAVE_SLOTS	3
enum { SAVE_EMPTY, SAVE_OK, SAVE_CORRUPT };

typedef struct {
	uint8_t		state;
	uint8_t		skill;
	uint16_t	pad;
	uint32_t	seq;
	float		game_time;
	char		map[16];
	char		title[24];
} save_info_t;

int	save_slots(save_info_t *out, int max);
int	save_write(int slot);
void	save_init(void);
int	save_frame(void);
int	save_load(int slot);
int	save_busy(void);
int	save_result(void);
int	settings_load(settings_t *s);
int	settings_store(const settings_t *s);

#endif
