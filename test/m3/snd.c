/* SPDX-License-Identifier: MIT */
/*
 * snd.c -- host driver of the sound tests (docs/specs/sound.md 5; run by
 * test/m3/snd.sh): the real engine (carve, io, res over a synthetic
 * campaign.rtb, entities, engine/snd/snd.c over the null audio backend)
 * driven frame by frame at 1/30 s, with the event log ("snd ...") on.
 *
 *   hl64-snd --root CONTENT S1|S2|S3|S4|S5|S6|S7|S8|all
 *
 * The content is snd.sh's synthetic set: raw 16-bit wav64 files under
 * sound/t/ and two sentences; the test res.def lines (snd.sh) name them in
 * this order: A (11025 Hz, 2 s), B (22050 Hz, 2 s), LOOP (22050 samples
 * at 11025 Hz, loop 11025), SHORT (0.1 s), D0..D15 (2 s each), sentences
 * TST0 "t/w1 w2(p120 v80) w3(e50)" and TST1 (six words).
 * Exit 0 when every check of the chosen tests passes.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hl64/ent.h"
#include "hl64/rec.h"
#include "hl64/settings.h"
#include "hl64/snd.h"
#include "engine/core/log.h"
#include "engine/core/sys.h"
#include "engine/io/io.h"
#include "engine/mem/carve.h"
#include "engine/snd/snd_hw.h"
#include "plat/host/host.h"

#define DT		(1.0 / 30.0)

enum { R_A, R_B, R_LOOP, R_SHORT, R_D0, R_TST0 = R_D0 + 16, R_TST1 };

static int fails, checks;
static float lis[3], right_v[3] = { 0, -1, 0 };

#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; \
	log_printf("CHECK FAIL %s:%d: ", __FILE__, __LINE__); log_printf(__VA_ARGS__); \
	log_printf("\n"); } } while (0)

static res_t R(int line)
{
	res_t r = res_def(line);

	if (r == RES_NONE)
		sys_error("snd test: res.def line %d has no id", line);
	return r;
}

static void frames(int n)
{
	while (n-- > 0)
		snd_frame(lis, right_v, DT);
	log_flush();
}

static int ent_at(float x, float y, float z)
{
	int e = ent_create(ENT_ALLOC_GAME);

	if (e < 0)
		sys_error("snd test: no entity");
	ent_cv(e)->origin[0] = x;
	ent_cv(e)->origin[1] = y;
	ent_cv(e)->origin[2] = z;
	return e;
}

static void reset(void)
{
	int v, i;
	snd_loop_info_t l;

	for (i = 0; i < SND_LOOPS; i++)
	{
		snd_loop_info(i, &l);
		if (l.used)
			snd_emit(l.ent, l.chan, l.res, 0, 0, SND_STOP, 0);
	}
	for (v = 0; v < SND_VOICES; v++)
	{
		snd_voice_info_t o;

		snd_voice_info(v, &o);
		if (o.busy)
			snd_stop(o.ent, -1);
	}
	for (i = 2; i < HL64_MAX_ENTITIES; i++)
		if (ent_is_live(i))
			ent_destroy(i);
	memset(lis, 0, sizeof(lis));
	right_v[0] = 0;
	right_v[1] = -1;
	right_v[2] = 0;
	frames(1);
}

/* voices playing (e, chan); the last one's index in *v */
static int on(int e, int chan, int *v)
{
	int i, n = 0;
	snd_voice_info_t o;

	for (i = 0; i < SND_VOICES; i++)
	{
		snd_voice_info(i, &o);
		if (o.busy && o.ent == e && o.chan == chan)
		{
			n++;
			if (v)
				*v = i;
		}
	}
	return n;
}

static int busy(void)
{
	int i, n = 0;
	snd_voice_info_t o;

	for (i = 0; i < SND_VOICES; i++)
	{
		snd_voice_info(i, &o);
		n += o.busy;
	}
	return n;
}

