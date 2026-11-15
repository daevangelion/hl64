/* SPDX-License-Identifier: MIT */
/*
 * snd.c -- sound services (include/hl64/snd.h; docs/architecture.md 10,
 * docs/specs/sound.md): GoldSrc channel semantics over SND_VOICES mixer
 * voices, loops in SND_LOOPS virtualised slots, SND_WAVES open wav64
 * streams with LRU close, sentences word after word, spatialisation from
 * the listener's view origin and right vector. The backend (snd_hw.h) is
 * the libdragon mixer on the cart and a null output on the host; both run
 * this file unchanged, so the host's event log is the cart's policy.
 *
 * Retargeted from the old tree's n64/port/snd_n64.c (audit entry
 * n64/port/snd_n64.c QUAKE-INTERFACE / CLEAN, docs/provenance-audit.md):
 * its voice policy (free voice, oldest one-shot, loop voice last), the
 * virtualised loop slots, the open-wave LRU (never closed under a voice,
 * RSP wait before close) and the word-after-word sentence voice are kept;
 * every Quake binding (S_* API, sfx_t names and the name registry, cvars,
 * commands, cl./sv. state, channel_t vocabulary) is replaced by res_t
 * ids, the rtb file table (res_file), the SENT / WORD tables and the
 * engine's component arrays. The spatialisation (audit-flagged range of
 * the old file, not read) is written from sound.md 2.
 *
 * Positions are global (map-local origin + g_M of the entity's layer), so
 * sources in both open layers spatialise without conversion. Nothing is
 * allocated: the tables live in the SOUND region, the backend uses the
 * rest of it.
 */
#include <math.h>
#include <stdarg.h>
#include <string.h>

#include "hl64/coll.h"
#include "hl64/ent.h"
#include "hl64/rec.h"
#include "hl64/settings.h"
#include "hl64/snd.h"
#include "engine/core/log.h"
#include "engine/core/dbg.h"
#include "engine/core/sys.h"
#include "engine/mem/carve.h"
#include "engine/save/sv.h"
#include "engine/save/svmod.h"
#include "engine/snd/snd_hw.h"

/* The listener's own entity (the player, engine/phys/player.h slot 1):
 * its sounds are not spatialised (sound.md 1, 2). */
#define SND_SELF	1
#define SENT_NONE	0xFFFF
#define SND_MAX_SOUNDS	8192	/* res_t index range (rtb KIND counts) */

typedef struct {
	void		*hw;		/* NULL = free slot */
	res_t		res;
	uint32_t	lru;		/* higher = used more recently */
	snd_wave_info_t	inf;		/* inf.resident: samples held by the backend */
} wave_t;

typedef struct {
	uint8_t		busy;
	int8_t		wave;		/* wave slot while a sample plays, -1 none */
	int8_t		loop;		/* owning loop slot, -1 = one-shot */
	int8_t		chan;
	uint16_t	ent;		/* ENT_NONE: fixed point (snd_ambient) */
	uint8_t		follow;		/* origin follows ent */
	uint8_t		pitch;		/* emit / sentence pitch */
	res_t		res;		/* sample playing */
	float		vol, attn;	/* emit / sentence volume, attenuation */
	float		org[3];		/* global */
	uint32_t	seq;		/* start order (stealing) */
	uint16_t	sent;		/* sentence id, SENT_NONE for a sample */
	uint16_t	word;		/* next word (WORD index) */
	uint8_t		left;		/* words still to start */
	uint8_t		wpitch;		/* word modifiers, % (100 = none) */
	uint8_t		wvol;
	uint8_t		paused;		/* snd_pause: 1 was playing (stopped), 2 was not */
	uint32_t	end_pos;	/* word end (sample), 0 = the wave's end */
	float		gap;		/* s between the last word's end and this one, -1 */
} voice_t;

typedef struct {
	uint8_t		used;
	int8_t		voice;		/* -1 = virtual */
	int8_t		chan;
	uint8_t		follow;
	uint16_t	ent;
	res_t		res;
	float		vol, attn;
	float		org[3];
	uint32_t	seq;
	uint8_t		pitch;
	uint8_t		pad[3];
} loop_t;

typedef struct {
	voice_t		v[SND_VOICES];
	loop_t		l[SND_LOOPS];
	wave_t		w[SND_WAVES];
	uint8_t		missing[SND_MAX_SOUNDS / 8];	/* SOUND ids that failed to open */
	float		lis[3], right[3];
	uint32_t	seq, clock, frame;
	double		last_dt;
	int		paused;		/* snd_pause */
	int		keep;		/* voice a wave-cap steal must not take */
	int		log;
	uint32_t	tables;		/* region bytes used by this file */
	snd_stats_t	st;
	uint32_t	frames;
	uint64_t	us_sum, pump_sum;
	uint8_t		mute;		/* snd_mute: output gain 0 */
} snd_state_t;

static snd_state_t	*S;

/* ---- helpers ---------------------------------------------------------------- */

static float clampf(float x, float lo, float hi)
{
	return x < lo ? lo : x > hi ? hi : x;
}

