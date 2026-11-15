/* SPDX-License-Identifier: MIT */
/*
 * pakfs.c -- sys.h's sys_pak_* (all but sys_pak_accessory, the backend's)
 * over the Controller Pak file system plat/pakfs.h describes. One bank;
 * the mounted state is the FAT, the note table and which note's chain
 * reaches each page, all read at mount and kept in step with every write.
 */
#include <stdint.h>
#include <string.h>

#include "engine/core/log.h"
#include "engine/core/sys.h"
#include "plat/pakfs.h"

#define ID_BYTES	32u
#define FAT_MAIN	0x100u
#define FAT_BACKUP	0x200u
#define NOTE_TABLE	0x300u
#define NOTE_BYTES	32u
#define FIRST_DATA	5u		/* ID page, two FAT copies, two note pages */
#define DATA_PAGES	(PAKFS_PAGES - FIRST_DATA)
#define FAT_END		0x0001u
#define FAT_FREE	0x0003u
#define STATUS_USED	0x02u

/* note bytes */
#define N_GAME		0
#define N_PUB		4
#define N_FIRST		6
#define N_STATUS	8
#define N_PAD		9
#define N_CRC		10
#define N_EXT		12
#define N_NAME		16

enum { OWN_NONE = -1, OWN_SHARED = -2 };

static struct {
	int		mounted;
	uint8_t		fat[PAKFS_PAGE];
	uint8_t		note[PAKFS_NOTES][NOTE_BYTES];
	int8_t		owner[PAKFS_PAGES];	/* note whose chain reaches the page, OWN_* */
	uint8_t		chain_ok[PAKFS_NOTES];	/* chain ends, no loop, no page shared */
	uint8_t		chain_len[PAKFS_NOTES];
	int		wnote;			/* note open for sys_pak_write, -1 none */
	uint32_t	wsize;
} pk = { .wnote = -1 };

static unsigned fat_get(const uint8_t *fat, unsigned page)
{
	return (unsigned)fat[2 * page] << 8 | fat[2 * page + 1];
}

static void fat_set(uint8_t *fat, unsigned page, unsigned v)
{
	fat[2 * page] = (uint8_t)(v >> 8);
	fat[2 * page + 1] = (uint8_t)v;
}

/* An entry naming a data page of bank 0. */
static int fat_is_page(unsigned v)
{
	return v >= FIRST_DATA && v < PAKFS_PAGES;
}

static uint8_t fat_sum(const uint8_t *fat)
{
	uint8_t s = 0;
	unsigned i;

	for (i = 2 * FIRST_DATA; i < PAKFS_PAGE; i++)
		s = (uint8_t)(s + fat[i]);
	return s;
}

static unsigned be16(const uint8_t *p)
{
	return (unsigned)p[0] << 8 | p[1];
}

/* The ID block's two sums: of its first 14 big-endian halfwords, and
 * 0xFFF2 minus that. */
static void id_sums(const uint8_t *id, unsigned *s1, unsigned *s2)
{
	unsigned s = 0, i;

	for (i = 0; i < 28; i += 2)
		s += be16(id + i);
	*s1 = s & 0xFFFFu;
	*s2 = (0xFFF2u - s) & 0xFFFFu;
}

/* The note's CRC-16 (polynomial 0x5935, CRC field zero), libdragon's mark
 * that the padding byte holds the last page's unused bytes. */
static unsigned note_crc(const uint8_t *note)
{
	uint8_t n[NOTE_BYTES];
	unsigned crc = 0, i, b;

	memcpy(n, note, NOTE_BYTES);
	n[N_CRC] = n[N_CRC + 1] = 0;
	for (i = 0; i < NOTE_BYTES; i++)
	{
		crc ^= (unsigned)n[i] << 8;
		for (b = 0; b < 8; b++)
			crc = (crc & 0x8000u) ? ((crc << 1) ^ 0x5935u) & 0xFFFFu : (crc << 1) & 0xFFFFu;
	}
	return crc;
}

/* ASCII capitals and digits -> the Pak's codepage, zero padded. */
static void cp_encode(uint8_t *dst, const char *src, unsigned n)
{
	unsigned i;

	memset(dst, 0, n);
	for (i = 0; i < n && src[i]; i++)
		dst[i] = (uint8_t)(src[i] >= 'A' && src[i] <= 'Z' ? 0x1A + (src[i] - 'A') : 0x10 + (src[i] - '0'));
}

static int note_zero(const uint8_t *p, unsigned n)
{
	while (n--)
		if (*p++)
			return 0;
	return 1;
}

/* Occupied to either reader: libdragon's status bit, libultra's nonzero
 * game and publisher codes. */
