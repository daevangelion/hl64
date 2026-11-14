/* SPDX-License-Identifier: MIT */
/*
 * coll.h -- collision: point contents, hull selection, traces against the
 * world's open layers, brush entities (rotated or not) and boxes, the move
 * query with its filters, hitbox refinement, surface materials and the
 * collision residency (docs/specs/trace-and-hulls.md, docs/architecture.md
 * 5.3 and 8, docs/formats/w64-format.md "Collision").
 *
 * Frames (architecture.md 8): every entry the game or physics calls takes
 * and returns coordinates in the frame of eng_frame_layer()
 * (include/hl64/ent.h; -1 = global): a layer's world model sits at
 * g_M(layer) - g_M(frame) (exactly 0 for the frame's own layer), and an
 * entity of another layer (a candidate of the move query, a contents
 * brush, col_clip of a stored view) is moved by g_M(its layer) -
 * g_M(frame). A model's `origin` argument is in the caller's frame: the
 * model frame is point - origin. col_want takes a global box (residency).
 * Every hull walk is iterative with an explicit stack;
 * no call allocates. A COLL chunk a walk needs that is not resident is read
 * synchronously (counted as blocked I/O); col_want() keeps the player's
 * collision window resident so that does not happen on the steady path.
 */
#ifndef HL64_COLL_H
#define HL64_COLL_H

#include <stdint.h>

/* Contents (HLSDK const.h values). */
#define COL_EMPTY		(-1)
#define COL_SOLID		(-2)
#define COL_WATER		(-3)
#define COL_SLIME		(-4)
#define COL_LAVA		(-5)
#define COL_SKY			(-6)
#define COL_CURRENT_0		(-9)
#define COL_CURRENT_DOWN	(-14)
#define COL_TRANSLUCENT		(-15)
#define COL_LADDER		(-16)

/* Entity solid types and the flags / render mode the filters read (HLSDK). */
#define COL_SOLID_NOT		0
#define COL_SOLID_TRIGGER	1
#define COL_SOLID_BBOX		2
#define COL_SOLID_SLIDEBOX	3
#define COL_SOLID_BSP		4
#define COL_FL_MONSTER		(1u << 5)
#define COL_FL_MONSTERCLIP	(1u << 23)	/* func_monsterclip: blocks monsters only */
#define COL_RENDER_NORMAL	0
#define COL_MOVETYPE_PUSHSTEP	13	/* func_pushable */

/* Move types (trace-and-hulls.md 4.4). */
#define COL_MOVE_NORMAL		0
#define COL_MOVE_NOMONSTERS	1
#define COL_MOVE_MISSILE	2
#define COL_MOVE_NOGLASS	0x100

#define COL_NONE		0xFFFF
#define COL_HULLS		4

/* Brush model reference: open layer slot (engine/world) and BSP model
 * index (0 = the layer's world model). */
#define COL_MODEL(slot, sub)	((uint16_t)((slot) << 12 | (sub)))
#define COL_MODEL_SLOT(m)	((m) >> 12)
#define COL_MODEL_SUB(m)	((m) & 0xFFF)

/* Hull boxes (HLSDK VEC_HULL / VEC_DUCK_HULL; hull 0 is the point). */
extern const float	col_hull_mins[COL_HULLS][3];
extern const float	col_hull_maxs[COL_HULLS][3];

/* Trace result (HLSDK TraceResult fields, spec 3). */
typedef struct {
	float		frac;		/* flFraction */
	float		end[3];		/* vecEndPos */
	float		normal[3];	/* vecPlaneNormal */
	float		dist;		/* flPlaneDist (brush entities: model frame, 4.2) */
	int16_t		ent;		/* pHit: entity index, 0 world, -1 none */
	uint8_t		allsolid, startsolid, inopen, inwater;
	uint8_t		hitgroup;	/* iHitgroup */
	uint8_t		pad;
} col_trace_t;

/* The collision view of an entity. The entity layer owns the storage
 * (pointers stay valid while the entity is linked) and keeps absmin /
 * absmax as the linked box (area-links-and-touch.md). */
typedef struct col_ent_s {
	float		origin[3];
	float		angles[3];
	float		mins[3], maxs[3];
	float		absmin[3], absmax[3];
	uint32_t	flags;		/* COL_FL_* bits of the entity's flags */
	uint16_t	index;		/* entity index (0 = world) */
	uint16_t	owner;		/* entity index, COL_NONE */
	uint16_t	model;		/* brush model COL_MODEL(), COL_NONE */
	uint16_t	studio;		/* studio model (hitboxes), COL_NONE */
	int16_t		skin;		/* contents brushes: COL_* contents (< -1) */
	uint8_t		solid;		/* COL_SOLID_* */
	uint8_t		rendermode;
	uint8_t		movetype;	/* MOVETYPE_* (PUSHSTEP: ignore_monsters still hits) */
	uint8_t		pad;
} col_ent_t;

/* An oriented hitbox (trace-and-hulls.md 4.5): box [mins, maxs] in the
 * bone frame; bone = 3 x 4 bone-to-world matrix (rows; column 3 the
 * translation), as HLSDK studio bone transforms. */
typedef struct {
	float		bone[3][4];
	float		mins[3], maxs[3];
	uint8_t		group;
	uint8_t		pad[3];
} col_hitbox_t;

/* ---- provided by other modules (link-time contract) ----------------------- */

/* Entity layer (engine/ent spatial grid): entities whose linked box meets
 * [mins, maxs] (frame of eng_frame_layer()), every solid type, ascending
 * index, no duplicates; at most max, returns the count. */
int		col_ents_box(const float mins[3], const float maxs[3], const col_ent_t **out, int max);

