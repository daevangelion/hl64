/* SPDX-License-Identifier: MIT */
/*
 * audio.c -- the cart's sound backend (engine/snd/snd_hw.h): libdragon
 * mixer voices over cart-streamed wav64 (WAV64_STREAMING_FULL). Retargeted
 * from the old tree's n64/port/snd_n64.c (audit entry n64/port/snd_n64.c
 * QUAKE-INTERFACE / CLEAN): its per-voice limits (16-bit, the 130 % rate
 * cap), the boot-time voice buffer preallocation and the
 * streaming load parameters are kept; the Quake S_* glue, cvars, path
 * building and Sys_* calls are not.
 *
 * The pump is libdragon's mixer_try_play: it queues the mixer's RSP rounds
 * and returns without waiting for them, unless the AI is down to its last
 * queued buffer (libdragon preview, mixer_poll_async).
 *
 * Memory (architecture 4.3, nothing on the system heap after boot): every
 * block libdragon's mixer and wav64 allocate for us comes from a
 * first-fit arena in the SOUND region (engine/mem/heap.h redirect, active
 * only inside our libdragon calls; frees of arena blocks are recognised
 * by address at any time).
 *
 * Wave reads: a libdragon filesystem ("snd:/<rom>/<size>") whose reads go
 * through engine/io's AUDIO class (io_read_audio) via a 16-aligned bounce
 * buffer, so the PI accounting sees them and they never race the I/O
 * scheduler's piece in flight.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <libdragon.h>
#include <system.h>

#include "engine/core/sys.h"
#include "engine/io/io.h"
#include "engine/mem/heap.h"
#include "engine/snd/snd_hw.h"
#include "plat/n64/n64.h"

#define BOUNCE		2048
#define FILES		16
#define HDR_BYTES	32
#define BLK_HDR		16u

/* ---- arena ----------------------------------------------------------------- */

typedef struct {
	uint32_t	size;		/* whole block, header included, multiple of 16 */
	uint32_t	used;
	uint32_t	pad[2];
} blk_t;

static uint8_t		*a_lo, *a_hi;	/* KSEG0 */
static uint32_t		a_used, a_peak;

static uint8_t *kseg0(const void *p)
{
	return (uint8_t *)(((uintptr_t)p & 0x1FFFFFFFu) | 0x80000000u);
}

static int arena_owns(const void *p)
{
	uint8_t *q = kseg0(p);

	return p && q >= a_lo && q < a_hi;
}

static void *arena_alloc(size_t align, size_t size)
{
	uint32_t need = BLK_HDR + (((uint32_t)size + 15u) & ~15u);
	uint8_t *q;

	if (align > 16)
		sys_error("snd: arena alignment %u", (unsigned)align);
	for (q = a_lo; q < a_hi; q += ((blk_t *)q)->size)
	{
		blk_t *b = (blk_t *)q;

		if (b->used || b->size < need)
			continue;
		if (b->size - need >= 2 * BLK_HDR)
		{
			blk_t *n = (blk_t *)(q + need);

			n->size = b->size - need;
			n->used = 0;
			b->size = need;
		}
		b->used = 1;
		a_used += b->size;
		if (a_used > a_peak)
			a_peak = a_used;
		return q + BLK_HDR;
	}
	return NULL;
}

static void arena_release(void *p)
{
	blk_t *b = (blk_t *)(kseg0(p) - BLK_HDR);
	uint8_t *q;

	if (!b->used)
		sys_error("snd: arena double free %p", p);
	b->used = 0;
	a_used -= b->size;
	/* coalesce free neighbours */
	for (q = a_lo; q < a_hi; )
	{
		blk_t *c = (blk_t *)q, *n = (blk_t *)(q + c->size);

		if (!c->used && (uint8_t *)n < a_hi && !n->used)
			c->size += n->size;
		else
			q += c->size;
	}
}

static size_t arena_size(void *p)
{
	return ((blk_t *)(kseg0(p) - BLK_HDR))->size - BLK_HDR;
}