static int clamp_pitch(int p)
{
	return p < 1 ? 1 : p > 255 ? 255 : p;
}

static float master(void)
{
	if (S->mute)
		return 0.0f;
	return (float)settings.vol_master * (float)settings.vol_sfx * 1e-4f;
}

/* Global position of entity e's sound source: its origin; brush
 * entities (origin at the map origin) sound from the centre of their box
 * (sound.md 1). 0 when e is not live. */
static int ent_pos(int e, float out[3])
{
	const col_ent_t *cv;
	const float *g;
	int k;

	if (!ent_is_live(e))
		return 0;
	cv = ent_cv(e);
	g = eng_layer_origin(ent_idc(e)->layer);
	for (k = 0; k < 3; k++)
	{
		out[k] = cv->origin[k] + g[k];
		if (cv->model != COL_NONE)
			out[k] += 0.5f * (cv->mins[k] + cv->maxs[k]);
	}
	return 1;
}

static void snd_logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void snd_logf(const char *fmt, ...)
{
	va_list ap;

	if (!S->log)
		return;
	va_start(ap, fmt);
	log_vprintf(fmt, ap);
	va_end(ap);
}

static const char *rname(res_t r)
{
	const char *n = res_name(r);

	return n && n[0] ? n : "-";
}

void snd_gains(const float src[3], float vol, float attn, int self, float *left, float *right)
{
	float d[3], dist, g, s, l, r;
	int k;

	vol = clampf(vol, 0.0f, 1.0f);
	if (self || attn <= 0.0f)
	{
		*left = *right = vol;
		return;
	}
	for (k = 0; k < 3; k++)
		d[k] = src[k] - S->lis[k];
	dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
	g = 1.0f - dist * attn / SND_CLIP_DIST;
	if (g <= 0.0f)
	{
		*left = *right = 0.0f;
		return;
	}
	s = 0.0f;
	if (dist > 1.0f)
		s = (S->right[0] * d[0] + S->right[1] * d[1] + S->right[2] * d[2]) / dist;
	l = vol * g * (1.0f - s);
	r = vol * g * (1.0f + s);
	*left = l > 1.0f ? 1.0f : l;
	*right = r > 1.0f ? 1.0f : r;
}

float snd_pitch_rate(uint32_t freq, int pitch)
{
	float rate = (float)freq * (float)clamp_pitch(pitch) * 0.01f;
	float cap = (float)SND_HZ * (SND_PITCH_CAP * 0.01f);

	return rate > cap ? cap : rate;
}

void snd_log_set(int on)
{
	if (S)
		S->log = on;
}

/* ---- waves ------------------------------------------------------------------ */

static int wave_in_use(int w)
{
	int v;

	for (v = 0; v < SND_VOICES; v++)
		if (S->v[v].busy && S->v[v].wave == w)
			return 1;
	return 0;
}

int snd_waves_open(void)
{
	int w, n = 0;

	for (w = 0; w < SND_WAVES; w++)
		n += S->w[w].hw != NULL;
	return n;
}

int snd_wave_voice_check(void)
{
	int v;

	for (v = 0; v < SND_VOICES; v++)
		if (S->v[v].busy && S->v[v].wave >= 0
			&& (!S->w[S->v[v].wave].hw || S->w[S->v[v].wave].res != S->v[v].res))
			return v;
	return -1;
}

static void voice_free(int v, const char *why);

/* An open wave slot no voice plays, the least recently used; -1 none.
 * Streamed waves close first: a resident one costs its whole sample to
 * open again. */
static int wave_lru_unused(void)
{
	int w, best = -1;

	for (w = 0; w < SND_WAVES; w++)
		if (S->w[w].hw && !wave_in_use(w) && (best < 0
			|| S->w[w].inf.resident < S->w[best].inf.resident
			|| (S->w[w].inf.resident == S->w[best].inf.resident
			&& S->w[w].lru < S->w[best].lru)))
			best = w;
	return best;
}

/* The playing one-shot (or sentence) started least recently, except
 * S->keep; -1 none. */
static int oldest_oneshot(void)
{
	int v, best = -1;

	for (v = 0; v < SND_VOICES; v++)
		if (S->v[v].busy && S->v[v].loop < 0 && v != S->keep
			&& (best < 0 || S->v[v].seq < S->v[best].seq))
			best = v;
	return best;
}

/* Open sound r (or find it open): its wave slot, -1 when it cannot play.
 * resident: ask the backend to hold the samples (SND_RESIDENT). */
