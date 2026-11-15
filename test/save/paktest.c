/* SPDX-License-Identifier: MIT */
/*
 * paktest.c -- host driver of the Controller Pak driver (plat/pakfs.c over
 * plat/host/pak_host.c), run by test/save/paktest.sh:
 *
 *   hl64-paktest roundtrip IMG          absent / rumble / Pak; create, write
 *                                       (header last, unaligned pieces),
 *                                       close, read, no-room, delete, remount
 *   hl64-paktest put IMG ID LEN SEED    create note ID of LEN bytes, pattern SEED
 *   hl64-paktest get IMG ID LEN SEED    note ID is LEN bytes of pattern SEED
 *   hl64-paktest del IMG ID             delete note ID
 *   hl64-paktest same BEFORE AFTER      every note of BEFORE that is not ours
 *                                       (entry, FAT chain, pages) and the ID
 *                                       page are byte-identical in AFTER
 *   hl64-paktest cut IMG OUTDIR         power cut after every 32-byte block of
 *                                       a create / write / close / create /
 *                                       delete sequence on a copy of IMG:
 *                                       each cut image mounts, foreign notes
 *                                       are untouched, our notes read back;
 *                                       cut images to OUTDIR/cut-<k>.pak
 *
 * Prints "paktest: <what> ok" lines; exit 0 all good, 1 a check failed.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "engine/core/log.h"
#include "engine/core/sys.h"
#include "plat/host/host.h"
#include "plat/pakfs.h"

void sys_log_write(const char *s, uint32_t n)
{
	fwrite(s, 1, n, stderr);
}

static int fails;

#define CHECK(c) do { if (!(c)) { printf("paktest: FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

/* power cuts: transfers stop after cut_left blocks (the cut transfer
 * writes its first blocks only) */
int __real_pakfs_dev_write(uint32_t addr, const void *src, uint32_t len);
static int cut_armed, cut_dead;
static uint32_t cut_left, blocks_written;

int __wrap_pakfs_dev_write(uint32_t addr, const void *src, uint32_t len)
{
	uint32_t first = addr / 32, end = (addr + len + 31) / 32, n = end - first, keep;

	if (cut_dead)
		return -1;
	if (!cut_armed || cut_left >= n)
	{
		cut_left -= cut_armed ? n : 0;
		blocks_written += n;
		return __real_pakfs_dev_write(addr, src, len);
	}
	keep = (first + cut_left) * 32 > addr ? (first + cut_left) * 32 - addr : 0;
	if (keep)
		__real_pakfs_dev_write(addr, src, keep);
	cut_dead = 1;
	return -1;
}

static uint8_t pat(uint32_t i, unsigned seed)
{
	return (uint8_t)(i * 131u + seed * 7u + (i >> 8));
}

static void fill(uint8_t *b, uint32_t len, unsigned seed)
{
	uint32_t i;

	for (i = 0; i < len; i++)
		b[i] = pat(i, seed);
}

static int load(const char *path, uint8_t img[PAKFS_BYTES])
{
	FILE *f = fopen(path, "rb");
	int ok = f && fread(img, 1, PAKFS_BYTES, f) == PAKFS_BYTES && fgetc(f) == EOF;

	if (f)
		fclose(f);
	return ok ? 0 : -1;
}

static int save_img(const char *path, const uint8_t img[PAKFS_BYTES])
{
	FILE *f = fopen(path, "wb");
	int ok = f && fwrite(img, 1, PAKFS_BYTES, f) == PAKFS_BYTES;

	if (f && fclose(f))
		ok = 0;
	return ok ? 0 : -1;
}

static int has_note(int id, uint32_t *size)
{
	uint8_t ids[PAKFS_NOTES];
	uint32_t bytes[PAKFS_NOTES];
	int n = sys_pak_notes(ids, bytes, PAKFS_NOTES), i;

	for (i = 0; i < n; i++)
		if (ids[i] == id)
		{
			if (size)
				*size = bytes[i];
			return 1;
		}
	return 0;
}

