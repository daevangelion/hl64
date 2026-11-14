/* SPDX-License-Identifier: MIT */
/*
 * w64.h -- the cooked .w64 layer package, version 2
 * (docs/formats/w64-format.md), as structs read in place. Retargeted from
 * the old tree's source/w64_format.h (audit entry CLEAN): the structs,
 * flags, fourccs and gid macros are kept; the HLSDK material-letter table
 * (its lines 110-120) is not carried (the game owns material letters);
 * w64_vert_t spells out the vertex pair layout so engine code needs no
 * tiny3d header (plat/n64/gpu.c asserts it equals T3DVertPacked).
 *
 * Big-endian on the cart, read in place; the host swaps every table and
 * chunk once when it lands (engine/world).
 */
#ifndef HL64_W64_H
#define HL64_W64_H

#include <stdint.h>

#define W64_MAGIC		0x5736344Cu	/* 'W64L' */
#define W64_VERSION		3	/* 3: TEXP additive ramps (W64_TEXF_RAMP) */

/* w64_header_t.flags */
#define W64_HF_COLL		1	/* COLL chunks + model / leaf contents / grid tables */

/* Chunk fourccs (big-endian u32 of the four characters). */
#define W64_CHUNK_REND		0x52454E44u	/* sector meshlets + light probes */
#define W64_CHUNK_MOVR		0x4D4F5652u	/* mover (submodel) meshlets, local space */
#define W64_CHUNK_TEXP		0x54455850u	/* texture page */
#define W64_CHUNK_COLL		0x434F4C4Cu	/* hull tree nodes */
#define W64_CHUNK_ENTS		0x454E5453u	/* HLE1 entity blob */
#define W64_CHUNK_VISR		0x56495352u	/* compressed PVS rows */

/* w64_sub_t.kind */
#define W64_SUB_NONE		0	/* not drawn (triggers, empty) */
#define W64_SUB_FOLDED		1	/* static: baked into the sector meshlets */
#define W64_SUB_MOVR		2	/* drawn from its MOVR chunk with the entity matrix */

/* w64_tex_t.flags; w64_texp_t.flags (TEXP pages, studio skins) use FENCE and RAMP */
#define W64_TEXF_FENCE		1	/* '{' texture: TLUT[0] transparent, alpha compare */
#define W64_TEXF_SKY		2
#define W64_TEXF_ANIM		4	/* '+' animation / alternate sequence */
#define W64_TEXF_MISSING	8	/* no texels: faces were not cooked */
#define W64_TEXF_RAMP		16	/* page: the cooked additive ramp (addblend.h) at
					 * W64_TEXP_RAMP, CI4 from 88 (render-fx.md 2a) */
#define W64_TEXP_RAMP		48	/* page offset of the ramp: after header + TLUT */

/* Strip index bits (tiny3d t3d_indexbuffer_convert encoding). */
#define W64_IDX_RESTART		0x8000
#define W64_IDX_LAST		0x4000

#define W64_NONE		0xFFFF	/* u16 "no index": style, page, chunk, texture */

/* Collision node ids: gid = COLL chunk (relative to coll_first) << 12 | local. */
#define W64_GID_LOCAL_BITS	12
#define W64_GID_LOCAL_MASK	0xFFFu
#define W64_GID_COLL(gid)	((uint32_t)(gid) >> W64_GID_LOCAL_BITS)
#define W64_GID_LOCAL(gid)	((uint32_t)(gid) & W64_GID_LOCAL_MASK)
#define W64_COLL_MAX_NODES	4095
#define W64_LEAF_OF(child)	(-1 - (child))	/* hull-0 leaf child -> BSP leaf */

#define W64_GRID_CELL		256	/* collision grid cell (u) */
#define W64_PROBE_STEP		64	/* light probe spacing (u) */
#define W64_MAX_MESHLET_VERTS	64
#define W64_MAX_MESHLET_IDX	104

typedef struct {
	uint32_t	magic;
	uint16_t	version;
	uint16_t	flags;		/* W64_HF_* */
	uint32_t	file_size;
	uint32_t	dir_ofs;	/* layer directory, from file start */
	uint32_t	dir_size;
	uint32_t	nchunks;
	uint32_t	dir_crc;	/* CRC-32 of the directory bytes */
	uint16_t	idx_stride;	/* tiny3d VERT_OUTPUT_SIZE the indices were cooked for */
	uint16_t	idx_base;	/* RSP_T3D_VERT_BUFFER & 0xFFFF */
	uint16_t	style0;		/* lightstyle 0 value baked into vertex colour */
	uint16_t	lightscale_x100;	/* light scale baked into vertex colour, x100 */
	int16_t		bbox[6];	/* world mins, maxs */
	uint16_t	max_verts;	/* per meshlet */
	uint16_t	max_idx;
	uint8_t		pad[12];
} w64_header_t;			/* 64 */