static int voice_with_seq(uint32_t seq)
{
	int i;
	snd_voice_info_t o;

	for (i = 0; i < SND_VOICES; i++)
	{
		snd_voice_info(i, &o);
		if (o.busy && o.seq == seq)
			return i;
	}
	return -1;
}

static int near(float a, float b)
{
	return fabsf(a - b) <= 1e-4f * (fabsf(b) > 1.0f ? fabsf(b) : 1.0f);
}

/* ---- S1: replacement, AUTO stacking, STOP of loops, CHANGE_PITCH ---------- */

static void s1(void)
{
	snd_voice_info_t o, o2;
	snd_loop_info_t l;
	snd_stats_t st;
	int e, v = -1, v2 = -1, i;
	uint32_t started;

	log_printf("S1 channels\n");
	reset();
	e = ent_at(100, 0, 0);
	snd_emit(e, CHAN_VOICE, R(R_A), 1, ATTN_NORM, 0, 100);
	frames(1);
	snd_emit(e, CHAN_VOICE, R(R_B), 1, ATTN_NORM, 0, 100);
	CHECK(on(e, CHAN_VOICE, &v) == 1, "one sound per entity channel");
	snd_voice_info(v, &o);
	CHECK(o.res == R(R_B), "VOICE holds the new sound");
	snd_emit(e, CHAN_WEAPON, R(R_A), 1, ATTN_NORM, 0, 100);
	CHECK(on(e, CHAN_VOICE, NULL) == 1 && on(e, CHAN_WEAPON, NULL) == 1,
		"other channels are independent");
	for (i = 0; i < 3; i++)
		snd_emit(e, CHAN_AUTO, R(R_A), 1, ATTN_NORM, 0, 100);
	CHECK(on(e, CHAN_AUTO, NULL) == 3, "AUTO stacks (%d)", on(e, CHAN_AUTO, NULL));

	/* STOP of a loop: slot and voice */
	snd_emit(e, CHAN_STATIC, R(R_LOOP), 1, ATTN_NORM, 0, 100);
	for (i = 0; i < SND_LOOPS; i++)
	{
		snd_loop_info(i, &l);
		if (l.used && l.ent == e)
			break;
	}
	CHECK(i < SND_LOOPS && l.voice >= 0, "loop on a voice");
	frames(40);	/* past the loop's 1 s intro: it keeps looping */
	CHECK(on(e, CHAN_STATIC, NULL) == 1, "loop still playing after its length");
	snd_emit(e, CHAN_STATIC, R(R_LOOP), 1, ATTN_NORM, SND_STOP, 100);
	snd_loop_info(i, &l);
	CHECK(!l.used && on(e, CHAN_STATIC, NULL) == 0, "STOP frees the loop slot and its voice");
	CHECK(on(e, CHAN_VOICE, NULL) == 1, "STOP leaves other channels");

	/* CHANGE_PITCH without restart */
	reset();
	e = ent_at(100, 0, 0);
	snd_emit(e, CHAN_ITEM, R(R_A), 1, ATTN_NORM, 0, 100);
	frames(10);
	on(e, CHAN_ITEM, &v);
	snd_voice_info(v, &o);
	snd_stats(&st, 0);
	started = st.started;
	snd_emit(e, CHAN_ITEM, R(R_A), 0.5f, ATTN_NORM, SND_CHANGE_PITCH, 150);
	on(e, CHAN_ITEM, &v2);
	snd_voice_info(v2, &o2);
	snd_stats(&st, 0);
	CHECK(v2 == v && o2.seq == o.seq && o2.pos >= o.pos && st.started == started,
		"CHANGE_PITCH keeps the voice playing (pos %lu -> %lu)", (unsigned long)o.pos,
		(unsigned long)o2.pos);
	CHECK(near(o2.rate, 11025 * 1.5f), "CHANGE_PITCH rate %g", (double)o2.rate);
	CHECK(near(o2.left, o.left), "CHANGE_PITCH leaves the volume");
	snd_emit(e, CHAN_ITEM, R(R_A), 0.5f, ATTN_NORM, SND_CHANGE_VOL, 100);
	snd_voice_info(v2, &o2);
	CHECK(o2.seq == o.seq && near(o2.left, o.left * 0.5f) && near(o2.rate, 11025 * 1.5f),
		"CHANGE_VOL halves the gains, keeps the pitch");
	frames(10);
	snd_voice_info(v2, &o2);
	CHECK(o2.pos > o.pos, "still advancing after the changes");

	/* absent sound: a key naming a file the player's data lacks gets no id
	 * from the cook (RES_NONE); every API path plays nothing and counts it */
	reset();
	e = ent_at(100, 0, 0);
	snd_stats(&st, 0);
	started = st.dropped;
	snd_emit(e, CHAN_BODY, RES_NONE, 1, ATTN_NORM, 0, 100);
	snd_emit(e, CHAN_STATIC, RES_NONE, 1, ATTN_NORM, 0, 100);
	snd_ambient(lis, RES_NONE, 1, ATTN_NORM, 0, 100);
	snd_emit(e, CHAN_BODY, RES_NONE, 1, ATTN_NORM, SND_CHANGE_VOL, 50);
	snd_emit(e, CHAN_STATIC, RES_NONE, 1, ATTN_NORM, SND_STOP, 100);
	frames(2);
	snd_stats(&st, 0);
	for (i = 0; i < SND_LOOPS; i++)
	{
		snd_loop_info(i, &l);
		if (l.used)
			break;
	}
	CHECK(busy() == 0 && i == SND_LOOPS, "absent sound: no voice, no loop");
	CHECK(st.dropped == started + 3, "absent sound: 3 starts dropped (%lu)",
		(unsigned long)(st.dropped - started));
}

