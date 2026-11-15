/* SPDX-License-Identifier: MIT */
/*
 * sv.c -- the save stream (sv.h, sv_int.h): sections put their bytes into
 * a flat staging buffer (plain copies: the snapshot frame stays short); a
 * coder then turns the staged state into the slot image a time budget at
 * a time over the following frames. A load decodes the image back into
 * the staging buffer in steps, and the sections get their bytes from it.
 *
 * Image format: one MSB-first bit stream. First the code lengths (4 bits
 * each, 0 = unused) of the literal / length alphabet (SYM_LL: bytes 0..255,
 * 256 the end, 257.. the deflate length codes for 3..258) and of the
 * distance alphabet (SYM_D: the deflate distance codes for 1..8192); the
 * codes are canonical (shorter first, then by symbol; at most 15 bits).
 * Then the tokens: a literal's code; a match's length code with its extra
 * bits, then its distance code with its extra bits (the copy may overlap
 * the bytes it produces: runs); the end code; zero bits to the byte.
 *
 * The coder parses the staged state twice in the same way -- once to count
 * the symbols (their code lengths), once to write them: matches from hash
 * chains (2048 heads, 8 KB of links, at most CHAIN_DEPTH candidates) with
 * a one-step lazy match. The whole state is in memory and the parse
 * depends only on it, so the image does not depend on how the coding is
 * split over frames.
 */
#include <string.h>

#include "engine/core/log.h"
#include "engine/core/sys.h"
#include "engine/save/sv.h"
#include "engine/save/sv_int.h"

#define WIN		8192u		/* match distances 1..WIN */
#define WMASK		(WIN - 1u)
#define HBITS		11
#define NHEAD		(1u << HBITS)
#define MINLEN		3u
#define MAXLEN		258u
#define CHAIN_DEPTH	16		/* candidates tried per position */
#define LAZY_MAX	32u		/* a match this long is taken without the lazy look */
#define SYM_END		256
#define SYM_LL		286		/* 256 bytes, the end, 29 length codes */
#define SYM_D		26		/* distance codes up to 8192 */
#define MAXBITS		15
#define CHUNK		128u		/* bytes coded / tokens decoded between clock reads (a
					 * step overruns its budget by one chunk at most) */
#define PARTS		96		/* sv_part marks (test builds) */

typedef char sv_work_fits[NHEAD * 4u + WIN * 2u <= SV_WORK_BYTES ? 1 : -1];

enum { PASS_COUNT, PASS_EMIT, PASS_DONE };

static struct {
	/* staging: the writer's buffer, the reader's after the decode */
	uint8_t		*raw;
	uint32_t	cap, pos, len;
	uint8_t		over, failed;
	const char	*why;
	uint8_t		*scratch;	/* sv_work_area: the coder's work area while it is idle */
	/* coder: raw[0, len) -> out */
	uint32_t	*head;		/* [NHEAD] position + 1 of the hash's last occurrence, 0 none */
	uint16_t	*prev;		/* [WIN] by position: distance back to the hash's previous one, 0 none */
	uint32_t	enc;		/* bytes parsed */
	uint8_t		pass;
	uint8_t		have_next;	/* the lazy look's match at enc is known */
	uint32_t	nlen, ndist;
	uint8_t		*out;
	uint32_t	ocap;
	uint32_t	need;		/* image bytes, written or not (past ocap) */
	uint32_t	bits, nbits;	/* bit writer / reader */
	uint32_t	freq[SYM_LL + SYM_D];
	uint16_t	code[SYM_LL + SYM_D];
	uint8_t		blen[SYM_LL + SYM_D];
	/* decoder: in[0, inn) -> raw */
	const uint8_t	*in;
	uint32_t	inn, at;
	uint8_t		dtables;	/* the code tables are read */
	uint16_t	count[2][MAXBITS + 1];
	uint16_t	sym[SYM_LL + SYM_D];	/* by code order: [0, SYM_LL) literal / length, then distance */
#ifdef HL64_TEST
	const char	*part_name[PARTS];
	uint32_t	part_raw[PARTS], part_coded[PARTS];
	uint64_t	part_us[PARTS], end_us;	/* the staging clock at each mark, at the end */
	uint64_t	check_t0;	/* sv_check_time(1)'s clock */
	uint32_t	check_us;	/* the check windows so far, left out of the clock */
	int		nparts, part_at;
#endif
} V;

