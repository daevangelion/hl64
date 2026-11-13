/* SPDX-License-Identifier: MIT */
/*
 * rtb.c -- the campaign.rtb reader's checks (docs/formats/rtb-format.md
 * "Validation"): pure functions over bytes, no engine state, so the host
 * fuzz test (test/m3/rec_fuzz.c) runs them on any input.
 */
#include <string.h>

#include "hl64/types.h"
#include "engine/res/res.h"

#define FILE_KIND(k)	((k) >= RES_K_MODEL && (k) <= RES_K_ANIM)
#define KIND_MAX	8192u

/* element sizes in file order (1 = byte pool, 0 = count only) */
static const uint8_t elt[RTB_NSECT] = {
	12, 1, 4, 4, 12, 4, 8, 4, 4, 1, 2, 0, 0, 40, 1, 8, 2, 4, 4, 4, 4, 4, 4, 4, 4, 1
};
static const char fcc[RTB_NSECT][5] = {
	"FILE", "STRS", "MAPS", "KIND", "RES_", "SENT", "WORD", "SGRP", "PATN",
	"PPOL", "DEFS", "CLAS", "GLOB", "TITL", "TTXT", "TARG", "TLMP", "NMAP",
	"NRES", "NSEN", "NSGR", "NCLS", "NTGT", "NGLB", "NTTL", "NSTR"
};

static inline uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }
static inline uint32_t rd32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

uint32_t rtb_crc32(uint32_t crc, const uint8_t *p, uint32_t n)
{
	int k;

	crc = ~crc;
	while (n--)
	{
		crc ^= *p++;
		for (k = 0; k < 8; k++)
			crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
	}
	return ~crc;
}

#define FAIL(o, why)	do { *bad = (o); return (why); } while (0)

const char *rtb_parse_dir(const uint8_t *b, uint32_t file_size, rtb_dir_t *d, uint32_t *bad)
{
	uint32_t i, at;

	memset(d, 0, sizeof(*d));
	if (file_size < RTB_DIR_END)
		FAIL(0, "file shorter than its header and directory");
	if (rd32(b) != RTB_MAGIC)
		FAIL(0, "magic");
	if (rd16(b + 4) != 1)
		FAIL(4, "version");
	if (rd16(b + 6))
		FAIL(6, "flags");
	if (rd32(b + 12) != file_size)
		FAIL(12, "file_size differs from the bytes present");
	if (rd16(b + 20) != RTB_NSECT)
		FAIL(20, "nsect");
	if (rd16(b + 22) || rd32(b + 28))
		FAIL(22, "reserved");
	if (rd32(b + 24) != 32)
		FAIL(24, "dir_ofs");
	d->file_size = file_size;
	d->build_id = rd32(b + 8);
	at = (RTB_DIR_END + 15) & ~15u;
	for (i = 0; i < RTB_NSECT; i++)
	{
		const uint8_t *e = b + 32 + 16 * i;
		uint32_t o = rd32(e + 4), s = rd32(e + 8), n = rd32(e + 12), want;

		if (memcmp(e, fcc[i], 4))
			FAIL(32 + 16 * i, "section order");
		if (o != at)
			FAIL(32 + 16 * i + 4, "section not contiguous / 16-aligned");
		want = elt[i] ? n * elt[i] : 0;
		if ((elt[i] && n > 0xFFFFFFFFu / elt[i]) || s != want)
			FAIL(32 + 16 * i + 8, "section size is not count x element size");
		if (s > file_size || o > file_size - s)
			FAIL(32 + 16 * i + 4, "section outside the file");
		d->ofs[i] = o;
		d->size[i] = s;
		d->count[i] = n;
		at = (o + s + 15) & ~15u;
	}
	if (at != file_size)
		FAIL(12, "bytes after the last section");
	d->cart_end = d->ofs[RTB_TARG];
	return NULL;
}

/* zero padding in [from, to) */
static int zeros(const uint8_t *b, uint32_t from, uint32_t to)
{
	while (from < to)
		if (b[from++])
			return 0;
	return 1;
}