/* ---- S2: stealing order with 17 one-shots and with 16 loops -------------- */

static void s2(void)
{
	snd_voice_info_t o;
	snd_loop_info_t l, l0;
	uint32_t first = 0, seq17;
	int e[17], i, v, li0 = -1;

	log_printf("S2 stealing\n");
	reset();
	for (i = 0; i < 17; i++)
	{
		e[i] = ent_at(10.0f * (float)i, 0, 0);
		snd_emit(e[i], CHAN_VOICE, R(R_D0 + i % 4), 1, ATTN_NORM, 0, 100);
		if (i == 0)
		{
			on(e[0], CHAN_VOICE, &v);
			snd_voice_info(v, &o);
			first = o.seq;
		}
		if (i == 1)
			frames(1);
	}
	CHECK(busy() == SND_VOICES, "16 voices busy");
	CHECK(on(e[0], CHAN_VOICE, NULL) == 0, "the oldest one-shot was stolen");
	for (i = 1; i < 17; i++)
		CHECK(on(e[i], CHAN_VOICE, NULL) == 1, "one-shot %d plays", i);
	CHECK(voice_with_seq(first) < 0, "first seq gone");
	on(e[16], CHAN_VOICE, &v);
	snd_voice_info(v, &o);
	seq17 = o.seq;
	snd_emit(e[0], CHAN_VOICE, R(R_D0), 1, ATTN_NORM, 0, 100);
	CHECK(on(e[1], CHAN_VOICE, NULL) == 0 && voice_with_seq(seq17) >= 0,
		"next steal takes the next oldest");

	/* one-shots are stolen before loops */
	reset();
	for (i = 0; i < 15; i++)
	{
		e[i] = ent_at(10.0f * (float)i, 0, 0);
		snd_emit(e[i], CHAN_STATIC, R(R_LOOP), 1, ATTN_NORM, 0, 100);
	}
	e[15] = ent_at(0, 50, 0);
	snd_emit(e[15], CHAN_VOICE, R(R_A), 1, ATTN_NORM, 0, 100);
	e[16] = ent_at(0, 60, 0);
	snd_emit(e[16], CHAN_VOICE, R(R_A), 1, ATTN_NORM, 0, 100);
	CHECK(on(e[15], CHAN_VOICE, NULL) == 0 && on(e[16], CHAN_VOICE, NULL) == 1,
		"a one-shot is stolen before any loop");
	for (i = 0; i < 15; i++)
		CHECK(on(e[i], CHAN_STATIC, NULL) == 1, "loop %d keeps its voice", i);

	/* 16 loops: a new sound takes the oldest loop's voice, that loop goes
	 * virtual and returns to the first idle voice from its loop start */
	reset();
	for (i = 0; i < 16; i++)
	{
		e[i] = ent_at(10.0f * (float)i, 0, 0);
		snd_emit(e[i], CHAN_STATIC, R(R_LOOP), 1, ATTN_NORM, 0, 100);
	}
	CHECK(busy() == 16, "16 loops on 16 voices");
	for (i = 0; i < SND_LOOPS; i++)
	{
		snd_loop_info(i, &l);
		if (l.used && l.ent == e[0])
			li0 = i;
	}
	e[16] = ent_at(0, 50, 0);
	snd_emit(e[16], CHAN_VOICE, R(R_SHORT), 1, ATTN_NORM, 0, 100);
	snd_loop_info(li0, &l0);
	CHECK(on(e[16], CHAN_VOICE, NULL) == 1 && l0.used && l0.voice < 0,
		"a one-shot takes the oldest loop's voice; that loop is virtual");
	for (i = 1; i < 16; i++)
		CHECK(on(e[i], CHAN_STATIC, NULL) == 1, "loop %d keeps its voice", i);
	frames(1);
	snd_loop_info(li0, &l0);
	CHECK(l0.voice < 0, "the virtual loop does not steal back");
	/* SHORT (0.1 s) ends; loop 0 resumes on the first frame with an idle
	 * voice, at its loop start (11025), then advances one frame (367.5) */
	for (i = 0; i < 10; i++)
	{
		frames(1);
		snd_loop_info(li0, &l0);
		if (l0.voice >= 0)
			break;
	}
	CHECK(l0.voice >= 0, "the loop returns when a voice is idle");
	if (l0.voice >= 0)
	{
		snd_voice_info(l0.voice, &o);
		CHECK(o.pos >= 11025 && o.pos <= 11025 + 368, "resumes at the loop start (%lu)",
			(unsigned long)o.pos);
	}
}

