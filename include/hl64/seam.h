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
/* 1 when no seam is approached, armed or switching (no seam window): an
 * autosave may be taken (engine/save/save.c). */
int		seam_quiet(void);
/* Hydration anchors (global positions): the player, and the player mapped
 * through the selected approached seam (p + T) while one is approached. */
int		seam_anchors(float (*out)[3], int max);
/* Approach ghosts (architecture.md 5.4 item 6): while a seam of `map` is
 * approached and its B is ready, B's live entities drawn into B's carry
 * box that no twin and no A entity of the same class within 1 u stands
 * for show in A's view, at their global position + off (off = -T).
 * seam_ghost: 1 when entity e is one for a view in map `map` this frame;
 * seam_ghosts: this frame's ones for map `map` into ents, the count. */
#define SEAM_GHOSTS	64
int		seam_ghost(int e, int map, float off[3]);
int		seam_ghosts(int map, int16_t ents[SEAM_GHOSTS], float off[3]);

typedef struct {
	uint32_t	approaches;	/* approach boxes entered */
	uint32_t	switches;
	uint32_t	forced;		/* cwg-format.md "forced" switches */
	uint32_t	open_fail;	/* B could not open (no layer / HLE room; a held
					 * neighbour once B's switch is pending) */
	uint32_t	open_wait;	/* B's open waited on a neighbour holding
					 * entities near the player, no switch pending */
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

/* Roll-up and `mem` lines: `prof seam_us avg= max= frames=` (resets the
 * window) and `mem seam: campaign used= size=` (the CWG3 file in the
 * campaign region). */
void		seam_prof_line(void);
void		seam_mem_line(void);

/* ---- the decoded graph (tests, the seam-window profiler) ---------------- */

#define SEAM_F_CLOSED	1	/* closed verified */
#define SEAM_F_NARROWED	2
#define SEAM_F_VOLUME	4	/* transition volume */
#define SEAM_F_OPEN	8	/* OPEN: cross-layer draw */

typedef struct {
	uint16_t	from, to;		/* CWG map indices */
	uint16_t	trigger;		/* A changelevel entity lump */
	uint16_t	kind;			/* 0 touch, 1 USEONLY */
	uint16_t	flags;			/* SEAM_F_*; 0 = rejected at run time */
	uint16_t	ntwin, npool, ndraw;
	const char	*landmark;
	const char	*changetarget;		/* NULL when absent */
	const uint16_t	*twins;			/* ntwin pairs: A lump, B lump */
	const uint16_t	*pool;			/* npool sector-pool entries */
	float		delta[3];		/* landmark B - landmark A */
	float		T[3];			/* g_B + delta - g_A */
	float		sw_mins[3], sw_maxs[3];	/* switch eye box, A-local */
	float		carry_mins[3], carry_maxs[3];
	float		ap_mins[3], ap_maxs[3];	/* approach player-origin box */
	uint32_t	need[3];		/* render, texture, collision bytes */
	uint32_t	bcrc;			/* B .w64 directory CRC */
} seam_info_t;

typedef struct {
	uint16_t	nmaps, nlandmarks, nlinks, nseams, ntwins, npool;
	uint16_t	start, train;		/* 0xFFFF absent */
	uint16_t	components;
	uint32_t	strbytes, size;		/* string table, whole file */
} seam_graph_t;

typedef struct {
	const char	*name;
	uint16_t	component, first_landmark, nlandmarks, first_link, nlinks;
	float		gofs[3], mins[3], maxs[3];
} seam_map_t;

/* 0 when loaded; -1 without a campaign (or a bad index). */
int		seam_graph(seam_graph_t *g);
int		seam_map(int map, seam_map_t *m);
int		seam_landmark(int i, const char **name, float org[3]);
/* dest map, landmark name, spawnflags, seam index (0xFFFF none) */
int		seam_link(int i, uint16_t *dest, const char **landmark, uint16_t *spawnflags,
			uint16_t *seam);
int		seam_info(int i, seam_info_t *s);

#endif
