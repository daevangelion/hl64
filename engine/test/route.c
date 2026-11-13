/* SPDX-License-Identifier: MIT */
/*
 * route.c -- the compiled-route VM (docs/formats/route-bytecode.md,
 * docs/architecture.md 15), frame loop step 13 in test builds.
 *
 * Load: the reader rules of route-bytecode.md (header, sizes, CRC, every
 * op and operand, ids against campaign.rtb's counts, lines, PASS last),
 * rtb_id = the loaded campaign.rtb's build_id. Run: ops in order until a
 * wait op is not yet satisfied; a failure logs `route FAIL <op#>
 * <source>:<line> <mnemonic> <reason>` and exits 1, PASS logs `route
 * PASS` and exits 0.
 *
 * M2 runs the player ops (WAIT, WAIT_MAP, WAIT_NEAR, LOOK, SETPOS,
 * BUTTONS, IMPULSE, GOD, SHOT, STATS, PASS). The entity and record ops
 * (WAIT_MOVED, WAIT_GONE, WAIT_CLASS, WAIT_GLOBAL, WAIT_END, LOOK_AT,
 * DROP) need M3's entities and records: reaching one fails the route.
 */
#include <math.h>
#include <string.h>

#include "engine/core/dbg.h"
#include "engine/core/log.h"
#include "engine/core/sys.h"
#include "engine/in/in.h"
#include "engine/io/io.h"
#include "engine/phys/player.h"
#include "engine/test/route.h"
#include "engine/world/wld.h"

#define RBC_MAGIC	0x52424331u
#define RBC_HDR		48
#define RBC_MAX_BYTES	16384
#define RTB_MAGIC	0x52544231u
#define RTB_DIR_MAX	32

enum {
	OP_WAIT = 1, OP_WAIT_MAP, OP_WAIT_NEAR, OP_WAIT_MOVED, OP_WAIT_GONE, OP_WAIT_CLASS,
	OP_WAIT_GLOBAL, OP_WAIT_END, OP_LOOK, OP_LOOK_AT, OP_SETPOS, OP_BUTTONS, OP_IMPULSE,
	OP_DROP, OP_GOD, OP_SHOT, OP_STATS, OP_PASS, OP_COUNT
};

static const struct {
	const char	*name;
	const char	*kinds;		/* u = u32, f = finite f32, r = f32 >= 0, c = class id */
} ops[OP_COUNT] = {
	[OP_WAIT] = { "WAIT", "u" },
	[OP_WAIT_MAP] = { "WAIT_MAP", "u" },
	[OP_WAIT_NEAR] = { "WAIT_NEAR", "fffru" },
	[OP_WAIT_MOVED] = { "WAIT_MOVED", "u" },
	[OP_WAIT_GONE] = { "WAIT_GONE", "u" },
	[OP_WAIT_CLASS] = { "WAIT_CLASS", "cfffru" },
	[OP_WAIT_GLOBAL] = { "WAIT_GLOBAL", "u" },
	[OP_WAIT_END] = { "WAIT_END", "u" },
	[OP_LOOK] = { "LOOK", "ff" },
	[OP_LOOK_AT] = { "LOOK_AT", "u" },
	[OP_SETPOS] = { "SETPOS", "fffff" },
	[OP_BUTTONS] = { "BUTTONS", "u" },
	[OP_IMPULSE] = { "IMPULSE", "" },
	[OP_DROP] = { "DROP", "cfff" },
	[OP_GOD] = { "GOD", "" },
	[OP_SHOT] = { "SHOT", "" },
	[OP_STATS] = { "STATS", "" },
	[OP_PASS] = { "PASS", "" },
};

#define BUTTON_MASK	(0x3FFFu & ~(1u << 6))

static struct {
	uint8_t		buf[RBC_MAX_BYTES] __attribute__((aligned(16)));
	uint32_t	size, code_ofs, code_end, lines_ofs, str_ofs, str_size;
	uint16_t	nops, src;
	uint8_t		loaded, done;
	uint32_t	pc;		/* byte offset of the current op */
	uint16_t	op;		/* its index */
	uint8_t		started;	/* the current op's start time is set */
	uint64_t	t0;		/* game time the current op started */
} rt;

