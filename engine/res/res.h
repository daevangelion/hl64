/* SPDX-License-Identifier: MIT */
/*
 * res.h -- engine/res internals: the campaign.rtb reader (docs/formats/
 * rtb-format.md "Validation") split from the boot loader so the host
 * tests can feed it any bytes (test/m3/rec_test.c fuzz). The public API is
 * the "resources" part of include/hl64/rec.h.
 */
#ifndef HL64_RES_INT_H
#define HL64_RES_INT_H

#include <stdint.h>

#define RTB_MAGIC	0x52544231u	/* 'RTB1' */
#define RTB_NSECT	26
#define RTB_CART	15		/* FILE .. TTXT */
#define RTB_DIR_END	(32 + 16 * RTB_NSECT)
#define RTB_TITLE_TEXT_MAX	1023	/* bytes of one title's text */

enum {
	RTB_FILE, RTB_STRS, RTB_MAPS, RTB_KIND, RTB_RES, RTB_SENT, RTB_WORD,
	RTB_SGRP, RTB_PATN, RTB_PPOL, RTB_DEFS, RTB_CLAS, RTB_GLOB, RTB_TITL,
	RTB_TTXT, RTB_TARG, RTB_TLMP, RTB_NMAP, RTB_NRES, RTB_NSEN, RTB_NSGR,
	RTB_NCLS, RTB_NTGT, RTB_NGLB, RTB_NTTL, RTB_NSTR
};

typedef struct {
	uint32_t	ofs[RTB_NSECT], size[RTB_NSECT], count[RTB_NSECT];
	uint32_t	file_size, build_id, cart_end;	/* cart_end = TARG ofs */
} rtb_dir_t;

/* Header and directory: b holds at least RTB_DIR_END bytes of a file of
 * file_size bytes. NULL, or the rule broken (offset in *bad). */
const char	*rtb_parse_dir(const uint8_t *b, uint32_t file_size, rtb_dir_t *d,
			uint32_t *bad);
/* Cart sections (FILE .. TTXT): b holds the file's first d->cart_end
 * bytes. Checks every cart rule of the validation list (padding between
 * the cart sections included). */
const char	*rtb_parse_cart(const uint8_t *b, const rtb_dir_t *d, uint32_t *bad);
/* Whole file (host, debug builds): padding everywhere, the host name
 * sections, build_id. */
const char	*rtb_parse_rest(const uint8_t *b, const rtb_dir_t *d, uint32_t *bad);
/* Copy the validated cart part into the RESTAB region (native order);
 * resolve = look the files up on the cart (sys_cart_addr), else rom 0.
 * Returns the bytes the tables use. */
uint32_t	res_install(const uint8_t *b, const rtb_dir_t *d, int resolve);
/* Drop the tables (tests). */
void		res_reset(void);
/* CRC-32 (zlib polynomial), running: crc = rtb_crc32(crc, p, n), start 0. */
uint32_t	rtb_crc32(uint32_t crc, const uint8_t *p, uint32_t n);

/* `mem` / roll-up figures. */
typedef struct {
	uint32_t	file_bytes, cart_bytes, table_bytes, region_bytes;
	uint32_t	files, res;
} res_stats_t;
void		res_stats(res_stats_t *s);
void		res_mem_line(void);

#endif
