/* SPDX-License-Identifier: MIT */
/*
 * main.c -- host entry point (docs/architecture.md 3.4). Same engine,
 * same carve and caps as the cart; debug commands come from the command
 * line as text, parsed here only (architecture 12) into the binary
 * commands the cart's channels deliver.
 *
 *   hl64-host [--root DIR] [--frames N] [--cmd "[@FRAME] NAME ARGS..."]...
 *
 * --root    cart root (the DFS image source); default "."
 * --frames  quit 0 after N frames (same as --cmd "@N quit 0")
 * --cmd     one debug command (engine/core/dbg.h names); run at frame
 *           FRAME (default 1). Settings may be named: "set gravity 600".
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hl64/limits.h"
#include "hl64/settings.h"
#include "engine/core/dbg.h"
#include "engine/core/eng.h"
#include "engine/core/log.h"
#include "engine/core/sys.h"
#include "plat/host/host.h"

static int usage(void)
{
	fprintf(stderr, "usage: hl64-host [--root DIR] [--frames N] "
		"[--cmd \"[@FRAME] NAME ARGS...\"]...\n");
	return 2;
}

/* Text -> binary command; 0 on success. */
static int parse_cmd(const char *text, dbg_cmd_t *c)
{
	char buf[256], *tok, *save = NULL, *end;
	const char *sig;
	int op;

	memset(c, 0, sizeof(*c));
	c->frame = 1;
	snprintf(buf, sizeof(buf), "%s", text);
	tok = strtok_r(buf, " \t", &save);
	if (tok && tok[0] == '@')
	{
		c->frame = (uint32_t)strtoul(tok + 1, &end, 10);
		if (*end)
			return -1;
		tok = strtok_r(NULL, " \t", &save);
	}
	if (!tok)
		return -1;
	for (op = 0; op < DBG_COUNT; op++)
		if (!strcmp(dbg_desc[op].name, tok))
			break;
	if (op == DBG_COUNT)
		return -1;
	c->op = (uint8_t)op;
	for (sig = dbg_desc[op].args; *sig; sig++)
	{
		tok = strtok_r(NULL, " \t", &save);
		if (!tok)
			return -1;
		if ((op == DBG_SET_SETTING || op == DBG_GET_SETTING) && sig == dbg_desc[op].args
			&& settings_find(tok) >= 0)
			c->arg[c->nargs].u = (uint32_t)settings_find(tok);
		else if (*sig == 'f')
		{
			c->arg[c->nargs].f = strtof(tok, &end);
			if (*end)
				return -1;
		}
		else
		{
			c->arg[c->nargs].u = (uint32_t)strtoul(tok, &end, 0);
			if (*end)
				return -1;
		}
		c->nargs++;
	}
	return strtok_r(NULL, " \t", &save) ? -1 : 0;
}

int main(int argc, char **argv)
{
	dbg_cmd_t cmds[HL64_DBG_QUEUE];
	const char *root = NULL;
	int i, ncmds = 0;

	for (i = 1; i < argc; i++)
	{
		char text[64];
		const char *cmd = NULL;

		if (!strcmp(argv[i], "--root") && i + 1 < argc)
			root = argv[++i];
		else if (!strcmp(argv[i], "--frames") && i + 1 < argc)
		{
			snprintf(text, sizeof(text), "@%s quit 0", argv[++i]);
			cmd = text;
		}
		else if (!strcmp(argv[i], "--cmd") && i + 1 < argc)
			cmd = argv[++i];
		else
			return usage();
		if (!cmd)
			continue;
		if (ncmds >= HL64_DBG_QUEUE || parse_cmd(cmd, &cmds[ncmds]))
		{
			fprintf(stderr, "hl64-host: bad command \"%s\"\n", cmd);
			return usage();
		}
		ncmds++;
	}

	host_io_init(root);
	eng_boot("host");
	for (i = 0; i < ncmds; i++)
		if (dbg_push(&cmds[i]))
		{
			log_printf("hl64-host: command %d rejected\n", i + 1);
			sys_exit(2);
		}
	for (;;)
		eng_frame();
}
