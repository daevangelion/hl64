/* SPDX-License-Identifier: MIT */
/*
 * snd_hw.h -- the sound backend under engine/snd/snd.c (docs/architecture.md
 * 10): mixer voices playing wav64 waves. plat/n64/audio.c drives the
 * libdragon mixer over cart-streamed wav64 (WAV64_STREAMING_FULL, wave
 * reads in the engine/io AUDIO class; a resident wave is read whole at
 * open, WAV64_STREAMING_NONE); plat/host/audio_host.c is the null
 * output of the host build: it reads the same wav64 headers through the
 * same I/O path and models each voice's play position from its rate and
 * the frame time, so voice allocation, loops and sentences run the same.
 *
 * Also the snd.c internals the host tests (test/m3/snd.c) call directly.
 */
#ifndef HL64_SND_HW_H
#define HL64_SND_HW_H

#include <stdint.h>

#include "hl64/types.h"

#define SND_VOICES		16	/* mixer voices */
#define SND_LOOPS		32	/* loop slots (sound.md 3) */
#define SND_WAVES		10	/* open wav64 streams */
#define SND_HZ			22050	/* output rate */
#define SND_PITCH_CAP		130	/* playback rate cap, % of SND_HZ */
#define SND_AUDIBLE		0.002f	/* loop ear-gain sum that holds a voice */
#define SND_CLIP_DIST		1000.0f	/* nominal clip distance (sound.md 2) */

typedef struct {
	uint32_t	freq;		/* sample rate, Hz */
	uint32_t	len;		/* samples */
	uint32_t	loop_len;	/* samples looped at the end, 0 = one-shot */
	uint8_t		format;		/* wav64 format: 0 raw, 1 VADPCM, 3 Opus */
	uint8_t		channels;
	uint8_t		resident;	/* samples held in memory (no reads per play) */
} snd_wave_info_t;

/* Boot: SND_VOICES voices at SND_HZ; mem/bytes = the SOUND region's
 * remainder after snd.c's tables (backend allocations live there). */
void		snd_hw_init(void *mem, uint32_t bytes);
/* Open the wav64 at cart range [rom, rom + size) into *wave: 0 ok, -1 not
 * a readable wav64 (never retried), -2 the backend's room is full now.
 * resident: hold its samples (VADPCM) when the room has them, else it
 * streams; info->resident says which. */
int		snd_hw_open(uint32_t rom, uint32_t size, int resident, snd_wave_info_t *info,
			void **wave);
/* Close a wave no voice plays (waits for the RSP first on the cart). */
void		snd_hw_close(void *wave);
/* Start wave on voice v from sample pos (0, or the loop start). */
void		snd_hw_play(int v, void *wave, uint32_t pos);
void		snd_hw_stop(int v);
int		snd_hw_playing(int v);
uint32_t	snd_hw_pos(int v);		/* samples played */
void		snd_hw_rate(int v, float hz);
void		snd_hw_vol(int v, float left, float right);
/* Feed the output (cart: mixer_try_play; host: advance by dt). */
void		snd_hw_pump(double dt);
/* Backend bytes in use now / at peak inside its part of the region. */
void		snd_hw_mem(uint32_t *now, uint32_t *peak);
/* Host: seconds into the last pump at which voice v's wave ended
 * (-1 still playing); 0 on the cart. */
double		snd_hw_end_offset(int v);

/* ---- snd.c internals used by the tests ------------------------------------- */

/* Ear gains of a source (sound.md 2) before master volume; self = the
 * listener's own sound. */
void		snd_gains(const float src[3], float vol, float attn, int self, float *left,
			float *right);
/* Playback rate of a sample rate at pitch (1..255), capped (sound.md 1). */
float		snd_pitch_rate(uint32_t freq, int pitch);
/* Event log lines ("snd ...") on / off; on by default in test builds. */
void		snd_log_set(int on);
/* Open waves now (S8). */
int		snd_waves_open(void);
/* S8 invariant: a busy voice whose wave slot is closed or holds another
 * sound (-1 when there is none). */
int		snd_wave_voice_check(void);

typedef struct {
	int		busy, ent, chan, loop;	/* ent -1 = fixed point */
	res_t		res;
	uint16_t	sent;			/* 0xFFFF = not a sentence */
	uint32_t	seq, pos;
	float		left, right;		/* ear gains before master volume */
	float		rate;			/* Hz */
	float		gap;			/* s before the current word, -1 first */
} snd_voice_info_t;
typedef struct {
	int		used, voice, ent, chan, pitch;
	res_t		res;
	float		vol;
} snd_loop_info_t;
void		snd_voice_info(int v, snd_voice_info_t *o);
void		snd_loop_info(int i, snd_loop_info_t *o);

#endif
