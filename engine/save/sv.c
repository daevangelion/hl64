/* SPDX-License-Identifier: MIT */
/*
 * sv.c -- the save stream (sv.h, sv_int.h): sections put their bytes into
 * a flat staging buffer (plain copies: the snapshot frame stays short); a
 * coder then turns the staged state into the slot image a time budget at
 * a time over the following frames. A load decodes the image back into
 * the staging buffer in steps, and the sections get their bytes from it.
 *
 * Image format: the state's size (4 bytes, big-endian), then one binary
 * range-coded stream (32-bit range, the carries go into the bytes before;
 * 4 bytes flush it) of LZ tokens, as in the published LZMA format. Every
 * decision is a bit with an adaptive 11-bit probability (moved 1/32 of
 * the way to each bit coded), most chosen by the last tokens' kinds (12
 * states):
 *   literal    is_match 0, the byte through a bit tree; right after a match
 *              or rep through trees picked by the byte at the last
 *              distance's bits until one differs;
 *   match      is_match 1, is_rep 0, the length (2..273: choice bits, then
 *              a 3 / 3 / 8-bit tree), the distance (1..WIN): a 5-bit slot
 *              tree by min(length, 5), then the slot's footer (a reverse
 *              tree below slot 14, above it direct bits and a 4-bit
 *              reverse tree); the distance goes to the front of the last 4;
 *   rep i      is_match 1, is_rep 1, rep_g0 / g1 / g2 pick one of the last
 *              4 distances (it moves to the front), then the length;
 *   short rep  rep 0 with rep0_long 0: one byte at the last distance.
 * The stream ends where the state's size is reached.
 *
 * The coder parses the staged state once; the whole state is in memory
 * and the parse depends only on it, so the image does not depend on how
 * the coding is split over frames. Matches come from 4-byte hash chains
 * (2048 heads, 8 KB of links, at most CHAIN_DEPTH candidates) and the
 * last 3-byte occurrence; at each position the token (literal, short
 * rep, rep, one of the match lengths found) that saves the most bits
 * against LIT16 literals, priced from the current probabilities, unless
 * a one-byte token and the best token one byte on save more.
 */
#include <string.h>

#include "engine/core/log.h"
#include "engine/core/sys.h"
#include "engine/save/sv.h"
#include "engine/save/sv_int.h"

#define WIN		8192u		/* match distances 1..WIN */
#define WMASK		(WIN - 1u)
#define HBITS		11		/* 4-byte hash heads */
#define NHEAD		(1u << HBITS)
#define H3BITS		10		/* 3-byte hash heads */
#define NHEAD3		(1u << H3BITS)
#define MINLEN		3u		/* a new distance's shortest match */
#define MAXLEN		273u		/* the length coder's 2..273 */
#define CHAIN_DEPTH	16		/* chain candidates tried per position */
#define NICE		32u		/* a token this long is taken without the lazy look */
#define NPAIR		4		/* match lengths kept per position */
#define LIT16		104		/* the parse's literal estimate: 6.5 bits, in 1/16 bits */
#define PBITS		11		/* probabilities: 11 bits */
#define PONE		(1u << PBITS)
#define PMOVE		5		/* a bit moves its probability 1/32 of the way */
#define TOP		(1u << 24)	/* the range coder's normalisation bound */
#define NSTATE		12		/* the last tokens' kinds: < 7 a literal last */
#define SLOT_BITS	5		/* distance slots: distances up to 2^16 */
#define END_SLOT	14		/* slots from here: direct bits + 4 modelled */
#define K_LIT		0u		/* the parse's token kinds: a literal, */
#define K_REP		1u		/* + i: rep i (length 1: the short rep), */
#define K_MAIN		5u		/* + j: match pair j */
#define CHUNK		128u		/* bytes coded / tokens decoded between clock reads (a
					 * step overruns its budget by one chunk at most) */
#define PARTS		96		/* sv_part marks (test builds) */

typedef char sv_work_fits[(NHEAD + NHEAD3 + WIN) * 2u <= SV_WORK_BYTES ? 1 : -1];

typedef uint16_t prob_t;

typedef struct
{
	prob_t	choice, choice2;
	prob_t	low[8], mid[8], high[256];
} lenmodel_t;