/* Studio pose (M3, game_studio_setup_bones): the entity's hitboxes at its
 * current pose, in the entity's own map-local frame; returns the count
 * (0: no hitboxes, the bounding box stands). Set to NULL (the default)
 * until a studio module exists: line traces then stop at the bounding
 * box. */
typedef int	(*col_hitbox_fn)(const col_ent_t *e, const col_hitbox_t **out);
void		col_set_hitbox_source(col_hitbox_fn fn);

/* ---- contents ------------------------------------------------------------- */

/* pfnPointContents (spec 1): world hull-0 contents, CURRENT_* as WATER,
 * LADDER / water inside a linked contents brush; SOLID outside every open
 * layer. */
int		col_contents(const float p[3]);
/* PM_TruePointContents: world only, CURRENT_* kept. */
int		col_true_contents(const float p[3]);
/* Contents brushes (SOLID_NOT brush entities with skin < -1: func_ladder,
 * func_water): the entity layer adds them when linked, removes them when
 * unlinked or freed. */
void		col_contents_add(const col_ent_t *e);
void		col_contents_remove(const col_ent_t *e);
/* Contents of hull k of a brush model placed at (origin, angles), at a
 * point (origin and point in one frame). */
int		col_model_contents(uint16_t model, const float origin[3], const float angles[3],
			int hull, const float p[3]);

/* Hull handle (pmove's PM_HullForBsp / PM_HullPointContents). root is the
 * walk start (node gid >= 0, or a leaf child < 0). */
typedef struct {
	uint16_t	model;
	uint8_t		hull;
	uint8_t		pad;
	int32_t		root;
} col_hull_t;
/* Fills h; offset = origin (the model frame origin, in the caller's
 * frame); returns 0 for a bad model. */
int		col_hull_get(uint16_t model, int hull, const float origin[3], col_hull_t *h,
			float offset[3]);
/* Contents at p (model frame: point - offset) walking from node. */
int		col_hull_contents(const col_hull_t *h, int32_t node, const float p[3]);

/* ---- traces ---------------------------------------------------------------- */

/* Hull for a moving box against a brush model (spec 2). */
int		col_hull_for_size(const float mins[3], const float maxs[3]);

/* The world (entity 0): every open layer whose box meets the move box,
 * nearest impact, ties to the lower slot (spec 4.1). */
void		col_trace_world(const float start[3], const float mins[3], const float maxs[3],
			const float end[3], col_trace_t *tr);
/* One brush model at (origin, angles) for hull k (the moving box is that
 * hull's box): spec 4.2 frames, plane dist in the model frame (the world
 * model too); tr->ent is left -1. */
void		col_trace_model(uint16_t model, const float origin[3], const float angles[3], int hull,
			const float start[3], const float end[3], col_trace_t *tr);
/* One entity (pfnTraceModel / pmove physents): SOLID_BSP and contents
 * brushes through their model (hull by size, 4.2), others as their box
 * (4.3); no hitboxes. tr->ent = e->index on a hit or start solid. A view
 * in the entity layer's storage is moved into the caller's frame; a copy
 * (pmove physents) is taken as already in it. */
void		col_clip(const col_ent_t *e, const float start[3], const float mins[3],
			const float maxs[3], const float end[3], col_trace_t *tr);
/* The move query (spec 4.4): world, then grid candidates with the filters,
 * hitboxes for zero-size line traces (4.5). pass may be NULL. */
void		col_move(const float start[3], const float mins[3], const float maxs[3],
			const float end[3], int type, const col_ent_t *pass, col_trace_t *tr);
/* pfnTraceLine: col_move with a zero box, except that a trace that hits
 * nothing reports the world (ent 0), not -1 [probe:measured T-entities,
 * T-rotated, T-hitbox]. */
void		col_trace_line(const float start[3], const float end[3], int type,
			const col_ent_t *pass, col_trace_t *tr);
/* Line trace against oriented hitboxes (4.5): nearest entry; returns 1 and
 * fills frac / end / normal / hitgroup on a hit. */
int		col_trace_hitboxes(const col_hitbox_t *hb, int n, const float start[3],
			const float end[3], col_trace_t *tr);

/* Surface material (spec 4.7, w64-format.md "Surface materials"): the HLSDK
 * texture-type character ('C' concrete when unknown) of the first hull-0
 * crossing out of a non-solid leaf of the model at (origin, angles) along
 * start -> end; 0 when nothing is crossed. */
int		col_material(uint16_t model, const float origin[3], const float angles[3],
			const float start[3], const float end[3]);

/* Brush model bounds (model frame). Returns 0 for a bad model. */
int		col_model_bounds(uint16_t model, float mins[3], float maxs[3]);

/* Boot (after wld_init): test builds register the `colbench MAP` debug
 * command (engine/coll/colbench.c). */
void		col_init(void);

/* ---- residency (architecture.md 5.3) ----------------------------------------- */

/* OPEN-class collision window, once per frame: the hull 1-3 chunks of the
 * grid cells within 128 u of [mins, maxs] (global) and the chunks of the
 * brushes that window touches (col_models_box). */
void		col_want(const float mins[3], const float maxs[3]);
/* The brush models (solid, trigger, contents) whose linked boxes meet a
 * global box: slot << 16 | submodel into out, at most max; the count
 * (the stream's prefetch windows of moving entities and point pins). */
int		col_models_box(const float lo[3], const float hi[3], uint32_t *out, int max);

typedef struct {
	uint32_t	traces, nodes;		/* since the last call */
	uint32_t	sync_loads, sync_us;	/* chunks read mid-trace (blocked I/O) */
} col_stats_t;
void		col_stats(col_stats_t *s);
/* The same counters without resetting them (since the last col_stats):
 * the game's AI work counters take deltas around a monster think. */
void		col_counts(col_stats_t *s);

#endif
