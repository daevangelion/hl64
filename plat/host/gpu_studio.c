/* SPDX-License-Identifier: MIT */
/*
 * gpu_studio.c -- the host's studio draw-list recorder (D5;
 * engine/render/gpu_studio.h): each entity's runs are walked as the cart
 * would replay them -- per meshlet its bone-run loads and triangles -- and
 * counted; an FNV-1a hash covers the bound skins, the entity and bone
 * matrices (rounded to 1/16 u and 1/4096) and the run list, so a route's
 * studio draw is comparable with itself across changes.
 */
#include <math.h>
#include <string.h>

#include "engine/render/gpu_studio.h"

static int		pool_half, pool_used;
static gpu_studio_stats_t cur, done;

static void mix(const void *p, uint32_t n)
{
	const uint8_t *b = p;

	while (n--)
		cur.hash = (cur.hash ^ *b++) * 16777619u;
}

static void mixf(float v, float q)
{
	int32_t i = (int32_t)lrintf(v * q);

	mix(&i, 4);
}

int gpu_studio_init(void *mem, uint32_t bytes)
{
	(void)mem;
	pool_half = (int)(bytes / 64 / 2);	/* T3DMat4FP size on the cart */
	return pool_half;
}

uint32_t gpu_studio_state_size(const sfmt_mesh_t *mesh)
{
	(void)mesh;
	return 16;
}

int gpu_studio_build(void *state, const sfmt_mesh_t *mesh)
{
	(void)mesh;
	memset(state, 0, 16);
	return 0;
}

void gpu_studio_release(void *state)
{
	(void)state;
}

void gpu_studio_begin(void)
{
	pool_used = 0;
	memset(&cur, 0, sizeof(cur));
	cur.hash = 2166136261u;
}

void gpu_studio_end(void)
{
	done = cur;
}

static void walk(const sfmt_mesh_t *mesh, const gpu_studio_ent_t *e)
{
	const uint8_t *base = (const uint8_t *)mesh;
	const sfmt_run_t *runs = (const sfmt_run_t *)(base + mesh->run_ofs);
	const sfmt_meshlet_t *ml = (const sfmt_meshlet_t *)(base + mesh->meshlet_ofs);
	int k, i;

	for (k = 0; k < e->nruns; k++)
	{
		const sfmt_run_t *ru = &runs[e->runs[k]];

		mix(&e->runs[k], 2);
		mix(&e->tex[k], 2);
		for (i = ru->meshlet_first; i < ru->meshlet_first + ru->meshlet_count; i++)
		{
			cur.meshlets++;
			cur.verts += ml[i].nverts;
			cur.tris += ml[i].ntris;
		}
	}
	mix(e->rgba, 4);
	mix(&e->mode, 1);
	cur.ents++;
	cur.runs += (uint32_t)e->nruns;
}

int gpu_studio_draw(void *state, const sfmt_mesh_t *mesh, const gpu_studio_ent_t *e)
{
	int k, r, c;

	(void)state;
	if (pool_used + 1 + e->nbones > pool_half)
		return -1;
	pool_used += 1 + e->nbones;
	for (k = 0; k < 3; k++)
		mixf(e->model.m[3][k], 16.0f);
	for (k = 0; k < e->nbones; k++)
		for (r = 0; r < 3; r++)
			for (c = 0; c < 4; c++)
				mixf(e->bones[k][r][c], c == 3 ? 16.0f : 4096.0f);
	walk(mesh, e);
	cur.mats += (uint32_t)(1 + e->nbones);
	return 0;
}

int gpu_studio_draw_cpu(void *state, const sfmt_mesh_t *mesh, const w64_vert_t *verts,
	const gpu_studio_ent_t *e)
{
	(void)state;
	if (pool_used + 1 > pool_half)
		return -1;
	pool_used++;
	mix(verts[0].posA, sizeof(verts[0].posA));
	walk(mesh, e);
	cur.mats++;
	return 0;
}

void gpu_studio_stats(gpu_studio_stats_t *s)
{
	*s = done;
}