typedef struct
{
	prob_t		is_match[NSTATE], is_rep[NSTATE];
	prob_t		rep_g0[NSTATE], rep_g1[NSTATE], rep_g2[NSTATE], rep0_long[NSTATE];
	prob_t		lit[0x300];	/* [1, 256) plain; 256 + 256 * bit: against the byte at rep[0] */
	prob_t		slot[4][1u << SLOT_BITS];
	prob_t		foot[124];	/* slots 4..13: reverse trees of 1..5 bits */
	prob_t		align[16];
	lenmodel_t	mlen, rlen;
} probs_t;

/* matches at a position: longer and farther each */
typedef struct
{
	uint32_t	n, len[NPAIR], dist[NPAIR];
} pairs_t;

static struct {
	/* staging: the writer's buffer, the reader's after the decode */
	uint8_t		*raw;
	uint32_t	cap, pos, len;
	uint8_t		over, failed;
	const char	*why;
	uint8_t		*scratch;	/* sv_work_area: the coder's work area while it is idle */
	/* coder: raw[0, len) -> out (the matcher in the work area) */
	uint16_t	*head;		/* [NHEAD] by 4-byte hash: the last position + 1 (low 16 bits), 0 none */
	uint16_t	*head3;		/* [NHEAD3] the same by 3-byte hash */
	uint16_t	*prev;		/* [WIN] by position: distance back to its 4-byte hash's previous one, 0 none */
	uint32_t	enc;		/* bytes parsed */
	uint8_t		have_next;	/* the matches at enc are known: pairs[cur] */
	uint8_t		cur;
	pairs_t		pairs[2];	/* at enc and (the lazy look) enc + 1 */
	uint8_t		cost[PONE >> 4];	/* a bit's price (1/16 bits) by its probability / 16 */
	uint8_t		*out;
	uint32_t	ocap;
	uint32_t	need;		/* image bytes, written or not (past ocap) */
	uint32_t	low;
	/* both: the range coder between steps, the token models */
	uint32_t	range, code;
	uint32_t	rep[4];		/* the last distances, the latest first */
	uint32_t	st;		/* the last tokens' kinds */
	probs_t		m;
	uint8_t		started, done;
	/* decoder: in[0, inn) -> raw[0, len) */
	const uint8_t	*in;
	uint32_t	inn, at;
#ifdef HL64_TEST
	const char	*part_name[PARTS];
	uint32_t	part_raw[PARTS], part_coded[PARTS];
	uint64_t	part_us[PARTS], end_us;	/* the staging clock at each mark, at the end */
	uint64_t	check_t0;	/* sv_check_time(1)'s clock */
	uint32_t	check_us;	/* the check windows so far, left out of the clock */
	int		nparts, part_at;
#endif
} V;

/* first probability of each footer slot 4..13 in probs_t.foot */
static const uint8_t foot_at[10] = { 0, 2, 4, 8, 12, 20, 28, 44, 60, 92 };

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

/* ---- token models ------------------------------------------------------------ */

static void model_reset(void)
{
	prob_t *p = (prob_t *)&V.m;
	uint32_t i;

	for (i = 0; i < sizeof(V.m) / sizeof(prob_t); i++)
		p[i] = PONE / 2;
	V.rep[0] = V.rep[1] = V.rep[2] = V.rep[3] = 1;
	V.st = 0;
}

static uint32_t st_lit(uint32_t s)
{
	return s < 4 ? 0 : s < 10 ? s - 3 : s - 6;
}

#define ST_MATCH(s)	((s) < 7 ? 7u : 10u)
#define ST_REP(s)	((s) < 7 ? 8u : 11u)
#define ST_SHORT(s)	((s) < 7 ? 9u : 11u)

/* The slot of d = distance - 1: d itself below 4, else twice its top
 * bit's index plus the bit under it. */
static uint32_t dist_slot(uint32_t d)
{
	uint32_t h = 1;

	if (d < 4)
		return d;
	while (d >> (h + 1))
		h++;
	return 2 * h + (d >> (h - 1) & 1u);
}

/* ---- coder: range coder ------------------------------------------------------ */

static void put_byte(uint32_t b)
{
	if (V.need < V.ocap)
		V.out[V.need] = (uint8_t)b;
	V.need++;
}

/* low += add, a carry going into the bytes written (the coded value stays
 * below 1: it stops inside the range-coded bytes) */