static int note_used(const uint8_t *note)
{
	return (note[N_STATUS] & STATUS_USED) ||
		(!note_zero(note + N_GAME, 4) && !note_zero(note + N_PUB, 2));
}

/* Note id's extension: the id in decimal. */
static void id_ext(uint8_t ext[4], int id)
{
	char s[3];

	s[0] = (char)(id < 10 ? '0' + id : '1');
	s[1] = (char)(id < 10 ? 0 : '0' + id - 10);
	s[2] = 0;
	cp_encode(ext, s, 4);
}

/* Our note's id, -1 if not ours. */
static int note_id(const uint8_t *note)
{
	uint8_t want[16];
	int id;

	if (!(note[N_STATUS] & STATUS_USED) || memcmp(note + N_GAME, PAKFS_GAME, 4) ||
		memcmp(note + N_PUB, PAKFS_PUB, 2))
		return -1;
	cp_encode(want, PAKFS_NAME, 16);
	if (memcmp(note + N_NAME, want, 16))
		return -1;
	for (id = 0; id < PAKFS_NOTES; id++)
	{
		id_ext(want, id);
		if (!memcmp(note + N_EXT, want, 4))
			return id;
	}
	return -1;
}

static int find_note(int id)
{
	int i;

	for (i = 0; i < PAKFS_NOTES; i++)
		if (note_id(pk.note[i]) == id)
			return i;
	return -1;
}

/* Our note's size: whole pages less libdragon's recorded padding. */
static uint32_t note_size(int i)
{
	const uint8_t *n = pk.note[i];
	uint32_t pad = be16(n + N_CRC) == note_crc(n) ? n[N_PAD] : 0;

	if (!pk.chain_ok[i] || !pk.chain_len[i] || pad >= PAKFS_PAGE)
		return 0;
	return pk.chain_len[i] * PAKFS_PAGE - pad;
}

/* Walk every occupied note's chain: which note reaches each page. A page
 * two chains reach, a loop or a chain that does not end marks the notes
 * involved broken (never read, written or freed beyond their own pages). */
static void map_chains(void)
{
	int i, j;

	memset(pk.owner, OWN_NONE, sizeof(pk.owner));
	for (i = 0; i < PAKFS_NOTES; i++)
	{
		unsigned v, n = 0;

		pk.chain_ok[i] = 0;
		pk.chain_len[i] = 0;
		if (!note_used(pk.note[i]))
			continue;
		v = be16(pk.note[i] + N_FIRST);
		while (fat_is_page(v) && n < DATA_PAGES)
		{
			if (pk.owner[v] == i)
				break;		/* loop */
			if (pk.owner[v] != OWN_NONE)
			{
				pk.owner[v] = OWN_SHARED;
				break;
			}
			pk.owner[v] = (int8_t)i;
			n++;
			v = fat_get(pk.fat, v);
		}
		pk.chain_len[i] = (uint8_t)n;
		pk.chain_ok[i] = v == FAT_END && n > 0;
	}
	/* a chain sharing a page with another is broken too (both ends) */
	for (i = 0; i < PAKFS_NOTES; i++)
	{
		unsigned v, n = 0;

		if (!pk.chain_ok[i])
			continue;
		for (v = be16(pk.note[i] + N_FIRST); fat_is_page(v) && n < DATA_PAGES; v = fat_get(pk.fat, v), n++)
			if (pk.owner[v] != i)
			{
				pk.chain_ok[i] = 0;
				break;
			}
	}
	for (j = FIRST_DATA; j < (int)PAKFS_PAGES; j++)
		if (pk.owner[j] == OWN_SHARED)
			log_printf("pak: page %d is in two notes' chains\n", j);
}

int sys_pak_mount(void)
{
	static const uint16_t id_at[4] = { 0x20, 0x60, 0x80, 0xC0 };
	uint8_t id[ID_BYTES];
	unsigned s1, s2, i;
	int found = 0, ours = 0, used = 0;

	sys_pak_unmount();
	for (i = 0; i < 4 && !found; i++)
	{
		if (pakfs_dev_read(id_at[i], id, ID_BYTES))
			return -1;
		id_sums(id, &s1, &s2);
		found = (s1 == be16(id + 28) && s2 == be16(id + 30)) ||
			/* DexDrive dumps' fixed wrong sum, as libdragon accepts it */
			(s1 == 0x0101 && be16(id + 28) == 0x0101 && be16(id + 30) == 0xFEFD);
	}
	if (!found)
	{
		log_printf("pak: no ID block checks out (unformatted)\n");
		return -1;
	}
	if (id[0x1A] != 1)
	{
		log_printf("pak: %u banks (only one supported)\n", id[0x1A]);
		return -1;
	}
	if (pakfs_dev_read(FAT_MAIN, pk.fat, PAKFS_PAGE))
		return -1;
	if (fat_sum(pk.fat) != pk.fat[1])
	{
		if (pakfs_dev_read(FAT_BACKUP, pk.fat, PAKFS_PAGE))
			return -1;
		if (fat_sum(pk.fat) != pk.fat[1])
		{
			log_printf("pak: both FAT copies fail their sums\n");
			return -1;
		}
		log_printf("pak: FAT main copy fails its sum; backup used\n");
	}
	if (pakfs_dev_read(NOTE_TABLE, pk.note, sizeof(pk.note)))
		return -1;
	map_chains();
	for (i = 0; i < PAKFS_NOTES; i++)
	{
		used += note_used(pk.note[i]);
		ours += note_id(pk.note[i]) >= 0;
	}
	pk.mounted = 1;
	log_printf("pak: mounted, %d notes (%d ours)\n", used, ours);
	return 0;
}

