/* SPDX-License-Identifier: MIT */
/*
 * res.c -- the campaign resource table at run time (docs/architecture.md
 * 6.5, docs/formats/rtb-format.md, docs/specs/precache-and-resources.md).
 *
 * Boot reads campaign.rtb's header, directory and cart part (FILE .. TTXT)
 * into the scratch region, validates every rule of rtb-format.md
 * "Validation" that concerns them (hostile input: any byte string either
 * passes or stops the boot naming the offset and the rule), resolves each
 * cart file once to its PI address, and keeps compact native-order tables
 * in the RESTAB region: files (rom, size), maps, kinds, resource entries,
 * sentences, words, groups, patterns, the res.def line ids, the titles
 * and the interest bits. The path strings are not kept. Host builds also check
 * the whole file (padding, name sections, build_id) and keep it for
 * res_name; cart test builds check build_id.
 *
 * Ids are indices: every accessor is a bounds check and a load, no string
 * compare (R6).
 */
#include <string.h>

#include "hl64/limits.h"
#include "hl64/rec.h"
#include "hl64/studio.h"
#include "engine/core/log.h"
#include "engine/core/sys.h"
#include "engine/io/io.h"
#include "engine/mem/carve.h"
#include "engine/res/res.h"

#define FILE_KIND(k)	((k) >= RES_K_MODEL && (k) <= RES_K_ANIM)
#define KIND_MAX	8192u

typedef struct { uint32_t rom, size; } rfile_t;
typedef struct { uint8_t kind, flags; uint16_t file; uint32_t offset, size; } rent_t;
typedef struct { uint16_t first; uint8_t count, pad; } rsent_t;
typedef struct { uint16_t first, count; } rgrp_t;
typedef struct { uint16_t ofs; uint8_t len, pad; } rpat_t;

static struct {
	int		loaded;
	uint32_t	nfiles, nmaps, nres, nsent, nword, ngrp, npat, nppol, ndefs;
	uint32_t	nclas, nglob, ntitl, nttxt;
	uint16_t	kcount[8], kfirst[8];
	rfile_t		*files;
	uint16_t	*maps;
	rent_t		*res;
	rsent_t		*sent;
	res_word_t	*word;
	rgrp_t		*grp;
	rpat_t		*pat;
	uint8_t		*ppol;
	uint16_t	*defs;
	res_title_t	*titl;
	char		*ttxt;
	uint8_t		*interest;	/* bit per RES_ entry, then sentence, group */
	uint16_t	*sprframes;	/* frames of each SPRITE id (.sc4 header) */
	uint32_t	used;
	res_stats_t	st;
#ifdef HL64_HOST
	const uint8_t	*names;		/* the whole file (host) */
	rtb_dir_t	dir;
#endif
} R;

static inline uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }
static inline uint32_t rd32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* ---- install ------------------------------------------------------------------ */

static void *take(uint32_t n)
{
	uint8_t *base = mem_region(MEM_RESTAB);
	uint32_t at = R.used;

	n = (n + 3) & ~3u;
	if (n > mem_region_bytes(MEM_RESTAB) - at)
		sys_error("campaign.rtb: tables need more than HL64_RESTAB_KB (%u KB)",
			(unsigned)HL64_RESTAB_KB);
	R.used = at + n;
	return base + at;
}

void res_reset(void)
{
	memset(&R, 0, sizeof(R));
}