static void add_low(uint32_t add)
{
	uint32_t l = V.low + add, i = V.need;

	if (l < V.low && V.need <= V.ocap)
		while (i > 4 && !++V.out[--i])
			;
	V.low = l;
}

static void enc_norm(void)
{
	while (V.range < TOP)
	{
		put_byte(V.low >> 24);
		V.low <<= 8;
		V.range <<= 8;
	}
}

static void enc_bit(prob_t *p, uint32_t bit)
{
	uint32_t bound = (V.range >> PBITS) * *p;

	if (bit)
	{
		add_low(bound);
		V.range -= bound;
		*p -= *p >> PMOVE;
	}
	else
	{
		V.range = bound;
		*p += (PONE - *p) >> PMOVE;
	}
	enc_norm();
}

/* n bits of v, high first, at even odds */
static void enc_direct(uint32_t v, uint32_t n)
{
	while (n--)
	{
		V.range >>= 1;
		if (v >> n & 1u)
			add_low(V.range);
		enc_norm();
	}
}

/* n bits of v through the tree p[1, 2^n): high first */
static void enc_tree(prob_t *p, uint32_t n, uint32_t v)
{
	uint32_t m = 1;

	while (n--)
	{
		uint32_t b = v >> n & 1u;

		enc_bit(p + m, b);
		m = m << 1 | b;
	}
}

/* ... low first */
static void enc_rtree(prob_t *p, uint32_t n, uint32_t v)
{
	uint32_t m = 1;

	while (n--)
	{
		uint32_t b = v & 1u;

		v >>= 1;
		enc_bit(p + m, b);
		m = m << 1 | b;
	}
}

/* ---- coder: tokens ------------------------------------------------------------ */

static void enc_len(lenmodel_t *lm, uint32_t len)
{
	len -= 2;
	if (len < 8)
	{
		enc_bit(&lm->choice, 0);
		enc_tree(lm->low, 3, len);
		return;
	}
	enc_bit(&lm->choice, 1);
	if (len < 16)
	{
		enc_bit(&lm->choice2, 0);
		enc_tree(lm->mid, 3, len - 8);
		return;
	}
	enc_bit(&lm->choice2, 1);
	enc_tree(lm->high, 8, len - 16);
}

static void enc_dist(uint32_t dist, uint32_t len)
{
	uint32_t d = dist - 1, s = dist_slot(d), n, base;

	enc_tree(V.m.slot[len < 5 ? len - 2 : 3], SLOT_BITS, s);
	if (s < 4)
		return;
	n = (s >> 1) - 1;
	base = (2u | (s & 1u)) << n;
	if (s < END_SLOT)
		enc_rtree(V.m.foot + foot_at[s - 4], n, d - base);
	else
	{
		enc_direct((d - base) >> 4, n - 4);
		enc_rtree(V.m.align, 4, (d - base) & 15u);
	}
}

static void put_lit(uint32_t p)
{
	prob_t *lp = V.m.lit;
	uint32_t c = V.raw[p];

	enc_bit(&V.m.is_match[V.st], 0);
	if (V.st >= 7)
	{
		uint32_t mb = V.raw[p - V.rep[0]], m = 1, i = 8, same = 1;

		while (i--)
		{
			uint32_t b = c >> i & 1u;

			if (same)
			{
				uint32_t x = mb >> i & 1u;

				enc_bit(lp + 0x100 + (x << 8) + m, b);
				same = b == x;
			}
			else
				enc_bit(lp + m, b);
			m = m << 1 | b;
		}
	}
	else
		enc_tree(lp, 8, c);
	V.st = st_lit(V.st);
}

static void put_match(uint32_t len, uint32_t dist)
{
	enc_bit(&V.m.is_match[V.st], 1);
	enc_bit(&V.m.is_rep[V.st], 0);
	enc_len(&V.m.mlen, len);
	enc_dist(dist, len);
	V.rep[3] = V.rep[2];
	V.rep[2] = V.rep[1];
	V.rep[1] = V.rep[0];
	V.rep[0] = dist;
	V.st = ST_MATCH(V.st);
}

