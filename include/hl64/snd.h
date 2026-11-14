/* SPDX-License-Identifier: MIT */
/*
 * snd.h -- sound services (docs/architecture.md 10, docs/specs/sound.md):
 * GoldSrc channel semantics over 16 mixer voices, cart-streamed wav64,
 * cooked sounds (res_t) and sentences (rtb SENT / WORD). The host build runs
 * the same voice allocation and spatialisation with null audio and an
 * event log (S1-S8).
 */
#ifndef HL64_SND_H
#define HL64_SND_H

#include <stdint.h>

#include "hl64/types.h"

/* HLSDK const.h */
#define CHAN_AUTO		0
#define CHAN_WEAPON		1
#define CHAN_VOICE		2
#define CHAN_ITEM		3
#define CHAN_BODY		4
#define CHAN_STREAM		5
#define CHAN_STATIC		6
#define ATTN_NONE		0.0f
#define ATTN_NORM		0.8f
#define ATTN_IDLE		2.0f
#define ATTN_STATIC		1.25f
#define SND_STOP		(1 << 5)
#define SND_CHANGE_VOL		(1 << 6)
#define SND_CHANGE_PITCH	(1 << 7)
#define SND_SPAWNING		(1 << 8)
#define PITCH_NORM		100

/* pfnEmitSound: source = entity e's origin, followed while it plays. */
void	snd_emit(int e, int chan, res_t sound, float vol, float attn, int flags, int pitch);
/* pfnEmitAmbientSound: fixed point, map-local in the caller's frame. */
void	snd_ambient(const float org[3], res_t sound, float vol, float attn, int flags, int pitch);
/* Sentence id (rtb SENT) on (e, chan): words in sequence on one voice. */
void	snd_sentence(int e, int chan, uint16_t sentence, float vol, float attn, int flags,
		int pitch);
/* Stop (e, chan) (sound.md 1 STOP); chan -1: every channel of e. */
void	snd_stop(int e, int chan);
/* Every voice and loop stops (New Game after an end section; GoldSrc
 * stops all sounds at disconnect). */
void	snd_stop_all(void);

/* Engine: boot, step 10 (listener = view origin + right vector, global),
 * free entity e's voices' positions when it is freed. */
void	snd_init(void);
void	snd_frame(const float listener[3], const float right[3], double dt);
void	snd_entity_freed(int e);

/* The window runs from the last reset: emits..dropped and the peaks count
 * in it, us = mean snd_frame CPU (backend pump included) over its frames,
 * us_max the worst frame. mem = SOUND region bytes in use (tables + the
 * backend's voice buffers and open waves), mem_peak its high-water mark
 * since boot, mem_region the region's size (HL64_SOUND_KB). */
typedef struct {
	uint32_t	emits, started, stolen, dropped;	/* in the window */
	uint32_t	voices, voices_peak, loops, loops_peak, waves_open, waves_peak;
	uint32_t	us, us_max, frames;
	uint32_t	pump_us;	/* mean backend pump (cart: mixer_try_play) per frame */
	uint32_t	mem, mem_peak, mem_region;
} snd_stats_t;
void	snd_stats(snd_stats_t *s, int reset);
/* Sounds and sentences started (snd_emit, snd_sentence calls) since boot:
 * the game's AI work counters take deltas around a monster think. */
uint32_t snd_emit_count(void);

#endif