/* Write [0, len) of pattern seed in pieces of 512, 37, 256 and 300 bytes,
 * the first 64 bytes (the header) last. */
static int put_pieces(uint32_t len, unsigned seed)
{
	static const uint32_t piece[4] = { 512, 37, 256, 300 };
	static uint8_t b[PAKFS_BYTES];
	uint32_t off = len < 64 ? len : 64, k = 0;

	fill(b, len, seed);
	while (off < len)
	{
		uint32_t n = piece[k++ & 3];

		if (n > len - off)
			n = len - off;
		if (sys_pak_write(b + off, off, n))
			return -1;
		off += n;
	}
	return sys_pak_write(b, 0, len < 64 ? len : 64);
}

static int get_check(int id, uint32_t len, unsigned seed)
{
	static uint8_t b[PAKFS_BYTES], want[PAKFS_BYTES];
	uint32_t size = 0;

	if (!has_note(id, &size) || size != len || sys_pak_read(id, b, 0, len))
		return -1;
	fill(want, len, seed);
	return memcmp(b, want, len) ? -1 : 0;
}

static int file_eq(const char *path, const uint8_t *img)
{
	static uint8_t now[PAKFS_BYTES];

	return !load(path, now) && !memcmp(now, img, PAKFS_BYTES);
}

static int roundtrip(const char *path)
{
	static uint8_t b[PAKFS_BYTES], before[PAKFS_BYTES];
	uint32_t free0, free1, size;
	int notes0, notes1;

	/* nothing in the controller, then a Rumble Pak */
	CHECK(sys_pak_accessory() == SYS_PAK_ABSENT);
	CHECK(sys_pak_mount() == -1);
	host_pak_set("rumble");
	CHECK(sys_pak_accessory() == SYS_PAK_RUMBLE);
	CHECK(sys_pak_mount() == -1);
	CHECK(sys_pak_stat(&free0, &notes0) == -1 && sys_pak_notes(NULL, NULL, 16) == -1);
	printf("paktest: absent / rumble ok\n");

	host_pak_set(path);
	CHECK(sys_pak_accessory() == SYS_PAK_CPAK);
	CHECK(sys_pak_mount() == 0);
	CHECK(sys_pak_stat(&free0, &notes0) == 0);
	CHECK(!has_note(0, NULL) && !has_note(15, NULL));
	printf("paktest: mounted, %u bytes free, %d notes free\n", free0, notes0);

	CHECK(sys_pak_create(0, 5000) == 0);
	CHECK(sys_pak_stat(&free1, &notes1) == 0 && free1 == free0 - 20 * PAKFS_PAGE && notes1 == notes0 - 1);
	CHECK(sys_pak_create(1, 100) == -1);			/* one write at a time */
	CHECK(put_pieces(5000, 1) == 0);
	CHECK(sys_pak_write(b, 4990, 11) == -1);		/* past the end */
	CHECK(sys_pak_close() == 0);
	CHECK(sys_pak_close() == -1);
	CHECK(sys_pak_write(b, 0, 1) == -1);			/* closed */
	CHECK(get_check(0, 5000, 1) == 0);
	fill(b + 2000, 5000, 1);
	CHECK(sys_pak_read(0, b, 777, 1234) == 0 && !memcmp(b, b + 2000 + 777, 1234));
	CHECK(sys_pak_read(0, b, 4999, 2) == -1 && sys_pak_read(9, b, 0, 1) == -1);
	CHECK(sys_pak_create(0, 10) == -1);			/* exists */
	printf("paktest: create / write / close / read ok\n");

	CHECK(sys_pak_create(15, 300) == 0 && put_pieces(300, 15) == 0 && sys_pak_close() == 0);
	{
		uint8_t ids[4] = { 0 };
		uint32_t bytes[4] = { 0 };

		CHECK(sys_pak_notes(ids, bytes, 4) == 2 && ids[0] == 0 && ids[1] == 15 &&
			bytes[0] == 5000 && bytes[1] == 300);
	}

	/* no room: nothing written */
	CHECK(sys_pak_stat(&free1, &notes1) == 0);
	CHECK(load(path, before) == 0);
	CHECK(sys_pak_create(7, free1 + 1) == -1);
	CHECK(file_eq(path, before));
	CHECK(sys_pak_create(7, free1) == 0 && sys_pak_close() == 0);
	CHECK(sys_pak_stat(&size, NULL) == 0 && size == 0);
	CHECK(load(path, before) == 0);
	CHECK(sys_pak_create(4, 1) == -1);
	CHECK(file_eq(path, before));
	CHECK(sys_pak_delete(7) == 0);
	CHECK(sys_pak_stat(&size, NULL) == 0 && size == free1);
	printf("paktest: no room ok (create of %u bytes fails, image unchanged)\n", free1 + 1);

	CHECK(sys_pak_delete(15) == 0 && sys_pak_delete(15) == -1);
	CHECK(!has_note(15, NULL) && has_note(0, NULL));
	sys_pak_unmount();
	CHECK(sys_pak_notes(NULL, NULL, 16) == -1);
	CHECK(sys_pak_mount() == 0);
	CHECK(get_check(0, 5000, 1) == 0);
	CHECK(sys_pak_stat(&free1, &notes1) == 0 && free1 == free0 - 20 * PAKFS_PAGE && notes1 == notes0 - 1);
	printf("paktest: delete / remount ok\n");
	return fails ? 1 : 0;
}