/* len bytes at the i-th last distance (len 1: the short rep) */
static void put_rep(uint32_t i, uint32_t len)
{
	uint32_t d = V.rep[i];

	enc_bit(&V.m.is_match[V.st], 1);
	enc_bit(&V.m.is_rep[V.st], 1);
	if (!i)
	{
		enc_bit(&V.m.rep_g0[V.st], 0);
		enc_bit(&V.m.rep0_long[V.st], len > 1);
	}
	else
	{
		enc_bit(&V.m.rep_g0[V.st], 1);
		if (i == 1)
			enc_bit(&V.m.rep_g1[V.st], 0);
		else
		{
			enc_bit(&V.m.rep_g1[V.st], 1);
			enc_bit(&V.m.rep_g2[V.st], i == 3);
		}
		for (; i; i--)
			V.rep[i] = V.rep[i - 1];
		V.rep[0] = d;
	}
	if (len == 1)
	{
		V.st = ST_SHORT(V.st);
		return;
	}
	enc_len(&V.m.rlen, len);
	V.st = ST_REP(V.st);
}

/* ---- coder: matcher ------------------------------------------------------------ */

static uint32_t hash3(uint32_t p)
{
	uint32_t v = (uint32_t)V.raw[p] << 16 | (uint32_t)V.raw[p + 1] << 8 | V.raw[p + 2];

	return (v * 2654435761u) >> (32 - H3BITS);
}

static uint32_t hash4(uint32_t p)
{
	uint32_t v = (uint32_t)V.raw[p] << 24 | (uint32_t)V.raw[p + 1] << 16
		| (uint32_t)V.raw[p + 2] << 8 | V.raw[p + 3];

	return (v * 2654435761u) >> (32 - HBITS);
}

/* A head (a position + 1's low 16 bits, 0 none) back to the latest
 * position + 1 at or before p with those bits (a stale one gives some
 * other position: its bytes are compared like any candidate's). */
static uint32_t head_at(uint32_t s, uint32_t p)
{
	return s ? p - ((p - s) & 0xFFFFu) : 0;
}

/* Chain position p (3 bytes left at least): *c3 the 3-byte head's last
 * position + 1; the 4-byte chain's previous one + 1 (0 none). */
static uint32_t insert(uint32_t p, uint32_t *c3)
{
	uint32_t h = hash3(p), c = 0;

	*c3 = head_at(V.head3[h], p);
	V.head3[h] = (uint16_t)(p + 1);
	if (V.len - p >= 4)
	{
		h = hash4(p);
		c = head_at(V.head[h], p);
		V.prev[p & WMASK] = (uint16_t)(c && p - (c - 1) < WIN ? p - (c - 1) : 0);
		V.head[h] = (uint16_t)(p + 1);
	}
	return c;
}

static void add_pair(pairs_t *m, uint32_t len, uint32_t dist)
{
	if (m->n == NPAIR)
	{
		/* the shortest goes */
		memmove(m->len, m->len + 1, (NPAIR - 1) * sizeof(m->len[0]));
		memmove(m->dist, m->dist + 1, (NPAIR - 1) * sizeof(m->dist[0]));
		m->n--;
	}
	m->len[m->n] = len;
	m->dist[m->n++] = dist;
}

/* The matches at p: the last 3-byte occurrence's, then those among
 * CHAIN_DEPTH chain candidates, each longer than the one before; p is
 * chained. */
static void find(uint32_t p, pairs_t *m)
{
	uint32_t avail = V.len - p, max, best = MINLEN - 1, c, c3, depth = CHAIN_DEPTH;

	m->n = 0;
	if (avail < MINLEN)
		return;
	c = insert(p, &c3);
	max = avail < MAXLEN ? avail : MAXLEN;
	if (c3 && p - (c3 - 1) <= WIN)
	{
		uint32_t q = c3 - 1, l = 0;

		while (l < max && V.raw[q + l] == V.raw[p + l])
			l++;
		if (l > best)
		{
			best = l;
			add_pair(m, l, p - q);
			if (l == max)
				return;
		}
	}
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
				add_pair(m, l, p - q);
				if (l == max)
					break;
			}
		}
		d = V.prev[q & WMASK];
		if (!d || d > q)
			break;
		c = q - d + 1;
	}
}

/* ---- coder: prices (1/16 bits) -------------------------------------------- */

/* cost[i]: 16 * -log2(p / PONE) for the middle p of bucket i (the
 * fraction's 4 bits by squaring) */
