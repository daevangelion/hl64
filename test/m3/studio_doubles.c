/* SPDX-License-Identifier: MIT */
/*
 * studio_doubles.c -- weak test doubles linked only into the studio test
 * drivers (hl64-studiotest, hl64-colltest) for services of slices that
 * land separately (Rec's resource table, FX, sound): models come from
 * studio_test_bind, effects and sounds are dropped. A real definition in
 * the link overrides each one.
 */
#include "engine/core/eng.h"
#include "hl64/fx.h"
#include "hl64/rec.h"
#include "hl64/snd.h"

__attribute__((weak)) int res_file(res_t r, int *file, uint32_t *ofs, uint32_t *size)
{
	(void)r;
	(void)file;
	(void)ofs;
	(void)size;
	return 0;
}

__attribute__((weak)) uint32_t res_file_rom(int file, uint32_t *size)
{
	(void)file;
	(void)size;
	return 0;
}

__attribute__((weak)) void fx_muzzle_flash(int e, int attachment, int type)
{
	(void)e;
	(void)attachment;
	(void)type;
}

__attribute__((weak)) void fx_sparks(const float org[3])
{
	(void)org;
}

__attribute__((weak)) void snd_ambient(const float org[3], res_t sound, float vol, float attn,
	int flags, int pitch)
{
	(void)org;
	(void)sound;
	(void)vol;
	(void)attn;
	(void)flags;
	(void)pitch;
}

/* engine/core/eng.c's frame counter (engine/world/seam.c stamps switches) */
__attribute__((weak)) uint32_t eng_frame_count(void)
{
	return 0;
}