/* The effective FAT of an image: main copy, or the backup when the main
 * copy's sum is wrong. */
static const uint8_t *img_fat(const uint8_t *img)
{
	uint8_t s = 0;
	unsigned i;

	for (i = 10; i < 256; i++)
		s = (uint8_t)(s + img[0x100 + i]);
	return s == img[0x101] ? img + 0x100 : img + 0x200;
}

static int same(const char *a, const char *b, int quiet)
{
	static uint8_t x[PAKFS_BYTES], y[PAKFS_BYTES];
	const uint8_t *fx, *fy;
	int i, notes = 0, pages = 0;

	if (load(a, x) || load(b, y))
	{
		printf("paktest: FAIL cannot read %s / %s\n", a, b);
		return 1;
	}
	fx = img_fat(x);
	fy = img_fat(y);
	CHECK(!memcmp(x, y, 256));
	for (i = 0; i < 16; i++)
	{
		const uint8_t *n = x + 0x300 + 32 * i;
		unsigned v, steps;
		int used = (n[8] & 2) || ((n[0] | n[1] | n[2] | n[3]) && (n[4] | n[5]));

		if (!used || (!memcmp(n, PAKFS_GAME, 4) && !memcmp(n + 4, PAKFS_PUB, 2)))
			continue;
		notes++;
		CHECK(!memcmp(n, y + 0x300 + 32 * i, 32));
		v = (unsigned)n[6] << 8 | n[7];
		for (steps = 0; v >= 5 && v < 128 && steps < 123; steps++)
		{
			CHECK(!memcmp(x + v * 256, y + v * 256, 256));
			CHECK(!memcmp(fx + 2 * v, fy + 2 * v, 2));
			pages++;
			v = (unsigned)fx[2 * v] << 8 | fx[2 * v + 1];
		}
	}
	if (!quiet || fails)
		printf("paktest: same: %d foreign notes, %d pages, ID page %s\n", notes, pages,
			fails ? "checked (FAILED)" : "byte-identical");
	return fails ? 1 : 0;
}