static void cost_init(void)
{
	uint32_t i;

	for (i = 0; i < PONE >> 4; i++)
	{
		uint32_t x = (i << 4) + 8, e = 0, f = 0, k;

		while (x < 0x8000u)
		{
			x <<= 1;
			e++;
		}
		for (k = 0; k < 4; k++)
		{
			x = x * x >> 15;
			f <<= 1;
			if (x >= 0x10000u)
			{
				x >>= 1;
				f |= 1;
			}
		}
		V.cost[i] = (uint8_t)(PBITS * 16 - ((15 - e) * 16 + f));
	}
}

static uint32_t pr_bit(const prob_t *p, uint32_t b)
{
	return V.cost[(b ? PONE - *p : *p) >> 4];
}

static uint32_t pr_tree(const prob_t *p, uint32_t n, uint32_t v)
{
	uint32_t m = 1, c = 0;

	while (n--)
	{
		uint32_t b = v >> n & 1u;

		c += pr_bit(p + m, b);
		m = m << 1 | b;
	}
	return c;
}

static uint32_t pr_rtree(const prob_t *p, uint32_t n, uint32_t v)
{
	uint32_t m = 1, c = 0;

	while (n--)
	{
		uint32_t b = v & 1u;

		v >>= 1;
		c += pr_bit(p + m, b);
		m = m << 1 | b;
	}
	return c;
}

static uint32_t pr_len(const lenmodel_t *lm, uint32_t len)
{
	len -= 2;
	if (len < 8)
		return pr_bit(&lm->choice, 0) + pr_tree(lm->low, 3, len);
	if (len < 16)
		return pr_bit(&lm->choice, 1) + pr_bit(&lm->choice2, 0) + pr_tree(lm->mid, 3, len - 8);
	return pr_bit(&lm->choice, 1) + pr_bit(&lm->choice2, 1) + pr_tree(lm->high, 8, len - 16);
}

static uint32_t pr_dist(uint32_t dist, uint32_t len)
{
	uint32_t d = dist - 1, s = dist_slot(d), c, n, base;

	c = pr_tree(V.m.slot[len < 5 ? len - 2 : 3], SLOT_BITS, s);
	if (s < 4)
		return c;
	n = (s >> 1) - 1;
	base = (2u | (s & 1u)) << n;
	if (s < END_SLOT)
		return c + pr_rtree(V.m.foot + foot_at[s - 4], n, d - base);
	return c + (n - 4) * 16 + pr_rtree(V.m.align, 4, (d - base) & 15u);
}

/* the literal at p after state st */
static uint32_t pr_lit(uint32_t p, uint32_t st)
{
	const prob_t *lp = V.m.lit;
	uint32_t c = V.raw[p], m = 1, i = 8, same = st >= 7, mb = 0, r;

	r = pr_bit(&V.m.is_match[st], 0);
	if (same)
		mb = V.raw[p - V.rep[0]];
	while (i--)
	{
		uint32_t b = c >> i & 1u;

		if (same)
		{
			uint32_t x = mb >> i & 1u;

			r += pr_bit(lp + 0x100 + (x << 8) + m, b);
			same = b == x;
		}
		else
			r += pr_bit(lp + m, b);
		m = m << 1 | b;
	}
	return r;
}

/* rep i of len (1: the short rep) after state st */
static uint32_t pr_rep(uint32_t st, uint32_t i, uint32_t len)
{
	uint32_t c = pr_bit(&V.m.is_match[st], 1) + pr_bit(&V.m.is_rep[st], 1);

	if (!i)
		c += pr_bit(&V.m.rep_g0[st], 0) + pr_bit(&V.m.rep0_long[st], len > 1);
	else if (i == 1)
		c += pr_bit(&V.m.rep_g0[st], 1) + pr_bit(&V.m.rep_g1[st], 0);
	else
		c += pr_bit(&V.m.rep_g0[st], 1) + pr_bit(&V.m.rep_g1[st], 1)
			+ pr_bit(&V.m.rep_g2[st], i == 3);
	return len > 1 ? c + pr_len(&V.m.rlen, len) : c;
}

static uint32_t pr_match(uint32_t st, uint32_t len, uint32_t dist)
{
	return pr_bit(&V.m.is_match[st], 1) + pr_bit(&V.m.is_rep[st], 0) + pr_len(&V.m.mlen, len)
		+ pr_dist(dist, len);
}