static uint32_t arena_largest(void)
{
	uint32_t best = 0;
	uint8_t *q;

	for (q = a_lo; q < a_hi; q += ((blk_t *)q)->size)
		if (!((blk_t *)q)->used && ((blk_t *)q)->size > best)
			best = ((blk_t *)q)->size;
	return best > BLK_HDR ? best - BLK_HDR : 0;
}

static mem_heap_redirect_t	redirect = {
	arena_alloc, arena_release, arena_size, arena_owns, 0
};

/* ---- the AUDIO-class filesystem -------------------------------------------- */

typedef struct {
	uint8_t		used;
	uint32_t	rom, size, pos;
} sfile_t;

static sfile_t	files[FILES];
static uint8_t	*bounce;	/* 16-aligned, BOUNCE bytes */

static uint32_t hex(const char **s)
{
	uint32_t v = 0;

	for (;; (*s)++)
	{
		char c = **s;

		if (c >= '0' && c <= '9')
			v = v << 4 | (uint32_t)(c - '0');
		else if (c >= 'a' && c <= 'f')
			v = v << 4 | (uint32_t)(c - 'a' + 10);
		else
			return v;
	}
}

/*
 * Voice buffers are sized at boot: every voice plays this silent wave once
 * and stops. libdragon sizes a voice's sample ring for the format it plays
 * (compressed VADPCM frames since libdragon preview), and a mono 4-bit
 * VADPCM ring (9-byte frames plus their mirrored margin) is the largest
 * the cook makes (mono VADPCM, PCM fallback), so no later play reallocates.
 * A wav64 version 10 file: header (32 B), VADPCM extension (96 B: 4
 * predictors of order 2, no flags or skip points, 4-bit residuals), one
 * zero codebook (136 B), two zero frames (18 B). Served by snd_fs as rom 0.
 */
static const uint8_t	silent_vadpcm[32 + 96 + 136 + 18] __attribute__((aligned(16))) = {
	'W', 'V', '6', '4', 10, 1, 1, 16,
	0, 0, 0x56, 0x22,	/* 22050 Hz */
	0, 0, 0, 32,		/* 32 samples */
	0, 0, 0, 0,		/* loop length */
	0, 0, 0, 0,		/* loop end */
	0, 0, 0x01, 0x08,	/* samples at 264 */
	0, 0, 0, 48,		/* decoder state bytes */
	4, 2, 0, 0, 0, 0, 4, 0,	/* predictors, order, flags, skip points, residual bits, attack frames */
};

static void *fs_open(char *name, int flags)
{
	const char *s = name;
	int i;

	(void)flags;
	for (i = 0; i < FILES && files[i].used; i++)
		;
	if (i == FILES)
	{
		errno = EMFILE;
		return NULL;
	}
	files[i].rom = hex(&s);
	if (*s++ != '/')
	{
		errno = ENOENT;
		return NULL;
	}
	files[i].size = hex(&s);
	if (!files[i].rom && files[i].size != sizeof(silent_vadpcm))
	{
		errno = ENOENT;
		return NULL;
	}
	files[i].pos = 0;
	files[i].used = 1;
	return &files[i];
}

static int fs_fstat(void *file, struct stat *st)
{
	memset(st, 0, sizeof(*st));
	st->st_size = ((sfile_t *)file)->size;
	st->st_mode = S_IFREG;
	return 0;
}

static int fs_lseek(void *file, int ofs, int dir)
{
	sfile_t *f = file;
	int p = dir == SEEK_SET ? ofs : dir == SEEK_CUR ? (int)f->pos + ofs : (int)f->size + ofs;

	if (p < 0 || (uint32_t)p > f->size)
	{
		errno = EINVAL;
		return -1;
	}
	f->pos = (uint32_t)p;
	return p;
}

