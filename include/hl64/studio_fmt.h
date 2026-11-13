/* SPDX-License-Identifier: MIT */
/*
 * studio_fmt.h -- the cooked studio blocks as C structs
 * (docs/formats/studio-format.md): the info block that studio_info()
 * returns, the animation chunk of studio_anim(), and the draw mesh the
 * engine's studio renderer reads. Natural alignment equals the cooked
 * layout. Resident blocks are in native byte order (the cart is
 * big-endian like the file; the host swaps once at load).
 */
#ifndef HL64_STUDIO_FMT_H
#define HL64_STUDIO_FMT_H

#include <stdint.h>

#define SFMT_MODEL_MAGIC	0x5336344Du	/* S64M */
#define SFMT_INFO_MAGIC		0x53494E46u	/* SINF */
#define SFMT_MESH_MAGIC		0x534D5348u	/* SMSH */
#define SFMT_ANIM_MAGIC		0x41363441u	/* A64A */
#define SFMT_VERSION		1
#define SFMT_F_CHROME		1
#define SFMT_F_VIEW		2

/* MDL texture flags (pad2 of a mesh texture) */
#define SFMT_NF_CHROME		0x0002
#define SFMT_NF_FULLBRIGHT	0x0004
#define SFMT_NF_ADDITIVE	0x0020
#define SFMT_NF_MASKED		0x0040

typedef struct {
	uint32_t	magic;
	uint16_t	version, flags;
	uint32_t	info_ofs, info_size;
	uint32_t	mesh_ofs, mesh_size;
	uint32_t	anim_size, anim_crc;
	char		name[32];
} sfmt_file_t;			/* 64 */

typedef struct {
	uint32_t	magic;
	uint8_t		numbones, numctl, numhitboxes, numattach;
	uint16_t	numseq;
	uint8_t		numbodyparts, numtransitions;
	uint32_t	flags;
	float		eyeposition[3];
	float		min[3], max[3];
	float		bbmin[3], bbmax[3];
	uint16_t	numevents, nsub;
	uint32_t	bone_ofs, ctl_ofs, hitbox_ofs, attach_ofs, seq_ofs, event_ofs;
	uint32_t	bodypart_ofs, trans_ofs, str_ofs, str_size;
	uint32_t	reserved[2];
} sfmt_info_t;			/* 128 */

typedef struct {
	int8_t		parent;
	int8_t		ctl[6];
	uint8_t		flags;
	float		value[6];
	float		scale[6];
} sfmt_bone_t;			/* 56 */

typedef struct {
	int8_t		bone;
	uint8_t		index;
	uint16_t	type;
	float		start, end;
	uint8_t		rest;
	uint8_t		pad[3];
} sfmt_ctl_t;			/* 16 */

typedef struct {
	uint8_t		bone, group;
	uint16_t	pad;
	float		bbmin[3], bbmax[3];
} sfmt_hitbox_t;		/* 28 */

typedef struct {
	uint8_t		bone;
	uint8_t		pad[3];
	float		org[3];
} sfmt_attach_t;		/* 16 */

typedef struct {
	uint16_t	label;
	uint8_t		numblends, motionbone;
	float		fps;
	uint32_t	flags;
	int16_t		activity;
	uint16_t	actweight;
	uint16_t	numframes, numevents;
	uint16_t	event_first;
	uint8_t		entrynode, exitnode;
	uint32_t	motiontype;
	float		linearmovement[3];
	float		bbmin[3], bbmax[3];
	uint16_t	blendtype[2];
	float		blendstart[2], blendend[2];
	uint8_t		nodeflags, pad;
	uint16_t	nextseq;
	uint32_t	anim_ofs, anim_size;
} sfmt_seq_t;			/* 96 */

typedef struct {
	int32_t		frame;
	int32_t		event;
	uint16_t	options;	/* string */
	uint16_t	res;		/* res_t, 0 none */
} sfmt_event_t;			/* 12 */

typedef struct {
	uint16_t	nummodels, base, sub_first, pad;
} sfmt_bodypart_t;		/* 8 */

typedef struct {
	uint32_t	magic;
	uint16_t	numseq, numbones;
	uint32_t	size, reserved;
} sfmt_anim_hdr_t;		/* 16 */

typedef struct {
	uint32_t	magic;
	uint16_t	nsub, nruns, nmeshlets, nloads, nslots, ntris;
	uint8_t		nbslots, pos_shift, nskinref, nfamilies;
	uint16_t	ntex, reserved;
	uint32_t	bslot_ofs, sub_ofs, run_ofs, meshlet_ofs, load_ofs, vert_ofs;
	uint32_t	tri_ofs, skin_ofs, tex_ofs, nbone_ofs;
} sfmt_mesh_t;			/* 64 */

typedef struct {
	uint16_t	run_first, run_count;
} sfmt_sub_t;

typedef struct {
	uint16_t	skinref, meshlet_first, meshlet_count, pad;
} sfmt_run_t;

typedef struct {
	uint16_t	load_first;
	uint8_t		nloads, nverts;
	uint16_t	tri_first, ntris;
} sfmt_meshlet_t;

typedef struct {
	uint8_t		bslot, cache, count, pad;
} sfmt_load_t;

/* Accessors over a resident (native-order) info block. */
#define SFMT_AT(info, ofs, type)	((const type *)((const uint8_t *)(info) + (ofs)))
#define SFMT_BONES(i)		SFMT_AT(i, (i)->bone_ofs, sfmt_bone_t)
#define SFMT_CTLS(i)		SFMT_AT(i, (i)->ctl_ofs, sfmt_ctl_t)
#define SFMT_HITBOXES(i)	SFMT_AT(i, (i)->hitbox_ofs, sfmt_hitbox_t)
#define SFMT_ATTACHES(i)	SFMT_AT(i, (i)->attach_ofs, sfmt_attach_t)
#define SFMT_SEQS(i)		SFMT_AT(i, (i)->seq_ofs, sfmt_seq_t)
#define SFMT_EVENTS(i)		SFMT_AT(i, (i)->event_ofs, sfmt_event_t)
#define SFMT_BODYPARTS(i)	SFMT_AT(i, (i)->bodypart_ofs, sfmt_bodypart_t)
#define SFMT_TRANS(i)		SFMT_AT(i, (i)->trans_ofs, uint8_t)
#define SFMT_STR(i, o)		SFMT_AT(i, (i)->str_ofs + (o), char)

/* Animation chunk value word: run header valid / total. */
#define SFMT_VALID(w)		((int)((uint16_t)(w) & 0xFF))
#define SFMT_TOTAL(w)		((int)((uint16_t)(w) >> 8))

#endif