/* ---- coder: parse ------------------------------------------------------------- */

/* The best one-byte token at p after state st: K_LIT or K_REP (the short
 * rep); *gain: the 1/16 bits it saves against a LIT16 literal. */
static uint32_t best_one(uint32_t p, uint32_t st, int32_t *gain)
{
	int32_t g = LIT16 - (int32_t)pr_lit(p, st);

	if (V.rep[0] <= p && V.raw[p] == V.raw[p - V.rep[0]])
	{
		int32_t gs = LIT16 - (int32_t)pr_rep(st, 0, 1);

		if (gs > g)
		{
			*gain = gs;
			return K_REP;
		}
	}
	*gain = g;
	return K_LIT;
}

/* The best token at p after state st (m: the matches at p): its kind,
 * *len, *gain (against len LIT16 literals). */
static uint32_t best_token(uint32_t p, uint32_t st, const pairs_t *m, uint32_t *len,
	int32_t *gain)
{
	uint32_t avail = V.len - p, max = avail < MAXLEN ? avail : MAXLEN;
	uint32_t kind = best_one(p, st, gain), i, j;

	*len = 1;
	for (i = 0; i < 4; i++)
	{
		uint32_t d = V.rep[i], l = 0;
		int32_t g;

		if (d > p || max < 2 || V.raw[p] != V.raw[p - d] || V.raw[p + 1] != V.raw[p + 1 - d])
			continue;
		while (l < max && V.raw[p + l] == V.raw[p + l - d])
			l++;
		g = (int32_t)(l * LIT16) - (int32_t)pr_rep(st, i, l);
		if (g > *gain)
		{
			*gain = g;
			*len = l;
			kind = K_REP + i;
		}
	}
	for (j = 0; j < m->n; j++)
	{
		uint32_t l = m->len[j], d = m->dist[j];
		int32_t g;

		if (d == V.rep[0] || d == V.rep[1] || d == V.rep[2] || d == V.rep[3])
			continue;	/* a rep above */
		g = (int32_t)(l * LIT16) - (int32_t)pr_match(st, l, d);
		if (g > *gain)
		{
			*gain = g;
			*len = l;
			kind = K_MAIN + j;
		}
	}
	return kind;
}

static void put_token(uint32_t p, uint32_t kind, uint32_t len, const pairs_t *m)
{
	if (kind == K_LIT)
		put_lit(p);
	else if (kind < K_MAIN)
		put_rep(kind - K_REP, len);
	else
		put_match(len, m->dist[kind - K_MAIN]);
}

#ifdef HL64_TEST
/* Parts whose first byte the coder reached: the image bytes so far. */
static void part_reach(void)
{
	while (V.part_at < V.nparts && V.part_raw[V.part_at] <= V.enc)
		V.part_coded[V.part_at++] = V.need;
}
#endif

/* Parse and code raw from enc up to until (a match may run past it). */
static void parse(uint32_t until)
{
	while (V.enc < until)
	{
		uint32_t p = V.enc, len, kind, i, ins = 1, c3;
		pairs_t *m = &V.pairs[V.cur], *m2 = &V.pairs[V.cur ^ 1u];
		int32_t gain;

#ifdef HL64_TEST
		part_reach();
#endif
		if (!V.have_next)
			find(p, m);
		V.have_next = 0;
		kind = best_token(p, V.st, m, &len, &gain);
		if (len > 1 && len < NICE && p + 1 < V.len)
		{
			uint32_t k1, l2;
			int32_t g1, g2;

			k1 = best_one(p, V.st, &g1);
			find(p + 1, m2);
			ins = 2;
			best_token(p + 1, k1 == K_LIT ? st_lit(V.st) : ST_SHORT(V.st), m2, &l2, &g2);
			if (g1 + g2 > gain)
			{
				/* better one byte on: this byte goes alone */
				put_token(p, k1, 1, m);
				V.enc++;
				V.cur ^= 1u;
				V.have_next = 1;	/* p + 1 is chained */
				continue;
			}
		}
		put_token(p, kind, len, m);
		for (i = ins; i < len; i++)
			if (V.len - (p + i) >= MINLEN)
				insert(p + i, &c3);
		V.enc = p + len;
	}
}