static int path_ok(const uint8_t *s, uint32_t n)
{
	uint32_t i, seg = 0;

	if (!n || s[0] == '/' || s[0] == '\\')
		return 0;
	for (i = 0; i <= n; i++)
	{
		if (i == n || s[i] == '/')
		{
			if (i - seg == 2 && s[seg] == '.' && s[seg + 1] == '.')
				return 0;
			seg = i + 1;
		}
		else if (s[i] == '\\' || s[i] == ':')
			return 0;
	}
	return 1;
}

const char *rtb_parse_cart(const uint8_t *b, const rtb_dir_t *d, uint32_t *bad)
{
	const uint8_t *strs = b + d->ofs[RTB_STRS], *kind = b + d->ofs[RTB_KIND];
	uint32_t i, k, sum, nfiles = d->count[RTB_FILE], nres = d->count[RTB_RES];
	uint32_t kc[8], kf[8];

	for (i = 0; i < RTB_CART; i++)
	{
		uint32_t from = i ? d->ofs[i - 1] + d->size[i - 1] : RTB_DIR_END;

		if (!zeros(b, from, d->ofs[i]))
			FAIL(from, "nonzero padding");
	}
	/* FILE / STRS */
	if (d->size[RTB_STRS] ? strs[d->size[RTB_STRS] - 1] != 0 : nfiles != 0)
		FAIL(d->ofs[RTB_STRS], "STRS does not end with NUL");
	for (i = 0; i < nfiles; i++)
	{
		const uint8_t *f = b + d->ofs[RTB_FILE] + 12 * i;
		uint32_t p = rd32(f);

		if (p >= d->size[RTB_STRS] || (p && strs[p - 1]))
			FAIL(d->ofs[RTB_FILE] + 12 * i, "file path offset");
		if (!path_ok(strs + p, (uint32_t)strlen((const char *)strs + p)))
			FAIL(d->ofs[RTB_FILE] + 12 * i, "file path not relative / has ..");
	}
	for (i = 0; i < d->count[RTB_MAPS]; i++)
		if (rd16(b + d->ofs[RTB_MAPS] + 4 * i) >= nfiles)
			FAIL(d->ofs[RTB_MAPS] + 4 * i, "MAPS.file");
	/* KIND */
	if (d->count[RTB_KIND] != 8)
		FAIL(d->ofs[RTB_KIND], "KIND count");
	for (k = 0, sum = 0; k < 8; k++)
	{
		kc[k] = rd16(kind + 4 * k);
		kf[k] = rd16(kind + 4 * k + 2);
		if (kc[k] > KIND_MAX)
			FAIL(d->ofs[RTB_KIND] + 4 * k, "kind count over 8192");
		if (FILE_KIND(k))
		{
			if (kf[k] != sum)
				FAIL(d->ofs[RTB_KIND] + 4 * k + 2, "kind first is not the running sum");
			sum += kc[k];
		}
		else if (kf[k] || ((k == 0 || k == RES_K_SUBMODEL) && kc[k]))
			FAIL(d->ofs[RTB_KIND] + 4 * k, "non-file kind first / count");
	}
	if (sum != nres)
		FAIL(d->ofs[RTB_KIND], "file-kind counts differ from RES_");
	if (kc[RES_K_SENTENCE] != d->count[RTB_SENT] || kc[RES_K_GROUP] != d->count[RTB_SGRP])
		FAIL(d->ofs[RTB_KIND], "SENTENCE / GROUP counts");
	/* RES_ */
	for (i = 0; i < nres; i++)
	{
		const uint8_t *e = b + d->ofs[RTB_RES] + 12 * i;
		uint32_t rk = e[0], fl = e[1], file = rd16(e + 2), o = rd32(e + 4), s = rd32(e + 8);
		uint32_t comp = (fl & 1) + (fl >> 4), fsize;

		if (!FILE_KIND(rk) || i < kf[rk] || i >= kf[rk] + kc[rk])
			FAIL(d->ofs[RTB_RES] + 12 * i, "RES_ entry outside its kind's range");
		if ((fl && rk != RES_K_MODEL) || (fl & 0x0E))
			FAIL(d->ofs[RTB_RES] + 12 * i + 1, "RES_ flags");
		if (file + comp >= nfiles)
			FAIL(d->ofs[RTB_RES] + 12 * i + 2, "RES_ file / companions");
		fsize = rd32(b + d->ofs[RTB_FILE] + 12 * file + 4);
		if (s > fsize || o > fsize - s)
			FAIL(d->ofs[RTB_RES] + 12 * i + 4, "RES_ range outside its file");
	}
	/* WORD / SENT / SGRP */
	for (i = 0; i < d->count[RTB_WORD]; i++)
	{
		const uint8_t *w = b + d->ofs[RTB_WORD] + 8 * i;
		uint32_t r = rd16(w);

		if (r >> 13 != RES_K_SOUND || (r & 0x1FFF) >= kc[RES_K_SOUND])
			FAIL(d->ofs[RTB_WORD] + 8 * i, "WORD.res is not a SOUND id");
		if (!w[2] || w[4] > 100 || w[5] > 100 || w[6] > 100)
			FAIL(d->ofs[RTB_WORD] + 8 * i + 2, "WORD modifiers");
	}
	for (i = 0; i < d->count[RTB_SENT]; i++)
	{
		const uint8_t *s = b + d->ofs[RTB_SENT] + 4 * i;

		if (!s[2] || rd16(s) + s[2] > d->count[RTB_WORD])
			FAIL(d->ofs[RTB_SENT] + 4 * i, "SENT word range");
	}
	for (i = 0, sum = 0; i < d->count[RTB_SGRP]; i++)
	{
		const uint8_t *g = b + d->ofs[RTB_SGRP] + 4 * i;

		if (rd16(g) != sum || !rd16(g + 2))
			FAIL(d->ofs[RTB_SGRP] + 4 * i, "SGRP does not tile SENT");
		sum += rd16(g + 2);
	}
	if (sum != d->count[RTB_SENT])
		FAIL(d->ofs[RTB_SGRP], "SGRP does not tile SENT");
	/* PATN / PPOL */
	for (i = 0; i < d->count[RTB_PPOL]; i++)
		if (b[d->ofs[RTB_PPOL] + i] > 25)
			FAIL(d->ofs[RTB_PPOL] + i, "PPOL letter");
	for (i = 0; i < d->count[RTB_PATN]; i++)
	{
		const uint8_t *p = b + d->ofs[RTB_PATN] + 4 * i;

		if (!p[2] || rd16(p) + p[2] > d->count[RTB_PPOL])
			FAIL(d->ofs[RTB_PATN] + 4 * i, "PATN range");
	}
	/* DEFS */
	for (i = 0; i < d->count[RTB_DEFS]; i++)
	{
		uint32_t r = rd16(b + d->ofs[RTB_DEFS] + 2 * i), rk = r >> 13;

		if (rk == 0 || rk == RES_K_ANIM || rk == RES_K_SUBMODEL
			|| (r & 0x1FFF) >= kc[rk])
			FAIL(d->ofs[RTB_DEFS] + 2 * i, "DEFS id");
	}
	if (d->count[RTB_GLOB] > 0xFFFF || d->count[RTB_CLAS] > 0xFFFF)
		FAIL(d->ofs[RTB_CLAS], "CLAS / GLOB count");
	if (!zeros(b, d->ofs[RTB_TTXT] + d->size[RTB_TTXT], d->cart_end))
		FAIL(d->ofs[RTB_TTXT] + d->size[RTB_TTXT], "nonzero padding");
	/* TITL / TTXT */
	if (d->count[RTB_TITL] > 0xFFFF)
		FAIL(d->ofs[RTB_TITL], "TITL count");
	if (d->size[RTB_TTXT] && b[d->ofs[RTB_TTXT] + d->size[RTB_TTXT] - 1])
		FAIL(d->ofs[RTB_TTXT], "TTXT does not end with NUL");
	for (i = 0; i < d->count[RTB_TITL]; i++)
	{
		const uint8_t *t = b + d->ofs[RTB_TITL] + 40 * i, *txt = b + d->ofs[RTB_TTXT];
		uint32_t to = rd32(t + 24), tl = rd16(t + 28), j;

		if (t[30] > 2 || t[31] || tl > RTB_TITLE_TEXT_MAX || to >= d->size[RTB_TTXT]
			|| tl >= d->size[RTB_TTXT] - to || txt[to + tl])
			FAIL(d->ofs[RTB_TITL] + 40 * i, "TITL text range / effect");
		for (j = 0; j < tl; j++)
			if (!txt[to + j])
				FAIL(d->ofs[RTB_TITL] + 40 * i, "TITL text holds NUL");
		for (j = 0; j < 6; j++)
		{
			uint32_t e = rd32(t + 4 * j) >> 23 & 0xFF;

			if (e >= 127 + 20)	/* NaN / inf / |v| >= 2^20 */
				FAIL(d->ofs[RTB_TITL] + 40 * i + 4 * j, "TITL value");
		}
	}
	return NULL;
}

