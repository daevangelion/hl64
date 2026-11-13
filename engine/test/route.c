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
 * Entity and record ops resolve a name id through campaign.rtb's test part
 * (TARG -> map + BSP entity lumps, TLMP), read from the cart once per op:
 * gid = GID_MAKE(the map's HLE slot, lump). Their queries never hydrate a
 * pending record (the route must not change what it measures). Positions
 * are map-local to the op's map, the frame of its HLE slot's entities.
 */
#include <math.h>
#include <string.h>

#include "engine/core/dbg.h"
#include "engine/core/hlmath.h"
#include "engine/core/log.h"
#include "engine/core/sys.h"
#include "engine/ent/rec_int.h"
#include "engine/in/in.h"
#include "engine/io/io.h"
#include "engine/phys/player.h"
#include "engine/test/route.h"
#include "engine/world/wld.h"
#include "hl64/ent.h"
#include "hl64/game.h"
#include "hl64/rec.h"

#define RBC_MAGIC	0x52424331u
#define RBC_HDR		48
#define RBC_MAX_BYTES	16384
#define RTB_MAGIC	0x52544231u
#define RTB_DIR_MAX	32

enum {
	OP_WAIT = 1, OP_WAIT_MAP, OP_WAIT_NEAR, OP_WAIT_MOVED, OP_WAIT_GONE, OP_WAIT_CLASS,
	OP_WAIT_GLOBAL, OP_WAIT_END, OP_LOOK, OP_LOOK_AT, OP_SETPOS, OP_BUTTONS, OP_IMPULSE,
	OP_DROP, OP_GOD, OP_SHOT, OP_STATS, OP_PASS, OP_START, OP_COUNT
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
	[OP_START] = { "START", "" },
};

#define BUTTON_MASK	(0x3FFFu & ~(1u << 6))
#define RT_LUMPS	64	/* records of one name id (TARG lump_count) */
#define RT_NAMES	48	/* distinct name ids a route's ops resolve */
#define RT_POOL		512	/* their lumps */
#ifndef M_PI
#define M_PI		3.14159265358979323846
#endif

/* A name id resolved at the op's start (rtb TARG / TLMP). */
typedef struct {
	uint16_t	map, count, spawners;	/* the last `spawners` lumps are monstermakers */
	uint16_t	lump[RT_LUMPS];
} rt_targ_t;

static struct {
	uint8_t		buf[RBC_MAX_BYTES] __attribute__((aligned(16)));
	uint32_t	size, code_ofs, code_end, lines_ofs, str_ofs, str_size;
	uint16_t	nops, src;
	uint8_t		loaded, done;
	uint32_t	pc;		/* byte offset of the current op */
	uint16_t	op;		/* its index */
	uint8_t		started;	/* the current op's start time is set */
	double		t0;		/* game time (s) the current op started */
	uint32_t	rtb_rom, targ_ofs, tlmp_ofs, ntlmp;	/* campaign.rtb test part */
	rt_targ_t	tg;		/* the current op's name */
	/* the ops' names, read at load (an op's start reads no cart) */
	struct {
		uint16_t	id, map, count, spawners, first;
	}		names[RT_NAMES];
	uint16_t	pool[RT_POOL];
	uint16_t	nnames, npool;
	int		start;		/* START map, -1 none */
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

/* campaign.rtb: build_id and the id-space counts the reader checks; the
 * test part's place (TARG, TLMP) into rt (res_init validated the file) */
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
	io_read_sync(hdr, rom, 32, "route rtb");
	nsect = rd16(hdr + 20);
	if (rd32(hdr) != RTB_MAGIC || rd16(hdr + 4) != 1 || rd32(hdr + 12) != size
		|| rd32(hdr + 24) != 32 || nsect > RTB_DIR_MAX || 32 + 16 * nsect > size)
		return -1;
	io_read_sync(hdr, rom, (32 + 16 * nsect + 1) & ~1u, "route rtb");
	t->build_id = rd32(hdr + 8);
	rt.rtb_rom = rom;
	for (i = 0; i < nsect; i++)
	{
		const uint8_t *d = hdr + 32 + 16 * i;
		uint32_t cc = rd32(d), n = rd32(d + 12);

		if (cc == 0x4D415053u)		/* MAPS */
			t->maps = n;
		else if (cc == 0x54415247u)	/* TARG */
		{
			t->targ = n;
			rt.targ_ofs = rd32(d + 4);
		}
		else if (cc == 0x544C4D50u)	/* TLMP */
		{
			rt.tlmp_ofs = rd32(d + 4);
			rt.ntlmp = n;
		}
		else if (cc == 0x434C4153u)	/* CLAS */
			t->clas = n;
		else if (cc == 0x474C4F42u)	/* GLOB */
			t->glob = n;
	}
	return 0;
}

/* Read name id `id`'s TARG entry and its TLMP lumps into the route's name
 * cache (load time: an op's start then reads no cart); -1 when it has more
 * than RT_LUMPS records, -2 when the cache is full (the rtb's own rules
 * were checked by res_init). */
static int tg_load(uint32_t id)
{
	static uint8_t w[2 * RT_LUMPS] __attribute__((aligned(16)));
	uint32_t first, count, i;

	for (i = 0; i < rt.nnames; i++)
		if (rt.names[i].id == id)
			return 0;
	if (rt.nnames >= RT_NAMES)
		return -2;
	io_read_sync(w, rt.rtb_rom + rt.targ_ofs + 8u * id, 8, "route names");
	first = rd16(w + 2);
	count = rd16(w + 4);
	if (count > RT_LUMPS || first + count > rt.ntlmp)
		return -1;
	if (rt.npool + count > RT_POOL)
		return -2;
	rt.names[rt.nnames].id = (uint16_t)id;
	rt.names[rt.nnames].map = rd16(w);
	rt.names[rt.nnames].count = (uint16_t)count;
	rt.names[rt.nnames].spawners = rd16(w + 6);
	rt.names[rt.nnames].first = rt.npool;
	io_read_sync(w, rt.rtb_rom + rt.tlmp_ofs + 2u * first, 2u * count, "route names");
	for (i = 0; i < count; i++)
		rt.pool[rt.npool++] = rd16(w + 2 * i);
	rt.nnames++;
	return 0;
}

/* The cached name id `id` (tg_load at load) into t; -1 when not cached. */
static int tg_get(uint32_t id, rt_targ_t *t)
{
	uint32_t i, k;

	for (i = 0; i < rt.nnames && rt.names[i].id != id; i++)
		;
	if (i == rt.nnames)
		return -1;
	t->map = rt.names[i].map;
	t->count = rt.names[i].count;
	t->spawners = rt.names[i].spawners;
	for (k = 0; k < t->count; k++)
		t->lump[k] = rt.pool[rt.names[i].first + k];
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
	rt.start = -1;
	rom = sys_cart_addr(path, &size);
	if (!rom)
		return 0;
	if (size < RBC_HDR || size > RBC_MAX_BYTES)
		return bad(path, 0, "size");
	io_read_sync(rt.buf, rom, (size + 1) & ~1u, "route");
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
		int ok_a, tk = 0;

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
		case OP_START:
			if (bb >= ids.maps || (int)bb >= wld_map_count())
				return bad(path, p, "map id");
			if (code == OP_SETPOS)
				ok_a = a <= 1;
			/* a dropped class is precached at load, as a map's classes
			 * are (its spawn then reads no model info) */
			if (code == OP_DROP)
				game_precache_class((cls_t)rd32(b + p + 4));
#ifdef HL64_TEST
			if (code == OP_START && i)
				return bad(path, p, "START is not the first op");
			if (code == OP_START)
				rt.start = (int)bb;
#else
			if (code == OP_START)
				return bad(path, p, "START needs a test build");
			/* a debug teleport (its blocked frames count as blk1ms_tele) */
			if (code == OP_SETPOS)
				return bad(path, p, "SETPOS needs a test build");
#endif
			break;
		case OP_WAIT_MOVED: case OP_WAIT_GONE:
			if (bb >= ids.targ)
				return bad(path, p, "target id");
			if ((tk = tg_load(bb)))
				return bad(path, p, tk == -1 ? "name has more than 64 records"
					: "more names than RT_NAMES / RT_POOL");
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
			if (!a && ((tk = tg_load(bb)) || tg_get(bb, &rt.tg) || rt.tg.map != rd32(b + p + 4)))
				return bad(path, p, tk == -2 ? "more names than RT_NAMES / RT_POOL"
					: "look_at name not of its map");
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

int rt_start_map(void)
{
	return rt.loaded ? rt.start : -1;
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

/* A wait op: 1 done, 0 not yet (fails past its timeout, bytecode µs). */
static int wait_until(int ok, uint32_t timeout_us, double time)
{
	if (ok)
		return 1;
	if ((time - rt.t0) * 1e6 > (double)timeout_us)
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

/* HLE slot (= world layer slot) of an open map, -1 when it is closed */
static int map_slot(int map)
{
	int s;

	for (s = 0; s < HL64_WORLD_LAYERS; s++)
	{
		const wld_layer_t *L = wld_layer(s);

		if (L && L->map == map)
			return s;
	}
	return -1;
}

static int alive(int e)
{
	return ent_is_live(e) && !(ent_cv(e)->flags & FL_KILLME);
}

static int differ3(const float *x, const float *y)
{
	return x[0] != y[0] || x[1] != y[1] || x[2] != y[2];
}

static float dist2(const float *x, const float *y)
{
	float d0 = x[0] - y[0], d1 = x[1] - y[1], d2 = x[2] - y[2];

	return d0 * d0 + d1 * d1 + d2 * d2;
}

/* The current op's map records carrying the name (TARG lumps before the
 * spawners). */
static int tg_named(void)
{
	return rt.tg.count - rt.tg.spawners;
}

/* The name in its map's HLE slot: the records' targetname, else (a name
 * only monstermaker children receive) the first spawner's netname. */
static name_t tg_name(int slot)
{
	gid_t16 g = GID_MAKE(slot, rt.tg.lump[0]);

	return tg_named() ? rec_targetname(g) : rec_key_name(g, RK_NETNAME);
}

/* Next runtime entity holding the name (monstermaker children), -1 at the
 * end. rec_next_named wakes pending records of the name, so it is only
 * asked once none is pending or dry: the route never hydrates. */
static int tg_child(int slot, int *it)
{
	name_t n;
	int i, e;

	if (!rt.tg.spawners)
		return -1;
	for (i = 0; i < tg_named(); i++)
	{
		int st = rec_state(GID_MAKE(slot, rt.tg.lump[i]));

		if (st == REC_PENDING || st == REC_DRY)
			return -1;
	}
	if ((n = tg_name(slot)) == NAME_NONE)
		return -1;
	while ((e = rec_next_named(n, it)) >= 0)
		if (ent_idc(e)->gid == GID_NONE && alive(e))
			return e;
	return -1;
}

/* WAIT_MOVED: velocity or avelocity non-zero, or (a map record's entity)
 * origin / angles differ from the record's keys. Zero angles do not count:
 * HLSDK SetMovedir turns a linear mover's `angle` / `angles` key into its
 * move direction and zeroes pev->angles at spawn (doors, buttons), so a
 * closed func_door with `angle 90` rests at angles 0 0 0. */
static int moved(int e, gid_t16 gid)
{
	static const float zero[3];
	const col_ent_t *cv = ent_cv(e);
	float o[3], a[3];

	if (differ3(ent_mot(e)->velocity, zero) || differ3(ent_mot(e)->avelocity, zero))
		return 1;
	if (gid == GID_NONE)
		return 0;
	rec_origin(gid, o);
	rec_angles(gid, a);
	return differ3(cv->origin, o) || (differ3(cv->angles, a) && differ3(cv->angles, zero));
}

/* the entity that satisfied a wait / LOOK_AT (diagnostics; not an op line) */
static void tell(const char *what, int e)
{
	const col_ent_t *cv = ent_cv(e);

	log_printf("route: %s entity %d cls %u org %.9g %.9g %.9g vel %.9g %.9g %.9g\n", what, e,
		(unsigned)ent_idc(e)->cls, (double)cv->origin[0], (double)cv->origin[1],
		(double)cv->origin[2], (double)ent_mot(e)->velocity[0],
		(double)ent_mot(e)->velocity[1], (double)ent_mot(e)->velocity[2]);
}

static int wait_moved(void)
{
	int slot = map_slot(rt.tg.map), i, e, it = 0;

	if (slot < 0)
		return 0;
	for (i = 0; i < tg_named(); i++)
	{
		gid_t16 g = GID_MAKE(slot, rt.tg.lump[i]);

		if ((e = rec_entity(g)) >= 0 && alive(e) && moved(e, g))
		{
			tell("moved", e);
			return 1;
		}
	}
	while ((e = tg_child(slot, &it)) >= 0)
		if (moved(e, GID_NONE))
		{
			tell("moved", e);
			return 1;
		}
	return 0;
}

/* A monstermaker has spawned all it will: its record is killed, or it has
 * neither think nor use left (HLSDK CMonsterMaker::MakeMonster clears both
 * when its count runs out): a logic record asks the game, an entity
 * spawner its own think / use. */
static int spawner_done(gid_t16 g)
{
	int e;

	if (rec_state(g) == REC_KILLED)
		return 1;
	if (rec_kind(g) == REC_KIND_LOGIC)
		return game_spawner_done(g);
	if ((e = rec_entity(g)) < 0)
		return 0;
	return !ent_tm(e)->think && !ent_tm(e)->use;
}

/* A record of the name is gone: killed, or its live entity is no longer
 * there physically (SOLID_NOT). GoldSrc keeps entities that share a
 * breakable's name but have no body (ambient_generic, env_sprite,
 * env_shake, a toggled-off trigger) in the map for good, SOLID_NOT: a
 * logic or point record (no entity, no body) is gone in the same way. A
 * pending or dry record is not gone (the route never hydrates). */
static int rec_gone(gid_t16 g)
{
	int e, st = rec_state(g);

	if (st == REC_KILLED)
		return 1;
	if (rec_kind(g) != REC_KIND_ENTITY)
		return st == REC_LIVE;
	if ((e = rec_entity(g)) < 0)
		return 0;
	return !alive(e) || ent_cv(e)->solid == COL_SOLID_NOT;
}

static int wait_gone(void)
{
	int slot = map_slot(rt.tg.map), i, it = 0;

	if (slot < 0)
		return 0;
	for (i = 0; i < tg_named(); i++)
		if (!rec_gone(GID_MAKE(slot, rt.tg.lump[i])))
			return 0;
	for (; i < rt.tg.count; i++)
		if (!spawner_done(GID_MAKE(slot, rt.tg.lump[i])))
			return 0;
	return tg_child(slot, &it) < 0;
}

/* WAIT_CLASS: w = x, y, z, r (map-local to map) */
static int wait_class(int map, cls_t cls, const float *w)
{
	int slot = map_slot(map), e = -1;

	if (slot < 0)
		return 0;
	while ((e = ent_next_class(e, cls, slot)) >= 0)
		if (alive(e) && ent_cv(e)->solid != COL_SOLID_NOT && dist2(ent_cv(e)->origin, w) <= w[3] * w[3])
		{
			tell("class", e);
			return 1;
		}
	return 0;
}

/* LOOK_AT candidate e of map: keep it when its centre is the nearest yet
 * to the eye (global positions; the lowest index wins a tie) */
static void look_cand(int e, int map, const float *eye, int *best, float *bd, float *bc)
{
	const col_ent_t *cv = ent_cv(e);
	const float *g = wld_map_origin(map);
	float c[3], d;
	int k;

	if (!alive(e))
		return;
	for (k = 0; k < 3; k++)
		c[k] = 0.5f * (cv->absmin[k] + cv->absmax[k]) + g[k];
	d = dist2(c, eye);
	if (*best < 0 || d < *bd)
	{
		*best = e;
		*bd = d;
		memcpy(bc, c, sizeof(c));
	}
}

/* LOOK_AT: by_class 0 the current op's name, 1 class id; -1 none live */
static int look_at(int by_class, uint32_t id, int map)
{
	const ent_player_t *pl = phy_player();
	int slot = map_slot(map), best = -1, e, it = 0, k;
	float eye[3], c[3], bd = 0, d[3], ang[3];

	if (slot < 0 || !phy_player_active())
		return -1;
	for (k = 0; k < 3; k++)
		eye[k] = pl->origin[k] + pl->view_ofs[k] + wld_map_origin(phy_player_map())[k];
	if (by_class)
	{
		for (e = -1; (e = ent_next_class(e, (cls_t)id, slot)) >= 0;)
			look_cand(e, map, eye, &best, &bd, c);
	}
	else
	{
		for (k = 0; k < tg_named(); k++)
			if ((e = rec_entity(GID_MAKE(slot, rt.tg.lump[k]))) >= 0)
				look_cand(e, map, eye, &best, &bd, c);
		while ((e = tg_child(slot, &it)) >= 0)
			look_cand(e, map, eye, &best, &bd, c);
	}
	if (best < 0)
		return -1;
	for (k = 0; k < 3; k++)
		d[k] = c[k] - eye[k];
	/* view angles: pitch down + (HLSDK), as pm_math VectorAngles */
	ang[0] = (float)(-hl_atan2(d[2], sqrt((double)d[0] * d[0] + (double)d[1] * d[1])) * 180 / M_PI);
	ang[1] = (float)(hl_atan2(d[1], d[0]) * 180 / M_PI);
	ang[2] = 0;
	in_set_angles(ang);
	tell("look_at", best);
	log_printf("route: look_at angles %.9g %.9g\n", (double)ang[0], (double)ang[1]);
	return 0;
}

/* DROP: HLSDK CBaseEntity::Create of class cls at a map-local point */
static const char *drop(int map, cls_t cls, const float *org)
{
	int slot = map_slot(map), prev = eng_frame_layer(), e, ok;

	if (slot < 0)
		return "map not open";
	eng_set_frame_layer(slot);
	e = ent_create(ENT_ALLOC_GAME);
	if (e >= 0)
		ent_set_origin(e, org);
	ok = e >= 0 && game_spawn_class(cls, e) == 0;
	eng_set_frame_layer(prev);
	if (ok)
		tell("drop", e);
	return e < 0 ? "no free entity" : ok ? NULL : "the game refused the class";
}

/* Route look-ahead pin (route-bytecode.md, residency; architecture.md 5.3
 * pin class): the next SETPOS destination, if its map is open, re-armed
 * at every op start until it runs (pins lapse after 3 s). */
static void lookahead_pin(void)
{
	uint32_t pc = rt.pc, i;

	for (i = rt.op; i < rt.nops; i++)
	{
		const uint8_t *p = rt.buf + pc;

		if (p[0] == OP_SETPOS)
		{
			float org[3] = { rdf(p + 4), rdf(p + 8), rdf(p + 12) };
			wld_layer_t *L = wld_layer_of_map(rd16(p + 2));

			if (L && wld_layer_ready(L) == 1)
				wld_pin_point(rd16(p + 2), org);
			return;
		}
		pc += 4u + 4u * (uint32_t)strlen(ops[p[0]].kinds);
	}
}

void rt_new_game(double old_time, double time)
{
	/* the current op keeps the game time it has waited so far */
	if (rt.started)
		rt.t0 = time - (old_time - rt.t0);
}

void rt_frame(double time)
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
			rt.t0 = time;
			if (code == OP_SHOT || code == OP_STATS)
				log_printf("route: %u %s %s\n", rt.op, ops[code].name, str_at(b));
			else
				log_printf("route: %u %s\n", rt.op, ops[code].name);
#ifdef HL64_PROFILE
			/* PROFILE: the memory report with the last STATS record
			 * (the same frame boundary; the gate tables read it) */
			if (code == OP_PASS)
				push(DBG_MEM, 0);
#endif
			/* the name's records (checked at load) */
			if (code == OP_WAIT_MOVED || code == OP_WAIT_GONE || (code == OP_LOOK_AT && !a))
				tg_get(b, &rt.tg);
			lookahead_pin();
		}
		switch (code)
		{
		case OP_WAIT:
			if ((time - rt.t0) * 1e6 < (double)rd32(p + 4))
				return;
			break;
		case OP_WAIT_MAP:
			if (!wait_until(home, rd32(p + 4), time))
				return;
			break;
		case OP_WAIT_NEAR:
			if (!wait_until(near(w, home), rd32(p + 20), time))
				return;
			break;
		case OP_WAIT_MOVED:
			if (!wait_until(wait_moved(), rd32(p + 4), time))
				return;
			break;
		case OP_WAIT_GONE:
			if (!wait_until(wait_gone(), rd32(p + 4), time))
				return;
			break;
		case OP_WAIT_CLASS:
			if (!wait_until(wait_class((int)b, (cls_t)rd32(p + 4), w + 1), rd32(p + 24), time))
				return;
			break;
		case OP_WAIT_GLOBAL:
			if (!wait_until(game_global_state((int)b) == (int)a, rd32(p + 4), time))
				return;
			break;
		case OP_WAIT_END:
			if (!wait_until(game_end_section() > 0, rd32(p + 4), time))
				return;
			break;
		case OP_LOOK_AT:
			if (look_at((int)a, b, (int)rd32(p + 4)))
			{
				fail("no live entity");
				return;
			}
			break;
		case OP_DROP:
		{
			const char *why = drop((int)b, (cls_t)rd32(p + 4), w + 1);

			if (why)
			{
				fail(why);
				return;
			}
			break;
		}
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
			if (phy_player_setpos((int)b, org, ang))
				fail("setpos refused");
			/* a test-only teleport: the destination's records hydrate
			 * in the next frames; their blocked frames count apart
			 * (blk1ms_tele, 1 s), not hidden */
			io_tele_window(30);
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
		case OP_START:	/* boot used it (eng_boot: New Game on that map) */
			break;
		case OP_PASS:
			/* the frame after it is reached: the debug commands queued
			 * by the ops before it (STATS, SHOT) run at that frame's
			 * boundary */
			if (time == rt.t0)
				return;
			log_printf("route PASS\n");
			log_flush();
			rt.done = 1;
			sys_exit(0);
			return;
		}
		rt.pc += 4u + 4u * (uint32_t)strlen(ops[code].kinds);
		rt.op++;
		rt.started = 0;
	}
}
