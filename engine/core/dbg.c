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