static int wave_get(res_t r, int resident)
{
	unsigned idx = RES_INDEX(r);
	int w, file, rc;
	uint32_t ofs, size, rom;

	if (RES_KIND(r) != RES_K_SOUND || idx >= SND_MAX_SOUNDS || (S->missing[idx >> 3] >> (idx & 7) & 1))
		return -1;
	for (w = 0; w < SND_WAVES; w++)
		if (S->w[w].hw && S->w[w].res == r)
		{
			S->w[w].lru = ++S->clock;
			return w;
		}
	for (w = 0; w < SND_WAVES && S->w[w].hw; w++)
		;
	if (w == SND_WAVES)
	{
		/* every wave open: close the LRU one no voice plays; when every
		 * open wave is under a voice, the oldest one-shots give way */
		while ((w = wave_lru_unused()) < 0)
		{
			int v = oldest_oneshot();

			if (v < 0)
			{
				snd_logf("snd %lu drop s%u %s wave-cap\n", (unsigned long)S->frame,
					(unsigned)r, rname(r));
				return -1;
			}
			S->st.stolen++;
			voice_free(v, "steal-wave");
		}
		snd_logf("snd %lu close s%u %s\n", (unsigned long)S->frame, (unsigned)S->w[w].res,
			rname(S->w[w].res));
		snd_hw_close(S->w[w].hw);
		S->w[w].hw = NULL;
	}
	if (!res_file(r, &file, &ofs, &size) || !(rom = res_file_rom(file, NULL)))
		rc = -1;
	else
		rc = snd_hw_open(rom + ofs, size, resident, &S->w[w].inf, &S->w[w].hw);
	if (rc)
	{
		S->w[w].hw = NULL;
		if (rc == -1)
			S->missing[idx >> 3] |= (uint8_t)(1u << (idx & 7));
		snd_logf("snd %lu drop s%u %s %s\n", (unsigned long)S->frame, (unsigned)r, rname(r),
			rc == -1 ? "missing" : "no-room");
		return -1;
	}
	S->w[w].res = r;
	S->w[w].lru = ++S->clock;
	rc = snd_waves_open();
	if ((uint32_t)rc > S->st.waves_peak)
		S->st.waves_peak = (uint32_t)rc;
	return w;
}

/* ---- voices ----------------------------------------------------------------- */

static void voice_apply(int v)
{
	voice_t *p = &S->v[v];
	float l, r, m = master();

	snd_gains(p->org, p->vol * p->wvol * 0.01f, p->attn, p->ent == SND_SELF, &l, &r);
	snd_hw_vol(v, l * m, r * m);
}

static void voice_rate(int v)
{
	voice_t *p = &S->v[v];

	snd_hw_rate(v, snd_pitch_rate(S->w[p->wave].inf.freq, p->pitch * p->wpitch / 100));
}

static uint32_t voices_busy(void)
{
	uint32_t n = 0;
	int v;

	for (v = 0; v < SND_VOICES; v++)
		n += S->v[v].busy;
	return n;
}

/* Start voice v's wave slot w from sample pos (fields set by the caller). */
static void voice_start(int v, int w, uint32_t pos, const char *what)
{
	voice_t *p = &S->v[v];
	float l, r;
	uint32_t n;

	p->busy = 1;
	p->wave = (int8_t)w;
	p->res = S->w[w].res;
	snd_hw_play(v, S->w[w].hw, pos);
	voice_rate(v);
	voice_apply(v);
	S->w[w].lru = ++S->clock;
	S->st.started++;
	n = voices_busy();
	if (n > S->st.voices_peak)
		S->st.voices_peak = n;
	if (S->log)
	{
		snd_gains(p->org, p->vol * p->wvol * 0.01f, p->attn, p->ent == SND_SELF, &l, &r);
		snd_logf("snd %lu %s v%d e%d c%d s%u %s l=%.3f r=%.3f rate=%d pos=%lu\n",
			(unsigned long)S->frame, what, v, p->ent == ENT_NONE ? -1 : (int)p->ent,
			(int)p->chan, (unsigned)p->res, rname(p->res), (double)l, (double)r,
			(int)(snd_pitch_rate(S->w[w].inf.freq, p->pitch * p->wpitch / 100) + 0.5f),
			(unsigned long)pos);
	}
}

/* Stop voice v; a loop that owned it goes virtual. */
static void voice_free(int v, const char *why)
{
	voice_t *p = &S->v[v];

	if (!p->busy)
		return;
	snd_hw_stop(v);
	if (p->loop >= 0 && S->l[p->loop].voice == v)
		S->l[p->loop].voice = -1;
	snd_logf("snd %lu %s v%d e%d c%d s%u %s\n", (unsigned long)S->frame, why, v,
		p->ent == ENT_NONE ? -1 : (int)p->ent, (int)p->chan, (unsigned)p->res, rname(p->res));
	memset(p, 0, sizeof(*p));
	p->wave = -1;
	p->loop = -1;
	p->sent = SENT_NONE;
}

/* A voice for a new sound: an idle one (a finished one-shot is reaped);
 * else (steal) the one-shot started least recently; a loop's voice only
 * when every voice holds a loop. -1 when steal is 0 and none is idle. */