uint32_t res_install(const uint8_t *b, const rtb_dir_t *d, int resolve)
{
	const uint8_t *strs = b + d->ofs[RTB_STRS];
	uint32_t i, k;

	res_reset();
	R.nfiles = d->count[RTB_FILE];
	R.nmaps = d->count[RTB_MAPS];
	R.nres = d->count[RTB_RES];
	R.nsent = d->count[RTB_SENT];
	R.nword = d->count[RTB_WORD];
	R.ngrp = d->count[RTB_SGRP];
	R.npat = d->count[RTB_PATN];
	R.nppol = d->count[RTB_PPOL];
	R.ndefs = d->count[RTB_DEFS];
	R.nclas = d->count[RTB_CLAS];
	R.nglob = d->count[RTB_GLOB];
	R.ntitl = d->count[RTB_TITL];
	R.nttxt = d->count[RTB_TTXT];
	for (k = 0; k < 8; k++)
	{
		R.kcount[k] = (uint16_t)rd16(b + d->ofs[RTB_KIND] + 4 * k);
		R.kfirst[k] = (uint16_t)rd16(b + d->ofs[RTB_KIND] + 4 * k + 2);
	}
	R.files = take(8 * R.nfiles);
	for (i = 0; i < R.nfiles; i++)
	{
		const uint8_t *f = b + d->ofs[RTB_FILE] + 12 * i;
		const char *path = (const char *)strs + rd32(f);
		uint32_t size = 0, rom = resolve ? sys_cart_addr(path, &size) : 0;

		R.files[i].size = rd32(f + 4);
		if (resolve && (!rom || size != R.files[i].size))
			sys_error("campaign.rtb: cart file %s %s (cook the content again)", path,
				rom ? "has another size" : "missing");
		R.files[i].rom = rom;
	}
	R.maps = take(2 * R.nmaps);
	for (i = 0; i < R.nmaps; i++)
		R.maps[i] = (uint16_t)rd16(b + d->ofs[RTB_MAPS] + 4 * i);
	R.res = take(12 * R.nres);
	for (i = 0; i < R.nres; i++)
	{
		const uint8_t *e = b + d->ofs[RTB_RES] + 12 * i;

		R.res[i].kind = e[0];
		R.res[i].flags = e[1];
		R.res[i].file = (uint16_t)rd16(e + 2);
		R.res[i].offset = rd32(e + 4);
		R.res[i].size = rd32(e + 8);
	}
	R.sent = take(4 * R.nsent);
	for (i = 0; i < R.nsent; i++)
	{
		const uint8_t *s = b + d->ofs[RTB_SENT] + 4 * i;

		R.sent[i].first = (uint16_t)rd16(s);
		R.sent[i].count = s[2];
		R.sent[i].pad = 0;
	}
	R.word = take(8 * R.nword);
	for (i = 0; i < R.nword; i++)
	{
		const uint8_t *w = b + d->ofs[RTB_WORD] + 8 * i;

		R.word[i].res = (res_t)rd16(w);
		R.word[i].pitch = w[2];
		R.word[i].volume = w[3];
		R.word[i].start = w[4];
		R.word[i].end = w[5];
		R.word[i].time = w[6];
		R.word[i].pad = 0;
	}
	R.grp = take(4 * R.ngrp);
	for (i = 0; i < R.ngrp; i++)
	{
		R.grp[i].first = (uint16_t)rd16(b + d->ofs[RTB_SGRP] + 4 * i);
		R.grp[i].count = (uint16_t)rd16(b + d->ofs[RTB_SGRP] + 4 * i + 2);
	}
	R.pat = take(4 * R.npat);
	for (i = 0; i < R.npat; i++)
	{
		R.pat[i].ofs = (uint16_t)rd16(b + d->ofs[RTB_PATN] + 4 * i);
		R.pat[i].len = b[d->ofs[RTB_PATN] + 4 * i + 2];
		R.pat[i].pad = 0;
	}
	R.ppol = take(R.nppol);
	memcpy(R.ppol, b + d->ofs[RTB_PPOL], R.nppol);
	R.defs = take(2 * R.ndefs);
	for (i = 0; i < R.ndefs; i++)
		R.defs[i] = (uint16_t)rd16(b + d->ofs[RTB_DEFS] + 2 * i);
	R.titl = take(40 * R.ntitl);
	for (i = 0; i < R.ntitl; i++)
	{
		const uint8_t *t = b + d->ofs[RTB_TITL] + 40 * i;
		union { uint32_t u; float f; } v;
		float *f = &R.titl[i].x;

		for (k = 0; k < 6; k++)
		{
			v.u = rd32(t + 4 * k);
			f[k] = v.f;
		}
		R.titl[i].text = rd32(t + 24);
		R.titl[i].text_len = (uint16_t)rd16(t + 28);
		R.titl[i].effect = t[30];
		R.titl[i].pad = 0;
		memcpy(R.titl[i].color1, t + 32, 4);
		memcpy(R.titl[i].color2, t + 36, 4);
	}
	R.ttxt = take(R.nttxt);
	memcpy(R.ttxt, b + d->ofs[RTB_TTXT], R.nttxt);
	R.interest = take((R.nres + R.nsent + R.ngrp + 7) / 8);
	memset(R.interest, 0, (R.nres + R.nsent + R.ngrp + 7) / 8);
	R.sprframes = take(2u * R.kcount[RES_K_SPRITE]);
	for (i = 0; i < R.kcount[RES_K_SPRITE]; i++)
	{
		const rent_t *e = &R.res[R.kfirst[RES_K_SPRITE] + i];
		uint8_t h[16] __attribute__((aligned(16)));

		R.sprframes[i] = 1;
		if (!resolve)
			continue;
		if (e->size < 8)
			sys_error("campaign.rtb: sprite %lu: file shorter than its header",
				(unsigned long)i);
		io_read_sync(h, R.files[e->file].rom + e->offset, 8);
		if (rd32(h) != 0x53433431u || !rd16(h + 6))
			sys_error("campaign.rtb: sprite %lu: not an SC41 sprite", (unsigned long)i);
		R.sprframes[i] = (uint16_t)rd16(h + 6);
	}
	R.loaded = 1;
	R.st.file_bytes = d->file_size;
	R.st.cart_bytes = d->cart_end;
	R.st.table_bytes = R.used;
	R.st.region_bytes = mem_region_bytes(MEM_RESTAB);
	R.st.files = R.nfiles;
	R.st.res = R.nres;
	return R.used;
}