static uint32_t rd32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint16_t rd16(const uint8_t *p)
{
	return (uint16_t)(p[0] << 8 | p[1]);
}

static float rdf(const uint8_t *p)
{
	uint32_t u = rd32(p);
	float f;

	memcpy(&f, &u, sizeof(f));
	return f;
}

static uint32_t crc32_z(const uint8_t *p, uint32_t n)
{
	uint32_t c = 0xFFFFFFFFu;
	int k;

	while (n--)
	{
		c ^= *p++;
		for (k = 0; k < 8; k++)
			c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
	}
	return ~c;
}

/* campaign.rtb: build_id and the id-space counts the reader checks */
typedef struct {
	uint32_t	build_id;
	uint32_t	maps, targ, clas, glob;
} rtb_ids_t;

static int rtb_read(rtb_ids_t *t)
{
	static uint8_t hdr[32 + 16 * RTB_DIR_MAX] __attribute__((aligned(16)));
	uint32_t size, rom = sys_cart_addr("campaign.rtb", &size), nsect, i;

	memset(t, 0, sizeof(*t));
	if (!rom || size < 32)
		return -1;
	io_read_sync(hdr, rom, 32);
	nsect = rd16(hdr + 20);
	if (rd32(hdr) != RTB_MAGIC || rd16(hdr + 4) != 1 || rd32(hdr + 12) != size
		|| rd32(hdr + 24) != 32 || nsect > RTB_DIR_MAX || 32 + 16 * nsect > size)
		return -1;
	io_read_sync(hdr, rom, (32 + 16 * nsect + 1) & ~1u);
	t->build_id = rd32(hdr + 8);
	for (i = 0; i < nsect; i++)
	{
		const uint8_t *d = hdr + 32 + 16 * i;
		uint32_t cc = rd32(d), n = rd32(d + 12);

		if (cc == 0x4D415053u)		/* MAPS */
			t->maps = n;
		else if (cc == 0x54415247u)	/* TARG */
			t->targ = n;
		else if (cc == 0x434C4153u)	/* CLAS */
			t->clas = n;
		else if (cc == 0x474C4F42u)	/* GLOB */
			t->glob = n;
	}
	return 0;
}

static int bad(const char *path, uint32_t ofs, const char *rule)
{
	log_printf("route: %s: offset %lu: %s\n", path, (unsigned long)ofs, rule);
	return -1;
}

/* a NUL-terminated printable label at str_ofs + o, <= 47 characters */
static int label_ok(uint32_t o)
{
	uint32_t i;

	if (o >= rt.str_size || (o && rt.buf[rt.str_ofs + o - 1]))
		return 0;
	for (i = o; i < rt.str_size && rt.buf[rt.str_ofs + i]; i++)
		if (rt.buf[rt.str_ofs + i] < 0x21 || rt.buf[rt.str_ofs + i] > 0x7E || i - o >= 47)
			return 0;
	return i < rt.str_size;
}

static const char *str_at(uint32_t o)
{
	return (const char *)rt.buf + rt.str_ofs + o;
}

