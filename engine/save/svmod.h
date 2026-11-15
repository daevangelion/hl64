/* SPDX-License-Identifier: MIT */
/*
 * svmod.h -- the save sections of the engine modules (engine/save/save.c
 * calls them in this order). Each writes its own state through sv.h;
 * each load reads it back into the module and returns 0, or -1 when the
 * section does not fit this build (the load then fails as corrupt).
 * Loads run after the session teardown, with the saved layers open in
 * their saved slots.
 */
#ifndef HL64_SVMOD_H
#define HL64_SVMOD_H

/* engine/core/eng.c: the frame count, a pending end-section restart */
void	eng_save_state(void);
int	eng_load_state(void);
/* engine/ent/pass.c: the game clock */
void	eng_clock_save(void);
int	eng_clock_load(void);
/* engine/core/rng.c */
void	rng_save(void);
int	rng_load(void);
/* engine/ent/rec.c: settle (open in-flight HLE slots, land map precache
 * holds; rec_settled: 1 when there is none), HLE slots (reopened at their
 * saved blocks), timers, uses, holders, runtime ids, twins, frozen maps,
 * the dehydrate store */
void	rec_settle(void);
int	rec_settled(void);
void	rec_save(void);
int	rec_load(void);
/* engine/ent/ent.c (+ heap.c, grid.c, priv.c): components, slot state,
 * think heap, active sets, grid, private slabs (game_priv_save / _load) */
void	ent_save(void);
int	ent_load(void);
void	ent_sets_save(void);
int	ent_sets_load(void);
void	ent_grid_save(void);
int	ent_grid_load(void);
void	ent_priv_save_all(void);
int	ent_priv_load_all(void);
/* After every section: the game's pointer fix-ups of the private blocks. */
int	ent_priv_fixup(void);
/* engine/coll/coll.c: the contents brushes, in their order */
void	col_contents_save(void);
int	col_contents_load(void);
/* engine/phys/pmove.c: the player block */
void	phy_player_save(void);
int	phy_player_load(void);
/* engine/phys/move.c: conveyors */
void	phy_conv_save(void);
int	phy_conv_load(void);
/* engine/in/in.c: view angles, held buttons, impulses */
void	in_save(void);
int	in_load(void);
/* engine/render/trans.c: renderfx fade remainders (they write renderamt) */
void	trans_save(void);
int	trans_load(void);
/* engine/render/decal.c: placed decals (their placements: a load rebuilds
 * their pieces when their chunks are resident) and pending placements */
void	decal_save(void);
int	decal_load(void);
/* engine/world/style.c: lightstyle letters by value + the tick */
void	wld_style_save(void);
int	wld_style_load(void);
/* engine/world/seam.c: the seam state machine */
void	seam_save(void);
int	seam_load(void);
/* engine/snd/snd.c: looping sounds (restarted on load) */
void	snd_save(void);
int	snd_load(void);
/* engine/render/studio.c: the ready animations of the models live entities
 * use (the hitbox source poses only those; a load reads them, load I/O,
 * and drops the others) */
void	studio_save(void);
int	studio_load(void);
/* engine/test/route.c (test builds): the route VM's position */
void	rt_save(void);
int	rt_load_state(void);

#endif