/* ---- S3: ear gains on the circle --------------------------------------- */

static void expect(float d, float ang, float attn, float vol, float *l, float *r)
{
	float g = 1.0f - d * attn / 1000.0f, s;

	if (attn <= 0.0f)
	{
		*l = *r = vol;
		return;
	}
	if (g <= 0.0f)
	{
		*l = *r = 0.0f;
		return;
	}
	/* right = (0,-1,0); a source at angle ang (deg, from +x toward +y) */
	s = d > 1.0f ? -sinf(ang * 3.14159265f / 180.0f) : 0.0f;
	*l = fminf(1.0f, vol * g * (1.0f - s));
	*r = fminf(1.0f, vol * g * (1.0f + s));
}

static void s3(void)
{
	static const float dists[] = { 0, 500, 1000, 1250 };
	static const float attns[] = { ATTN_NONE, ATTN_NORM, ATTN_IDLE, ATTN_STATIC };
	snd_voice_info_t o;
	float l, r, el, er, vol;
	int di, ai, k, e, v, n = 0;

	log_printf("S3 gains\n");
	reset();
	for (di = 0; di < 4; di++)
		for (ai = 0; ai < 4; ai++)
			for (k = 0; k < 8; k++)
			{
				float a = 45.0f * (float)k, d = dists[di];
				float src[3] = { d * cosf(a * 3.14159265f / 180.0f),
					d * sinf(a * 3.14159265f / 180.0f), 0 };

				for (vol = 0.5f; vol <= 1.0f; vol += 0.5f)
				{
					snd_gains(src, vol, attns[ai], 0, &l, &r);
					expect(d, a, attns[ai], vol, &el, &er);
					CHECK(fabsf(l - el) < 1e-4f && fabsf(r - er) < 1e-4f,
						"d %g ang %g attn %g vol %g: %g/%g want %g/%g", (double)d,
						(double)a, (double)attns[ai], (double)vol, (double)l, (double)r,
						(double)el, (double)er);
					n++;
				}
				/* the same through an emit: a voice, or a drop when silent */
				e = ent_at(src[0], src[1], src[2]);
				snd_emit(e, CHAN_BODY, R(R_A), 1, attns[ai], 0, 100);
				/* the distance the engine sees (float position) */
				expect(sqrtf(src[0] * src[0] + src[1] * src[1]), a, attns[ai], 1, &el,
					&er);
				if (el + er <= 0.0f)
					CHECK(on(e, CHAN_BODY, NULL) == 0, "silent one-shot dropped (d %g)",
						(double)d);
				else if (on(e, CHAN_BODY, &v) == 1)
				{
					snd_voice_info(v, &o);
					CHECK(fabsf(o.left - el) < 1e-4f && fabsf(o.right - er) < 1e-4f,
						"emit gains d %g ang %g", (double)d, (double)a);
				}
				else
					CHECK(0, "audible one-shot did not start (d %g)", (double)d);
				snd_stop(e, -1);
				ent_destroy(e);
			}
	/* named properties */
	{
		float c[3] = { 500, 0, 0 }, rt[3] = { 0, -500, 0 }, fr[3] = { 1249, 0, 0 };

		snd_gains(c, 1, ATTN_NORM, 0, &l, &r);
		CHECK(near(l, 0.6f) && near(r, 0.6f), "centred: both ears at g (no halving)");
		snd_gains(rt, 1, ATTN_NORM, 0, &l, &r);
		CHECK(near(l, 0) && near(r, 1), "right: full right (2 x 0.6 clamped), silent left");
		snd_gains(fr, 1, ATTN_NORM, 0, &l, &r);
		CHECK(l > 0 && r > 0, "NORM still audible at 1249 u");
		fr[0] = 1250;
		snd_gains(fr, 1, ATTN_NORM, 0, &l, &r);
		CHECK(l == 0 && r == 0, "NORM silent at 1250 u");
		fr[0] = 500;
		snd_gains(fr, 1, ATTN_IDLE, 0, &l, &r);
		CHECK(l == 0 && r == 0, "IDLE silent at 500 u");
		fr[0] = 800;
		snd_gains(fr, 1, ATTN_STATIC, 0, &l, &r);
		CHECK(l == 0 && r == 0, "STATIC silent at 800 u");
		snd_gains(rt, 0.7f, ATTN_NORM, 1, &l, &r);
		CHECK(near(l, 0.7f) && near(r, 0.7f), "own sounds: both ears = volume");
	}
	settings.vol_master = 50;
	frames(1);
	settings.vol_master = 100;
	log_printf("S3 %d gain cases\n", n);
}

