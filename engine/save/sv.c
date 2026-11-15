/* SPDX-License-Identifier: MIT */
/*
 * sv.c -- the save stream (sv.h): an LZSS coder between the sections the
 * engine and the game put / get and the slot image.
 *
 * Image format: groups of 8 tokens, each group led by a flag byte (MSB
 * first: 0 literal, 1 match). A literal is one byte. A match is a big-
 * endian u16 (len - 3) << 13 | (offset - 1), offset 1..8192 back into the
 * bytes already produced (it may overlap the bytes it produces: runs); a
 * length field of 7 is followed by a byte, length = 10 + that byte. The
 * coder keeps 8 KB of history; matches come from a 4096-entry table of the
 * last position of each 3-byte hash.
 */
#include <string.h>

#include "engine/save/sv.h"
#include "engine/save/sv_int.h"

#define WIN		8192u		/* match offsets 1..WIN */
#define RING		16384u		/* writer history + lookahead */
#define RMASK		(RING - 1u)
#define WMASK		(WIN - 1u)
#define HBITS		12
#define NHEAD		(1u << HBITS)
#define MINLEN		3u
#define MAXLEN		264u		/* 10 + 254 */
#define PEND		1024u		/* writer bytes put, not yet coded, at most */

typedef char sv_work_fits[(RING + NHEAD * 4u <= SV_WORK_BYTES && WIN <= SV_WORK_BYTES) ? 1 : -1];

static struct {
	/* writer */
	uint8_t		*ring;
	uint32_t	*head;		/* position + 1 of the hash's last occurrence, 0 none */
	uint32_t	pos;		/* bytes put */
	uint32_t	enc;		/* bytes coded */
	uint8_t		*out;
	uint32_t	cap, n, flagat;
	uint32_t	need;		/* bytes coded, written or not (past cap) */
	uint8_t		nflag;
	uint8_t		over;
	/* reader */
	const uint8_t	*in;
	uint32_t	inn, at;
	uint8_t		*win;
	uint32_t	rpos, mlen, moff;
	uint8_t		rflags, rnflag;
	uint8_t		failed;
	const char	*why;
} V;

/* ---- writer ------------------------------------------------------------- */

static void out_byte(uint32_t b)
{
	V.need++;
	if (V.n < V.cap)
		V.out[V.n++] = (uint8_t)b;
	else
		V.over = 1;
}

static void token(int match)
{
	if (!V.nflag)
	{
		V.flagat = V.n;
		out_byte(0);
	}
	if (match && V.flagat < V.cap && !V.over)
		V.out[V.flagat] |= (uint8_t)(0x80u >> V.nflag);
	V.nflag = (uint8_t)((V.nflag + 1) & 7);
}

static uint32_t hash3(uint32_t p)
{
	uint32_t v = (uint32_t)V.ring[p & RMASK] << 16 | (uint32_t)V.ring[(p + 1) & RMASK] << 8 |
		V.ring[(p + 2) & RMASK];

	return (v * 2654435761u) >> (32 - HBITS);
}

static void code(uint32_t until)
{
	while (V.enc < until)
	{
		uint32_t avail = V.pos - V.enc, best = 0, boff = 0;

		if (avail >= MINLEN)
		{
			uint32_t h = hash3(V.enc), c = V.head[h];

			V.head[h] = V.enc + 1;
			if (c && V.enc - (c - 1) <= WIN)
			{
				uint32_t max = avail < MAXLEN ? avail : MAXLEN, l = 0;

				c--;
				while (l < max && V.ring[(c + l) & RMASK] == V.ring[(V.enc + l) & RMASK])
					l++;
				if (l >= MINLEN)
				{
					best = l;
					boff = V.enc - c;
				}
			}
		}
		if (best)
		{
			uint32_t lc = best - MINLEN < 7 ? best - MINLEN : 7, t = lc << 13 | (boff - 1), i;

			token(1);
			out_byte(t >> 8);
			out_byte(t & 0xFF);
			if (lc == 7)
				out_byte(best - 10);
			for (i = 1; i < best && V.enc + i + 2 < V.pos; i++)
				V.head[hash3(V.enc + i)] = V.enc + i + 1;
			V.enc += best;
		}
		else
		{
			token(0);
			out_byte(V.ring[V.enc & RMASK]);
			V.enc++;
		}
	}
}