/* deflate's length and distance codes: base and extra bits */
static const uint16_t lbase[29] = {
	3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115,
	131, 163, 195, 227, 258
};
static const uint8_t lext[29] = {
	0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};
static const uint16_t dbase[SYM_D] = {
	1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537,
	2049, 3073, 4097, 6145
};
static const uint8_t dext[SYM_D] = {
	0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11
};

/* ---- writer: staging ---------------------------------------------------- */

void sv_write_begin(void *raw, uint32_t cap, void *work)
{
	memset(&V, 0, sizeof(V));
	V.raw = raw;
	V.cap = cap;
	V.scratch = work;
}

uint32_t sv_write_end(void)
{
	V.len = V.pos;
#ifdef HL64_TEST
	V.end_us = sys_time_us() - V.check_us;
#endif
	return V.over || V.failed ? 0 : V.pos;
}

uint32_t sv_write_pos(void)
{
	return V.pos;
}

void *sv_work_area(uint32_t *bytes)
{
	*bytes = V.scratch ? SV_WORK_BYTES : 0;
	return V.scratch;
}

void sv_put(const void *p, uint32_t n)
{
	if (V.over || n > V.cap - V.pos)
	{
		V.over = 1;
		V.pos += n;	/* the size it would have needed */
		return;
	}
	memcpy(V.raw + V.pos, p, n);
	V.pos += n;
}

#ifdef HL64_TEST
void sv_part(const char *name)
{
	if (V.nparts < PARTS)
	{
		V.part_name[V.nparts] = name;
		V.part_us[V.nparts] = sys_time_us() - V.check_us;
		V.part_raw[V.nparts++] = V.pos;
	}
}

void sv_check_time(int on)
{
	if (on)
		V.check_t0 = sys_time_us();
	else
		V.check_us += (uint32_t)(sys_time_us() - V.check_t0);
}

uint32_t sv_check_us(void)
{
	return V.check_us;
}
#endif

void sv_tag(uint32_t tag)
{
	sv_u32(tag);
}

void sv_u8(uint32_t v)
{
	uint8_t b = (uint8_t)v;

	sv_put(&b, 1);
}

void sv_u16(uint32_t v)
{
	uint16_t b = (uint16_t)v;

	sv_put(&b, 2);
}

void sv_u32(uint32_t v)
{
	sv_put(&v, 4);
}

void sv_f32(float v)
{
	sv_put(&v, 4);
}

void sv_f64(double v)
{
	sv_put(&v, 8);
}

/* ---- coder ---------------------------------------------------------------- */

static void put_bits(uint32_t v, uint32_t n)
{
	V.bits = V.bits << n | (v & ((1u << n) - 1u));
	V.nbits += n;
	while (V.nbits >= 8)
	{
		V.nbits -= 8;
		if (V.need < V.ocap)
			V.out[V.need] = (uint8_t)(V.bits >> V.nbits);
		V.need++;
	}
}

static uint32_t hash3(uint32_t p)
{
	uint32_t v = (uint32_t)V.raw[p] << 16 | (uint32_t)V.raw[p + 1] << 8 | V.raw[p + 2];

	return (v * 2654435761u) >> (32 - HBITS);
}

/* Chain position p (3 bytes left at least); the hash's previous position + 1. */
static uint32_t insert(uint32_t p)
{
	uint32_t h = hash3(p), c = V.head[h];

	V.prev[p & WMASK] = (uint16_t)(c && p - (c - 1) < WIN ? p - (c - 1) : 0);
	V.head[h] = p + 1;
	return c;
}

/* The longest match at p among CHAIN_DEPTH candidates (0: none); p is
 * chained. */
static uint32_t find(uint32_t p, uint32_t *dist)
{
	uint32_t avail = V.len - p, max, best = 0, c, depth = CHAIN_DEPTH;

	if (avail < MINLEN)
		return 0;
	c = insert(p);
	max = avail < MAXLEN ? avail : MAXLEN;
	while (c && depth--)
	{
		uint32_t q = c - 1, l = 0, d;

		if (p - q > WIN)
			break;
		if (V.raw[q + best] == V.raw[p + best])
		{
			while (l < max && V.raw[q + l] == V.raw[p + l])
				l++;
			if (l > best)
			{
				best = l;
				*dist = p - q;
				if (l == max)
					break;
			}
		}
		d = V.prev[q & WMASK];
		if (!d || d > q)
			break;
		c = q - d + 1;
	}
	return best >= MINLEN ? best : 0;
}