/* ---- S4 (host part): a moving source pans --------------------------------- */

static void s4(void)
{
	snd_voice_info_t o;
	float prev = -2.0f;
	int e, v, f, mono = 1, steps = 0;

	log_printf("S4 moving source\n");
	reset();
	e = ent_at(300, 400, 0);	/* left of the listener (right = -y) */
	snd_sentence(e, CHAN_VOICE, (uint16_t)RES_INDEX(R(R_TST1)), 1, ATTN_NORM, 0, 100);
	for (f = 0; f < 40 && on(e, CHAN_VOICE, &v); f++)
	{
		float pan;

		snd_voice_info(v, &o);
		pan = (o.right - o.left) / (o.right + o.left);
		if (pan < prev - 1e-5f)
			mono = 0;
		prev = pan;
		steps++;
		ent_cv(e)->origin[1] -= 20.0f;	/* walks across the view, left to right */
		frames(1);
	}
	CHECK(steps >= 30, "voice followed for %d frames", steps);
	CHECK(mono && prev > 0.5f, "pan rises left -> right as the source crosses (last %g)",
		(double)prev);
}

/* ---- S5: loops virtualise and resume from the loop start ----------------- */

static void s5(void)
{
	snd_loop_info_t l;
	snd_voice_info_t o;
	int e, i, li = -1;

	log_printf("S5 virtual loops\n");
	reset();
	e = ent_at(200, 0, 0);
	snd_emit(e, CHAN_STATIC, R(R_LOOP), 1, ATTN_NORM, 0, 100);
	for (i = 0; i < SND_LOOPS; i++)
	{
		snd_loop_info(i, &l);
		if (l.used)
			li = i;
	}
	CHECK(li >= 0 && l.voice >= 0, "audible loop holds a voice");
	frames(5);
	lis[0] = -2000;		/* listener walks away: beyond 1250 u */
	frames(1);
	snd_loop_info(li, &l);
	CHECK(l.used && l.voice < 0 && busy() == 0, "inaudible loop is virtual (no voice)");
	/* audible threshold: sum >= 0.002 */
	lis[0] = 200.0f - 1249.0f;	/* g = 1 - 1249 * 0.8 / 1000 = 0.0008 per ear: sum 0.0016 */
	frames(1);
	snd_loop_info(li, &l);
	CHECK(l.voice < 0, "sum 0.0016 < 0.002 stays virtual");
	lis[0] = 200.0f - 1247.0f;	/* g = 0.0024: sum 0.0048 */
	frames(1);
	snd_loop_info(li, &l);
	CHECK(l.voice >= 0, "sum 0.0048 >= 0.002 takes a voice");
	if (l.voice >= 0)
	{
		snd_voice_info(l.voice, &o);
		CHECK(o.pos >= 11025 && o.pos <= 11025 + 368 + 1,
			"resumed from the loop start (pos %lu)", (unsigned long)o.pos);
	}
	/* a loop that starts inaudible starts virtual */
	reset();
	e = ent_at(5000, 0, 0);
	snd_emit(e, CHAN_STATIC, R(R_LOOP), 1, ATTN_NORM, 0, 100);
	CHECK(busy() == 0, "inaudible new loop starts virtual");
	snd_emit(e, CHAN_STATIC, R(R_LOOP), 1, ATTN_NORM, SND_STOP, 100);
	snd_loop_info(0, &l);
	CHECK(!l.used, "STOP frees a virtual loop");
	/* full table drops */
	reset();
	for (i = 0; i < SND_LOOPS + 1; i++)
		snd_emit(ent_at(5000, (float)i, 0), CHAN_STATIC, R(R_LOOP), 1, ATTN_NORM, 0, 100);
	{
		snd_stats_t st;

		snd_stats(&st, 0);
		CHECK(st.loops == SND_LOOPS && st.dropped >= 1, "33rd loop dropped (%lu loops)",
			(unsigned long)st.loops);
	}
}

