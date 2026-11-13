/* SPDX-License-Identifier: MIT */
/*
 * dbg.c -- the debug command queue (dbg.h). A fixed array in push order;
 * dbg_run executes the due entries and keeps the rest in order. Handlers
 * are registered by the module that owns the command.
 */
#include <string.h>

#include "hl64/limits.h"
#include "engine/core/dbg.h"
#include "engine/core/log.h"
#include "engine/core/sys.h"
#include "engine/io/io.h"

const dbg_desc_t dbg_desc[DBG_COUNT] = {
#define DBG_DESC(id, name, args, test) { name, args, test },
	DBG_COMMANDS(DBG_DESC)
#undef DBG_DESC
};

static dbg_handler_t	handlers[DBG_COUNT];
static dbg_cmd_t	queue[HL64_DBG_QUEUE];
static int		queued;
static uint32_t		failures;

void dbg_init(void)
{
	memset(handlers, 0, sizeof(handlers));
	queued = 0;
	failures = 0;
}

void dbg_handler_set(int op, dbg_handler_t fn)
{
	if (op >= 0 && op < DBG_COUNT)
		handlers[op] = fn;
}

int dbg_push(const dbg_cmd_t *c)
{
	if (c->op >= DBG_COUNT || c->nargs != strlen(dbg_desc[c->op].args))
		return -1;
#ifndef HL64_TEST
	if (dbg_desc[c->op].test_only)
		return -1;
#endif
	if (queued >= HL64_DBG_QUEUE)
		return -1;
	queue[queued++] = *c;
	return 0;
}

uint32_t dbg_run(uint32_t frame)
{
	int i, kept = 0, n = queued;

	for (i = 0; i < n; i++)
	{
		dbg_cmd_t c = queue[i];

		if ((int32_t)(frame - c.frame) < 0)
		{
			queue[kept++] = c;
			continue;
		}
		if (!handlers[c.op])
		{
			log_printf("dbg: %s: no handler\n", dbg_desc[c.op].name);
			failures++;
		}
		else if (handlers[c.op](&c))
		{
			log_printf("dbg: %s failed\n", dbg_desc[c.op].name);
			failures++;
		}
	}
	/* handlers may push: entries added during the loop sit after n */
	if (queued > n)
	{
		memmove(queue + kept, queue + n, (size_t)(queued - n) * sizeof(queue[0]));
		kept += queued - n;
	}
	queued = kept;
	return failures;
}

static uint32_t rd32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

int dbg_load(const char *path)
{
	static uint8_t buf[DBG_FILE_HDR + DBG_FILE_REC * HL64_DBG_QUEUE] __attribute__((aligned(16)));
	uint32_t size, rom = sys_cart_addr(path, &size), n, i, k;

	if (!rom)
		return 0;
	if (size < DBG_FILE_HDR || size > sizeof(buf))
		sys_error("%s: size %lu", path, (unsigned long)size);
	io_read_sync(buf, rom, (size + 1) & ~1u, "dbg.cmd");
	n = (uint32_t)(buf[6] << 8 | buf[7]);
	if (rd32(buf) != DBG_FILE_MAGIC || buf[4] || buf[5] != 1
		|| size != DBG_FILE_HDR + DBG_FILE_REC * n)
		sys_error("%s: not a version-1 command file", path);
	for (i = 0; i < n; i++)
	{
		const uint8_t *r = buf + DBG_FILE_HDR + DBG_FILE_REC * i;
		dbg_cmd_t c;

		memset(&c, 0, sizeof(c));
		c.frame = rd32(r);
		c.op = r[4];
		c.nargs = r[5];
		if (c.nargs > DBG_MAX_ARGS)
			sys_error("%s: command %lu: %u args", path, (unsigned long)i, c.nargs);
		for (k = 0; k < c.nargs; k++)
			c.arg[k].u = rd32(r + 8 + 4 * k);
		if (dbg_push(&c))
			sys_error("%s: command %lu rejected", path, (unsigned long)i);
	}
	log_printf("dbg: %lu commands from %s\n", (unsigned long)n, path);
	return (int)n;
}
