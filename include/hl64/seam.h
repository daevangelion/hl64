/* SPDX-License-Identifier: MIT */
/*
 * seam.h -- campaign seams (docs/architecture.md 5.4, docs/formats/
 * cwg-format.md): the CWG3 graph at run time, approach / switch / crossing
 * / leave, twins, carried entities, forced-switch and continuity stats.
 * Implemented by engine/world/seam.c (M4, slice Seam).
 */
#ifndef HL64_SEAM_H
#define HL64_SEAM_H

#include <stdint.h>

#include "hl64/types.h"

/* ---- game -> engine ----------------------------------------------------- */

/*
 * A trigger_changelevel fired (HLSDK ChangeLevelNow, after its transition
 * volume check): `trigger` is the changelevel's record, `use` 1 for the
 * use path, 0 for touch. Touch seams arm (the switch happens in seam_frame
 * when the eye enters the switch box, or forced by the cwg-format.md rules);
 * USEONLY seams switch at the next seam_frame. Returns 1 when a seam took
 * it, 0 when ignored (no seam for the link, or inside the after-switch
 * window where changelevel touches are ignored until a frame with none).
 * The seam delivers the changetarget in the destination map at the
 * crossing; the game does not fire it.
 */
int		seam_changelevel(gid_t16 trigger, int use);

/* ---- engine -------------------------------------------------------------- */

/* Boot, after wld_init: the whole CWG3 file into the campaign region
 * (validated; sys_error when malformed). */
void		seam_init(void);
/* New Game on map: no seam armed or approached, home = map. */
void		seam_new_game(int map);
/* Frame step 7 (after the active-set pass): approach, layer / HLE opens,
 * pins, switch, crossing, leave and close. */
void		seam_frame(void);
/* Hydration anchors (global positions): the player, and the player mapped
 * through the selected approached seam (p + T) while one is approached. */
int		seam_anchors(float (*out)[3], int max);

typedef struct {
	uint32_t	approaches;	/* approach boxes entered */
	uint32_t	switches;
	uint32_t	forced;		/* cwg-format.md "forced" switches */
	uint32_t	open_fail;	/* B could not open (no layer / HLE room) */
	uint32_t	late;		/* switch frames deferred until B was ready */
	uint32_t	twins;		/* twin handoffs */
	uint32_t	carried;	/* entities re-homed with the player */
	uint32_t	continuity_bad;	/* carry-box entities missing / extra after a switch */
	uint32_t	closes;		/* layers closed by the leave rule */
} seam_stats_t;
void		seam_stats(seam_stats_t *s);

/* Frame of the last switch (eng frame counter), 0 none; used by the
 * seam-window profiler. */
uint32_t	seam_last_switch_frame(void);

#endif
