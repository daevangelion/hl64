/* SPDX-License-Identifier: MIT */
/*
 * draw2d.c -- 2D services for the HUD (include/hl64/draw2d.h,
 * docs/architecture.md 9.8): the sheets of game/defs/hud.def are read
 * once at boot from their cooked ui8 files (gfx/<name>.ui8: u32 'UI8 ',
 * u16 version 1, u16 flags (1 keyed CI8, 2 I8 intensity), u16 w, h, u32
 * pix_ofs, u32 file size, name[48], then w x h bytes) into the HUD region
 * as I4 coverage (keyed: index 255 -> 0, else full; intensity: the top
 * four bits), and FONT lines also read their glyph table (gfx/<name>.fnt:
 * u32 'FNT1', u16 row height, u16 rows, 256 x {u16 offset (y * 256 + x),
 * u16 width}, big-endian). Draw calls go to the platform seam
 * (engine/render/gpu_2d.h): rdpq on the cart, the recorder on the host.
 * Cooked files are untrusted: sizes and offsets are checked; a missing or
 * bad sheet is logged and not drawn.
 */
#include <string.h>

#include "hl64/draw2d.h"
#include "hl64/limits.h"
#include "engine/core/log.h"
#include "engine/core/sys.h"
#include "engine/io/io.h"
#include "engine/mem/carve.h"
#include "engine/render/gpu_2d.h"

enum {
#define HUD(id, file)	D2S_##id,
#define FONT(id, file)	D2S_##id,
#include "game/defs/hud.def"
#undef HUD
#undef FONT
	D2S_COUNT
};

static const char *const sheet_file[D2S_COUNT] = {
#define HUD(id, file)	file,
#define FONT(id, file)	file,
#include "game/defs/hud.def"
#undef HUD
#undef FONT
};

static const uint8_t sheet_font[D2S_COUNT] = {
#define HUD(id, file)	0,
#define FONT(id, file)	1,
#include "game/defs/hud.def"
#undef HUD
#undef FONT
};

#define UI8_MAGIC	0x55493820u
#define FNT_MAGIC	0x464E5431u
#define UI8_KEYED	1
#define UI8_INTENSITY	2
#define CHUNK		16384u

static struct {
	uint16_t	w[D2S_COUNT], h[D2S_COUNT];
	uint8_t		ok[D2S_COUNT];
	int		font;			/* sheet id of the text font, -1 none */
	uint16_t	rowh;
	uint16_t	gofs[256];
	uint8_t		gw[256];
	uint32_t	used;			/* HUD region bytes */
	uint64_t	t0;
	uint32_t	us, us_max, frames, us_sum, draws;
} D;

static uint32_t be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint16_t be16(const uint8_t *p)
{
	return (uint16_t)(p[0] << 8 | p[1]);
}

/* bytes [ofs, ofs + len) of the cart file at rom into scratch (len <=
 * CHUNK); the DMA covers whole 16 B lines */
static const uint8_t *read_at(uint32_t rom, uint32_t ofs, uint32_t len)
{
	uint8_t *s = mem_region(MEM_SCRATCH);
	uint32_t a = ofs & ~15u, n = (ofs - a + len + 15) & ~15u;

	io_read_sync(s, rom + a, n, "hud");
	return s + (ofs - a);
}

static int load_sheet(int id, uint8_t *dst, uint32_t room)
{
	uint32_t size, rom = sys_cart_addr(sheet_file[id], &size), pix, fsize, stride, y0, y, x;
	const uint8_t *h;
	uint16_t flags, w, hh;

	if (!rom || size < 68)
	{
		log_printf("d2: %s: missing\n", sheet_file[id]);
		return -1;
	}
	h = read_at(rom, 0, 68);
	flags = be16(h + 6);
	w = be16(h + 8);
	hh = be16(h + 10);
	pix = be32(h + 12);
	fsize = be32(h + 16);
	if (be32(h) != UI8_MAGIC || be16(h + 4) != 1 || !w || !hh || w > 1024 || hh > 1024 ||
		fsize > size || pix < 68 || pix + (uint32_t)w * hh > fsize)
	{
		log_printf("d2: %s: bad ui8 header\n", sheet_file[id]);
		return -1;
	}
	stride = ((uint32_t)w + 15) / 16 * 8;
	if (stride * hh > room)
	{
		log_printf("d2: %s: %lu B over the HUD region\n", sheet_file[id],
			(unsigned long)(stride * hh));
		return -1;
	}
	memset(dst, 0, stride * hh);
	for (y0 = 0; y0 < hh; )
	{
		uint32_t rows = CHUNK / w;
		const uint8_t *src;

		if (rows > hh - y0)
			rows = hh - y0;
		src = read_at(rom, pix + y0 * w, rows * w);
		for (y = 0; y < rows; y++)
			for (x = 0; x < w; x++)
			{
				uint8_t t = src[y * w + x], v;

				if (flags & UI8_INTENSITY)
					v = t >> 4;
				else
					v = t == 255 ? 0 : 15;
				dst[(y0 + y) * stride + x / 2] |= (uint8_t)((x & 1) ? v : v << 4);
			}
		y0 += rows;
	}
	D.w[id] = w;
	D.h[id] = hh;
	gpu_2d_sheet(id, dst, w, hh, (int)stride);
	return (int)(stride * hh);
}

