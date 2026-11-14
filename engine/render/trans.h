/* SPDX-License-Identifier: MIT */
/*
 * trans.h -- the translucent pass and the effects draw entry points of the
 * renderer (docs/specs/render-fx.md 1-2, docs/architecture.md 9.5-9.7).
 *
 * Frame step 11 order (architecture.md 3.2): world, movers, studio,
 * particles, translucent, decals, view model, HUD. ren_frame calls
 * fx_draw_begin before the studio entities (the studio module and the
 * mover draw hand their translucent entities over while they draw), then
 * fx_draw_particles / fx_draw_trans / fx_draw_decals after them. The
 * screen fade and titles are drawn by the HUD (fx_view, fx_text_current).
 */
#ifndef HL64_TRANS_H
#define HL64_TRANS_H

#include <stdint.h>

/* This frame's blend 0..255 of entity e from renderamt / renderfx
 * (render-fx.md 1); 0 = not drawn (render mode not Normal). */
int	ren_blend(int e);
/* A visible studio entity whose render mode is not Normal / TransAlpha:
 * drawn by the translucent pass (studio_draw_trans) back to front among
 * the sprites, brushes and effects. dist2 = squared eye distance. */
void	ren_trans_studio(int e, float dist2);
/* A visible brush mover whose render mode is not Normal (TransAlpha too:
 * lit, alpha compare, Z write); the pass calls ren_mover_draw
 * (engine/render/world.c). */
void	ren_trans_brush(int e, float dist2);
void	ren_mover_draw(int e, int gpu_mode, const uint8_t prim[4]);

void	fx_draw_begin(void);
void	fx_draw_particles(void);
void	fx_draw_trans(void);
void	fx_draw_decals(void);

typedef struct {
	uint32_t	items, cutouts, dropped, sprites, studio, brushes, temps, beams;
	uint32_t	glow_occluded, invisible, us;
	uint32_t	decals, decal_verts;	/* decals submitted, their vertices */
} fx_draw_stats_t;
/* Last drawn frame. */
void	fx_draw_stats(fx_draw_stats_t *s);
/* The mem command's `mem fx: decals` line: sheet, slots, verts and the
 * decal region's peak (sheet + verts + the largest sheet each slot held). */
void	decal_mem_line(void);

#endif