static int voice_pick(int steal)
{
	int v, best = -1;

	for (v = 0; v < SND_VOICES; v++)
		if (!S->v[v].busy)
			return v;
	for (v = 0; v < SND_VOICES; v++)
		if (S->v[v].loop < 0 && S->v[v].sent == SENT_NONE && !S->v[v].paused &&
			!snd_hw_playing(v))
		{
			voice_free(v, "end");
			return v;
		}
	if (!steal)
		return -1;
	if ((best = oldest_oneshot()) < 0)
		for (v = 0; v < SND_VOICES; v++)
			if (v != S->keep && (best < 0 || S->v[v].seq < S->v[best].seq))
				best = v;
	if (best < 0)
		return -1;
	S->st.stolen++;
	voice_free(best, S->v[best].loop >= 0 ? "steal-loop" : "steal");
	return best;
}

static void voice_init(int v, int e, int chan, float vol, float attn, int pitch,
	const float org[3], int follow)
{
	voice_t *p = &S->v[v];

	memset(p, 0, sizeof(*p));
	p->wave = -1;
	p->loop = -1;
	p->sent = SENT_NONE;
	p->chan = (int8_t)chan;
	p->ent = (uint16_t)e;
	p->follow = (uint8_t)follow;
	p->pitch = (uint8_t)clamp_pitch(pitch);
	p->vol = clampf(vol, 0.0f, 1.0f);
	p->attn = attn > 0.0f ? attn : 0.0f;
	p->wvol = 100;
	p->wpitch = 100;
	p->gap = -1.0f;
	memcpy(p->org, org, sizeof(p->org));
	p->seq = ++S->seq;
}

/* ---- sentences -------------------------------------------------------------- */

/* Start the next playable word on voice v, or free it at the end. */
static void sentence_next(int v, double gap)
{
	voice_t *p = &S->v[v];

	S->keep = v;
	while (p->left)
	{
		const res_word_t *wd = res_word(p->word);
		int w;

		p->word++;
		p->left--;
		if (!wd || (w = wave_get(wd->res, 0)) < 0)
			continue;
		p->wvol = wd->volume;
		p->wpitch = wd->pitch;
		p->end_pos = wd->end > 0 && wd->end < 100
			? (uint32_t)((uint64_t)S->w[w].inf.len * wd->end / 100) : 0;
		voice_start(v, w, 0, "word");
		if (gap >= 0.0)
		{
			p->gap = (float)gap;
			snd_logf("snd %lu gap v%d sent%u word%u gap_us=%lu\n", (unsigned long)S->frame,
				v, (unsigned)p->sent, (unsigned)(p->word - 1),
				(unsigned long)(gap * 1e6 + 0.5));
		}
		S->keep = -1;
		return;
	}
	S->keep = -1;
	voice_free(v, "end");
}

/* ---- loops ------------------------------------------------------------------ */

static uint32_t loops_used(void)
{
	uint32_t n = 0;
	int i;

	for (i = 0; i < SND_LOOPS; i++)
		n += S->l[i].used;
	return n;
}

static void loop_follow(loop_t *lp)
{
	if (lp->ent == SND_SELF)
		memcpy(lp->org, S->lis, sizeof(lp->org));
	else if (lp->follow && !ent_pos(lp->ent, lp->org))
		lp->follow = 0;
}

/* Give an audible loop a voice (a new loop may steal; a returning one
 * takes an idle voice only), take it from an inaudible one, else set its
 * ear gains. */
static void loop_update(int i, int fresh)
{
	loop_t *lp = &S->l[i];
	float l, r;
	int v, w;

	loop_follow(lp);
	snd_gains(lp->org, lp->vol, lp->attn, lp->ent == SND_SELF, &l, &r);
	if (l + r < SND_AUDIBLE)
	{
		if (lp->voice >= 0)
			voice_free(lp->voice, "virtual");
		return;
	}
	if (lp->voice >= 0)
	{
		voice_t *p = &S->v[(int)lp->voice];

		memcpy(p->org, lp->org, sizeof(p->org));
		voice_apply(lp->voice);
		return;
	}
	if ((w = wave_get(lp->res, 0)) < 0)
		return;
	if ((v = voice_pick(fresh)) < 0)
		return;
	voice_init(v, lp->ent, lp->chan, lp->vol, lp->attn, lp->pitch, lp->org, lp->follow);
	S->v[v].seq = lp->seq;
	S->v[v].loop = (int8_t)i;
	lp->voice = (int8_t)v;
	voice_start(v, w, fresh ? 0 : S->w[w].inf.len - S->w[w].inf.loop_len,
		fresh ? "start" : "resume");
}

static void loop_start(int e, int chan, res_t r, float vol, float attn, int pitch,
	const float org[3], int follow)
{
	loop_t *lp;
	uint32_t n;
	int i;

	for (i = 0; i < SND_LOOPS && S->l[i].used; i++)
		;
	if (i == SND_LOOPS)
	{
		S->st.dropped++;
		snd_logf("snd %lu drop s%u %s loops-full\n", (unsigned long)S->frame, (unsigned)r,
			rname(r));
		return;
	}
	lp = &S->l[i];
	memset(lp, 0, sizeof(*lp));
	lp->used = 1;
	lp->voice = -1;
	lp->chan = (int8_t)chan;
	lp->ent = (uint16_t)e;
	lp->follow = (uint8_t)follow;
	lp->res = r;
	lp->vol = clampf(vol, 0.0f, 1.0f);
	lp->attn = attn > 0.0f ? attn : 0.0f;
	lp->pitch = (uint8_t)clamp_pitch(pitch);
	memcpy(lp->org, org, sizeof(lp->org));
	lp->seq = ++S->seq;
	n = loops_used();
	if (n > S->st.loops_peak)
		S->st.loops_peak = n;
	snd_logf("snd %lu loop L%d e%d c%d s%u %s\n", (unsigned long)S->frame, i,
		e == ENT_NONE ? -1 : e, chan, (unsigned)r, rname(r));
	loop_update(i, 1);
	if (lp->voice < 0)
		snd_logf("snd %lu virtual L%d\n", (unsigned long)S->frame, i);
}