/* A symbol: counted (PASS_COUNT) or written. */
static void sym_out(uint32_t s)
{
	if (V.pass == PASS_COUNT)
		V.freq[s]++;
	else
		put_bits(V.code[s], V.blen[s]);
}

static void extra_out(uint32_t v, uint32_t n)
{
	if (V.pass == PASS_EMIT)
		put_bits(v, n);
}

static void match_out(uint32_t len, uint32_t dist)
{
	int k;

	for (k = 28; lbase[k] > len; k--)
		;
	sym_out(257u + (uint32_t)k);
	extra_out(len - lbase[k], lext[k]);
	for (k = SYM_D - 1; dbase[k] > dist; k--)
		;
	sym_out(SYM_LL + (uint32_t)k);
	extra_out(dist - dbase[k], dext[k]);
}

#ifdef HL64_TEST
/* Parts whose first byte the coder reached: the image bytes so far. */
static void part_reach(void)
{
	while (V.part_at < V.nparts && V.part_raw[V.part_at] <= V.enc)
		V.part_coded[V.part_at++] = V.need;
}
#endif

/* Parse raw from enc up to until (a match may run past it). */
static void parse(uint32_t until)
{
	while (V.enc < until)
	{
		uint32_t len, dist = 0, i = 1;

#ifdef HL64_TEST
		if (V.pass == PASS_EMIT)
			part_reach();
#endif
		if (V.have_next)
		{
			len = V.nlen;
			dist = V.ndist;
			V.have_next = 0;
		}
		else
			len = find(V.enc, &dist);
		if (!len)
		{
			sym_out(V.raw[V.enc++]);
			continue;
		}
		if (len < LAZY_MAX && V.enc + 1 < V.len)
		{
			uint32_t d2 = 0, l2 = find(V.enc + 1, &d2);

			if (l2 > len)
			{
				/* a longer match one byte on: this byte goes as a literal */
				sym_out(V.raw[V.enc++]);
				V.have_next = 1;
				V.nlen = l2;
				V.ndist = d2;
				continue;
			}
			i = 2;
		}
		for (; i < len; i++)
			if (V.len - (V.enc + i) >= MINLEN)
				insert(V.enc + i);
		match_out(len, dist);
		V.enc += len;
	}
}

/* Code lengths of n symbols (at most MAXBITS): a Huffman tree over the
 * counts (ties: the lower node first), the counts halved until it is
 * shallow enough; one used symbol gets 1 bit. w, parent: 2n scratch. */
static void huff_lengths(const uint32_t *freq, uint32_t n, uint8_t *len, uint32_t *w,
	int16_t *parent)
{
	uint32_t shift, i;

	for (shift = 0;; shift++)
	{
		uint32_t nodes = n, used = 0, maxl = 0;

		for (i = 0; i < n; i++)
		{
			w[i] = freq[i] ? (freq[i] >> shift) | 1u : 0;
			parent[i] = -1;
			used += w[i] != 0;
			len[i] = 0;
		}
		if (used < 2)
		{
			for (i = 0; i < n; i++)
				len[i] = (uint8_t)(w[i] != 0);
			return;
		}
		for (;;)
		{
			int a = -1, b = -1;

			for (i = 0; i < nodes; i++)
			{
				if (!w[i] || parent[i] >= 0)
					continue;
				if (a < 0 || w[i] < w[a])
				{
					b = a;
					a = (int)i;
				}
				else if (b < 0 || w[i] < w[b])
					b = (int)i;
			}
			if (b < 0)
				break;
			w[nodes] = w[a] + w[b];
			parent[nodes] = -1;
			parent[a] = parent[b] = (int16_t)nodes;
			nodes++;
		}
		for (i = 0; i < n; i++)
		{
			uint32_t d = 0;
			int j;

			if (!w[i])
				continue;
			for (j = parent[i]; j >= 0; j = parent[j])
				d++;
			len[i] = (uint8_t)d;
			if (d > maxl)
				maxl = d;
		}
		if (maxl <= MAXBITS)
			return;
	}
}

/* Canonical codes from the lengths (deflate's order). */
static void huff_codes(const uint8_t *len, uint32_t n, uint16_t *code)
{
	uint16_t cnt[MAXBITS + 1], next[MAXBITS + 1];
	uint32_t i, b, c = 0;

	memset(cnt, 0, sizeof(cnt));
	for (i = 0; i < n; i++)
		cnt[len[i]]++;
	cnt[0] = 0;
	for (b = 1; b <= MAXBITS; b++)
	{
		c = (c + cnt[b - 1]) << 1;
		next[b] = (uint16_t)c;
	}
	for (i = 0; i < n; i++)
		if (len[i])
			code[i] = next[len[i]]++;
}

