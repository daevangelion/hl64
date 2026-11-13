/* SPDX-License-Identifier: MIT */
/*
 * audio_host.c -- the host build's null audio (engine/snd/snd_hw.h): no
 * output. A wave is opened by reading its wav64 header (libdragon's
 * documented layout: "WV64", version 6, format, channels, bits, then
 * big-endian rate, length, loop length, data offset, state size) through
 * the engine/io AUDIO class like the cart; each voice's play position
 * advances by rate x dt at every pump, a looped wave wraps to its loop
 * start, a one-shot ends at its length. engine/snd/snd.c above it is the
 * cart's code, so voice allocation, loops, sentences and the event log are
 * the cart's.
 */
#include <string.h>

#include "engine/core/sys.h"
#include "engine/io/io.h"
#include "engine/snd/snd_hw.h"

#define HDR_BYTES	28

typedef struct {
	uint8_t		used;
	snd_wave_info_t	inf;
} hwave_t;

typedef struct {
	const hwave_t	*w;
	double		pos;		/* samples */
	double		end_off;	/* s into the last pump at which it ended, -1 */
	float		rate;
	uint8_t		playing;
} hvoice_t;

typedef struct {
	hwave_t		w[SND_WAVES];
	hvoice_t	v[SND_VOICES];
	uint8_t		hdr[32] __attribute__((aligned(16)));
} host_audio_t;

static host_audio_t	*H;
static uint32_t		h_bytes;

static uint32_t be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

void snd_hw_init(void *mem, uint32_t bytes)
{
	if (bytes < sizeof(host_audio_t))
		sys_error("snd: SOUND region %lu B left under the host backend's %lu B",
			(unsigned long)bytes, (unsigned long)sizeof(host_audio_t));
	H = mem;
	memset(H, 0, sizeof(*H));
	h_bytes = ((uint32_t)sizeof(host_audio_t) + 15u) & ~15u;
}

int snd_hw_open(uint32_t rom, uint32_t size, snd_wave_info_t *info, void **wave)
{
	const uint8_t *h = H->hdr;
	int i;

	*wave = NULL;
	if (!rom || size < HDR_BYTES || (rom & 1))
		return -1;
	io_read_audio(H->hdr, rom, HDR_BYTES);
	if (memcmp(h, "WV64", 4) || h[4] != 6 || (h[5] != 0 && h[5] != 1) || (h[6] != 1 && h[6] != 2)
		|| h[7] != 16)
		return -1;
	memset(info, 0, sizeof(*info));
	info->format = h[5];
	info->channels = h[6];
	info->freq = be32(h + 8);
	info->len = be32(h + 12);
	info->loop_len = be32(h + 16);
	if (!info->freq || !info->len || info->loop_len > info->len || be32(h + 20) > size)
		return -1;
	for (i = 0; i < SND_WAVES; i++)
		if (!H->w[i].used)
		{
			H->w[i].used = 1;
			H->w[i].inf = *info;
			*wave = &H->w[i];
			return 0;
		}
	return -2;
}

void snd_hw_close(void *wave)
{
	int v;

	for (v = 0; v < SND_VOICES; v++)
		if (H->v[v].playing && H->v[v].w == wave)
			sys_error("snd: wave closed under voice %d", v);
	((hwave_t *)wave)->used = 0;
}

void snd_hw_play(int v, void *wave, uint32_t pos)
{
	hvoice_t *p = &H->v[v];

	p->w = wave;
	p->pos = pos;
	p->end_off = -1.0;
	p->rate = (float)p->w->inf.freq;
	p->playing = 1;
}

void snd_hw_stop(int v)
{
	H->v[v].playing = 0;
	H->v[v].w = NULL;
}

int snd_hw_playing(int v)
{
	return H->v[v].playing;
}

uint32_t snd_hw_pos(int v)
{
	return (uint32_t)H->v[v].pos;
}

void snd_hw_rate(int v, float hz)
{
	H->v[v].rate = hz;
}

void snd_hw_vol(int v, float left, float right)
{
	(void)v;
	(void)left;
	(void)right;
}

void snd_hw_pump(double dt)
{
	int v;

	for (v = 0; v < SND_VOICES; v++)
	{
		hvoice_t *p = &H->v[v];
		const snd_wave_info_t *in;
		double np;

		p->end_off = -1.0;
		if (!p->playing)
			continue;
		in = &p->w->inf;
		np = p->pos + (double)p->rate * dt;
		if (np < in->len)
			p->pos = np;
		else if (in->loop_len)
		{
			double ls = (double)(in->len - in->loop_len);

			while (np >= in->len)
				np -= in->loop_len;
			p->pos = np < ls ? ls : np;
		}
		else
		{
			p->end_off = ((double)in->len - p->pos) / (double)p->rate;
			p->pos = in->len;
			p->playing = 0;
		}
	}
}

void snd_hw_mem(uint32_t *now, uint32_t *peak)
{
	*now = *peak = h_bytes;
}

double snd_hw_end_offset(int v)
{
	return H->v[v].end_off;
}