static void load_font(int id)
{
	char path[64];
	uint32_t size, rom, n = (uint32_t)strlen(sheet_file[id]), i;
	const uint8_t *f;

	if (n < 4 || n + 1 > sizeof(path))
		return;
	memcpy(path, sheet_file[id], n - 4);
	memcpy(path + n - 4, ".fnt", 5);
	rom = sys_cart_addr(path, &size);
	if (!rom || size < 8 + 1024)
	{
		log_printf("d2: %s: missing (no text)\n", path);
		return;
	}
	f = read_at(rom, 0, 8 + 1024);
	if (be32(f) != FNT_MAGIC || !be16(f + 4) || be16(f + 4) > D.h[id])
	{
		log_printf("d2: %s: bad font table\n", path);
		return;
	}
	D.rowh = be16(f + 4);
	for (i = 0; i < 256; i++)
	{
		uint16_t o = be16(f + 8 + 4 * i), w = be16(f + 10 + 4 * i);

		/* the glyph must lie inside the sheet */
		if (w > 32 || (o & 255) + w > D.w[id] || (uint32_t)(o >> 8) + D.rowh > D.h[id])
			w = 0;
		D.gofs[i] = o;
		D.gw[i] = (uint8_t)w;
	}
	D.font = id;
}

void d2_init(void)
{
	uint8_t *base = mem_region(MEM_HUD);
	uint32_t room = mem_region_bytes(MEM_HUD);
	int i, n;

	memset(&D, 0, sizeof(D));
	D.font = -1;
	for (i = 0; i < D2S_COUNT; i++)
	{
		n = load_sheet(i, base + D.used, room - D.used);
		if (n < 0)
			continue;
		D.ok[i] = 1;
		D.used = (D.used + (uint32_t)n + 15) & ~15u;
		if (sheet_font[i])
			load_font(i);
	}
	log_printf("mem: hud %lu of %lu B (%d sheets, font %s)\n", (unsigned long)D.used,
		(unsigned long)room, D2S_COUNT, D.font >= 0 ? "yes" : "no");
}

int d2_sheet_w(int sheet)
{
	return sheet >= 0 && sheet < D2S_COUNT ? D.w[sheet] : 0;
}

int d2_sheet_h(int sheet)
{
	return sheet >= 0 && sheet < D2S_COUNT ? D.h[sheet] : 0;
}

void d2_screen(int *w, int *h)
{
	*w = 320;
	*h = 240;
}

void d2_pic(int sheet, int sx, int sy, int w, int h, int x, int y, uint8_t r, uint8_t g,
	uint8_t b, uint8_t a, int mode)
{
	uint8_t c[4] = { r, g, b, a };

	if (sheet < 0 || sheet >= D2S_COUNT || !D.ok[sheet] || w <= 0 || h <= 0)
		return;
	/* clip the source to the sheet and the destination to the screen */
	if (sx < 0 || sy < 0 || sx + w > D.w[sheet] || sy + h > D.h[sheet])
		return;
	if (x < 0) { sx -= x; w += x; x = 0; }
	if (y < 0) { sy -= y; h += y; y = 0; }
	if (x + w > 320) w = 320 - x;
	if (y + h > 240) h = 240 - y;
	if (w <= 0 || h <= 0)
		return;
	gpu_2d_pic(sheet, sx, sy, w, h, x, y, c, mode);
	D.draws++;
}

void d2_fill(int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a, int mode)
{
	uint8_t c[4] = { r, g, b, a };

	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > 320) w = 320 - x;
	if (y + h > 240) h = 240 - y;
	if (w <= 0 || h <= 0)
		return;
	gpu_2d_fill(x, y, w, h, c, mode);
	D.draws++;
}

int d2_text(int x, int y, const char *s, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
	int x0 = x;

	if (D.font < 0 || !s)
		return 0;
	for (; *s; s++)
	{
		unsigned c = (uint8_t)*s;

		if (!D.gw[c])
			continue;
		d2_pic(D.font, D.gofs[c] & 255, D.gofs[c] >> 8, D.gw[c], D.rowh, x, y, r, g, b, a,
			D2_ADD);
		x += D.gw[c];
	}
	return x - x0;
}

int d2_text_height(void)
{
	return D.rowh;
}

void d2_begin(void)
{
	D.t0 = sys_time_us();
	gpu_2d_begin();
}

void d2_end(void)
{
	uint32_t us;

	gpu_2d_end();
	us = (uint32_t)(sys_time_us() - D.t0);
	D.us_sum += us;
	if (us > D.us_max)
		D.us_max = us;
	D.frames++;
}

void d2_rollup(uint32_t *avg, uint32_t *max, uint32_t *draws, uint32_t *bytes)
{
	*avg = D.frames ? D.us_sum / D.frames : 0;
	*max = D.us_max;
	*draws = D.frames ? D.draws / D.frames : 0;
	*bytes = D.used;
	D.us_sum = D.us_max = D.frames = D.draws = 0;
}