int rt_load(const char *path)
{
	uint32_t rom, size, i, p, lines_size;
	rtb_ids_t ids;
	const uint8_t *b = rt.buf;

	memset(&rt, 0, sizeof(rt));
	rom = sys_cart_addr(path, &size);
	if (!rom)
		return 0;
	if (size < RBC_HDR || size > RBC_MAX_BYTES)
		return bad(path, 0, "size");
	io_read_sync(rt.buf, rom, (size + 1) & ~1u);
	rt.size = size;
	rt.nops = rd16(b + 20);
	rt.code_ofs = rd32(b + 24);
	rt.code_end = rt.code_ofs + rd32(b + 28);
	rt.lines_ofs = rd32(b + 32);
	rt.str_ofs = rd32(b + 36);
	rt.str_size = rd32(b + 40);
	rt.src = rd16(b + 44);
	lines_size = (2u * rt.nops + 3u) & ~3u;
	if (rd32(b) != RBC_MAGIC || rd16(b + 4) != 1 || rd16(b + 6) || rd16(b + 22) || rd16(b + 46))
		return bad(path, 0, "header");
	if (rd32(b + 12) != size)
		return bad(path, 12, "file_size");
	if (crc32_z(b + RBC_HDR, size - RBC_HDR) != rd32(b + 16))
		return bad(path, 16, "crc");
	if (!rt.nops || rt.code_ofs != RBC_HDR || (rd32(b + 28) & 3) || rt.lines_ofs != rt.code_end
		|| rt.str_ofs != rt.lines_ofs + lines_size || rt.str_ofs > size
		|| rt.str_size != size - rt.str_ofs)
		return bad(path, 20, "sections");
	for (i = 2u * rt.nops; i < lines_size; i++)
		if (b[rt.lines_ofs + i])
			return bad(path, rt.lines_ofs + i, "line table padding");
	if (!label_ok(rt.src))
		return bad(path, 44, "source name");
	if (rtb_read(&ids))
		return bad("campaign.rtb", 0, "missing or malformed");
	if (rd32(b + 8) != ids.build_id)
		return bad(path, 8, "rtb_id differs from campaign.rtb build_id");

	for (i = 0, p = rt.code_ofs; i < rt.nops; i++)
	{
		uint32_t code, a, bb, nk, j, line = rd16(b + rt.lines_ofs + 2 * i);
		int ok_a;

		if (p + 4 > rt.code_end)
			return bad(path, p, "op runs past the code");
		code = b[p];
		a = b[p + 1];
		bb = rd16(b + p + 2);
		if (!code || code >= OP_COUNT)
			return bad(path, p, "unknown opcode");
		nk = (uint32_t)strlen(ops[code].kinds);
		if (p + 4 + 4 * nk > rt.code_end)
			return bad(path, p, "operands run past the code");
		for (j = 0; j < nk; j++)
		{
			const uint8_t *w = b + p + 4 + 4 * j;
			char k = ops[code].kinds[j];

			if ((k == 'f' || k == 'r') && (!isfinite(rdf(w)) || (k == 'r' && rdf(w) < 0)))
				return bad(path, p + 4 + 4 * j, "bad float operand");
			if (k == 'c' && rd32(w) >= ids.clas)
				return bad(path, p + 4 + 4 * j, "class id");
		}
		if (!line)
			return bad(path, rt.lines_ofs + 2 * i, "source line 0");
		ok_a = a == 0;
		switch (code)
		{
		case OP_WAIT_MAP: case OP_WAIT_NEAR: case OP_WAIT_CLASS: case OP_SETPOS: case OP_DROP:
			if (bb >= ids.maps || (int)bb >= wld_map_count())
				return bad(path, p, "map id");
			if (code == OP_SETPOS)
				ok_a = a <= 1;
			break;
		case OP_WAIT_MOVED: case OP_WAIT_GONE:
			if (bb >= ids.targ)
				return bad(path, p, "target id");
			break;
		case OP_WAIT_GLOBAL:
			ok_a = a <= 2;
			if (bb >= ids.glob)
				return bad(path, p, "global id");
			break;
		case OP_LOOK_AT:
			ok_a = a <= 1;
			if (rd32(b + p + 4) >= ids.maps || bb >= (a ? ids.clas : ids.targ))
				return bad(path, p, "look_at ids");
			break;
		case OP_BUTTONS:
			if ((bb & ~BUTTON_MASK) || (rd32(b + p + 4) & ~BUTTON_MASK) || (bb & rd32(b + p + 4)))
				return bad(path, p, "button masks");
			break;
		case OP_IMPULSE:
			ok_a = 1;
			break;
		case OP_GOD:
			ok_a = a <= 1;
			break;
		case OP_SHOT: case OP_STATS:
			if (!label_ok(bb))
				return bad(path, p, "label");
			break;
		case OP_PASS:
			if (i != rt.nops - 1u)
				return bad(path, p, "PASS before the last op");
			break;
		}
		if ((code == OP_WAIT || code == OP_WAIT_END || code == OP_LOOK || code == OP_IMPULSE
			|| code == OP_GOD || code == OP_PASS) && bb)
			return bad(path, p, "b must be 0");
		if (!ok_a)
			return bad(path, p, "bad a operand");
		if (i == rt.nops - 1u && code != OP_PASS)
			return bad(path, p, "last op is not PASS");
		p += 4 + 4 * nk;
	}
	if (p != rt.code_end)
		return bad(path, p, "code bytes after the last op");
	rt.loaded = 1;
	rt.pc = rt.code_ofs;
	log_printf("route: %s: %u ops from %s\n", str_at(rt.src), rt.nops, path);
	return 1;
}