void res_stats(res_stats_t *s)
{
	*s = R.st;
}

#ifdef HL64_HOST
/* host: the whole file, for the full check and res_name (never the cart) */
static uint8_t host_file[1u << 20] __attribute__((aligned(16)));
#endif

void res_init(void)
{
	uint8_t *buf = mem_region(MEM_SCRATCH);
	uint32_t size, rom = sys_cart_addr("campaign.rtb", &size), bad = 0;
	const char *why;
	rtb_dir_t d;

	res_reset();
	if (!rom)
	{
		log_printf("res: no campaign.rtb\n");
		return;
	}
	if (size < RTB_DIR_END)
		sys_error("campaign.rtb: offset 0: file shorter than its header and directory");
	io_read_sync(buf, rom, (RTB_DIR_END + 1) & ~1u);
	if ((why = rtb_parse_dir(buf, size, &d, &bad)))
		sys_error("campaign.rtb: offset %lu: %s", (unsigned long)bad, why);
	if (d.cart_end > mem_region_bytes(MEM_SCRATCH))
		sys_error("campaign.rtb: cart part %lu B over the scratch region",
			(unsigned long)d.cart_end);
	io_read_sync(buf, rom, (d.cart_end + 1) & ~1u);
	if ((why = rtb_parse_cart(buf, &d, &bad)))
		sys_error("campaign.rtb: offset %lu: %s", (unsigned long)bad, why);
#ifdef HL64_HOST
	if (size > sizeof(host_file))
		sys_error("campaign.rtb: %lu B over the host check buffer", (unsigned long)size);
	io_read_sync(host_file, rom, (size + 1) & ~1u);
	if ((why = rtb_parse_rest(host_file, &d, &bad)))
		sys_error("campaign.rtb: offset %lu: %s", (unsigned long)bad, why);
#elif defined(HL64_TEST)
	{
		/* cart test builds: build_id over the file, streamed */
		uint32_t crc = rtb_crc32(0, buf + 32, d.cart_end - 32), at = d.cart_end;
		uint32_t piece = mem_region_bytes(MEM_SCRATCH) / 2;

		res_install(buf, &d, 1);
		while (at < size)
		{
			uint32_t n = size - at < piece ? size - at : piece;

			io_read_sync(buf, rom + at, (n + 1) & ~1u);
			crc = rtb_crc32(crc, buf, n);
			at += n;
		}
		if (crc != d.build_id)
			sys_error("campaign.rtb: offset 8: build_id");
	}
#endif
#ifndef HL64_TEST
	res_install(buf, &d, 1);
#elif defined(HL64_HOST)
	res_install(buf, &d, 1);
	R.dir = d;
	R.names = host_file;
#endif
	log_printf("res: campaign.rtb %lu B (cart part %lu B), %lu files, %u models %u sprites "
		"%u sounds %u anims %lu sentences %lu groups %lu patterns, tables %lu B of %lu\n",
		(unsigned long)size, (unsigned long)d.cart_end, (unsigned long)R.nfiles,
		R.kcount[RES_K_MODEL], R.kcount[RES_K_SPRITE], R.kcount[RES_K_SOUND],
		R.kcount[RES_K_ANIM], (unsigned long)R.nsent, (unsigned long)R.ngrp,
		(unsigned long)R.npat, (unsigned long)R.used,
		(unsigned long)mem_region_bytes(MEM_RESTAB));
}

/* ---- ids ------------------------------------------------------------------------ */

/* RES_ index of a file-kind id, -1 bad */
static int entry(res_t r)
{
	uint32_t k = RES_KIND(r), i = RES_INDEX(r);

	if (!R.loaded || !FILE_KIND(k) || i >= R.kcount[k])
		return -1;
	return (int)(R.kfirst[k] + i);
}

int res_count(int kind)
{
	return R.loaded && kind >= 0 && kind < 8 ? R.kcount[kind] : 0;
}

int res_file(res_t r, int *file, uint32_t *ofs, uint32_t *size)
{
	int i = entry(r);

	if (i < 0)
		return 0;
	*file = R.res[i].file;
	*ofs = R.res[i].offset;
	*size = R.res[i].size;
	return 1;
}

uint32_t res_file_rom(int file, uint32_t *size)
{
	if (!R.loaded || file < 0 || (uint32_t)file >= R.nfiles)
		return 0;
	if (size)
		*size = R.files[file].size;
	return R.files[file].rom;
}

uint8_t res_flags(res_t r)
{
	int i = entry(r);

	return i < 0 ? 0 : R.res[i].flags;
}

