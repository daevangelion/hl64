/* SPDX-License-Identifier: MIT */
/*
 * studio.h -- studio models (docs/architecture.md 9.4, D3(b); docs/specs/
 * trace-and-hulls.md 4.5): residency of cooked studio models and their
 * per-sequence animation chunks, the model info the game's animation code
 * reads (sequences, events, attachments, bones, hitboxes, controllers),
 * and the engine draw. The pose maths (HLSDK StudioModelRenderer /
 * animation.cpp) live in game/studio (HLSDK licence) and read the cooked
 * info blocks through this API; the engine never interprets MDL structs.
 * Formats: docs/formats/studio-*.md (cooked by tools/studio).
 */
#ifndef HL64_STUDIO_H
#define HL64_STUDIO_H

#include <stdint.h>

#include "hl64/types.h"

/* The model's cooked info block (bones, controllers, sequences with
 * their fps / frames / flags / motion / events, attachments, hitboxes,
 * body parts): resident whenever the model is (pinned by live entities);
 * loads synchronously on first use (counted). Layout: the studio format
 * doc. NULL for a non-studio res. */
const void	*studio_info(res_t model, uint32_t *size);
/* Animation chunk of one sequence (D3(b)): resident or NULL; a NULL
 * chunk requests it (prefetch class) and the game holds the last pose. */
const void	*studio_anim(res_t model, int sequence, uint32_t *size);
/* Body combinations of the model (product of body-part model counts;
 * GoldSrc ModelFrames for studio models), 1 when not resident yet. */
int		studio_body_count(res_t model);
/* Interest in a sequence's chunk ahead of use (scripted sequences). */
void		studio_want(res_t model, int sequence);

/* Engine: boot, per-frame residency (live entity models pinned), draw of
 * the visible studio entities with bones from game_studio_setup_bones,
 * hitbox source for engine/coll (col_set_hitbox_source). */
void		studio_init(void);
void		studio_frame(void);

typedef struct {
	uint32_t	models, pool_used, pool_peak, anims, sync_loads;
	uint32_t	drawn, bones_us, draw_us, us_max;
} studio_stats_t;
void		studio_stats(studio_stats_t *s, int reset);

#endif
