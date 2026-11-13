/* SPDX-License-Identifier: MIT */
/*
 * sprite.c -- sprites (docs/specs/render-fx.md 3): the cooked .sc4 CI4
 * sprite files (tools/cook/hl_sprite_cook.py layout) in an LRU cache in the
 * sprites region, frame selection and the camera-facing / oriented quad
 * of each SPR v2 orientation type. Written from render-fx.md 3 and the
 * cook's format note; the old tree's r64_trans.c (changed after the
 * provenance audit) was not opened.
 *
 * Cache: whole files, one PI read each at prefetch priority on first use;
 * a sprite whose data has not landed is skipped that frame. A model
 * unused for 90 frames, or the least recently used one when a new file
 * does not fit, is freed behind a two-frame fence (the RDP may still read
 * its texels).
 */
#include <math.h>
#include <string.h>

#include "hl64/limits.h"
#include "hl64/rec.h"
#include "engine/core/hlmath.h"
#include "engine/core/log.h"
#include "engine/core/sys.h"
#include "engine/io/io.h"
#include "engine/mem/carve.h"
#include "engine/mem/pool.h"
#include "engine/render/fx_int.h"
#include "engine/render/ren.h"

#define SC4_MAGIC	0x53433431u	/* 'SC41' */
#define SC4_HEADER	80
#define SC4_FRAME	16
#define IDLE_FRAMES	90
#define FENCE_FRAMES	2

enum { SE_FREE, SE_LOADING, SE_RESIDENT, SE_BAD };

typedef struct {
	io_req_t	req;
	mem_blk_t	*blk;
	uint8_t		*data;
	uint32_t	size, used;
	res_t		r;
	uint8_t		state;
	uint8_t		pad;
	fx_sprite_t	s;
} sent_t;

static sent_t		ent[HL64_FX_SPRITE_MODELS];
static mem_pool_t	pool;
static mem_blk_t	desc[2 * HL64_FX_SPRITE_MODELS + 4];
static sprite_stats_t	st;
static uint32_t		frame;

void sprite_init(void)
{
	memset(ent, 0, sizeof(ent));
	memset(&st, 0, sizeof(st));
	mem_pool_init(&pool, "sprites", mem_region(MEM_SPRITE_CACHE),
		mem_region_bytes(MEM_SPRITE_CACHE), desc, (int)(sizeof(desc) / sizeof(desc[0])));
	frame = 0;
}

static uint16_t be16(const uint8_t *p)
{
	return (uint16_t)(p[0] << 8 | p[1]);
}

static uint32_t be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static int pow2(unsigned v)
{
	return v >= 2 && v <= 64 && !(v & (v - 1));
}

/* Validate the landed bytes (hostile input): images, entity frames (first
 * image, count inside the images; groups' interval ends increasing), the
 * host swaps the tables to native order once. 0 ok. */
static int parse(sent_t *e)
{
	uint8_t *b = e->data;
	uint32_t size = e->size, i, gofs, eofs;
	int n, ni;

	if (size < SC4_HEADER || be32(b) != SC4_MAGIC || be16(b + 4) != 2)
		return -1;
	n = be16(b + 6);
	ni = be16(b + 14);
	gofs = SC4_HEADER + (uint32_t)ni * SC4_FRAME;
	eofs = gofs + 4u * (uint32_t)n;
	if (n < 1 || ni < n || eofs + 2u * (uint32_t)ni > size || b[8] > FX_SPR_PARALLEL_ORIENTED)
		return -2;
	for (i = 0; i < (uint32_t)ni; i++)
	{
		const uint8_t *f = b + SC4_HEADER + SC4_FRAME * i;
		uint32_t ofs = be32(f + 12);

		if (!pow2(f[8]) || !pow2(f[9]) || ofs & 7 || ofs > size
			|| (uint32_t)f[8] * f[9] / 2 > size - ofs)
			return -3;
	}
	for (i = 0; i < (uint32_t)n; i++)
	{
		uint32_t first = be16(b + gofs + 4 * i), cnt = be16(b + gofs + 4 * i + 2), k;

		if (!cnt || first + cnt > (uint32_t)ni)
			return -4;
		for (k = 1; k < cnt && cnt > 1; k++)
			if (be16(b + eofs + 2 * (first + k)) <= be16(b + eofs + 2 * (first + k - 1)))
				return -5;
		if (cnt > 1 && !be16(b + eofs + 2 * first))
			return -5;
	}
#ifdef HL64_HOST
	/* host: native order in place (the cart reads it big-endian as is) */
	for (i = 0; i < 32; i++)
	{
		uint16_t v = be16(b + 16 + 2 * i);

		memcpy(b + 16 + 2 * i, &v, 2);
	}
	for (i = 0; i < (uint32_t)ni; i++)
	{
		uint8_t *f = b + SC4_HEADER + SC4_FRAME * i;
		fx_sprframe_t fr;

		fr.left = (int16_t)be16(f);
		fr.up = (int16_t)be16(f + 2);
		fr.w = be16(f + 4);
		fr.h = be16(f + 6);
		fr.tw = f[8];
		fr.th = f[9];
		fr.pad = 0;
		fr.ci4 = be32(f + 12);
		memcpy(f, &fr, sizeof(fr));
	}
	for (i = gofs; i < eofs + 2u * (uint32_t)ni; i += 2)
	{
		uint16_t v = be16(b + i);

		memcpy(b + i, &v, 2);
	}
#endif
	e->s.base = b;
	e->s.tlut = (const uint16_t *)(b + 16);
	e->s.frames = (const fx_sprframe_t *)(b + SC4_HEADER);
	e->s.groups = (const uint16_t *)(b + gofs);
	e->s.ends = (const uint16_t *)(b + eofs);
	e->s.nframes = n;
	e->s.nimages = ni;
	e->s.type = b[8];
	e->s.texformat = b[9];
	memcpy(e->s.tint, b + 10, 3);
	return 0;
}