void sys_pak_unmount(void)
{
	pk.mounted = 0;
	pk.wnote = -1;
}

int sys_pak_stat(uint32_t *free_bytes, int *free_notes)
{
	uint32_t pages = 0;
	int notes = 0;
	unsigned p;
	int i;

	if (!pk.mounted)
		return -1;
	for (p = FIRST_DATA; p < PAKFS_PAGES; p++)
		pages += fat_get(pk.fat, p) == FAT_FREE && pk.owner[p] == OWN_NONE;
	for (i = 0; i < PAKFS_NOTES; i++)
		notes += !note_used(pk.note[i]);
	if (free_bytes)
		*free_bytes = pages * PAKFS_PAGE;
	if (free_notes)
		*free_notes = notes;
	return 0;
}

int sys_pak_notes(uint8_t *ids, uint32_t *bytes, int max)
{
	int id, i, n = 0;

	if (!pk.mounted)
		return -1;
	for (id = 0; id < PAKFS_NOTES && n < max; id++)
	{
		if ((i = find_note(id)) < 0)
			continue;
		if (ids)
			ids[n] = (uint8_t)id;
		if (bytes)
			bytes[n] = note_size(i);
		n++;
	}
	return n;
}

/* Transfer [off, off + len) of note i's data (its chain checks out),
 * pages that follow each other on the Pak in one transfer: dst set reads,
 * else writes src. 0 ok, -1 failed. */
static int note_xfer(int i, void *dst, const void *src, uint32_t off, uint32_t len)
{
	unsigned v = be16(pk.note[i] + N_FIRST);
	uint32_t skip;

	for (skip = off / PAKFS_PAGE; skip; skip--)
		v = fat_get(pk.fat, v);
	off %= PAKFS_PAGE;
	while (len)
	{
		uint32_t addr = v * PAKFS_PAGE + off, n = PAKFS_PAGE - off;
		unsigned last = v;

		while (n < len && fat_get(pk.fat, last) == last + 1)
		{
			last++;
			n += PAKFS_PAGE;
		}
		if (n > len)
			n = len;
		if (dst ? pakfs_dev_read(addr, dst, n) : pakfs_dev_write(addr, src, n))
			return -1;
		if (dst)
			dst = (uint8_t *)dst + n;
		else
			src = (const uint8_t *)src + n;
		v = fat_get(pk.fat, last);
		len -= n;
		off = 0;
	}
	return 0;
}

int sys_pak_read(int id, void *dst, uint32_t off, uint32_t len)
{
	int i;
	uint32_t size;

	if (!pk.mounted || id < 0 || id >= PAKFS_NOTES || (i = find_note(id)) < 0)
		return -1;
	size = note_size(i);
	if (off > size || len > size - off)
		return -1;
	return len ? note_xfer(i, dst, NULL, off, len) : 0;
}

/* Write the FAT: main copy, then backup. A failure unmounts. */
static int fat_write(const uint8_t *fat)
{
	if (pakfs_dev_write(FAT_MAIN, fat, PAKFS_PAGE) || pakfs_dev_write(FAT_BACKUP, fat, PAKFS_PAGE))
	{
		log_printf("pak: FAT write failed; unmounted\n");
		sys_pak_unmount();
		return -1;
	}
	memcpy(pk.fat, fat, PAKFS_PAGE);
	return 0;
}

static int note_write(int i, const uint8_t *note)
{
	if (pakfs_dev_write(NOTE_TABLE + (uint32_t)i * NOTE_BYTES, note, NOTE_BYTES))
	{
		log_printf("pak: note write failed; unmounted\n");
		sys_pak_unmount();
		return -1;
	}
	memcpy(pk.note[i], note, NOTE_BYTES);
	return 0;
}

