/* SPDX-License-Identifier: MIT */
/*
 * stream.c -- host tests of the M4 residency (engine/world/stream.c,
 * world.c, engine/io; docs/architecture.md 5.2-5.4), run by
 * test/m4/stream.sh on cooked content:
 *
 *   T1 async open: wld_open_async completes through engine/io without a
 *      synchronous read or a frame over the budget, wld_layer stays NULL
 *      until ready, and the layer equals a synchronously opened one.
 *   T2 seam pins: B opened async from a CWG seam's approach box, its seam
 *      set + window pinned: resident before a simulated switch, the 128 KB
 *      seam budget while missing, the switch re-base is not a teleport.
 *   T3 change-driven target set: recompute only on eye leaf / cell /
 *      velocity bucket / pins / collision window changes, else reassert.
 *   T4 eviction classes: weaker never evicts stronger, stronger evicts
 *      weaker, prefetch spares chunks used in the last 30 frames, queued-
 *      not-started reads go first, promotion of a queued read.
 *   T5 teleport windows: pop-in inside a window is popin_tele, outside
 *      popin; a seam re-base is not a teleport.
 *   T6 blocked-I/O positive control: a deliberate 64 KB synchronous read
 *      counts as a frame > 1 ms; load I/O (io_set_load) does not; inside
 *      a test teleport window (io_tele_window) it counts as blk1ms_tele.
 *
 * Usage: hl64-stream CONTENT_DIR. Output: `stream: T<n> <what>: PASS|FAIL
 * ...` lines, then `stream: N failed`; exit 0 when none failed.
 */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hl64/ent.h"
#include "hl64/limits.h"
#include "engine/core/log.h"
#include "engine/core/sys.h"
#include "engine/io/io.h"
#include "engine/mem/carve.h"
#include "engine/phys/phys.h"
#include "engine/world/wld.h"
#include "engine/world/wld_int.h"
#include "plat/host/host.h"

static int fails;
static uint32_t fr = 1;
static double now = 1.0;