int sprite_image(const fx_sprite_t *s, int f, double t)
{
	const uint16_t *g;
	uint32_t total, ms;
	int k;

	if (f < 0)
		f = 0;
	g = s->groups + 2 * (f % s->nframes);
	if (g[1] <= 1)
		return g[0];
	/* the member whose interval holds t within the group's cycle */
	total = s->ends[g[0] + g[1] - 1];
	ms = (uint32_t)fmod(t * 1000.0, (double)total);
	for (k = 0; k < g[1] - 1; k++)
		if (ms < s->ends[g[0] + k])
			break;
	return g[0] + k;
}

static void landed(io_req_t *r)
{
	sent_t *e = &ent[r->user];
	int bad = parse(e);

	if (bad)
	{
		log_printf("fx: sprite %s: bad .sc4 (rule %d)\n", res_name(e->r), bad);
		mem_pool_free(&pool, e->blk, frame + FENCE_FRAMES);
		e->blk = NULL;
		e->data = NULL;
		e->state = SE_BAD;
		return;
	}
	e->state = SE_RESIDENT;
	st.loads++;
}

static void drop(sent_t *e)
{
	if (e->blk)
		mem_pool_free(&pool, e->blk, frame + FENCE_FRAMES);
	memset(e, 0, sizeof(*e));
	st.evicted++;
}

/* Free the least recently used resident model not drawn in the fence
 * window; 0 when none can go. */
static int evict_lru(void)
{
	int i, best = -1;

	for (i = 0; i < HL64_FX_SPRITE_MODELS; i++)
		if (ent[i].state == SE_RESIDENT && ent[i].used + FENCE_FRAMES < frame
			&& (best < 0 || ent[i].used < ent[best].used))
			best = i;
	if (best < 0)
		return 0;
	drop(&ent[best]);
	return 1;
}

static void request(res_t r)
{
	int i, file, slot = -1;
	uint32_t ofs, size, rom, len;
	sent_t *e;

	if (!res_file(r, &file, &ofs, &size) || !(rom = res_file_rom(file, NULL)) || size < SC4_HEADER
		|| size > mem_region_bytes(MEM_SPRITE_CACHE))
		return;
	for (i = 0; i < HL64_FX_SPRITE_MODELS && slot < 0; i++)
		if (ent[i].state == SE_FREE)
			slot = i;
	if (slot < 0)
	{
		if (!evict_lru())
			return;
		for (i = 0; i < HL64_FX_SPRITE_MODELS && slot < 0; i++)
			if (ent[i].state == SE_FREE)
				slot = i;
	}
	e = &ent[slot];
	len = (size + 15) & ~15u;
	for (;;)
	{
		mem_pool_retire(&pool, frame);
		if ((e->blk = mem_pool_alloc(&pool, len, (void **)&e->data)))
			break;
		if (!evict_lru())
			return;		/* everything is in use: try again next frame */
	}
	e->r = r;
	e->size = size;
	e->used = frame;
	e->state = SE_LOADING;
	memset(&e->req, 0, sizeof(e->req));
	e->req.rom = (rom + ofs) & ~1u;
	e->req.dst = e->data;
	e->req.len = (size + 1) & ~1u;
	e->req.prio = IO_PRIO_PREFETCH;
	e->req.cb = landed;
	e->req.user = slot;
	/* an odd file start: the cook aligns files, a misaligned one is bad */
	if ((rom + ofs) & 1)
	{
		mem_pool_free(&pool, e->blk, 0);
		e->blk = NULL;
		e->state = SE_BAD;
		return;
	}
	io_submit(&e->req);
}

#ifdef HL64_HOST
int sprite_test_inject(res_t r, uint8_t *bytes, uint32_t size)
{
	int i;

	for (i = 0; i < HL64_FX_SPRITE_MODELS; i++)
		if (ent[i].state == SE_FREE)
		{
			sent_t *e = &ent[i];

			memset(e, 0, sizeof(*e));
			e->r = r;
			e->data = bytes;
			e->size = size;
			e->used = frame;
			if (parse(e))
			{
				memset(e, 0, sizeof(*e));
				return -1;
			}
			e->state = SE_RESIDENT;
			return 0;
		}
	return -1;
}
#endif

