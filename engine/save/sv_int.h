/* SPDX-License-Identifier: MIT */
/*
 * sv_int.h -- engine/save internals: the stream's staging, coder and
 * decoder around a slot image (sv.c), shared by save.c.
 */
#ifndef HL64_SV_INT_H
#define HL64_SV_INT_H

#include <stdint.h>

/* Work memory of the coder (16-aligned): its hash heads and chain links. */
#define SV_WORK_BYTES	(24u * 1024u)

/* Writer: the sections' bytes go to raw[0, cap); work (SV_WORK_BYTES, the
 * coder's, idle until sv_code_begin) is the sections' sv_work_area. */
void		sv_write_begin(void *raw, uint32_t cap, void *work);
/* The staged size, 0 when it did not fit cap (or a section failed). */
uint32_t	sv_write_end(void);
/* Bytes put so far (past cap too: what it would need). */
uint32_t	sv_write_pos(void);
#ifdef HL64_TEST
/* Time the sv_check_time windows took since sv_write_begin (us). */
uint32_t	sv_check_us(void);
#endif

/* Coder over the staged bytes (after sv_write_end) into out[0, cap):
 * sv_code_step works for about budget_us, 1 while bytes are left; then
 * sv_code_end: the image size, 0 when it does not fit cap (test builds
 * log the sv_part table). sv_code_need: image bytes so far. */
void		sv_code_begin(void *out, uint32_t cap, void *work);
int		sv_code_step(uint32_t budget_us);
uint32_t	sv_code_end(void);
uint32_t	sv_code_need(void);

/* Decoder: in[0, n) into raw[0, cap); sv_decode_step: 1 more to do, 0
 * done, -1 corrupt; sv_decode_end: the state's size, 0 corrupt. */
void		sv_decode_begin(const void *in, uint32_t n, void *raw, uint32_t cap);
int		sv_decode_step(uint32_t budget_us);
uint32_t	sv_decode_end(void);

/* Reader over the decoded raw[0, n); work as for the writer. */
void		sv_read_begin(const void *raw, uint32_t n, void *work);
/* Bytes got so far. */
uint32_t	sv_read_pos(void);
/* The first failure's reason, NULL when none. */
const char	*sv_fail_reason(void);

/* CRC-32 (IEEE, reflected), crc = 0 to start. */
uint32_t	sv_crc32(uint32_t crc, const void *p, uint32_t n);

#endif