static void check(const char *t, int ok, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void check(const char *t, int ok, const char *fmt, ...)
{
	va_list ap;

	printf("stream: %s: %s", t, ok ? "PASS " : "FAIL ");
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
	fails += !ok;
}

/* One frame as eng_frame orders it: io step 3, the stream at step 9, the
 * render-time pumps. home < 0: no stream frame. */
static void frame(int home, const float eye[3], const float vel[3])
{
	int i;

	wld_frame(fr++);
	io_frame_begin(33333);
	io_pump();
	now += 1.0 / 30.0;
	eng_set_clock(now, 1.0 / 30.0);
	if (home >= 0)
		wld_stream_frame(home, eye, vel);
	for (i = 0; i < 6; i++)
		io_pump();
	log_flush();
}

static void frames(int n)
{
	static const float z[3];

	while (n-- > 0)
		frame(-1, z, z);
}

/* Frame boundaries without a pump: fences retire, queued reads stay
 * queued (not started). */
static void retire(int n)
{
	while (n-- > 0)
		wld_frame(fr++);
}

/* ---- campaign.cwg seams (docs/formats/cwg-format.md) --------------------- */

static uint8_t cwg[256 * 1024];

static uint16_t be16(const uint8_t *p)
{
	return (uint16_t)(p[0] << 8 | p[1]);
}

static float bef(const uint8_t *p)
{
	union { uint32_t u; float f; } v;

	v.u = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
	return v.f;
}

typedef struct {
	int		from, to, flags;
	float		delta[3], sw[6], approach[6], T[3];
	uint16_t	pool[WLD_PIN_POOL];
	int		npool;
} seam_t;

static int find_seam(const char *root, int from, int to, seam_t *s)
{
	char path[1024];
	FILE *f;
	size_t n;
	uint32_t nmaps, nlm, nln, nseam, ntwin, seamofs, poolofs, i;
	int k;

	snprintf(path, sizeof(path), "%s/maps/campaign.cwg", root);
	if (!(f = fopen(path, "rb")))
		return -1;
	n = fread(cwg, 1, sizeof(cwg), f);
	fclose(f);
	if (n < 32)
		return -1;
	nmaps = be16(cwg + 8);
	nlm = be16(cwg + 10);
	nln = be16(cwg + 12);
	nseam = be16(cwg + 20);
	ntwin = be16(cwg + 22);
	seamofs = 32 + 48 * nmaps + 16 * nlm + 8 * nln;
	poolofs = seamofs + 136 * nseam + 4 * ntwin;
	for (i = 0; i < nseam; i++)
	{
		const uint8_t *r = cwg + seamofs + 136 * i;
		uint32_t first = be16(r + 18), cnt = be16(r + 20);

		if (be16(r) != from || be16(r + 2) != to)
			continue;
		s->from = from;
		s->to = to;
		s->flags = be16(r + 12);
		for (k = 0; k < 3; k++)
		{
			s->delta[k] = bef(r + 24 + 4 * k);
			s->T[k] = bef(r + 124 + 4 * k);
		}
		for (k = 0; k < 6; k++)
		{
			s->sw[k] = bef(r + 36 + 4 * k);
			s->approach[k] = bef(r + 84 + 4 * k);
		}
		s->npool = (int)(cnt < WLD_PIN_POOL ? cnt : WLD_PIN_POOL);
		for (k = 0; k < s->npool; k++)
			s->pool[k] = be16(cwg + poolofs + 2 * (first + (uint32_t)k));
		return 0;
	}
	return -1;
}

/* ---- T1 async open --------------------------------------------------------- */

static void t1_async_open(const char *name)
{
	int map = wld_map_find(name), n, slot;
	wld_layer_t *L;
	uint32_t nchunks, dsz, nmov;
	float start[3];
	io_stats_t a, b;
	static const float z[3];

	if (map < 0)
	{
		check("T1 async open", 0, "no map %s", name);
		return;
	}
	L = wld_open(map);
	nchunks = L->dir->nchunks;
	dsz = L->hdr.dir_size;
	nmov = L->nmovers;
	memcpy(start, L->start_org, sizeof(start));
	wld_close(L);
	frames(3);

	io_stats(&a, 0);
	L = wld_open_async(map);
	slot = L ? L->slot : -1;
	check("T1 async open", L && !wld_layer(slot) && wld_layer_of_map(map) == L && !wld_layer_ready(L),
		"in flight: wld_layer NULL, wld_layer_of_map = it, ready 0");
	for (n = 0; L && wld_layer_ready(L) == 0 && n < 300; n++)
		frame(map, z, z);
	io_stats(&b, 0);
	check("T1 async open", L && wld_layer_ready(L) == 1 && wld_layer(slot) == L,
		"%s ready after %d frames", name, n);
	check("T1 async open", b.sync_calls == a.sync_calls && b.life_frames_blocked_1ms
		== a.life_frames_blocked_1ms && b.life_frames_budget_over == a.life_frames_budget_over,
		"sync reads %lu, frames > 1 ms %lu, budget over %lu",
		(unsigned long)(b.sync_calls - a.sync_calls),
		(unsigned long)(b.life_frames_blocked_1ms - a.life_frames_blocked_1ms),
		(unsigned long)(b.life_frames_budget_over - a.life_frames_budget_over));
	if (L && L->ready == 1)
		check("T1 async open", L->dir->nchunks == nchunks && L->hdr.dir_size == dsz
			&& L->nmovers == nmov && !memcmp(start, L->start_org, sizeof(start)),
			"same directory, movers and start as the sync open (%lu chunks, %lu movers)",
			(unsigned long)nchunks, (unsigned long)nmov);
	wld_close(L);
	frames(3);
}

/* ---- T2 seam pins ----------------------------------------------------------- */

static int coll_window_resident(const wld_layer_t *L, const float lo[3], const float hi[3])
{
	const w64_grid_t *g = L->grid;
	int r[6], k, x, y, z, i;

	for (k = 0; k < 3; k++)
	{
		float a = (lo[k] - g->origin[k]) / (float)g->cell, b = (hi[k] - g->origin[k]) / (float)g->cell;

		r[k] = a < 0 ? 0 : a >= g->dims[k] ? g->dims[k] : (int)a;
		r[k + 3] = b < 0 ? -1 : b >= g->dims[k] ? g->dims[k] - 1 : (int)b;
	}
	for (z = r[2]; z <= r[5]; z++)
		for (y = r[1]; y <= r[4]; y++)
			for (x = r[0]; x <= r[3]; x++)
			{
				const uint16_t *v = L->glists + L->cells[x + g->dims[0] * (y + g->dims[1] * z)].list;

				for (i = 1; i <= v[0]; i++)
					if (L->ch[L->dir->coll_first + v[i]].state < WLD_CH_RESIDENT)
						return 0;
			}
	return 1;
}

static void t2_seam(const char *root, const char *an, const char *bn)
{
	int a = wld_map_find(an), b = wld_map_find(bn), n, k, seam_budget = 1;
	float eye[3], vel[3] = { 0, 0, 0 }, box[6], eyeb[3];
	wld_layer_t *A, *B;
	wld_stream_stats_t s0, s1;
	io_stats_t i0, i1;
	seam_t s;

	if (a < 0 || b < 0 || find_seam(root, a, b, &s))
	{
		check("T2 seam pins", 0, "no seam %s -> %s", an, bn);
		return;
	}
	A = wld_open(a);
	for (k = 0; k < 3; k++)
		eye[k] = 0.5f * (s.sw[k] + s.sw[k + 3]);	/* a proved eye, in the approach */
	frames(2);
	frame(a, eye, vel);
	io_stats(&i0, 0);
	wld_stream_stats(&s0, 0);
	B = wld_open_async(b);
	for (n = 0; B && wld_layer_ready(B) == 0 && n < 300; n++)
		frame(a, eye, vel);
	check("T2 seam pins", B && wld_layer_ready(B) == 1, "%s opened async in %d frames", bn, n);
	if (!B || B->ready != 1)
		return;
	/* the switch eye box in B's frame, the player box under it */
	for (k = 0; k < 3; k++)
	{
		float h = k == 2 ? 36.0f : 16.0f;

		box[k] = s.sw[k] + s.delta[k] - (k == 2 ? 28.0f : 0) - h;
		box[k + 3] = s.sw[k + 3] + s.delta[k] - (k == 2 ? 28.0f : 0) + h;
	}
	wld_pin_seam(B, s.pool, s.npool);
	wld_pin_seam_window(B, box, box + 3);
	for (n = 0; !wld_pin_resident(B) && n < 300; n++)
	{
		frame(a, eye, vel);
		if (!wld_pin_resident(B) && io_budget() != IO_BUDGET_SEAM)
			seam_budget = 0;
	}
	check("T2 seam pins", wld_pin_resident(B), "%s -> %s seam set (%d entries) + window "
		"resident after %d frames", an, bn, s.npool, n);
	if (!wld_pin_resident(B))
	{
		wld_stats_t w;

		wld_stats(&w);
		printf("stream:   pools render %lu texture %lu coll %lu B used, nofit %lu, io queued %lu B\n",
			(unsigned long)w.pool_used[0], (unsigned long)w.pool_used[1],
			(unsigned long)w.pool_used[2], (unsigned long)w.nofit, (unsigned long)io_queued());
	}
	if (!wld_pin_resident(B))
		for (k = 0; k < s.npool; k++)
		{
			uint16_t e = s.pool[k];
			int c = (e & 0x8000) ? B->subs[e & 0x7FFF].chunk : B->sects[e].rend_chunk;

			printf("stream:   entry %04x chunk %d state %d cls %d want %s\n", e, c,
				B->ch[c].state, B->ch[c].cls, B->ch[c].want == wld_tgen ? "now" : "old");
			if (!(e & 0x8000))
			{
				const w64_sector_t *sc = &B->sects[e];
				int j;

				for (j = 0; j < sc->page_count; j++)
				{
					int pc = B->dir->texp_first + B->pagepool[sc->page_first + j];

					if (B->ch[pc].state < WLD_CH_RESIDENT)
						printf("stream:     page chunk %d state %d\n", pc, B->ch[pc].state);
				}
			}
		}
	check("T2 seam pins", seam_budget, "128 KB seam budget while the pin was missing chunks");
	frame(a, eye, vel);
	check("T2 seam pins", io_budget() == IO_BUDGET, "64 KB budget once resident");
	{
		float lo[3], hi[3];

		for (k = 0; k < 3; k++)
		{
			lo[k] = box[k] - 128;
			hi[k] = box[k + 3] + 128;
		}
		check("T2 seam pins", coll_window_resident(B, lo, hi),
			"B collision window resident before the switch (no sync COLL read after it)");
	}
	/* simulated switch: home := B, the player re-based by T (global) */
	for (k = 0; k < 3; k++)
		eyeb[k] = eye[k] + s.delta[k];
	wld_stream_rebase(s.T);
	frame(b, eyeb, vel);
	wld_stream_stats(&s1, 0);
	io_stats(&i1, 0);
	check("T2 seam pins", s1.teleports == s0.teleports, "the switch re-base is no teleport");
	check("T2 seam pins", i1.life_frames_budget_over == i0.life_frames_budget_over
		&& i1.life_frames_blocked_1ms == i0.life_frames_blocked_1ms && i1.sync_calls == i0.sync_calls,
		"no frame over budget, no sync read, no blocked frame through open + pin + switch");
	wld_unpin_seam(B);
	wld_close(B);
	wld_close(A);
	frames(3);
}

/* ---- T3 change-driven recompute --------------------------------------------- */

static void t3_recompute(const char *name)
{
	int map = wld_map_find(name), i;
	wld_layer_t *L = wld_open(map);
	float eye[3], vel[3] = { 0, 0, 0 }, lo[3], hi[3];
	wld_stream_stats_t a, b;
	static const uint32_t none[1];

	memcpy(eye, L->start_org, sizeof(eye));
	eye[2] += 28;
	frame(map, eye, vel);
	wld_stream_stats(&a, 0);
	for (i = 0; i < 20; i++)
		frame(map, eye, vel);
	wld_stream_stats(&b, 0);
	check("T3 recompute", b.recompute == a.recompute && b.reassert == a.reassert + 20,
		"standing still 20 frames: recompute +%lu reassert +%lu",
		(unsigned long)(b.recompute - a.recompute), (unsigned long)(b.reassert - a.reassert));
	a = b;
	eye[0] += 1;
	frame(map, eye, vel);
	wld_stream_stats(&b, 0);
	check("T3 recompute", b.recompute == a.recompute, "1 u within the leaf and cell: no recompute");
	a = b;
	vel[0] = 300;
	frame(map, eye, vel);
	vel[0] = 310;
	frame(map, eye, vel);
	wld_stream_stats(&b, 0);
	check("T3 recompute", b.recompute == a.recompute + 1,
		"velocity 0 -> 300 (new bucket) -> 310 (same bucket): recompute +%lu",
		(unsigned long)(b.recompute - a.recompute));
	a = b;
	{
		float far[3] = { L->start_org[0], L->start_org[1] + 400, L->start_org[2] };

		wld_pin_point(map, far);
		frame(map, eye, vel);
		wld_pin_point(map, far);
		frame(map, eye, vel);
		wld_stream_stats(&b, 0);
		check("T3 recompute", b.recompute == a.recompute + 1 && b.pins == a.pins + 1,
			"point pin 400 u away: recompute +%lu, re-arming it: none",
			(unsigned long)(b.recompute - a.recompute));
		a = b;
		far[2] += 28;
		frame(map, far, vel);
		wld_stream_stats(&b, 0);
		check("T3 recompute", b.recompute == a.recompute + 1,
			"arrival at the pin: it lapses (recompute +%lu)",
			(unsigned long)(b.recompute - a.recompute));
		frame(map, eye, vel);
	}
	wld_stream_stats(&a, 0);
	for (i = 0; i < 3; i++)
	{
		lo[i] = eye[i] - 200;
		hi[i] = eye[i] + 200;
	}
	lo[0] += L->gofs[0];
	hi[0] += L->gofs[0];
	lo[1] += L->gofs[1];
	hi[1] += L->gofs[1];
	lo[2] += L->gofs[2];
	hi[2] += L->gofs[2];
	wld_stream_coll(lo, hi, none, 0);
	frame(map, eye, vel);
	wld_stream_coll(lo, hi, none, 0);
	frame(map, eye, vel);
	lo[2] += 600;
	hi[2] += 600;
	wld_stream_coll(lo, hi, none, 0);
	frame(map, eye, vel);
	wld_stream_stats(&b, 0);
	check("T3 recompute", b.recompute == a.recompute + 2,
		"collision window: new (+1), same (+0), 600 u up (+1): recompute +%lu",
		(unsigned long)(b.recompute - a.recompute));
	wld_close(L);
	frames(3);
}

/* ---- T4 eviction classes ------------------------------------------------------ */

static int land_all(void)
{
	int n;

	for (n = 0; n < 100 && io_busy(); n++)
		frames(1);
	return !io_busy();
}

static int tex_resident(const wld_layer_t *L, int p)
{
	return L->ch[L->dir->texp_first + p].state >= WLD_CH_RESIDENT;
}

/* Name every TEXP page of L at cls (a new generation) until the texture
 * pool is full (a page no eviction for cls makes room for, after the
 * fences retired and the reads landed); that page, -1 none. */
static int fill_textures(wld_layer_t *L, int cls)
{
	int p, stuck = 0;

	if (!++wld_tgen)
		wld_tgen = 1;
	for (p = 0; p < L->dir->npages; p++)
	{
		if (wld_name(L, L->dir->texp_first + p, cls))
		{
			stuck = 0;
			continue;
		}
		if (stuck++ < 50)
		{
			/* busy request slots or fenced room: land, retire, retry */
			frames(1);
			retire(3);
			p--;
			continue;
		}
		break;
	}
	land_all();
	return p < L->dir->npages ? p : -1;
}

/* Re-name every resident page at cls in a new generation. */
static void rename_resident(wld_layer_t *L, int cls)
{
	int p;

	if (!++wld_tgen)
		wld_tgen = 1;
	for (p = 0; p < L->dir->npages; p++)
		if (tex_resident(L, p))
			wld_name(L, L->dir->texp_first + p, cls);
}

/* wld_name, retried after fence retirements (no pump) up to 10 rounds. */
static int name_rounds(wld_layer_t *L, int c, int cls, int *rounds)
{
	for (*rounds = 0; *rounds < 10; ++*rounds)
	{
		if (wld_name(L, c, cls))
			return 1;
		retire(3);
	}
	return 0;
}

static void t4_evict(void)
{
	wld_layer_t *L = NULL;
	wld_stats_t w0, w1;
	int m, q, p, k, best = -1, rounds;
	uint32_t most = 0, ev, eq, pr;

	/* the map whose TEXP pages overflow the texture pool the most */
	for (m = 0; m < wld_map_count(); m++)
	{
		uint32_t sum = 0;

		L = wld_open(m);
		for (p = 0; p < L->dir->npages; p++)
			sum += (L->chunks[L->dir->texp_first + p].size + 15) & ~15u;
		if (sum > most)
		{
			most = sum;
			best = m;
		}
		wld_close(L);
		frames(3);
	}
	if (most <= HL64_POOL_TEXTURE_KB * 1024u)
	{
		check("T4 evict", 0, "no map's pages overflow the texture pool (largest %lu B)",
			(unsigned long)most);
		return;
	}
	L = wld_open(best);
	q = fill_textures(L, WLD_CLS_PREFETCH);
	check("T4 evict", q >= 0, "%s: %lu B of pages, pool full at page %d", wld_map_name(best),
		(unsigned long)most, q);
	if (q < 0)
		return;

	/* E1: a weaker class never evicts a stronger one */
	rename_resident(L, WLD_CLS_OPEN);
	frames(3);
	wld_stats(&w0);
	k = wld_name(L, L->dir->texp_first + q, WLD_CLS_PREFETCH);
	wld_stats(&w1);
	check("T4 evict", !k && w1.evictions == w0.evictions && !tex_resident(L, q),
		"E1 prefetch request into a pool of open chunks: refused, no eviction");

	/* E2: a stronger class evicts a weaker one (the LRU), only as much as
	 * the request needs: evicted TEXP bytes free behind their two-frame
	 * fence, the request waits for them (rounds) instead of evicting on */
	rename_resident(L, WLD_CLS_PREFETCH);
	frames(40);
	wld_stats(&w0);
	k = name_rounds(L, L->dir->texp_first + q, WLD_CLS_PIN, &rounds);
	wld_stats(&w1);
	check("T4 evict", k && w1.evictions > w0.evictions && w1.evictions - w0.evictions <= 8,
		"E2 pin request into a pool of prefetch chunks: queued after %d fence rounds, "
		"%lu evicted", rounds, (unsigned long)(w1.evictions - w0.evictions));
	land_all();

	/* E3: prefetch spares a chunk used in the last 30 frames */
	q = fill_textures(L, WLD_CLS_PREFETCH);
	retire(3);
	if (!++wld_tgen)
		wld_tgen = 1;
	for (p = 0; p < L->dir->npages; p++)
		if (tex_resident(L, p))
			wld_chunk_use(L, L->dir->texp_first + p);
	frames(10);
	wld_stats(&w0);
	k = q >= 0 && wld_name(L, L->dir->texp_first + q, WLD_CLS_PREFETCH);
	wld_stats(&w1);
	check("T4 evict", q >= 0 && !k && w1.evictions == w0.evictions,
		"E3 prefetch, every chunk used 10 frames ago: refused (page %d, queued %d, "
		"%lu evicted)", q, k, (unsigned long)(w1.evictions - w0.evictions));
	frames(25);
	k = q >= 0 && name_rounds(L, L->dir->texp_first + q, WLD_CLS_PREFETCH, &rounds);
	wld_stats(&w1);
	check("T4 evict", k && w1.evictions > w0.evictions,
		"E3 prefetch, chunks used 35 frames ago: queued (%lu evicted)",
		(unsigned long)(w1.evictions - w0.evictions));
	land_all();

	/* E4: queued-not-started reads of a weaker class go first. The pool:
	 * pin chunks, a few unwanted ones the prefetch reads (queued, never
	 * pumped here) take the room of. E5: a queued read a stronger class
	 * needs is promoted. */
	fill_textures(L, WLD_CLS_PIN);
	frames(40);
	{
		int queued[16], nq = 0, open_q = -1, spare = 16, full = 0;

		if (!++wld_tgen)
			wld_tgen = 1;
		for (p = L->dir->npages - 1; p >= 0; p--)
			if (tex_resident(L, p) && spare-- <= 0)
				wld_name(L, L->dir->texp_first + p, WLD_CLS_PIN);
		/* up to 15 reads: a request slot stays free (no slot steal) */
		for (p = 0; p < L->dir->npages && nq < 15; p++)
		{
			if (L->ch[L->dir->texp_first + p].state != WLD_CH_ABSENT)
				continue;
			if (!wld_name(L, L->dir->texp_first + p, WLD_CLS_PREFETCH))
			{
				retire(3);
				if (!wld_name(L, L->dir->texp_first + p, WLD_CLS_PREFETCH))
				{
					full = 1;
					open_q = p;	/* the page that does not fit */
					break;
				}
			}
			queued[nq++] = p;
		}
		check("T4 evict", nq > 0 && full && open_q >= 0,
			"E4 setup: %d prefetch reads queued into a full pool", nq);
		if (nq > 0 && full && open_q >= 0)
		{
			ev = (wld_stats(&w0), w0.evictions);
			eq = wld_n_evict_queued;
			pr = wld_n_promoted;
			k = wld_name(L, L->dir->texp_first + open_q, WLD_CLS_OPEN);
			wld_stats(&w1);
			check("T4 evict", k && wld_n_evict_queued > eq && w1.evictions == ev,
				"E4 open request into a full pool: queued, %lu queued prefetch reads "
				"cancelled, %lu resident chunks evicted",
				(unsigned long)(wld_n_evict_queued - eq), (unsigned long)(w1.evictions - ev));
			for (k = 0; k < nq; k++)
				if (L->ch[L->dir->texp_first + queued[k]].state == WLD_CH_LOADING)
					break;
			if (k < nq)
			{
				wld_name(L, L->dir->texp_first + queued[k], WLD_CLS_OPEN);
				check("T4 evict", wld_n_promoted == pr + 1, "E5 queued prefetch read "
					"needed at open: promoted");
			}
			else
				check("T4 evict", 0, "E5: no queued prefetch read left to promote");
		}
	}
	land_all();
	wld_close(L);
	frames(3);
}

/* ---- T5 teleport windows ---------------------------------------------------- */

static void t5_teleport(const char *name)
{
	int map = wld_map_find(name), i;
	wld_layer_t *L = wld_open(map);
	float eye[3], vel[3] = { 0, 0, 0 }, T[3] = { 2000, 0, 0 };
	wld_stream_stats_t a, b;

	memcpy(eye, L->start_org, sizeof(eye));
	eye[2] += 28;
	frame(map, eye, vel);
	for (i = 0; i < 40; i++)
		frame(map, eye, vel);
	wld_stream_stats(&a, 0);
	wld_stream_missing(3);
	wld_stream_stats(&b, 0);
	check("T5 teleport", b.popin == a.popin + 1 && b.popin_tele == a.popin_tele,
		"missing outside a window: popin");
	a = b;
	vel[0] = 300;
	for (i = 0; i < 5; i++)
	{
		eye[0] += 10;
		frame(map, eye, vel);
	}
	wld_stream_stats(&b, 0);
	check("T5 teleport", b.teleports == a.teleports, "walking 300 u/s: no teleport");
	a = b;
	vel[0] = 0;
	eye[1] += 1000;
	frame(map, eye, vel);
	wld_stream_missing(2);
	wld_stream_stats(&b, 0);
	check("T5 teleport", b.teleports == a.teleports + 1 && b.popin_tele == a.popin_tele + 1
		&& b.popin == a.popin, "a 1000 u jump: teleport, missing inside its window: popin_tele");
	a = b;
	for (i = 0; i < 33; i++)
		frame(map, eye, vel);
	wld_stream_missing(2);
	wld_stream_stats(&b, 0);
	check("T5 teleport", b.popin == a.popin + 1, "1.1 s later: popin again");
	a = b;
	wld_stream_rebase(T);
	for (i = 0; i < 3; i++)
		eye[i] += T[i];
	frame(map, eye, vel);
	wld_stream_stats(&b, 0);
	check("T5 teleport", b.teleports == a.teleports, "a 2000 u seam re-base: no teleport");
	wld_close(L);
	frames(3);
}

/* ---- T6 blocked-I/O positive control ------------------------------------------- */

static uint8_t big[64 * 1024] __attribute__((aligned(16)));

static void t6_blocked(void)
{
	uint32_t size, rom = sys_cart_addr("maps/hldemo1.w64", &size);
	io_stats_t a, b;

	if (!rom || size < sizeof(big))
	{
		check("T6 blocked", 0, "no maps/hldemo1.w64 >= 64 KB");
		return;
	}
	frames(1);
	io_stats(&a, 0);
	io_read_sync(big, rom, sizeof(big));
	frames(1);
	io_stats(&b, 0);
	check("T6 blocked", b.life_frames_blocked_1ms == a.life_frames_blocked_1ms + 1
		&& b.life_blocked_max_us >= 1000, "a 64 KB sync read mid-game: a frame > 1 ms "
		"(blkmax %lu us)", (unsigned long)b.life_blocked_max_us);
	a = b;
	io_set_load(1);
	io_read_sync(big, rom, sizeof(big));
	io_set_load(0);
	frames(1);
	io_read_sync(big, rom, 512);
	frames(1);
	io_stats(&b, 0);
	check("T6 blocked", b.life_frames_blocked_1ms == a.life_frames_blocked_1ms
		&& b.life_frames_blocked_1ms_tele == a.life_frames_blocked_1ms_tele,
		"the same read as load I/O (io_set_load), a 512 B read: not counted");
	a = b;
	io_tele_window(2);
	io_read_sync(big, rom, sizeof(big));
	frames(1);
	io_read_sync(big, rom, sizeof(big));
	frames(1);
	io_read_sync(big, rom, sizeof(big));
	frames(1);
	io_stats(&b, 0);
	check("T6 blocked", b.life_frames_blocked_1ms_tele == a.life_frames_blocked_1ms_tele + 2
		&& b.life_frames_blocked_1ms == a.life_frames_blocked_1ms + 1,
		"the read in a 2-frame teleport window: 2 frames blk1ms_tele (%lu), the frame "
		"after it blk1ms (%lu)",
		(unsigned long)(b.life_frames_blocked_1ms_tele - a.life_frames_blocked_1ms_tele),
		(unsigned long)(b.life_frames_blocked_1ms - a.life_frames_blocked_1ms));
}

int main(int argc, char **argv)
{
	const char *root;

	if (argc != 2)
	{
		fprintf(stderr, "usage: hl64-stream CONTENT_DIR\n");
		return 2;
	}
	root = argv[1];
	host_io_init(root);
	mem_boot_carve();
	io_init();
	ent_init();
	if (!wld_init())
	{
		fprintf(stderr, "stream: no campaign in %s\n", root);
		return 2;
	}
	eng_clock_new_game();
	frames(1);	/* boot ends: the first frame */
	t6_blocked();
	t1_async_open("hldemo3");
	t1_async_open("t0a0b");
	t2_seam(root, "hldemo1", "hldemo2");
	t2_seam(root, "hldemo2", "hldemo3");
	t2_seam(root, "hldemo3", "hldemo2");
	t2_seam(root, "t0a0c", "t0a0d");
	t3_recompute("hldemo2");
	t5_teleport("hldemo2");
	t4_evict();
	printf("stream: %d failed\n", fails);
	log_flush();
	return fails ? 1 : 0;
}