typedef struct {
	uint16_t	nsect, nleaf, ntex, npages, nsub, nchunks, texp_first, nvislists;
	uint32_t	chunkdir_ofs;	/* offsets below: from directory start */
	uint32_t	sect_ofs;
	uint32_t	pagepool_ofs;
	uint32_t	tex_ofs;
	uint32_t	sub_ofs;
	uint32_t	leaf_ofs;
	uint32_t	vispool_ofs;
	uint16_t	coll_first;	/* chunk index of the first COLL chunk */
	uint16_t	ncoll;
	uint16_t	coll_pinned;	/* first coll_pinned COLL chunks: world hull 0 */
	uint16_t	nleafs_all;	/* BSP leaf lump count */
	uint16_t	ents_chunk;	/* ENTS chunk index, W64_NONE none */
	uint16_t	visr_chunk;	/* VISR chunk index, W64_NONE none */
	uint32_t	model_ofs;	/* w64_model_t[nsub] */
	uint32_t	leafc_ofs;	/* int8_t contents[nleafs_all] */
	uint32_t	grid_ofs;	/* w64_grid_t */
	uint32_t	mat_ofs;	/* w64_mat_t surface materials, 0 = none */
	uint8_t		pad[24];
} w64_dir_t;			/* 96 */

typedef struct {
	uint16_t	nent, nmix, nface, pad;
	uint32_t	start_ofs, ent_ofs, mix_ofs, face_ofs;
} w64_mat_t;			/* 24 */

typedef struct {
	uint32_t	gid;
	uint16_t	first, count;
} w64_matmix_t;			/* 8 */

typedef struct {
	int16_t		lo[2], hi[2];
	uint8_t		mat, pad;
} w64_matface_t;		/* 10 */

typedef struct {
	uint32_t	fourcc;
	uint32_t	ofs;		/* from file start */
	uint32_t	size;
	uint32_t	crc;
} w64_chunk_t;			/* 16 */

typedef struct {
	int16_t		bbox[6];
	uint16_t	rend_chunk;
	uint16_t	page_first;	/* into the page pool */
	uint16_t	page_count;
	uint16_t	pad;
} w64_sector_t;			/* 20 */

typedef struct {
	uint16_t	page;		/* TEXP page (texp_first + page = chunk), W64_NONE */
	uint8_t		flags;
	uint8_t		anim_total;	/* world-surfaces.md 4 */
	uint8_t		anim_min;
	uint8_t		anim_max;
	uint16_t	pad;
	uint16_t	anim_next;	/* texture index */
	uint16_t	alternate;	/* texture index of the other sequence's frame 0 */
} w64_tex_t;			/* 12 */

typedef struct {
	uint16_t	kind;		/* W64_SUB_* */
	uint16_t	chunk;		/* MOVR chunk index */
	int16_t		bbox[6];	/* local space */
} w64_sub_t;			/* 16 */

typedef struct {
	float		mins[3], maxs[3], origin[3];
	int32_t		head[4];	/* hull roots: gid (>= 0) or a leaf child (< 0) */
	int32_t		visleafs;
	uint16_t	coll_first;	/* relative to dir.coll_first */
	uint16_t	coll_count;
	uint8_t		pad[4];
} w64_model_t;			/* 64 */

typedef struct {
	int16_t		origin[3];	/* min corner of cell (0,0,0) */
	uint16_t	dims[3];
	uint16_t	cell;		/* W64_GRID_CELL */
	uint16_t	nstart;
	uint32_t	cells_ofs;	/* w64_gcell_t[], from directory start */
	uint32_t	start_ofs;	/* int32_t[nstart] */
	uint32_t	lists_ofs;	/* u16 list pool */
	uint32_t	pad;
} w64_grid_t;			/* 32 */

typedef struct {
	uint16_t	list;
	uint16_t	start;
} w64_gcell_t;			/* 4 */

/* COLL chunk: header, w64_cplane_t[nplanes], w64_cnode_t[nnodes], u32 extern[nextern]. */
typedef struct {
	uint16_t	nnodes;
	uint16_t	nx, ny, nz;	/* [0,nx) axial X, [nx,ny) Y, [ny,nz) Z, then non-axial */
	uint16_t	nplanes;
	uint16_t	nextern;
	uint32_t	pad;
} w64_coll_t;			/* 16 */

