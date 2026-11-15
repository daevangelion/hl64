/* SPDX-License-Identifier: MIT */
/*
 * settings.c -- defaults and the debug-channel descriptor table of the
 * typed settings record (include/hl64/settings.h, architecture 12).
 * Movevars are the HL defaults of specs/pmove-host.md 4; yaw/pitch speeds
 * are HL's documented client defaults (210 / 225 deg/s).
 */
#include <stddef.h>
#include <string.h>

#include "hl64/limits.h"
#include "hl64/settings.h"

settings_t settings;

const setting_desc_t settings_desc[SET_COUNT] = {
#define SETTINGS_DESC(id, field, type, lo, hi, persist) \
	{ #field, (uint16_t)offsetof(settings_t, field), SET_T_##type, persist, lo, hi },
	SETTINGS_FIELDS(SETTINGS_DESC)
#undef SETTINGS_DESC
};

void settings_reset(void)
{
	memset(&settings, 0, sizeof(settings));
	settings.gravity = 800;
	settings.stopspeed = 100;
	settings.maxspeed = 320;
	settings.spectatormaxspeed = 500;
	settings.accelerate = 10;
	settings.airaccelerate = 10;
	settings.wateraccelerate = 10;
	settings.friction = 4;
	settings.edgefriction = 2;
	settings.waterfriction = 1;
	settings.entgravity = 1;
	settings.bounce = 1;
	settings.stepsize = 18;
	settings.maxvelocity = 2000;
	settings.zmax = 4096;
	settings.yawspeed = 210;
	settings.pitchspeed = 225;
	settings.fov = 90;
	settings.footsteps = 1;
	settings.skill = 1;
	settings.vol_master = 100;
	settings.vol_sfx = 100;
	settings.stick_deadzone = 8;
	settings.r_openseam = 1;
	settings.r_dither = 1;
	settings.ai_spread = 1;
	settings.ai_budget = HL64_AI_BUDGET;
	settings.ai_hold = HL64_AI_HOLD;
	settings.look_sens = 100;
	/* 100: GoldSrc's defaults play the MP3s at MP3Volume 0.8 and the sounds
	 * at volume 0.7 (music 1.2 dB over the sounds); with vol_sfx 100 the
	 * nearest is 100 (docs/specs/sound.md 6, music level) */
	settings.vol_music = 100;
}

int settings_set(int id, float v)
{
	const setting_desc_t *d;
	char *p;

	if (id < 0 || id >= SET_COUNT)
		return -1;
	d = &settings_desc[id];
	p = (char *)&settings + d->offset;
	if (!(v >= d->lo))	/* NaN too */
		v = d->lo;
	if (v > d->hi)
		v = d->hi;
	if (d->type == SET_T_F32)
		memcpy(p, &v, sizeof(v));
	else
		*(uint8_t *)p = (uint8_t)(v + 0.5f);
	return 0;
}

int settings_get(int id, float *v)
{
	const setting_desc_t *d;
	const char *p;

	if (id < 0 || id >= SET_COUNT)
		return -1;
	d = &settings_desc[id];
	p = (const char *)&settings + d->offset;
	if (d->type == SET_T_F32)
		memcpy(v, p, sizeof(*v));
	else
		*v = *(const uint8_t *)p;
	return 0;
}

int settings_find(const char *name)
{
	int i;

	for (i = 0; i < SET_COUNT; i++)
		if (!strcmp(settings_desc[i].name, name))
			return i;
	return -1;
}