/* ---- S6: pitch mapping and cap --------------------------------------------- */

static void s6(void)
{
	snd_voice_info_t o;
	int p, e, v;

	log_printf("S6 pitch\n");
	reset();
	for (p = 1; p <= 255; p++)
	{
		float a = snd_pitch_rate(11025, p), b = snd_pitch_rate(22050, p);
		float wb = 22050.0f * (float)p / 100.0f;

		CHECK(near(a, 11025.0f * (float)p / 100.0f), "11025 Hz pitch %d: %g", p, (double)a);
		CHECK(near(b, wb > 28665.0f ? 28665.0f : wb), "22050 Hz pitch %d: %g", p, (double)b);
	}
	CHECK(near(snd_pitch_rate(22050, 0), 220.5f) && near(snd_pitch_rate(11025, 400), 28114.75f),
		"pitch clamped to 1..255");
	CHECK(near(snd_pitch_rate(44100, 100), 28665.0f), "cap = 130 %% of 22050 Hz");
	e = ent_at(10, 0, 0);
	snd_emit(e, CHAN_WEAPON, R(R_B), 1, ATTN_NORM, 0, 200);
	on(e, CHAN_WEAPON, &v);
	snd_voice_info(v, &o);
	CHECK(near(o.rate, 28665.0f), "emit at pitch 200 of 22050 Hz capped (%g)", (double)o.rate);
	frames(30);
	snd_voice_info(v, &o);
	CHECK(o.pos >= (uint32_t)(28665.0 * 29 * DT) && o.pos <= (uint32_t)(28665.0 * 30 * DT) + 1,
		"plays at the capped rate (pos %lu)", (unsigned long)o.pos);
}