int sys_pak_create(int id, uint32_t len)
{
	uint8_t fat[PAKFS_PAGE], note[NOTE_BYTES];
	unsigned pages, p, prev = 0, first = 0, n = 0, crc;
	int i;

	if (!pk.mounted || pk.wnote >= 0 || id < 0 || id >= PAKFS_NOTES || !len ||
		len > DATA_PAGES * PAKFS_PAGE || find_note(id) >= 0)
		return -1;
	for (i = 0; i < PAKFS_NOTES && note_used(pk.note[i]); i++)
		;
	if (i == PAKFS_NOTES)
		return -1;
	pages = (len + PAKFS_PAGE - 1) / PAKFS_PAGE;

	/* the lowest free pages, chained in order */
	memcpy(fat, pk.fat, sizeof(fat));
	for (p = FIRST_DATA; p < PAKFS_PAGES && n < pages; p++)
	{
		if (fat_get(pk.fat, p) != FAT_FREE || pk.owner[p] != OWN_NONE)
			continue;
		if (n)
			fat_set(fat, prev, p);
		else
			first = p;
		prev = p;
		n++;
	}
	if (n < pages)
		return -1;
	fat_set(fat, prev, FAT_END);
	fat[1] = fat_sum(fat);

	memset(note, 0, sizeof(note));
	memcpy(note + N_GAME, PAKFS_GAME, 4);
	memcpy(note + N_PUB, PAKFS_PUB, 2);
	note[N_FIRST] = 0;
	note[N_FIRST + 1] = (uint8_t)first;
	note[N_STATUS] = STATUS_USED;
	note[N_PAD] = (uint8_t)(pages * PAKFS_PAGE - len);
	id_ext(note + N_EXT, id);
	cp_encode(note + N_NAME, PAKFS_NAME, 16);
	crc = note_crc(note);
	note[N_CRC] = (uint8_t)(crc >> 8);
	note[N_CRC + 1] = (uint8_t)crc;

	/* the chain first: power lost before the note leaves it unreferenced */
	if (fat_write(fat) || note_write(i, note))
		return -1;
	map_chains();
	pk.wnote = i;
	pk.wsize = len;
	return 0;
}

int sys_pak_write(const void *src, uint32_t off, uint32_t len)
{
	if (!pk.mounted || pk.wnote < 0 || off > pk.wsize || len > pk.wsize - off)
		return -1;
	return len ? note_xfer(pk.wnote, NULL, src, off, len) : 0;
}

int sys_pak_close(void)
{
	if (!pk.mounted || pk.wnote < 0)
		return -1;
	pk.wnote = -1;
	return 0;
}

int sys_pak_delete(int id)
{
	uint8_t fat[PAKFS_PAGE], note[NOTE_BYTES];
	unsigned p;
	int i;

	if (!pk.mounted || id < 0 || id >= PAKFS_NOTES || (i = find_note(id)) < 0)
		return -1;
	if (pk.wnote == i)
		pk.wnote = -1;
	memcpy(fat, pk.fat, sizeof(fat));
	for (p = FIRST_DATA; p < PAKFS_PAGES; p++)
		if (pk.owner[p] == i)
			fat_set(fat, p, FAT_FREE);
	fat[1] = fat_sum(fat);
	memset(note, 0, sizeof(note));
	/* the note first: power lost before the FAT leaves its pages unreferenced */
	if (note_write(i, note) || fat_write(fat))
		return -1;
	map_chains();
	return 0;
}

void pakfs_format(uint8_t img[PAKFS_BYTES])
{
	static const char serial[24] = "HL64 HOST PAK";
	uint8_t id[ID_BYTES];
	unsigned s1, s2, p;

	memset(img, 0, PAKFS_BYTES);
	memset(id, 0, sizeof(id));
	memcpy(id, serial, sizeof(serial));
	id[0x19] = 0x01;		/* device id */
	id[0x1A] = 0x01;		/* one bank */
	id_sums(id, &s1, &s2);
	id[28] = (uint8_t)(s1 >> 8);
	id[29] = (uint8_t)s1;
	id[30] = (uint8_t)(s2 >> 8);
	id[31] = (uint8_t)s2;
	memcpy(img + 0x20, id, ID_BYTES);
	memcpy(img + 0x60, id, ID_BYTES);
	memcpy(img + 0x80, id, ID_BYTES);
	memcpy(img + 0xC0, id, ID_BYTES);
	for (p = FIRST_DATA; p < PAKFS_PAGES; p++)
		fat_set(img + FAT_MAIN, p, FAT_FREE);
	img[FAT_MAIN + 1] = fat_sum(img + FAT_MAIN);
	memcpy(img + FAT_BACKUP, img + FAT_MAIN, PAKFS_PAGE);
}
