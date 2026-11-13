/* SPDX-License-Identifier: MIT */
/*
 * hlecook — BSP30 → the map's entity chunk (the .w64 ENTS chunk,
 * docs/formats/w64-format.md): the HLE1 entity blob of the entity lump
 * (docs/formats/hle-format-v1.md), then the HLN1 node graph when the map
 * has info_node entities (hle_cook_bsp, hl_nodecook.h). Little endian; the
 * engine fixes byte order in place (hle_fixup, hln_fixup).
 *
 * Usage: hlecook <map.bsp> <out.hle>
 * Malformed input: "hlecook: error: <file>: <what>" on stderr, exit 1.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "hl_entcook.h"
#include "hl_nodecook.h"

static int fail (const char *file, const char *what)
{
	fprintf (stderr, "hlecook: error: %s: %s\n", file, what);
	return 1;
}

int main (int argc, char **argv)
{
	FILE		*f;
	long		sz;
	uint8_t		*bsp, *blob = NULL;
	int		olen = 0;
	hle_stats_t	st;
	hln_stats_t	nst;
	const char	*err;

	if (argc != 3)
	{
		fprintf (stderr, "usage: hlecook <map.bsp> <out.hle>\n");
		return 2;
	}
	f = fopen (argv[1], "rb");
	if (!f)
		return fail (argv[1], strerror (errno));
	if (fseek (f, 0, SEEK_END) || (sz = ftell (f)) < 0
		|| fseek (f, 0, SEEK_SET))
	{
		fclose (f);
		return fail (argv[1], "cannot size the file");
	}
	bsp = malloc ((size_t)sz + 1);
	if (!bsp || fread (bsp, 1, (size_t)sz, f) != (size_t)sz)
	{
		fclose (f);
		free (bsp);
		return fail (argv[1], "read failed");
	}
	fclose (f);
	err = hle_cook_bsp (bsp, (size_t)sz, 0, &blob, &olen, &st, &nst);
	if (err)
	{
		free (bsp);
		return fail (argv[1], err);
	}
	f = fopen (argv[2], "wb");
	if (!f || fwrite (blob, 1, (size_t)olen, f) != (size_t)olen)
	{
		if (f)
			fclose (f);
		free (blob);
		free (bsp);
		return fail (argv[2], "write failed");
	}
	if (fclose (f))
	{
		free (blob);
		free (bsp);
		return fail (argv[2], "write failed");
	}
	printf ("hlecook: %s: %d records (%d pruned, %d dropped classes), %d keys; "
		"node graph %d nodes %d links (%d visible, %d walk / %d inline "
		"rejected); %d bytes\n", argv[1], st.kept, st.pruned,
		st.dropped, st.keys, nst.nodes, nst.links, nst.visible,
		nst.rejected_walk, nst.rejected_inline, olen);
	free (blob);
	free (bsp);
	return 0;
}