/* ---- S7 (host part): sentence words, modifiers, gap, stop ------------------ */

static void s7(void)
{
	snd_voice_info_t o;
	res_t seen[8];
	float gaps[8], rates[8], vols[8];
	int e, v, f, n = 0, first, nw, i;
	const res_word_t *w;

	log_printf("S7 sentences\n");
	reset();
	e = ent_at(10, 0, 0);
	nw = res_sentence_words((uint16_t)RES_INDEX(R(R_TST0)), &first);
	CHECK(nw == 3, "TST0 has 3 words (%d)", nw);
	snd_sentence(e, CHAN_VOICE, (uint16_t)RES_INDEX(R(R_TST0)), 1, ATTN_NONE, 0, 100);
	for (f = 0; f < 60; f++)
	{
		if (!on(e, CHAN_VOICE, &v))
			break;
		snd_voice_info(v, &o);
		if (!n || seen[n - 1] != o.res || o.gap != gaps[n - 1])
		{
			if (n < 8)
			{
				seen[n] = o.res;
				gaps[n] = o.gap;
				rates[n] = o.rate;
				vols[n] = o.left;
				n++;
			}
		}
		frames(1);
	}
	CHECK(n == 3, "three words played (%d)", n);
	for (i = 0; i < n && i < nw; i++)
	{
		w = res_word(first + i);
		CHECK(w && seen[i] == w->res, "word %d in order", i);
	}
	if (n == 3)
	{
		CHECK(gaps[0] < 0, "first word starts at once");
		CHECK(gaps[1] >= 0 && gaps[1] <= DT + 1e-6 && gaps[2] >= 0 && gaps[2] <= DT + 1e-6,
			"gaps %g, %g s <= one frame", (double)gaps[1], (double)gaps[2]);
		CHECK(near(rates[0], 11025) && near(rates[1], 11025 * 1.2f) && near(rates[2], 11025),
			"(p120) on word 2: %g %g %g", (double)rates[0], (double)rates[1], (double)rates[2]);
		CHECK(near(vols[0], 1) && near(vols[1], 0.8f) && near(vols[2], 1),
			"(v80) on word 2: %g %g %g", (double)vols[0], (double)vols[1], (double)vols[2]);
	}
	/* (e50): word 3 (0.4 s) stops at half: the sentence lasts ~0.3 + 0.25 +
	 * 0.2 s + frame rounding */
	CHECK(f >= 21 && f <= 25, "sentence over after %d frames with (e50)", f);

	/* stop mid-sentence: no later word starts */
	reset();
	e = ent_at(10, 0, 0);
	snd_sentence(e, CHAN_VOICE, (uint16_t)RES_INDEX(R(R_TST1)), 1, ATTN_NORM, 0, 100);
	frames(12);	/* into word 2 */
	CHECK(on(e, CHAN_VOICE, NULL) == 1, "sentence playing");
	snd_stop(e, CHAN_VOICE);
	CHECK(on(e, CHAN_VOICE, NULL) == 0, "stop ends the sentence");
	frames(30);
	CHECK(busy() == 0, "no later word starts");
	/* a new sentence on the channel replaces the old one */
	snd_sentence(e, CHAN_VOICE, (uint16_t)RES_INDEX(R(R_TST1)), 1, ATTN_NORM, 0, 100);
	frames(3);
	snd_sentence(e, CHAN_VOICE, (uint16_t)RES_INDEX(R(R_TST0)), 1, ATTN_NORM, 0, 100);
	CHECK(on(e, CHAN_VOICE, NULL) == 1, "one sentence per entity channel");
}