/* ---- channel operations ------------------------------------------------------ */

/* (e, chan) matches; chan < 0 = every channel; for ent ENT_NONE (fixed
 * points) the sound r identifies the source. */
static int match(uint16_t ent, int8_t chan, res_t res, int e, int c, res_t r)
{
	if (ent != (uint16_t)e)
		return 0;
	if (e == ENT_NONE)
		return res == r;
	return c < 0 || chan == c;
}

static void stop_match(int e, int c, res_t r, const char *why)
{
	int i;

	for (i = 0; i < SND_VOICES; i++)
	{
		voice_t *p = &S->v[i];

		if (p->busy && p->loop < 0 && match(p->ent, p->chan, p->res, e, c, r))
			voice_free(i, why);
	}
	for (i = 0; i < SND_LOOPS; i++)
	{
		loop_t *lp = &S->l[i];

		if (lp->used && match(lp->ent, lp->chan, lp->res, e, c, r))
		{
			if (lp->voice >= 0)
				voice_free(lp->voice, why);
			lp->used = 0;
			snd_logf("snd %lu loop-end L%d %s\n", (unsigned long)S->frame, i, why);
		}
	}
}

static void change_match(int e, int c, res_t r, int flags, float vol, int pitch)
{
	int i;

	vol = clampf(vol, 0.0f, 1.0f);
	pitch = clamp_pitch(pitch);
	for (i = 0; i < SND_LOOPS; i++)
	{
		loop_t *lp = &S->l[i];

		if (!lp->used || !match(lp->ent, lp->chan, lp->res, e, c, r))
			continue;
		if (flags & SND_CHANGE_VOL)
			lp->vol = vol;
		if (flags & SND_CHANGE_PITCH)
			lp->pitch = (uint8_t)pitch;
	}
	for (i = 0; i < SND_VOICES; i++)
	{
		voice_t *p = &S->v[i];

		if (!p->busy)
			continue;
		if (p->loop >= 0)
		{
			loop_t *lp = &S->l[p->loop];

			if (!match(lp->ent, lp->chan, lp->res, e, c, r))
				continue;
		}
		else if (!match(p->ent, p->chan, p->res, e, c, r))
			continue;
		if (flags & SND_CHANGE_VOL)
			p->vol = vol;
		if (flags & SND_CHANGE_PITCH)
			p->pitch = (uint8_t)pitch;
		if (p->wave >= 0)
		{
			voice_rate(i);
			voice_apply(i);
		}
		snd_logf("snd %lu change v%d vol=%.3f pitch=%d\n", (unsigned long)S->frame, i,
			(double)p->vol, (int)p->pitch);
	}
}

/* A one-shot no ear hears: dropped (logged). */
static int silent(int e, res_t r, const float org[3], float vol, float attn)
{
	float l, r2;

	snd_gains(org, vol, attn, e == SND_SELF, &l, &r2);
	if (l + r2 > 0.0f)
		return 0;
	S->st.dropped++;
	snd_logf("snd %lu drop s%u %s silent\n", (unsigned long)S->frame, (unsigned)r, rname(r));
	return 1;
}

/* Shared start path of snd_emit / snd_ambient (flags without STOP /
 * CHANGE_*: SND_RESIDENT only). A resident sound is a one-shot: silent,
 * its wave is not opened (that reads its whole sample). */
static void start_sample(int e, int chan, res_t r, float vol, float attn, int pitch,
	const float org[3], int follow, int flags)
{
	int w, v, resident = (flags & SND_RESIDENT) != 0;

	if (chan != CHAN_AUTO && e != ENT_NONE)
		stop_match(e, chan, r, "replace");
	if (resident && silent(e, r, org, vol, attn))
		return;
	if ((w = wave_get(r, resident)) < 0)
	{
		S->st.dropped++;
		return;
	}
	if (S->w[w].inf.loop_len)
	{
		loop_start(e, chan, r, vol, attn, pitch, org, follow);
		return;
	}
	if (!resident && silent(e, r, org, vol, attn))
		return;
	S->keep = -1;
	if ((v = voice_pick(1)) < 0)
	{
		S->st.dropped++;
		return;
	}
	voice_init(v, e, chan, vol, attn, pitch, org, follow);
	voice_start(v, w, 0, "start");
}