typedef struct {
	float		normal[3];
	float		dist;
} w64_cplane_t;			/* 16 */

typedef struct {
	union {
		float		dist;	/* axial: plane dist */
		uint32_t	plane;	/* non-axial: chunk plane index */
	} u;
	int16_t		child[2];	/* < nnodes local; >= nnodes extern; < 0 leaf */
} w64_cnode_t;			/* 8 */

/* REND / MOVR chunk */
typedef struct {
	uint16_t	nmeshlets;
	uint16_t	nstyled;
	uint32_t	vert_ofs;	/* offsets from chunk start */
	uint32_t	idx_ofs;
	uint32_t	style_ofs;
	uint32_t	probe_ofs;	/* REND light probes, 0 = none */
	uint8_t		probe_dims[3];
	uint8_t		pad;
} w64_rend_t;			/* 24, then meshlets[], then style records[] */

typedef struct {
	int16_t		bbox[6];
	uint32_t	vert;		/* vofs << 8 | nverts */
	uint32_t	idx;		/* iofs << 8 | nidx */
	uint16_t	tex;		/* texture index */
	uint16_t	style;		/* style record index or W64_NONE */
} w64_meshlet_t;		/* 24 */

typedef struct {
	uint8_t		styles[4];	/* lightstyle numbers, 255 = unused */
	uint32_t	data_ofs;	/* from chunk start: nverts x nstyles u8 luxels */
} w64_style_t;			/* 8 */

/* A vertex pair as cooked (tiny3d T3DVertPacked). */
typedef struct {
	int16_t		posA[3];
	uint16_t	normA;		/* 5.6.5 packed normal */
	int16_t		posB[3];
	uint16_t	normB;
	uint32_t	rgbaA;
	uint32_t	rgbaB;
	int16_t		stA[2];		/* s10.5 texel coordinates */
	int16_t		stB[2];
} w64_vert_t;			/* 32 */

/* TEXP chunk: header, TLUT (16 x RGBA5551) at 16, the additive ramp at 48
 * when W64_TEXF_RAMP, CI4 texels at ci4_ofs. */
typedef struct {
	uint16_t	w, h;
	uint8_t		flags;		/* W64_TEXF_FENCE, W64_TEXF_RAMP */
	uint8_t		pad[3];
	uint32_t	ci4_ofs;
	uint32_t	pad2;
} w64_texp_t;			/* 16 */

#define W64_MESHLET_NV(m)	((int)((m)->vert & 0xFF))
#define W64_MESHLET_VOFS(m)	((m)->vert >> 8)
#define W64_MESHLET_NI(m)	((int)((m)->idx & 0xFF))
#define W64_MESHLET_IOFS(m)	((m)->idx >> 8)

/* Natural alignment equals the cooked layout (no packing needed). */
_Static_assert(sizeof(w64_header_t) == 64, "w64_header_t");
_Static_assert(sizeof(w64_dir_t) == 96, "w64_dir_t");
_Static_assert(sizeof(w64_chunk_t) == 16, "w64_chunk_t");
_Static_assert(sizeof(w64_sector_t) == 20, "w64_sector_t");
_Static_assert(sizeof(w64_tex_t) == 12, "w64_tex_t");
_Static_assert(sizeof(w64_sub_t) == 16, "w64_sub_t");
_Static_assert(sizeof(w64_model_t) == 64, "w64_model_t");
_Static_assert(sizeof(w64_grid_t) == 32, "w64_grid_t");
_Static_assert(sizeof(w64_gcell_t) == 4, "w64_gcell_t");
_Static_assert(sizeof(w64_coll_t) == 16, "w64_coll_t");
_Static_assert(sizeof(w64_cplane_t) == 16, "w64_cplane_t");
_Static_assert(sizeof(w64_cnode_t) == 8, "w64_cnode_t");
_Static_assert(sizeof(w64_rend_t) == 24, "w64_rend_t");
_Static_assert(sizeof(w64_meshlet_t) == 24, "w64_meshlet_t");
_Static_assert(sizeof(w64_style_t) == 8, "w64_style_t");
_Static_assert(sizeof(w64_vert_t) == 32, "w64_vert_t");
_Static_assert(sizeof(w64_texp_t) == 16, "w64_texp_t");
_Static_assert(sizeof(w64_mat_t) == 24, "w64_mat_t");
_Static_assert(sizeof(w64_matmix_t) == 8, "w64_matmix_t");
_Static_assert(sizeof(w64_matface_t) == 10, "w64_matface_t");

#endif