const fx_sprite_t *sprite_get(res_t r)
{
	int i;

	if (RES_KIND(r) != RES_K_SPRITE)
		return NULL;
	for (i = 0; i < HL64_FX_SPRITE_MODELS; i++)
		if (ent[i].r == r && ent[i].state != SE_FREE)
		{
			ent[i].used = frame;
			if (ent[i].state == SE_RESIDENT)
				return &ent[i].s;
			st.skipped++;
			return NULL;
		}
	request(r);
	st.skipped++;
	return NULL;
}

void sprite_frame(void)
{
	int i;
	uint32_t bytes = 0, res = 0;

	frame++;
	mem_pool_retire(&pool, frame);
	for (i = 0; i < HL64_FX_SPRITE_MODELS; i++)
	{
		if (ent[i].state == SE_RESIDENT && ent[i].used + IDLE_FRAMES < frame)
			drop(&ent[i]);
		if (ent[i].state == SE_RESIDENT || ent[i].state == SE_LOADING)
		{
			bytes += ent[i].size;
			res++;
		}
	}
	st.resident = res;
	st.bytes = pool.used;
	if (pool.used > st.peak)
		st.peak = pool.used;
	(void)bytes;
}

void sprite_stats(sprite_stats_t *s, int reset)
{
	*s = st;
	if (reset)
	{
		st.loads = st.evicted = st.skipped = 0;
		st.peak = pool.used;
	}
}

/* ---- quad ------------------------------------------------------------------- */

static void norm(float v[3])
{
	float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);

	if (l > 1e-6f)
	{
		v[0] /= l;
		v[1] /= l;
		v[2] /= l;
	}
}

/* The quad's right / up axes for the sprite type (render-fx.md 3). */
static void axes(int type, const float org[3], const float ang[3], float r[3], float u[3])
{
	float f[3];
	int k;

	switch (type)
	{
	case FX_SPR_PARALLEL_UPRIGHT:
		/* faces the view plane, stays vertical */
		r[0] = fx_s.right[0];
		r[1] = fx_s.right[1];
		r[2] = 0;
		norm(r);
		u[0] = u[1] = 0;
		u[2] = 1;
		break;
	case FX_SPR_FACING_UPRIGHT:
		/* faces the viewer's position, stays vertical: right is the
		 * horizontal perpendicular of eye -> sprite */
		r[0] = org[1] - fx_s.eye[1];
		r[1] = -(org[0] - fx_s.eye[0]);
		r[2] = 0;
		norm(r);
		u[0] = u[1] = 0;
		u[2] = 1;
		break;
	case FX_SPR_ORIENTED:
		ren_angle_axes(ang, f, r, u);
		break;
	case FX_SPR_PARALLEL_ORIENTED:
	{
		/* the view plane's axes turned by the entity's roll */
		float a = ang[2] * (3.14159265358979f / 180.0f), c = hl_cosf(a), s = hl_sinf(a);

		for (k = 0; k < 3; k++)
		{
			r[k] = c * fx_s.right[k] + s * fx_s.up[k];
			u[k] = c * fx_s.up[k] - s * fx_s.right[k];
		}
		break;
	}
	default:	/* FX_SPR_PARALLEL */
		memcpy(r, fx_s.right, sizeof(fx_s.right));
		memcpy(u, fx_s.up, sizeof(fx_s.up));
	}
}

void sprite_draw(const fx_sprite_t *s, int f, const float org[3], const float ang[3], float scale,
	int gpu_mode, const uint8_t rgba[4])
{
	static const uint8_t idx[6] = { 0, 1, 2, 0, 2, 3 };
	const fx_sprframe_t *fr;
	gpu_fx_vert_t v[4];
	float r[3], u[3], x0, x1, y0, y1;
	int k, i;

	if (!s || s->nframes < 1)
		return;
	if (f < 0)
		f = 0;
	fr = &s->frames[sprite_image(s, f, fx_s.now)];
	if (scale <= 0)
		scale = 1;
	axes(s->type, org, ang, r, u);
	x0 = (float)fr->left * scale;
	x1 = (float)(fr->left + fr->w) * scale;
	y0 = (float)(fr->up - fr->h) * scale;
	y1 = (float)fr->up * scale;
	for (i = 0; i < 4; i++)
	{
		float x = (i == 0 || i == 3) ? x0 : x1, y = i < 2 ? y1 : y0, p[3];

		for (k = 0; k < 3; k++)
			p[k] = org[k] + x * r[k] + y * u[k];
		if (!fx_cam_pos(p, v[i].pos))
			return;
		v[i].st[0] = (int16_t)(((i == 0 || i == 3) ? 0 : fr->tw) * 32);
		v[i].st[1] = (int16_t)((i < 2 ? 0 : fr->th) * 32);
		memcpy(v[i].rgba, rgba, 4);
	}
	gpu_fx_mode(gpu_mode, NULL);
	gpu_fx_tex_ci4(s->tlut, s->base + fr->ci4, fr->tw, fr->th);
	gpu_fx_tris(v, 4, idx, 2);
}