/* ---- API --------------------------------------------------------------------- */

/* calls of snd_emit / snd_sentence since boot, counted before anything
 * the backend decides (host and cart count the same) */
static uint32_t emit_calls;

uint32_t snd_emit_count(void)
{
	return emit_calls;
}

void snd_emit(int e, int chan, res_t sound, float vol, float attn, int flags, int pitch)
{
	float org[3];

	emit_calls++;

	if (!S)
		return;
	S->st.emits++;
	if (flags & SND_STOP)
	{
		stop_match(e, chan, sound, "stop");
		return;
	}
	if (flags & (SND_CHANGE_VOL | SND_CHANGE_PITCH))
	{
		change_match(e, chan, sound, flags, vol, pitch);
		return;
	}
	if (e == SND_SELF)
		memcpy(org, S->lis, sizeof(org));
	else if (!ent_pos(e, org))
	{
		S->st.dropped++;
		return;
	}
	start_sample(e, chan, sound, vol, attn, pitch, org, 1, flags);
}

void snd_ambient(const float org[3], res_t sound, float vol, float attn, int flags, int pitch)
{
	const float *g;
	float p[3];
	int k;

	if (!S)
		return;
	S->st.emits++;
	if (flags & SND_STOP)
	{
		stop_match(ENT_NONE, CHAN_STATIC, sound, "stop");
		return;
	}
	if (flags & (SND_CHANGE_VOL | SND_CHANGE_PITCH))
	{
		change_match(ENT_NONE, CHAN_STATIC, sound, flags, vol, pitch);
		return;
	}
	g = eng_layer_origin(eng_frame_layer());
	for (k = 0; k < 3; k++)
		p[k] = org[k] + g[k];
	start_sample(ENT_NONE, CHAN_STATIC, sound, vol, attn, pitch, p, 0, flags);
}

void snd_sentence(int e, int chan, uint16_t sentence, float vol, float attn, int flags,
	int pitch)
{
	float org[3], l, r;
	int first, n, v;

	emit_calls++;
	if (!S)
		return;
	S->st.emits++;
	if (flags & SND_STOP)
	{
		stop_match(e, chan, 0, "stop");
		return;
	}
	if (flags & (SND_CHANGE_VOL | SND_CHANGE_PITCH))
	{
		change_match(e, chan, 0, flags, vol, pitch);
		return;
	}
	if (e == SND_SELF)
		memcpy(org, S->lis, sizeof(org));
	else if (!ent_pos(e, org))
	{
		S->st.dropped++;
		return;
	}
	if (chan != CHAN_AUTO)
		stop_match(e, chan, 0, "replace");
	n = res_sentence_words(sentence, &first);
	snd_gains(org, vol, attn, e == SND_SELF, &l, &r);
	if (n <= 0 || l + r <= 0.0f)
	{
		S->st.dropped++;
		snd_logf("snd %lu drop sent%u %s\n", (unsigned long)S->frame, (unsigned)sentence,
			n <= 0 ? "unknown" : "silent");
		return;
	}
	S->keep = -1;
	if ((v = voice_pick(1)) < 0)
	{
		S->st.dropped++;
		return;
	}
	voice_init(v, e, chan, vol, attn, pitch, org, 1);
	S->v[v].busy = 1;
	S->v[v].sent = sentence;
	S->v[v].word = (uint16_t)first;
	S->v[v].left = (uint8_t)(n > 255 ? 255 : n);
	snd_logf("snd %lu sentence v%d e%d c%d sent%u words=%d\n", (unsigned long)S->frame, v, e,
		chan, (unsigned)sentence, n);
	sentence_next(v, -1.0);
}

void snd_stop(int e, int chan)
{
	if (S)
		stop_match(e, chan, 0, "stop");
}

/* The session ended (New Game, end of section, Quit to title): every
 * voice stops and every wave closes, so the title holds no wave of the
 * last game (the sound region is back to what the waves cost before it;
 * eng.c title_settle checks it). */
void snd_stop_all(void)
{
	int i, n = 0;

	if (!S)
		return;
	S->paused = 0;
	for (i = 0; i < SND_LOOPS; i++)
		S->l[i].used = 0;
	for (i = 0; i < SND_VOICES; i++)
		if (S->v[i].busy)
			voice_free(i, "new game");
	for (i = 0; i < SND_WAVES; i++)
		if (S->w[i].hw)
		{
			snd_hw_close(S->w[i].hw);
			S->w[i].hw = NULL;
			n++;
		}
	snd_logf("snd %lu new game: %d waves closed\n", (unsigned long)S->frame, n);
}