/* ---- S8: <= 10 open waves, none closed under a voice ------------------------ */

static void s8(void)
{
	snd_stats_t st;
	int i, f, e, max = 0, bad = 0, n;

	log_printf("S8 waves\n");
	reset();
	e = ent_at(10, 0, 0);
	for (f = 0; f < 120; f++)
	{
		/* a new sound every frame on AUTO, cycling 16 distinct waves */
		snd_emit(e, CHAN_AUTO, R(R_D0 + f % 16), 1, ATTN_NORM, 0, 100);
		if (f % 7 == 0)
			snd_sentence(e, CHAN_VOICE, (uint16_t)RES_INDEX(R(R_TST1)), 1, ATTN_NORM, 0, 100);
		n = snd_waves_open();
		if (n > max)
			max = n;
		if (snd_wave_voice_check() >= 0)
			bad++;
		frames(1);
		if (snd_wave_voice_check() >= 0)
			bad++;
	}
	snd_stats(&st, 0);
	CHECK(max <= SND_WAVES && st.waves_peak <= SND_WAVES, "open waves peak %d (stats %lu)", max,
		(unsigned long)st.waves_peak);
	CHECK(max == SND_WAVES, "the cap was reached");
	CHECK(bad == 0, "no voice on a closed or reused wave (%d)", bad);
	CHECK(st.started > 120, "sounds kept starting (%lu)", (unsigned long)st.started);
	for (i = 0; i < 200; i++)
		frames(1);
	CHECK(busy() == 0 && snd_waves_open() <= SND_WAVES, "all over; waves stay open for reuse");
	/* LRU: with everything idle a new sound closes the least recently used */
	snd_emit(e, CHAN_AUTO, R(R_A), 1, ATTN_NORM, 0, 100);
	CHECK(on(e, CHAN_AUTO, NULL) == 1 && snd_waves_open() <= SND_WAVES, "LRU close for a new wave");
}

int main(int argc, char **argv)
{
	const char *t;
	snd_stats_t st;

	if (argc < 4 || strcmp(argv[1], "--root"))
	{
		fprintf(stderr, "usage: hl64-snd --root CONTENT S1..S8|all\n");
		return 2;
	}
	host_io_init(argv[2]);
	mem_boot_carve();
	settings_reset();
	io_init();
	ent_init();
	res_init();
	snd_init();
	snd_log_set(1);
	t = argv[3];
	if (!strcmp(t, "all") || !strcmp(t, "S1"))
		s1();
	if (!strcmp(t, "all") || !strcmp(t, "S2"))
		s2();
	if (!strcmp(t, "all") || !strcmp(t, "S3"))
		s3();
	if (!strcmp(t, "all") || !strcmp(t, "S4"))
		s4();
	if (!strcmp(t, "all") || !strcmp(t, "S5"))
		s5();
	if (!strcmp(t, "all") || !strcmp(t, "S6"))
		s6();
	if (!strcmp(t, "all") || !strcmp(t, "S7"))
		s7();
	if (!strcmp(t, "all") || !strcmp(t, "S8"))
		s8();
	snd_stats(&st, 0);
	log_printf("snd stats started=%lu stolen=%lu dropped=%lu voices_peak=%lu loops_peak=%lu "
		"waves_peak=%lu mem_peak=%lu region=%lu\n", (unsigned long)st.started,
		(unsigned long)st.stolen, (unsigned long)st.dropped, (unsigned long)st.voices_peak,
		(unsigned long)st.loops_peak, (unsigned long)st.waves_peak, (unsigned long)st.mem_peak,
		(unsigned long)st.mem_region);
	log_printf("snd test %s: %d checks, %d failed: %s\n", t, checks, fails, fails ? "FAIL" : "PASS");
	log_flush();
	return fails ? 1 : 0;
}