static int fs_read(void *file, uint8_t *ptr, int len)
{
	sfile_t *f = file;
	int done = 0;

	if (len > (int)(f->size - f->pos))
		len = (int)(f->size - f->pos);
	if (!f->rom)
	{
		memcpy(ptr, silent_vadpcm + f->pos, (size_t)len);
		f->pos += (uint32_t)len;
		done = len;
	}
	while (done < len)
	{
		uint32_t at = f->rom + f->pos, odd = at & 1u;
		uint32_t n = (uint32_t)(len - done);

		if (n > BOUNCE - 2)
			n = BOUNCE - 2;
		io_read_audio(bounce, at - odd, (n + odd + 1u) & ~1u);
		memcpy(ptr + done, bounce + odd, n);
		f->pos += n;
		done += (int)n;
	}
	/* cached destinations (wav64 header, codebook, the CachedAddr of a
	 * voice's uncached sample buffer): the RSP reads RDRAM */
	if (len > 0 && ((uintptr_t)ptr & 0xE0000000u) == 0x80000000u)
		data_cache_hit_writeback_invalidate(ptr, (unsigned long)len);
	return len;
}

static int fs_close(void *file)
{
	((sfile_t *)file)->used = 0;
	return 0;
}

static filesystem_t	snd_fs = {
	.open = fs_open,
	.fstat = fs_fstat,
	.lseek = fs_lseek,
	.read = fs_read,
	.close = fs_close,
};

/* ---- backend --------------------------------------------------------------- */

typedef struct {
	wav64_t		*wav;
} cwave_t;

static cwave_t	waves[SND_WAVES];

void snd_hw_init(void *mem, uint32_t bytes)
{
	wav64_loadparms_t parms;
	wav64_t *silent;
	char path[32];
	uint8_t *m = kseg0(mem);
	blk_t *b;
	int i;

	if (bytes < BOUNCE + 4 * BLK_HDR)
		sys_error("snd: SOUND region %lu B left for the backend", (unsigned long)bytes);
	bounce = m;
	a_lo = m + BOUNCE;
	a_hi = m + (bytes & ~15u);
	b = (blk_t *)a_lo;
	b->size = (uint32_t)(a_hi - a_lo);
	b->used = 0;
	a_used = a_peak = 0;
	if (attach_filesystem("snd:/", &snd_fs) < 0)
		sys_error("snd: cannot attach the snd:/ filesystem");
	mem_heap_redirect(&redirect);
	redirect.active = 1;
	mixer_init(SND_VOICES);
	/*
	 * Voice buffers now, not on a voice's first play: mixer_ch_play
	 * allocates a voice's sample ring when it first sounds and keeps it
	 * while the formats that follow fit. libdragon sizes it for two AI
	 * buffers of input at the voice's rate cap (no byte cap of ours: a
	 * smaller ring would wrap onto samples the RSP has yet to read), in
	 * the wave's units, plus its decoder state; silent_vadpcm sizes it
	 * (mono 4-bit VADPCM: 4224 + 48 B). Stopped before any poll.
	 */
	snprintf(path, sizeof(path), "snd:/0/%x", (unsigned)sizeof(silent_vadpcm));
	memset(&parms, 0, sizeof(parms));
	parms.streaming_mode = WAV64_STREAMING_FULL;
	silent = wav64_load(path, &parms);
	if (!silent)
		sys_error("snd: cannot open the voice-sizing wave");
	for (i = 0; i < SND_VOICES; i++)
	{
		mixer_ch_set_limits(i, 16, (float)SND_HZ * (SND_PITCH_CAP * 0.01f), 0);
		wav64_play(silent, i);
		mixer_ch_stop(i);
		mixer_ch_set_vol(i, 1.0f, 1.0f);
	}
	wav64_close(silent);
	redirect.active = 0;
	mixer_set_vol(1.0f);
}

static uint32_t be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