res_t res_def(int line)
{
	return R.loaded && line >= 0 && (uint32_t)line < R.ndefs ? R.defs[line] : RES_NONE;
}

int res_model_frames(res_t r)
{
	int i = entry(r);

	if (i < 0)
		return 1;
	if (R.res[i].kind == RES_K_SPRITE)
		return R.sprframes[RES_INDEX(r)];
	if (R.res[i].kind == RES_K_MODEL)
		return studio_body_count(r);
	return 1;
}

void res_mem_line(void)
{
	log_printf("mem res: file=%lu cart=%lu tables=%lu region=%lu files=%lu res=%lu titles=%lu\n",
		(unsigned long)R.st.file_bytes, (unsigned long)R.st.cart_bytes,
		(unsigned long)R.used, (unsigned long)mem_region_bytes(MEM_RESTAB),
		(unsigned long)R.nfiles, (unsigned long)R.nres, (unsigned long)R.ntitl);
}

static void interest_set(uint32_t bit)
{
	R.interest[bit >> 3] |= (uint8_t)(1u << (bit & 7));
}

static int interest_bit(res_t r)
{
	uint32_t k = RES_KIND(r), i = RES_INDEX(r);
	int e = entry(r);

	if (e >= 0)
		return e;
	if (!R.loaded)
		return -1;
	if (k == RES_K_SENTENCE && i < R.nsent)
		return (int)(R.nres + i);
	if (k == RES_K_GROUP && i < R.ngrp)
		return (int)(R.nres + R.nsent + i);
	return -1;
}

void res_precache(res_t r)
{
	int b = interest_bit(r);
	uint32_t i, j;

	if (b < 0)
		return;
	interest_set((uint32_t)b);
	if (RES_KIND(r) == RES_K_SENTENCE || RES_KIND(r) == RES_K_GROUP)
	{
		uint32_t s0 = RES_INDEX(r), s1 = s0 + 1;

		if (RES_KIND(r) == RES_K_GROUP)
		{
			s0 = R.grp[RES_INDEX(r)].first;
			s1 = s0 + R.grp[RES_INDEX(r)].count;
		}
		for (i = s0; i < s1; i++)
		{
			interest_set(R.nres + i);
			for (j = R.sent[i].first; j < (uint32_t)R.sent[i].first + R.sent[i].count; j++)
				interest_set((uint32_t)entry(R.word[j].res));
		}
	}
}

int res_interest(res_t r)
{
	int b = interest_bit(r);

	return b >= 0 && (R.interest[b >> 3] >> (b & 7) & 1);
}

int res_sentence_words(uint16_t sentence, int *first)
{
	if (!R.loaded || sentence >= R.nsent)
		return 0;
	*first = R.sent[sentence].first;
	return R.sent[sentence].count;
}

const res_word_t *res_word(int i)
{
	return R.loaded && i >= 0 && (uint32_t)i < R.nword ? &R.word[i] : NULL;
}

int res_group(uint16_t group, uint16_t *first_sentence)
{
	if (!R.loaded || group >= R.ngrp)
		return 0;
	*first_sentence = R.grp[group].first;
	return R.grp[group].count;
}

const uint8_t *res_pattern(uint16_t pattern, int *len)
{
	if (!R.loaded || pattern >= R.npat)
		return NULL;
	*len = R.pat[pattern].len;
	return R.ppol + R.pat[pattern].ofs;
}

int res_patterns(void)
{
	return (int)R.npat;
}

int res_globals(void)
{
	return (int)R.nglob;
}

int res_classes(void)
{
	return (int)R.nclas;
}

const res_title_t *res_title(uint16_t id)
{
	return R.loaded && id < R.ntitl ? &R.titl[id] : NULL;
}

const char *res_title_text(uint16_t id)
{
	return R.loaded && id < R.ntitl ? R.ttxt + R.titl[id].text : NULL;
}

int res_titles(void)
{
	return (int)R.ntitl;
}

const char *res_name(res_t r)
{
#ifdef HL64_HOST
	const rtb_dir_t *d = &R.dir;
	uint32_t k = RES_KIND(r), i = RES_INDEX(r), o;
	int sect = -1, e = entry(r);

	if (!R.names)
		return "";
	if (e >= 0)
	{
		sect = RTB_NRES;
		i = (uint32_t)e;
	}
	else if (k == RES_K_SENTENCE && i < R.nsent)
		sect = RTB_NSEN;
	else if (k == RES_K_GROUP && i < R.ngrp)
		sect = RTB_NSGR;
	if (sect < 0)
		return "";
	o = rd32(R.names + d->ofs[sect] + 4 * i);
	return (const char *)R.names + d->ofs[RTB_NSTR] + o;
#else
	(void)r;
	return "";
#endif
}