static void matcher_reset(void)
{
	memset(V.head, 0, NHEAD * sizeof(uint32_t));
	memset(V.prev, 0, WIN * sizeof(uint16_t));
	V.enc = 0;
	V.have_next = 0;
}

/* Counted: the code tables (built in the matcher's memory), their lengths
 * at the image start, the second parse. */
static void begin_emit(void)
{
	uint32_t *w = V.head;
	int16_t *parent = (int16_t *)(w + 2 * SYM_LL);
	uint32_t i;

	V.freq[SYM_END]++;
	huff_lengths(V.freq, SYM_LL, V.blen, w, parent);
	huff_lengths(V.freq + SYM_LL, SYM_D, V.blen + SYM_LL, w, parent);
	huff_codes(V.blen, SYM_LL, V.code);
	huff_codes(V.blen + SYM_LL, SYM_D, V.code + SYM_LL);
	for (i = 0; i < SYM_LL + SYM_D; i++)
		put_bits(V.blen[i], 4);
	matcher_reset();
	V.pass = PASS_EMIT;
}

void sv_code_begin(void *out, uint32_t cap, void *work)
{
	V.head = work;
	V.prev = (uint16_t *)((uint8_t *)work + NHEAD * sizeof(uint32_t));
	matcher_reset();
	V.out = out;
	V.ocap = cap;
	V.need = V.bits = V.nbits = 0;
	V.pass = PASS_COUNT;
	memset(V.freq, 0, sizeof(V.freq));
#ifdef HL64_TEST
	V.part_at = 0;
#endif
}

int sv_code_step(uint32_t budget_us)
{
	uint64_t t0 = sys_time_us();

	while (V.pass != PASS_DONE)
	{
		if (V.enc >= V.len)
		{
			if (V.pass == PASS_COUNT)
				begin_emit();
			else
			{
				sym_out(SYM_END);
				if (V.nbits)
					put_bits(0, 8 - V.nbits);
				V.pass = PASS_DONE;
			}
			continue;
		}
		parse(V.len - V.enc > CHUNK ? V.enc + CHUNK : V.len);
		if (V.need > V.ocap)
			return 0;	/* too big: no point going on */
		if (sys_time_us() - t0 >= budget_us)
			return V.pass != PASS_DONE;
	}
	return 0;
}

uint32_t sv_code_end(void)
{
#ifdef HL64_TEST
	int i;

	part_reach();
	for (i = 0; i < V.nparts && V.pass == PASS_DONE; i++)
	{
		uint32_t re = i + 1 < V.nparts ? V.part_raw[i + 1] : V.len;
		uint32_t ce = i + 1 < V.nparts ? V.part_coded[i + 1] : V.need;
		uint64_t ue = i + 1 < V.nparts ? V.part_us[i + 1] : V.end_us;

		log_printf("save: part %s %lu B -> %lu B, staged in %lu us\n", V.part_name[i],
			(unsigned long)(re - V.part_raw[i]), (unsigned long)(ce - V.part_coded[i]),
			(unsigned long)(ue - V.part_us[i]));
	}
#endif
	return V.pass != PASS_DONE || V.need > V.ocap ? 0 : V.need;
}

uint32_t sv_code_need(void)
{
	return V.need;
}

/* ---- decoder -------------------------------------------------------------- */

void sv_decode_begin(const void *in, uint32_t n, void *raw, uint32_t cap)
{
	memset(&V, 0, sizeof(V));
	V.in = in;
	V.inn = n;
	V.raw = raw;
	V.cap = cap;
}

/* The next n bits (n <= 16), -1 past the image's end. */
static int get_bits(uint32_t n)
{
	while (V.nbits < n)
	{
		if (V.at >= V.inn)
			return -1;
		V.bits = V.bits << 8 | V.in[V.at++];
		V.nbits += 8;
	}
	V.nbits -= n;
	return (int)((V.bits >> V.nbits) & ((1u << n) - 1u));
}

/* The code lengths at the image start: per table the count of each
 * length and the symbols in code order; -1 when a table is over-
 * subscribed or has no end code. */