int rt_active(void)
{
	return rt.loaded && !rt.done;
}

static void fail(const char *reason)
{
	log_printf("route FAIL %u %s:%u %s %s\n", rt.op, str_at(rt.src),
		rd16(rt.buf + rt.lines_ofs + 2u * rt.op), ops[rt.buf[rt.pc]].name, reason);
	log_flush();
	rt.done = 1;
	sys_exit(1);
}

static void push(int op, uint32_t arg)
{
	dbg_cmd_t c;

	memset(&c, 0, sizeof(c));
	c.op = (uint8_t)op;
	c.nargs = (uint8_t)(dbg_desc[op].args[0] ? 1 : 0);
	c.arg[0].u = arg;
	if (dbg_push(&c))
		fail("debug queue full");
}

/* A wait op: 1 done, 0 not yet (fails past its timeout). */
static int wait_until(int ok, uint32_t timeout, uint64_t now_us)
{
	if (ok)
		return 1;
	if (now_us - rt.t0 > timeout)
		fail("timeout");
	return 0;
}

/* inside_map: the player's home map is `map`, so his origin is map-local
 * like the route's point */
static int near(const float *w, int inside_map)
{
	const ent_player_t *pl = phy_player();
	float d2 = 0;
	int k;

	if (!inside_map)
		return 0;
	for (k = 0; k < 3; k++)
	{
		float d = pl->origin[k] - w[k];

		d2 += d * d;
	}
	return d2 <= w[3] * w[3];
}

void rt_frame(uint64_t now_us)
{
	while (rt_active())
	{
		const uint8_t *p = rt.buf + rt.pc;
		uint32_t code = p[0], a = p[1], b = rd16(p + 2);
		float w[5];
		int k, home = phy_player_active() && phy_player_map() == (int)b;

		for (k = 0; k < 5 && k < (int)strlen(ops[code].kinds); k++)
			w[k] = rdf(p + 4 + 4 * k);
		if (!rt.started)
		{
			rt.started = 1;
			rt.t0 = now_us;
			if (code == OP_SHOT || code == OP_STATS)
				log_printf("route: %u %s %s\n", rt.op, ops[code].name, str_at(b));
			else
				log_printf("route: %u %s\n", rt.op, ops[code].name);
		}
		switch (code)
		{
		case OP_WAIT:
			if (now_us - rt.t0 < rd32(p + 4))
				return;
			break;
		case OP_WAIT_MAP:
			if (!wait_until(home, rd32(p + 4), now_us))
				return;
			break;
		case OP_WAIT_NEAR:
			if (!wait_until(near(w, home), rd32(p + 20), now_us))
				return;
			break;
		case OP_LOOK:
		{
			float ang[3] = { w[0], w[1], 0 };

			in_set_angles(ang);
			break;
		}
		case OP_SETPOS:
		{
			float org[3] = { w[0], w[1], w[2] };
			const float *cur = in_angles();
			float ang[3] = { a ? w[3] : cur[0], a ? w[4] : cur[1], 0 };

			if (!home)
				fail("home map differs");
			if (phy_player_spawn((int)b, org, ang))
				fail("setpos refused");
			break;
		}
		case OP_BUTTONS:
			in_buttons((uint16_t)b, (uint16_t)rd32(p + 4));
			break;
		case OP_IMPULSE:
			in_impulse((uint8_t)a);
			break;
		case OP_GOD:
			push(DBG_GOD, a);
			break;
		case OP_SHOT:
			push(DBG_SHOT, rt.op);
			break;
		case OP_STATS:
			push(DBG_STATS, rt.op);
			break;
		case OP_PASS:
			log_printf("route PASS\n");
			log_flush();
			rt.done = 1;
			sys_exit(0);
		default:
			fail("needs the M3 entities and records");
			return;
		}
		rt.pc += 4u + 4u * (uint32_t)strlen(ops[code].kinds);
		rt.op++;
		rt.started = 0;
	}
}
