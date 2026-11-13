/* SPDX-License-Identifier: MIT */
/*
 * dlight.c -- the dynamic light pool and the world falloff
 * (docs/specs/dlights.md 1, 3), written from the spec. Sources (entity
 * effects, game effects, the flashlight) allocate lights from M3 on.
 */
#include <string.h>

#include "hl64/limits.h"
#include "engine/render/ren.h"

static ren_dlight_t	lights[HL64_DLIGHTS];
static uint8_t		live[HL64_DLIGHTS];

ren_dlight_t *ren_dlight_alloc(int key, double time)
{
	int i, pick = 0;

	if (key)
	{
		for (i = 0; i < HL64_DLIGHTS; i++)
			if (live[i] && lights[i].key == key)
				break;
		if (i < HL64_DLIGHTS)
		{
			pick = i;
			goto out;
		}
	}
	for (i = 0; i < HL64_DLIGHTS; i++)
		if (!live[i] || lights[i].die < time)
		{
			pick = i;
			break;
		}
out:
	memset(&lights[pick], 0, sizeof(lights[pick]));
	lights[pick].key = key;
	lights[pick].world = 1;
	live[pick] = 1;
	return &lights[pick];
}

void ren_dlight_frame(double time, double dt)
{
	int i;

	for (i = 0; i < HL64_DLIGHTS; i++)
	{
		ren_dlight_t *l = &lights[i];

		if (!live[i])
			continue;
		if (l->die < time)
		{
			live[i] = 0;
			continue;
		}
		l->radius -= l->decay * (float)dt;
		if (l->radius <= 0)
			live[i] = 0;
	}
}

int ren_dlight_live(int i)
{
	return live[i];
}

ren_dlight_t *ren_dlight(int i)
{
	return &lights[i];
}

/* max(0, (radius - |p|) - q) when radius - |p| >= minlight, else 0 */
float ren_dlight_falloff(float radius, float minlight, float p, float q)
{
	float reach = radius - (p < 0 ? -p : p), c;

	if (reach < minlight)
		return 0;
	c = reach - q;
	return c > 0 ? c : 0;
}