void snd_pause(int on)
{
	int v;

	if (!S || !on == !S->paused)
		return;
	S->paused = on != 0;
	for (v = 0; v < SND_VOICES; v++)
	{
		voice_t *p = &S->v[v];

		if (!p->busy)
			continue;
		if (on)
		{
			p->paused = 2;
			if (p->wave >= 0 && snd_hw_playing(v))
			{
				p->paused = 1;
				snd_hw_stop(v);
			}
			continue;
		}
		/* a VADPCM wave seeks only to its start or its loop start: a loop
		 * resumes at its loop start (virtual until loop_update gives it a
		 * voice), a sentence with its next word, a stopped one-shot ends */
		if (p->paused == 1 && p->loop >= 0)
			voice_free(v, "pause-loop");
		else if (p->paused == 1 && p->sent != SENT_NONE)
		{
			p->paused = 0;
			p->wave = -1;
			sentence_next(v, 0.0);
		}
		else if (p->paused == 1)
			voice_free(v, "pause-cut");
		else
			p->paused = 0;
	}
	snd_logf("snd %lu %s\n", (unsigned long)S->frame, on ? "pause" : "resume");
}

void snd_ui(res_t sound, float vol)
{
	if (!S)
		return;
	S->st.emits++;
	start_sample(ENT_NONE, CHAN_AUTO, sound, vol, ATTN_NONE, PITCH_NORM, S->lis, 0, 0);
}

void snd_mute(int on)
{
	int v;

	if (!S || !on == !S->mute)
		return;
	S->mute = (uint8_t)(on != 0);
	for (v = 0; v < SND_VOICES; v++)
		if (S->v[v].busy)
			voice_apply(v);
	snd_logf("snd %lu %s\n", (unsigned long)S->frame, on ? "mute" : "unmute");
}

void snd_voice_info(int v, snd_voice_info_t *o)
{
	const voice_t *p = &S->v[v];

	memset(o, 0, sizeof(*o));
	o->busy = p->busy;
	o->loop = p->loop;
	o->sent = p->sent;
	o->gap = p->gap;
	if (!p->busy)
		return;
	o->ent = p->ent == ENT_NONE ? -1 : p->ent;
	o->chan = p->chan;
	o->res = p->res;
	o->seq = p->seq;
	if (p->wave >= 0)
	{
		o->pos = snd_hw_pos(v);
		o->rate = snd_pitch_rate(S->w[p->wave].inf.freq, p->pitch * p->wpitch / 100);
		snd_gains(p->org, p->vol * p->wvol * 0.01f, p->attn, p->ent == SND_SELF, &o->left,
			&o->right);
	}
}

void snd_loop_info(int i, snd_loop_info_t *o)
{
	const loop_t *lp = &S->l[i];

	memset(o, 0, sizeof(*o));
	o->used = lp->used;
	o->voice = lp->voice;
	o->ent = lp->ent == ENT_NONE ? -1 : lp->ent;
	o->chan = lp->chan;
	o->res = lp->res;
	o->vol = lp->vol;
	o->pitch = lp->pitch;
}

void snd_entity_freed(int e)
{
	int i;

	if (!S)
		return;
	for (i = 0; i < SND_VOICES; i++)
		if (S->v[i].busy && S->v[i].ent == (uint16_t)e)
			S->v[i].follow = 0;
	for (i = 0; i < SND_LOOPS; i++)
		if (S->l[i].used && S->l[i].ent == (uint16_t)e)
			S->l[i].follow = 0;
}

/* DBG_SOUND (test builds): play a sound (AUTO) or a sentence (VOICE) from
 * entity e, 0 = the listener's own */
static int cmd_sound(const dbg_cmd_t *c)
{
	res_t r = (res_t)c->arg[0].u;
	int e = c->arg[1].u ? (int)c->arg[1].u : SND_SELF;
	int pitch = c->arg[2].u ? (int)c->arg[2].u : PITCH_NORM;

	if (RES_KIND(r) == RES_K_SENTENCE)
		snd_sentence(e, CHAN_VOICE, (uint16_t)RES_INDEX(r), 1.0f, ATTN_NORM, 0, pitch);
	else if (RES_KIND(r) == RES_K_SOUND)
		snd_emit(e, CHAN_AUTO, r, 1.0f, ATTN_NORM, 0, pitch);
	else
		return 1;
	return 0;
}

void snd_init(void)
{
	uint8_t *m = mem_region(MEM_SOUND);
	uint32_t n = mem_region_bytes(MEM_SOUND), t = ((uint32_t)sizeof(snd_state_t) + 15u) & ~15u;
	int v;

	if (!m || n < t)
		sys_error("snd: SOUND region %lu B under the %lu B tables", (unsigned long)n,
			(unsigned long)t);
	S = (snd_state_t *)m;
	memset(S, 0, sizeof(*S));
	S->tables = t;
	S->keep = -1;
	S->right[1] = -1.0f;
#ifdef HL64_TEST
	S->log = 1;
#endif
	for (v = 0; v < SND_VOICES; v++)
	{
		S->v[v].wave = -1;
		S->v[v].loop = -1;
		S->v[v].sent = SENT_NONE;
	}
	snd_hw_init(m + t, n - t);
	dbg_handler_set(DBG_SOUND, cmd_sound);
	log_printf("snd: %d voices @ %d Hz, %d loops, %d waves; tables %lu B of %lu B\n",
		SND_VOICES, SND_HZ, SND_LOOPS, SND_WAVES, (unsigned long)t, (unsigned long)n);
}