void sv_code_begin(void *out, uint32_t cap, void *work)
{
	V.head = work;
	V.head3 = V.head + NHEAD;
	V.prev = V.head3 + NHEAD3;
	memset(work, 0, (NHEAD + NHEAD3 + WIN) * sizeof(uint16_t));
	V.enc = 0;
	V.have_next = 0;
	V.cur = 0;
	cost_init();
	V.out = out;
	V.ocap = cap;
	V.need = 0;
	put_byte(V.len >> 24);
	put_byte(V.len >> 16);
	put_byte(V.len >> 8);
	put_byte(V.len);
	V.low = 0;
	V.range = 0xFFFFFFFFu;
	model_reset();
	V.done = 0;
#ifdef HL64_TEST
	V.part_at = 0;
#endif
}

int sv_code_step(uint32_t budget_us)
{
	uint64_t t0 = sys_time_us();

	while (!V.done)
	{
		if (V.enc >= V.len)
		{
			int i;

			for (i = 0; i < 4; i++)
			{
				put_byte(V.low >> 24);
				V.low <<= 8;
			}
			V.done = 1;
			break;
		}
		parse(V.len - V.enc > CHUNK ? V.enc + CHUNK : V.len);
		if (V.need > V.ocap)
			return 0;	/* too big: no point going on */
		if (sys_time_us() - t0 >= budget_us)
			return !V.done;
	}
	return 0;
}