static int read_tables(void)
{
	uint16_t offs[MAXBITS + 1];
	uint32_t t, i, b;

	for (i = 0; i < SYM_LL + SYM_D; i++)
	{
		int v = get_bits(4);

		if (v < 0)
			return -1;
		V.blen[i] = (uint8_t)v;
	}
	for (t = 0; t < 2; t++)
	{
		uint32_t base = t ? SYM_LL : 0, n = t ? SYM_D : SYM_LL;
		int32_t left = 1;

		memset(V.count[t], 0, sizeof(V.count[t]));
		for (i = 0; i < n; i++)
			V.count[t][V.blen[base + i]]++;
		for (b = 1; b <= MAXBITS; b++)
		{
			left = 2 * left - V.count[t][b];
			if (left < 0)
				return -1;
		}
		offs[1] = 0;
		for (b = 1; b < MAXBITS; b++)
			offs[b + 1] = (uint16_t)(offs[b] + V.count[t][b]);
		for (i = 0; i < n; i++)
			if (V.blen[base + i])
				V.sym[base + offs[V.blen[base + i]]++] = (uint16_t)i;
	}
	return V.blen[SYM_END] ? 0 : -1;
}

/* One symbol of table t (canonical, a bit at a time), -1 corrupt. */
static int decode_sym(uint32_t t)
{
	const uint16_t *cnt = V.count[t], *s = V.sym + (t ? SYM_LL : 0);
	int32_t code = 0, first = 0, index = 0;
	uint32_t b;

	for (b = 1; b <= MAXBITS; b++)
	{
		int bit = get_bits(1);

		if (bit < 0)
			return -1;
		code |= bit;
		if (code - cnt[b] < first)
			return s[index + (code - first)];
		index += cnt[b];
		first = (first + cnt[b]) << 1;
		code <<= 1;
	}
	return -1;
}

/* One token: 1 more to come, 0 the end code, -1 corrupt. */
static int decode_token(void)
{
	int s = decode_sym(0), e;
	uint32_t len, dist, i;

	if (s < 0)
		return -1;
	if (s < 256)
	{
		if (V.pos >= V.cap)
			return -1;
		V.raw[V.pos++] = (uint8_t)s;
		return 1;
	}
	if (s == SYM_END)
		return 0;
	s -= 257;
	if (s >= 29 || (e = get_bits(lext[s])) < 0)
		return -1;
	len = lbase[s] + (uint32_t)e;
	if ((s = decode_sym(1)) < 0 || (e = get_bits(dext[s])) < 0)
		return -1;
	dist = dbase[s] + (uint32_t)e;
	if (dist > V.pos || len > V.cap - V.pos)
		return -1;
	for (i = 0; i < len; i++, V.pos++)
		V.raw[V.pos] = V.raw[V.pos - dist];
	return 1;
}

int sv_decode_step(uint32_t budget_us)
{
	uint64_t t0 = sys_time_us();

	if (!V.dtables)
	{
		if (read_tables())
		{
			sv_fail("image does not decode");
			return -1;
		}
		V.dtables = 1;
	}
	for (;;)
	{
		uint32_t k;

		for (k = 0; k < CHUNK; k++)
		{
			int r = decode_token();

			if (r <= 0)
			{
				if (r < 0)
					sv_fail("image does not decode");
				V.len = V.pos;
				return r;
			}
		}
		if (sys_time_us() - t0 >= budget_us)
			return 1;
	}
}

uint32_t sv_decode_end(void)
{
	return V.failed ? 0 : V.len;
}

/* ---- reader: staging ------------------------------------------------------ */

void sv_read_begin(const void *raw, uint32_t n, void *work)
{
	memset(&V, 0, sizeof(V));
	V.raw = (uint8_t *)raw;
	V.len = n;
	V.scratch = work;
}

uint32_t sv_read_pos(void)
{
	return V.pos;
}

const char *sv_fail_reason(void)
{
	return V.failed ? V.why : NULL;
}

void sv_fail(const char *why)
{
	if (!V.failed)
	{
		V.failed = 1;
		V.why = why;
	}
}

int sv_failed(void)
{
	return V.failed;
}

void sv_get(void *p, uint32_t n)
{
	if (V.failed || n > V.len - V.pos)
	{
		if (!V.failed)
			sv_fail("state ends early");
		memset(p, 0, n);
		return;
	}
	memcpy(p, V.raw + V.pos, n);
	V.pos += n;
}

int sv_expect(uint32_t tag)
{
	if (sv_get_u32() != tag)
		sv_fail("section tag");
	return V.failed ? -1 : 0;
}

uint32_t sv_get_u8(void)
{
	uint8_t v;

	sv_get(&v, 1);
	return v;
}