void snd_frame(const float listener[3], const float right[3], double dt)
{
	uint64_t t0;
	uint32_t us;
	int i;

	if (!S)
		return;
	t0 = sys_time_us();
	memcpy(S->lis, listener, sizeof(S->lis));
	memcpy(S->right, right, sizeof(S->right));
	for (i = 0; i < SND_VOICES; i++)
	{
		voice_t *p = &S->v[i];

		if (!p->busy || p->loop >= 0 || p->paused)
			continue;
		if (p->ent == SND_SELF)
			memcpy(p->org, S->lis, sizeof(p->org));
		else if (p->follow && !ent_pos(p->ent, p->org))
			p->follow = 0;
		if (p->sent != SENT_NONE)
		{
			if (!snd_hw_playing(i) || (p->end_pos && snd_hw_pos(i) >= p->end_pos))
			{
				double off = snd_hw_end_offset(i);
				double gap = off >= 0.0 ? S->last_dt - off : S->last_dt;

				/* word over: the next starts this frame (<= one frame) */
				snd_hw_stop(i);
				p->wave = -1;
				sentence_next(i, gap);
				if (!p->busy)
					continue;
			}
		}
		else if (!snd_hw_playing(i))
		{
			voice_free(i, "end");
			continue;
		}
		voice_apply(i);
	}
	for (i = 0; i < SND_LOOPS; i++)
		if (S->l[i].used && !S->paused)
		{
			int had = S->l[i].voice >= 0;

			loop_update(i, 0);
			if (had && S->l[i].voice < 0)
				snd_logf("snd %lu virtual L%d\n", (unsigned long)S->frame, i);
		}
	{
		uint64_t p0 = sys_time_us();

		snd_hw_pump(dt);
		S->pump_sum += sys_time_us() - p0;
	}
	S->last_dt = dt;
	S->frame++;
	S->st.voices = voices_busy();
	S->st.loops = loops_used();
	S->st.waves_open = (uint32_t)snd_waves_open();
	us = (uint32_t)(sys_time_us() - t0);
	S->us_sum += us;
	S->frames++;
	if (us > S->st.us_max)
		S->st.us_max = us;
}

void snd_stats(snd_stats_t *s, int reset)
{
	uint32_t now, peak;

	if (!S)
	{
		memset(s, 0, sizeof(*s));
		return;
	}
	*s = S->st;
	s->voices = voices_busy();
	s->loops = loops_used();
	s->waves_open = (uint32_t)snd_waves_open();
	s->frames = S->frames;
	s->us = S->frames ? (uint32_t)(S->us_sum / S->frames) : 0;
	s->pump_us = S->frames ? (uint32_t)(S->pump_sum / S->frames) : 0;
	snd_hw_mem(&now, &peak);
	s->mem = S->tables + now;
	s->mem_peak = S->tables + peak;
	s->mem_region = mem_region_bytes(MEM_SOUND);
	if (reset)
	{
		S->st.emits = S->st.started = S->st.stolen = S->st.dropped = 0;
		S->st.voices_peak = s->voices;
		S->st.loops_peak = s->loops;
		S->st.waves_peak = s->waves_open;
		S->st.us_max = 0;
		S->us_sum = 0;
		S->pump_sum = 0;
		S->frames = 0;
	}
}

/* ---- saved games (architecture.md 14) ------------------------------------------- */

/* Looping sounds (ambients, machinery, the game's loops): the game does
 * not start them again, so a load does, from the start of their loop.
 * One-shots and sentences end with the frame the save was taken in. */
void snd_save(void)
{
	int i;

	sv_tag(SV_TAG('S', 'N', 'D', '1'));
	for (i = 0; i < SND_LOOPS; i++)
	{
		const loop_t *lp = &S->l[i];

		sv_u8(lp->used);
		if (!lp->used)
			continue;
		sv_u8((uint8_t)lp->chan);
		sv_u8(lp->follow);
		sv_u8(lp->pitch);
		sv_u16(lp->ent);
		sv_u16(lp->res);
		sv_f32(lp->vol);
		sv_f32(lp->attn);
		sv_put(lp->org, sizeof(lp->org));
	}
}

int snd_load(void)
{
	int i;

	if (sv_expect(SV_TAG('S', 'N', 'D', '1')))
		return -1;
	snd_stop_all();
	for (i = 0; i < SND_LOOPS && !sv_failed(); i++)
	{
		int chan, follow, pitch, e;
		res_t r;
		float vol, attn, org[3];

		if (!sv_get_u8())
			continue;
		chan = (int8_t)sv_get_u8();
		follow = (int)sv_get_u8();
		pitch = (int)sv_get_u8();
		e = (int)sv_get_u16();
		r = (res_t)sv_get_u16();
		vol = sv_get_f32();
		attn = sv_get_f32();
		sv_get(org, sizeof(org));
		if (!sv_failed())
			loop_start(e == ENT_NONE ? ENT_NONE : e, chan, r, vol, attn, pitch, org, follow);
	}
	return sv_failed() ? -1 : 0;
}