uint32_t sv_code_end(void)
{
#ifdef HL64_TEST
	int i;

	part_reach();
	for (i = 0; i < V.nparts && V.done; i++)
	{
		uint32_t re = i + 1 < V.nparts ? V.part_raw[i + 1] : V.len;
		uint32_t ce = i + 1 < V.nparts ? V.part_coded[i + 1] : V.need;
		uint64_t ue = i + 1 < V.nparts ? V.part_us[i + 1] : V.end_us;

		log_printf("save: part %s %lu B -> %lu B, staged in %lu us\n", V.part_name[i],
			(unsigned long)(re - V.part_raw[i]), (unsigned long)(ce - V.part_coded[i]),
			(unsigned long)(ue - V.part_us[i]));
	}
#endif
	return !V.done || V.need > V.ocap ? 0 : V.need;
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

/* The range decoder's state within a step (a local: registers). */
typedef struct
{
	uint32_t	range, code, bad;
	const uint8_t	*in, *end;
} rd_t;

static void dec_norm(rd_t *r)
{
	while (r->range < TOP)
	{
		r->range <<= 8;
		r->code <<= 8;
		if (r->in < r->end)
			r->code |= *r->in++;
		else
			r->bad = 1;	/* past the image's end */
	}
}

static uint32_t dec_bit(rd_t *r, prob_t *p)
{
	uint32_t bound = (r->range >> PBITS) * *p, b;

	if (r->code < bound)
	{
		r->range = bound;
		*p += (PONE - *p) >> PMOVE;
		b = 0;
	}
	else
	{
		r->code -= bound;
		r->range -= bound;
		*p -= *p >> PMOVE;
		b = 1;
	}
	dec_norm(r);
	return b;
}

static uint32_t dec_direct(rd_t *r, uint32_t n)
{
	uint32_t v = 0;

	while (n--)
	{
		r->range >>= 1;
		v <<= 1;
		if (r->code >= r->range)
		{
			r->code -= r->range;
			v |= 1;
		}
		dec_norm(r);
	}
	return v;
}

static uint32_t dec_tree(rd_t *r, prob_t *p, uint32_t n)
{
	uint32_t m = 1, i;

	for (i = 0; i < n; i++)
		m = m << 1 | dec_bit(r, p + m);
	return m - (1u << n);
}

static uint32_t dec_rtree(rd_t *r, prob_t *p, uint32_t n)
{
	uint32_t m = 1, v = 0, i;

	for (i = 0; i < n; i++)
	{
		uint32_t b = dec_bit(r, p + m);

		m = m << 1 | b;
		v |= b << i;
	}
	return v;
}

static uint32_t dec_len(rd_t *r, lenmodel_t *lm)
{
	if (!dec_bit(r, &lm->choice))
		return 2 + dec_tree(r, lm->low, 3);
	if (!dec_bit(r, &lm->choice2))
		return 10 + dec_tree(r, lm->mid, 3);
	return 18 + dec_tree(r, lm->high, 8);
}

static uint32_t dec_dist(rd_t *r, uint32_t len)
{
	uint32_t s = dec_tree(r, V.m.slot[len < 5 ? len - 2 : 3], SLOT_BITS), n, base;

	if (s < 4)
		return s + 1;
	n = (s >> 1) - 1;
	base = (2u | (s & 1u)) << n;
	if (s < END_SLOT)
		return base + dec_rtree(r, V.m.foot + foot_at[s - 4], n) + 1;
	base += dec_direct(r, n - 4) << 4;
	return base + dec_rtree(r, V.m.align, 4) + 1;
}

/* One token; -1 corrupt. */
static int decode_token(rd_t *r)
{
	uint32_t p = V.pos, len, dist, i;

	if (!dec_bit(r, &V.m.is_match[V.st]))
	{
		prob_t *lp = V.m.lit;
		uint32_t m = 1;

		if (V.st >= 7)
		{
			uint32_t mb = V.raw[p - V.rep[0]], same = 1;

			for (i = 8; i--;)
			{
				uint32_t b;

				if (same)
				{
					uint32_t x = mb >> i & 1u;

					b = dec_bit(r, lp + 0x100 + (x << 8) + m);
					same = b == x;
				}
				else
					b = dec_bit(r, lp + m);
				m = m << 1 | b;
			}
		}
		else
			m = dec_tree(r, lp, 8);
		V.raw[V.pos++] = (uint8_t)m;
		V.st = st_lit(V.st);
		return 0;
	}
	if (!dec_bit(r, &V.m.is_rep[V.st]))
	{
		len = dec_len(r, &V.m.mlen);
		V.rep[3] = V.rep[2];
		V.rep[2] = V.rep[1];
		V.rep[1] = V.rep[0];
		V.rep[0] = dec_dist(r, len);
		V.st = ST_MATCH(V.st);
	}
	else
	{
		if (!dec_bit(r, &V.m.rep_g0[V.st]))
			i = 0;
		else if (!dec_bit(r, &V.m.rep_g1[V.st]))
			i = 1;
		else
			i = 2 + dec_bit(r, &V.m.rep_g2[V.st]);
		if (!i && !dec_bit(r, &V.m.rep0_long[V.st]))
		{
			len = 1;
			V.st = ST_SHORT(V.st);
		}
		else
		{
			len = dec_len(r, &V.m.rlen);
			V.st = ST_REP(V.st);
		}
		for (dist = V.rep[i]; i; i--)
			V.rep[i] = V.rep[i - 1];
		V.rep[0] = dist;
	}
	dist = V.rep[0];
	if (dist > p || len > V.len - p)
		return -1;
	for (i = 0; i < len; i++)
		V.raw[p + i] = V.raw[p + i - dist];
	V.pos = p + len;
	return 0;
}

int sv_decode_step(uint32_t budget_us)
{
	uint64_t t0 = sys_time_us();
	rd_t r;

	if (!V.started)
	{
		uint32_t i;

		if (V.inn < 8)
			goto corrupt;
		for (i = 0; i < 4; i++)
			V.len = V.len << 8 | V.in[i];
		for (; i < 8; i++)
			V.code = V.code << 8 | V.in[i];
		if (V.len > V.cap)
			goto corrupt;
		V.at = 8;
		V.range = 0xFFFFFFFFu;
		model_reset();
		V.started = 1;
	}
	r.range = V.range;
	r.code = V.code;
	r.bad = 0;
	r.in = V.in + V.at;
	r.end = V.in + V.inn;
	for (;;)
	{
		uint32_t k;

		for (k = 0; k < CHUNK; k++)
		{
			if (V.pos >= V.len)
			{
				if (r.in != r.end)
					goto corrupt;	/* bytes left over */
				V.done = 1;
				return 0;
			}
			if (decode_token(&r) || r.bad)
				goto corrupt;
		}
		if (sys_time_us() - t0 >= budget_us)
		{
			V.range = r.range;
			V.code = r.code;
			V.at = (uint32_t)(r.in - V.in);
			return 1;
		}
	}
corrupt:
	sv_fail("image does not decode");
	return -1;
}

uint32_t sv_decode_end(void)
{
	return V.failed || !V.done ? 0 : V.len;
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