void sv_write_begin(void *out, uint32_t cap, void *work)
{
	memset(&V, 0, sizeof(V));
	V.ring = work;
	V.head = (uint32_t *)((uint8_t *)work + RING);
	memset(V.head, 0, NHEAD * sizeof(uint32_t));
	V.out = out;
	V.cap = cap;
}

uint32_t sv_write_end(uint32_t *raw)
{
	code(V.pos);
	if (raw)
		*raw = V.pos;
	V.ring = NULL;
	return V.over || V.failed ? 0 : V.n;
}

void sv_write_pos(uint32_t *raw, uint32_t *coded)
{
	*raw = V.pos;
	*coded = V.need;
}

void sv_put(const void *p, uint32_t n)
{
	const uint8_t *s = p;

	if (!V.ring)
		return;
	while (n)
	{
		uint32_t take = PEND - (V.pos - V.enc), at = V.pos & RMASK, k;

		if (take > n)
			take = n;
		k = RING - at < take ? RING - at : take;
		memcpy(V.ring + at, s, k);
		memcpy(V.ring, s + k, take - k);
		V.pos += take;
		s += take;
		n -= take;
		if (V.pos - V.enc > MAXLEN)
			code(V.pos - MAXLEN);
	}
}

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

/* ---- reader ------------------------------------------------------------- */

void sv_read_begin(const void *in, uint32_t n, void *work)
{
	memset(&V, 0, sizeof(V));
	V.in = in;
	V.inn = n;
	V.win = work;
}

uint32_t sv_read_pos(void)
{
	return V.rpos;
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

/* The next byte, -1 at the end of the image. */
static int get_byte(void)
{
	uint8_t b;

	if (V.mlen)
	{
		b = V.win[(V.rpos - V.moff) & WMASK];
		V.mlen--;
	}
	else
	{
		if (!V.rnflag)
		{
			if (V.at >= V.inn)
				return -1;
			V.rflags = V.in[V.at++];
			V.rnflag = 8;
		}
		V.rnflag--;
		if (!(V.rflags & 0x80))
		{
			V.rflags = (uint8_t)(V.rflags << 1);
			if (V.at >= V.inn)
				return -1;
			b = V.in[V.at++];
		}
		else
		{
			uint32_t t, len;

			V.rflags = (uint8_t)(V.rflags << 1);
			if (V.at + 2 > V.inn)
				return -1;
			t = (uint32_t)V.in[V.at] << 8 | V.in[V.at + 1];
			V.at += 2;
			len = (t >> 13) + MINLEN;
			if ((t >> 13) == 7)
			{
				if (V.at >= V.inn)
					return -1;
				len = 10u + V.in[V.at++];
			}
			V.moff = (t & 0x1FFFu) + 1u;
			if (V.moff > V.rpos)
				return -1;
			b = V.win[(V.rpos - V.moff) & WMASK];
			V.mlen = len - 1;
		}
	}
	V.win[V.rpos & WMASK] = b;
	V.rpos++;
	return b;
}

void sv_get(void *p, uint32_t n)
{
	uint8_t *d = p;
	uint32_t i;

	if (!V.win || V.failed)
	{
		memset(d, 0, n);
		return;
	}
	for (i = 0; i < n; i++)
	{
		int b = get_byte();

		if (b < 0)
		{
			sv_fail("image ends early");
			memset(d + i, 0, n - i);
			return;
		}
		d[i] = (uint8_t)b;
	}
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