static int cut(const char *path, const char *outdir)
{
	static uint8_t base[PAKFS_BYTES], now[PAKFS_BYTES], buf[PAKFS_BYTES];
	char work[1100], snap[1100];
	uint32_t total, k;
	int bad = 0;

	if (load(path, base))
	{
		printf("paktest: FAIL cannot read %s\n", path);
		return 1;
	}
	snprintf(work, sizeof(work), "%s/work.pak", outdir);
	CHECK(save_img(work, base) == 0);
	host_pak_set(work);
	/* the sequence once, uncut: its block count */
	blocks_written = 0;
	CHECK(sys_pak_mount() == 0);
	CHECK(sys_pak_create(2, 3000) == 0 && put_pieces(3000, 2) == 0 && sys_pak_close() == 0);
	CHECK(sys_pak_create(5, 700) == 0 && put_pieces(700, 5) == 0 && sys_pak_close() == 0);
	CHECK(sys_pak_delete(2) == 0);
	total = blocks_written;
	CHECK(get_check(5, 700, 5) == 0 && !has_note(2, NULL));
	if (fails)
		return 1;

	for (k = 0; k <= total; k++)
	{
		int f0 = fails;
		uint32_t size;

		CHECK(save_img(work, base) == 0);
		sys_pak_unmount();
		cut_armed = 1;
		cut_dead = 0;
		cut_left = k;
		if (!sys_pak_mount() && !sys_pak_create(2, 3000) && !put_pieces(3000, 2) && !sys_pak_close() &&
			!sys_pak_create(5, 700) && !put_pieces(700, 5) && !sys_pak_close())
			sys_pak_delete(2);
		cut_armed = 0;
		cut_dead = 0;
		/* power back: a fresh mount */
		sys_pak_unmount();
		CHECK(sys_pak_mount() == 0);
		/* a listed note of ours reads back whole (its data may be torn) */
		if (has_note(2, &size))
			CHECK(size == 3000 && sys_pak_read(2, buf, 0, 3000) == 0);
		if (has_note(5, &size))
			CHECK(size == 700 && sys_pak_read(5, buf, 0, 700) == 0);
		snprintf(snap, sizeof(snap), "%s/cut-%u.pak", outdir, k);
		CHECK(load(work, now) == 0 && save_img(snap, now) == 0);
		same(path, snap, 1);
		if (fails != f0)
		{
			printf("paktest: FAIL cut after %u blocks\n", k);
			bad++;
		}
	}
	printf("paktest: cut: %u cut points (every block of %u), %d bad\n", total + 1, total, bad);
	return fails ? 1 : 0;
}

int main(int argc, char **argv)
{
	int r = 2;

	if (argc == 3 && !strcmp(argv[1], "roundtrip"))
		r = roundtrip(argv[2]);
	else if (argc == 6 && (!strcmp(argv[1], "put") || !strcmp(argv[1], "get")))
	{
		int id = atoi(argv[3]);
		uint32_t len = (uint32_t)strtoul(argv[4], NULL, 0);
		unsigned seed = (unsigned)atoi(argv[5]);

		host_pak_set(argv[2]);
		CHECK(sys_pak_accessory() == SYS_PAK_CPAK && sys_pak_mount() == 0);
		if (argv[1][0] == 'p')
			CHECK(sys_pak_create(id, len) == 0 && put_pieces(len, seed) == 0 && sys_pak_close() == 0);
		else
			CHECK(get_check(id, len, seed) == 0);
		printf("paktest: %s %d (%u bytes) %s\n", argv[1], id, len, fails ? "FAILED" : "ok");
		r = fails ? 1 : 0;
	}
	else if (argc == 4 && !strcmp(argv[1], "del"))
	{
		host_pak_set(argv[2]);
		CHECK(sys_pak_mount() == 0 && sys_pak_delete(atoi(argv[3])) == 0);
		printf("paktest: del %s %s\n", argv[3], fails ? "FAILED" : "ok");
		r = fails ? 1 : 0;
	}
	else if (argc == 4 && !strcmp(argv[1], "same"))
		r = same(argv[2], argv[3], 0);
	else if (argc == 4 && !strcmp(argv[1], "cut"))
		r = cut(argv[2], argv[3]);
	else
		fprintf(stderr, "usage: hl64-paktest roundtrip IMG | put|get IMG ID LEN SEED | del IMG ID | "
			"same BEFORE AFTER | cut IMG OUTDIR\n");
	log_flush();
	return r;
}
