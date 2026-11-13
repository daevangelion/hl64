/* SPDX-License-Identifier: MIT */
/*
 * rec_fuzz.c -- hostile-input fuzz of the campaign.rtb reader
 * (engine/res/rtb.c) and the HLE1 / HLN1 chunk check (engine/ent/rec_hle.c):
 * built natively with -fsanitize=address,undefined by test/m3/rec.py.
 * The unmodified inputs must pass; every mutant (random byte / u16 / u32
 * writes, section-directory and header fields, truncations) must be
 * accepted or rejected without a sanitizer report. Prints the counts.
 *
 *   hl64-rec-fuzz campaign.rtb ents.bin ITERATIONS SEED
 *       ents.bin: a map's ENTS chunk (HLE1 + HLN1), as packaged
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "engine/ent/rec_int.h"
#include "engine/res/res.h"

static uint32_t rng;

static uint32_t rnd(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return rng;
}

static uint8_t *load(const char *path, uint32_t *n)
{
	FILE *f = fopen(path, "rb");
	long sz;
	uint8_t *b;

	if (!f || fseek(f, 0, SEEK_END) || (sz = ftell(f)) < 0 || fseek(f, 0, SEEK_SET))
		exit(2);
	b = malloc((size_t)sz + 16);
	if (!b || fread(b, 1, (size_t)sz, f) != (size_t)sz)
		exit(2);
	fclose(f);
	*n = (uint32_t)sz;
	return b;
}

static void mutate(uint8_t *b, uint32_t *n, uint32_t hot)
{
	int k, edits = 1 + (int)(rnd() % 4);

	for (k = 0; k < edits && *n; k++)
	{
		uint32_t at = rnd() % 4 ? rnd() % *n : rnd() % (hot < *n ? hot : *n);

		switch (rnd() % 6)
		{
		case 0: b[at] ^= (uint8_t)(1u << (rnd() % 8)); break;
		case 1: b[at] = (uint8_t)rnd(); break;
		case 2: if (at + 2 <= *n) { b[at] = (uint8_t)rnd(); b[at + 1] = 0xFF; } break;
		case 3: if (at + 4 <= *n) memset(b + at, 0xFF, 4); break;
		case 4: if (at + 4 <= *n) { uint32_t v = rnd() % 4096; memcpy(b + at, &v, 4); } break;
		case 5: *n = at; break;
		}
	}
}

/* the rtb reader as res_init runs it, on a buffer of exactly n bytes */
static int rtb_once(const uint8_t *src, uint32_t n)
{
	uint8_t *b = malloc(n ? n : 1);
	rtb_dir_t d;
	uint32_t bad;
	int ok;

	memcpy(b, src, n);
	ok = n >= RTB_DIR_END && !rtb_parse_dir(b, n, &d, &bad) && !rtb_parse_cart(b, &d, &bad)
		&& !rtb_parse_rest(b, &d, &bad);
	free(b);
	return ok;
}

static int hle_once(const uint8_t *src, uint32_t n, const rec_limits_t *lim)
{
	void *b = malloc((size_t)n + 16);	/* 16-aligned (glibc, 64-bit) */
	const void *g;
	uint32_t gs;
	int ok;

	memcpy(b, src, n);
	ok = !rec_hle_check(b, n, lim, &g, &gs);
	/* a passing chunk passes again (idempotent fixup) */
	if (ok && rec_hle_check(b, n, lim, &g, &gs))
	{
		printf("FAIL hle check not idempotent\n");
		exit(1);
	}
	free(b);
	return ok;
}

int main(int argc, char **argv)
{
	uint32_t rn, hn, i, iters;
	uint8_t *rtb, *hle, *buf;
	rec_limits_t lim;
	int racc = 0, hacc = 0, k;

	if (argc != 5)
	{
		fprintf(stderr, "usage: hl64-rec-fuzz campaign.rtb ents.bin ITERATIONS SEED\n");
		return 2;
	}
	rtb = load(argv[1], &rn);
	hle = load(argv[2], &hn);
	iters = (uint32_t)strtoul(argv[3], NULL, 10);
	rng = (uint32_t)strtoul(argv[4], NULL, 10) | 1;
	memset(&lim, 0, sizeof(lim));
	lim.classes = RC__N;
	lim.patterns = lim.globals = lim.titles = 64;
	for (k = 0; k < 8; k++)
		lim.kcount[k] = 512;
	lim.nsub = 1024;
	if (!rtb_once(rtb, rn) || !hle_once(hle, hn, &lim))
	{
		printf("FAIL the unmodified inputs do not pass\n");
		return 1;
	}
	buf = malloc((rn > hn ? rn : hn) + 16);
	for (i = 0; i < iters; i++)
	{
		uint32_t n = rn;

		memcpy(buf, rtb, rn);
		mutate(buf, &n, RTB_DIR_END);
		racc += rtb_once(buf, n);
		n = hn;
		memcpy(buf, hle, hn);
		mutate(buf, &n, 96);
		hacc += hle_once(buf, n, &lim);
	}
	printf("fuzz: %lu rtb mutants (%d accepted), %lu hle mutants (%d accepted)\n",
		(unsigned long)iters, racc, (unsigned long)iters, hacc);
	free(buf);
	free(rtb);
	free(hle);
	return 0;
}
