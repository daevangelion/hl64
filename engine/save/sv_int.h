/* SPDX-License-Identifier: MIT */
/*
 * sv_int.h -- engine/save internals: the stream's begin / end around a
 * slot image (sv.c), shared by save.c.
 */
#ifndef HL64_SV_INT_H
#define HL64_SV_INT_H

#include <stdint.h>

/* Work memory of the coder (16-aligned): the writer's history ring and
 * match heads, or the reader's history window. */
#define SV_WORK_BYTES	(32u * 1024u)

/* Writer: compressed bytes go to out[0, cap). */
void		sv_write_begin(void *out, uint32_t cap, void *work);
/* Flush; the compressed size, 0 when it did not fit cap. *raw = bytes put. */
uint32_t	sv_write_end(uint32_t *raw);
/* Bytes put so far, and bytes coded so far (past cap too). */
void		sv_write_pos(uint32_t *raw, uint32_t *coded);

/* Reader over in[0, n). */
void		sv_read_begin(const void *in, uint32_t n, void *work);
/* Bytes got so far (uncompressed). */
uint32_t	sv_read_pos(void);
/* The first failure's reason, NULL when none. */
const char	*sv_fail_reason(void);

/* CRC-32 (IEEE, reflected), crc = 0 to start. */
uint32_t	sv_crc32(uint32_t crc, const void *p, uint32_t n);

#endif
