/* SPDX-License-Identifier: MIT */
/*
 * rec_hle.c -- validation of a map's HLE1 entity chunk and its HLN1 node
 * graph before the record module uses them in place
 * (docs/formats/hle-format-v1.md "Byte order", "Ids", "Node graph").
 * Pure over bytes and the id-space counts: the host fuzz test
 * (test/m3/rec_fuzz.c) feeds it any input.
 */
#include <stdint.h>
#include <string.h>

#define HLE_FORMAT_ONLY
#define HLN_FORMAT_ONLY
#include "tools/w64c/hl_entcook.h"
#include "tools/w64c/hl_nodecook.h"

#include "engine/ent/rec_int.h"

/* a string-valued key's id by its key and the record's class */
static const char *str_id_ok(const rec_limits_t *lim, uint16_t key, uint16_t cls, uint16_t id)
{
	uint32_t k = (uint32_t)id >> 13, i = (uint32_t)id & 0x1FFF;

	if (id == HLE_NONE)
		return NULL;
	if (key == RK_PATTERN)
		return id < lim->patterns ? NULL : "pattern id";
	if (key == RK_GLOBALSTATE)
		return id < lim->globals ? NULL : "global-state id";
	if (key == RK_MONSTERTYPE)
		return id < lim->classes ? NULL : "class id";
	if ((key == RK_MESSAGE && cls == RC_ENV_MESSAGE)
		|| (key == RK_CHAPTERTITLE && cls == RC_WORLDSPAWN))
		return id < lim->titles ? NULL : "title id";
	if (key == RK_TEXTURE && cls == RC_INFODECAL)
		return id < lim->ndecals ? NULL : "decal id";
	if (k == RES_K_SUBMODEL)
		return i && i < lim->nsub && i < 1024 ? NULL : "submodel id";
	if (k == 0 || k == RES_K_ANIM || i >= lim->kcount[k])
		return "res_t outside the table";
	return NULL;
}

const char *rec_hle_check(void *blob, uint32_t len, const rec_limits_t *lim,
	const void **graph, uint32_t *graph_size)
{
	uint8_t *b = blob;
	const hle_header_t *h = blob;
	const hle_rec_t *r;
	const hle_class_t *cl;
	const hle_keyname_t *kn;
	const hle_name_t *nm;
	const hle_key_t *keys;
	const uint16_t *l2r, *ll;
	const char *why;
	uint32_t i, j, tail;

	*graph = NULL;
	*graph_size = 0;
	if ((why = hle_fixup(blob, len)))
		return why;
	if (!(h->flags & HLE_F_IDS))
		return "no ids (cooked before the id step)";
	r = (const hle_rec_t *)(b + h->ofs_recs);
	cl = (const hle_class_t *)(b + h->ofs_classes);
	kn = (const hle_keyname_t *)(b + h->ofs_keynames);
	nm = (const hle_name_t *)(b + h->ofs_names);
	keys = (const hle_key_t *)(b + h->ofs_keys);
	l2r = (const uint16_t *)(b + h->ofs_lump2rec);
	ll = (const uint16_t *)(b + h->ofs_lumplist);
	for (i = 0; i < h->numclasses; i++)
		if (cl[i].id != HLE_NONE && cl[i].id >= lim->classes)
			return "class id";
	for (i = 0; i < h->numkeynames; i++)
		if (kn[i].id >= RK__N)
			return "key id outside keys.def";
	for (i = 0; i < h->numrecs; i++)
	{
		uint16_t cls = cl[r[i].cls].id;

		if (l2r[r[i].lump] != i)
			return "lump map disagrees with its record";
		if (r[i].flags & HLE_RF_AUXFIRE)
		{
			for (j = 0; j < r[i].aux_count; j++)
			{
				float d = ((const hle_fire_t *)(b + h->ofs_fire))[r[i].aux_first + j].delay;

				if (!(d >= 0.0f && d < 1e6f))
					return "fire delay";
			}
		}
		else
			for (j = 0; j < r[i].aux_count; j++)
				if (l2r[ll[r[i].aux_first + j]] == HLE_NONE)
					return "aux lump is not a kept record";
		for (j = r[i].key_first; j < (uint32_t)r[i].key_first + r[i].key_num; j++)
		{
			const hle_keyname_t *k = &kn[keys[j].kn];

			if (k->kind == HLE_KS)
			{
				if ((why = str_id_ok(lim, k->id, cls, keys[j].id)))
					return why;
			}
			else if (k->kind == HLE_KN)
			{
				if (keys[j].id != HLE_NONE && keys[j].id >= lim->classes)
					return "class id";
			}
			else if (keys[j].id != HLE_NONE)
				return "typed id on a number key";
		}
	}
	for (i = 0; i < h->numnames; i++)
		for (j = 0; j < nm[i].count; j++)
			if (l2r[ll[nm[i].first + j]] == HLE_NONE)
				return "name list lump is not a kept record";
	/* the node graph after align16 of the HLE1 size */
	tail = (h->size + 15) & ~15u;
	if (tail < len && len - tail >= 4)
	{
		const char *gw;

		if ((gw = hln_fixup(b + tail, len - tail)))
			return gw;
		*graph = b + tail;
		*graph_size = len - tail;
	}
	return NULL;
}
