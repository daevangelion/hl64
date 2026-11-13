/* SPDX-License-Identifier: MIT */
/*
 * wld_int.h -- engine/world internals shared by world.c (packages, layer
 * open, chunk requests, eviction) and stream.c (the residency classes'
 * target set, pins, prefetch, teleport and pop-in accounting).
 */
#ifndef HL64_WLD_INT_H
#define HL64_WLD_INT_H

#include "engine/world/wld.h"

/* Target-set generation (never 0): a chunk with want == wld_tgen is in
 * this generation's set with class cls. */
extern uint16_t		wld_tgen;
/* Frame number of wld_frame (+1; 0 = never used). */
extern uint32_t		wld_frame_no;
/* Bumped when a layer becomes ready or closes (a target-set input). */
extern uint32_t		wld_layer_gen;

/* Layer struct of slot s whether ready, opening or closed. */
wld_layer_t	*wld_slot(int s);
/* Name chunk c in the current generation with class cls: the strongest
 * naming of a generation wins; an absent chunk is requested at the class's
 * priority, a queued request of a weaker class is promoted. 0 when it is
 * absent and could not be queued (no request slot / no room). */
int		wld_name(wld_layer_t *L, int c, int cls);
/* Advance an async open (header, directory + CRC, tables, pinned hull 0,
 * ENTS scan); crc_budget bytes of the directory CRC per call (0 = all). */
void		wld_open_step(wld_layer_t *L, uint32_t crc_budget);
/* Counters of world.c's request side (stream stats). */
extern uint32_t		wld_n_promoted, wld_n_evict_queued, wld_n_prefetch_req;
extern uint32_t		wld_n_open_async, wld_n_open_fail;
/* wld_close: the slot's seam pin goes. */
void		wld_stream_closed(int slot);
/* After the stream named a generation: the bytes its open + pin classes
 * want per pool, into wld_stats_t.want_peak. */
void		wld_want_measure(void);
/* PVS rows (world.c, architecture.md 5.5): name leaf's row of L in the
 * current generation at class cls (queued asynchronously into the row
 * cache unless cached or loading); wld_pvs_step, once per frame, moves
 * the landed offset pairs on to their row reads. */
void		wld_pvs_want(wld_layer_t *L, int leaf, int cls);
void		wld_pvs_step(void);
#ifdef HL64_HOST
/* Host tests: hold `bytes` of the texture pool (a locked block at its
 * high end; 0 gives it back), so an eviction test fills it with one map. */
void		wld_test_squeeze(uint32_t bytes);
#endif

#endif