uint32_t sv_get_u16(void)
{
	uint16_t v;

	sv_get(&v, 2);
	return v;
}

uint32_t sv_get_u32(void)
{
	uint32_t v;

	sv_get(&v, 4);
	return v;
}

float sv_get_f32(void)
{
	float v;

	sv_get(&v, 4);
	return v;
}

double sv_get_f64(void)
{
	double v;

	sv_get(&v, 8);
	return v;
}

/* ---- CRC-32 ------------------------------------------------------------- */

uint32_t sv_crc32(uint32_t crc, const void *p, uint32_t n)
{
	static const uint32_t nib[16] = {
		0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u,
		0x4DB26158u, 0x5005713Cu, 0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
		0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu
	};
	const uint8_t *s = p;

	crc = ~crc;
	while (n--)
	{
		crc ^= *s++;
		crc = (crc >> 4) ^ nib[crc & 15];
		crc = (crc >> 4) ^ nib[crc & 15];
	}
	return ~crc;
}

/* ---- delta rows ----------------------------------------------------------- */

/* Per group of 8 words a mask byte (bit k: word k differs from ref), then
 * the differing words; the n % 4 tail bytes as they are. With room for
 * the worst case the row goes straight into the staging, word loads when
 * both rows are word-aligned (component rows, private blocks). */
void sv_put_delta(const void *p, const void *ref, uint32_t n)
{
	const uint8_t *s = p, *r = ref;
	uint32_t w = n / 4, g, k;

	if (!V.over && V.cap - V.pos >= (w + 7) / 8 + 4 * w + (n & 3u))
	{
		uint8_t *o = V.raw + V.pos;

		if (!(((uintptr_t)s | (uintptr_t)r) & 3u))
		{
			const uint32_t *a = (const uint32_t *)p, *b = (const uint32_t *)ref;

			for (g = 0; g < w; g += 8)
			{
				uint32_t e = w - g < 8 ? w - g : 8, m = 0;
				uint8_t *mo = o++;

				for (k = 0; k < e; k++)
				{
					uint32_t v = a[g + k];

					if (v != (b ? b[g + k] : 0u))
					{
						m |= 1u << k;
						memcpy(o, &v, 4);
						o += 4;
					}
				}
				*mo = (uint8_t)m;
			}
		}
		else
			for (g = 0; g < w; g += 8)
			{
				uint32_t e = w - g < 8 ? w - g : 8, m = 0;
				uint8_t *mo = o++;

				for (k = 0; k < e; k++)
				{
					uint32_t v, x = 0;

					memcpy(&v, s + 4 * (g + k), 4);
					if (r)
						memcpy(&x, r + 4 * (g + k), 4);
					if (v != x)
					{
						m |= 1u << k;
						memcpy(o, &v, 4);
						o += 4;
					}
				}
				*mo = (uint8_t)m;
			}
		memcpy(o, s + 4 * w, n & 3u);
		V.pos = (uint32_t)(o + (n & 3u) - V.raw);
		return;
	}
	/* no room for it: what it would need (the snapshot fails as too big) */
	for (g = 0; g < w; g += 8)
	{
		uint32_t m = 0, e = w - g < 8 ? w - g : 8, b = 0;

		for (k = 0; k < e; k++)
		{
			uint32_t a;

			memcpy(&a, s + 4 * (g + k), 4);
			if (r)
				memcpy(&b, r + 4 * (g + k), 4);
			if (a != b)
				m |= 1u << k;
		}
		sv_u8(m);
		for (k = 0; k < e; k++)
			if (m & (1u << k))
				sv_put(s + 4 * (g + k), 4);
	}
	sv_put(s + 4 * w, n & 3u);
}

void sv_get_delta(void *p, const void *ref, uint32_t n, uint8_t *mask)
{
	uint8_t *d = p;
	const uint8_t *r = ref;
	uint32_t w = n / 4, g, k;

	for (g = 0; g < w; g += 8)
	{
		uint32_t m = sv_get_u8(), e = w - g < 8 ? w - g : 8;

		if (m >> e)
			sv_fail("delta mask");
		if (mask)
			mask[g / 8] = (uint8_t)m;
		for (k = 0; k < e; k++)
		{
			uint8_t *o = d + 4 * (g + k);

			if (m & (1u << k))
				sv_get(o, 4);
			else if (!r)
				memset(o, 0, 4);
			else if (r != d)
				memcpy(o, r + 4 * (g + k), 4);
		}
	}
	sv_get(d + 4 * w, n & 3u);
}