/* a name section: count = id space, offsets into NSTR */
static const char *names_ok(const uint8_t *b, const rtb_dir_t *d, int sect, uint32_t want,
	uint32_t *bad)
{
	uint32_t i, n = d->size[RTB_NSTR];
	const uint8_t *pool = b + d->ofs[RTB_NSTR];

	if (d->count[sect] != want)
		FAIL(d->ofs[sect], "name section count differs from its id space");
	for (i = 0; i < want; i++)
	{
		uint32_t o = rd32(b + d->ofs[sect] + 4 * i);

		if (o >= n || (o && pool[o - 1]))
			FAIL(d->ofs[sect] + 4 * i, "name offset");
	}
	return NULL;
}

const char *rtb_parse_rest(const uint8_t *b, const rtb_dir_t *d, uint32_t *bad)
{
	const char *why;
	uint32_t i, maps = d->count[RTB_MAPS], last = 0;

	for (i = RTB_CART; i < RTB_NSECT; i++)
		if (!zeros(b, d->ofs[i - 1] + d->size[i - 1], d->ofs[i]))
			FAIL(d->ofs[i - 1] + d->size[i - 1], "nonzero padding");
	if (!zeros(b, d->ofs[RTB_NSTR] + d->size[RTB_NSTR], d->file_size))
		FAIL(d->ofs[RTB_NSTR] + d->size[RTB_NSTR], "nonzero padding");
	if (rtb_crc32(0, b + 32, d->file_size - 32) != d->build_id)
		FAIL(8, "build_id");
	for (i = 0; i < d->count[RTB_TARG]; i++)
	{
		const uint8_t *t = b + d->ofs[RTB_TARG] + 8 * i;
		uint32_t m = rd16(t), f = rd16(t + 2), n = rd16(t + 4), sp = rd16(t + 6);

		if (m >= maps || m < last || !n || f + n > d->count[RTB_TLMP] || sp > n)
			FAIL(d->ofs[RTB_TARG] + 8 * i, "TARG entry");
		last = m;
	}
	if (d->size[RTB_NSTR] && b[d->ofs[RTB_NSTR] + d->size[RTB_NSTR] - 1])
		FAIL(d->ofs[RTB_NSTR], "NSTR does not end with NUL");
	if ((why = names_ok(b, d, RTB_NMAP, maps, bad))
		|| (why = names_ok(b, d, RTB_NRES, d->count[RTB_RES], bad))
		|| (why = names_ok(b, d, RTB_NSEN, d->count[RTB_SENT], bad))
		|| (why = names_ok(b, d, RTB_NSGR, d->count[RTB_SGRP], bad))
		|| (why = names_ok(b, d, RTB_NCLS, d->count[RTB_CLAS], bad))
		|| (why = names_ok(b, d, RTB_NTGT, d->count[RTB_TARG], bad))
		|| (why = names_ok(b, d, RTB_NGLB, d->count[RTB_GLOB], bad))
		|| (why = names_ok(b, d, RTB_NTTL, d->count[RTB_TITL], bad)))
		return why;
	return NULL;
}