int snd_hw_open(uint32_t rom, uint32_t size, snd_wave_info_t *info, void **wave)
{
	wav64_loadparms_t parms;
	char path[32];
	uint8_t h[HDR_BYTES];
	uint32_t ext, need;
	int i;

	*wave = NULL;
	if (!rom || size < HDR_BYTES || (rom & 1))
		return -1;
	io_read_audio(bounce, rom, HDR_BYTES);
	memcpy(h, bounce, HDR_BYTES);
	/* wav64 asserts on what it cannot play: check first (version 10, VADPCM
	 * or raw, mono / stereo, 16-bit, a loop only at the end, data inside
	 * the file) */
	if (memcmp(h, "WV64", 4) || h[4] != 10 || (h[5] != 0 && h[5] != 1) || (h[6] != 1 && h[6] != 2)
		|| h[7] != 16 || !be32(h + 8) || !be32(h + 12) || be32(h + 16) > be32(h + 12)
		|| be32(h + 20) || be32(h + 24) < HDR_BYTES || be32(h + 24) > size)
		return -1;
	for (i = 0; i < SND_WAVES && waves[i].wav; i++)
		;
	/* wav64 state + waveform + name + extended header (+ VADPCM Huffman
	 * tables 3 x 256) in one block, then the file handle */
	ext = be32(h + 24) - HDR_BYTES;
	need = 256 + ((ext + 15u) & ~15u) + 3 * 256 + 64;
	if (i == SND_WAVES || arena_largest() < need)
		return -2;
	/* wav64 opens (and closes) the file through snd_fs */
	snprintf(path, sizeof(path), "snd:/%lx/%lx", (unsigned long)rom, (unsigned long)size);
	memset(&parms, 0, sizeof(parms));
	parms.streaming_mode = WAV64_STREAMING_FULL;
	redirect.active = 1;
	waves[i].wav = wav64_load(path, &parms);
	redirect.active = 0;
	if (!waves[i].wav)
		return -2;
	memset(info, 0, sizeof(*info));
	info->format = h[5];
	info->channels = h[6];
	info->freq = (uint32_t)waves[i].wav->wave.frequency;
	info->len = (uint32_t)waves[i].wav->wave.len;
	info->loop_len = (uint32_t)waves[i].wav->wave.loop_len;
	*wave = &waves[i];
	return 0;
}

void snd_hw_close(void *wave)
{
	cwave_t *w = wave;

	/* wav64_close waits for the mixer rounds still reading the wave */
	wav64_close(w->wav);
	w->wav = NULL;
}

void snd_hw_play(int v, void *wave, uint32_t pos)
{
	cwave_t *w = wave;

	redirect.active = 1;	/* a bigger decoder state reallocates the buffer */
	wav64_play(w->wav, v);
	redirect.active = 0;
	if (pos)
		mixer_ch_set_pos(v, (double)pos);
}

void snd_hw_stop(int v)
{
	mixer_ch_stop(v);
}

int snd_hw_playing(int v)
{
	return mixer_ch_playing(v);
}

uint32_t snd_hw_pos(int v)
{
	return (uint32_t)mixer_ch_get_pos(v);
}

void snd_hw_rate(int v, float hz)
{
	mixer_ch_set_freq(v, hz);
}

void snd_hw_vol(int v, float left, float right)
{
	mixer_ch_set_vol(v, left, right);
}

#ifdef HL64_PROFILE
static unsigned	pump_n[2];
static uint64_t	pump_ticks[2];

void audio_pump_take(unsigned n[2], uint64_t ticks[2])
{
	int k;

	for (k = 0; k < 2; k++)
	{
		n[k] = pump_n[k];
		ticks[k] = pump_ticks[k];
		pump_n[k] = 0;
		pump_ticks[k] = 0;
	}
}
#endif

void snd_hw_pump(double dt)
{
#ifdef HL64_PROFILE
	int busy = vid_gpu_busy() != 0;
	uint32_t t0 = TICKS_READ();
#endif

	(void)dt;
	mixer_try_play();
#ifdef HL64_PROFILE
	pump_n[busy]++;
	pump_ticks[busy] += TICKS_READ() - t0;
#endif
}

void snd_hw_mem(uint32_t *now, uint32_t *peak)
{
	*now = BOUNCE + a_used;
	*peak = BOUNCE + a_peak;
}

double snd_hw_end_offset(int v)
{
	(void)v;
	return 0.0;
}
