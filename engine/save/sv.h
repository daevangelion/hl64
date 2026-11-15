/* SPDX-License-Identifier: MIT */
/*
 * sv.h -- the save stream (docs/architecture.md 14): one writer or one
 * reader at a time, LZ-compressed into / out of the slot image. Sections
 * follow each other in a fixed order, each opened by a 4-byte tag; a
 * reader that meets another tag, or runs past the end, marks the save
 * corrupt (sv_failed) and every later get reads zeros. Values are in the
 * machine's own byte order (a save never moves between cart and host).
 */
#ifndef HL64_SV_H
#define HL64_SV_H

#include <stdint.h>

#define SV_TAG(a, b, c, d) \
	((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

/* ---- writer ------------------------------------------------------------- */

void		sv_tag(uint32_t tag);
void		sv_put(const void *p, uint32_t n);
void		sv_u8(uint32_t v);
void		sv_u16(uint32_t v);
void		sv_u32(uint32_t v);
void		sv_f32(float v);
void		sv_f64(double v);

/* ---- reader ------------------------------------------------------------- */

/* The next section's tag must be tag: 0, else -1 (corrupt). */
int		sv_expect(uint32_t tag);
void		sv_get(void *p, uint32_t n);
uint32_t	sv_get_u8(void);
uint32_t	sv_get_u16(void);
uint32_t	sv_get_u32(void);
float		sv_get_f32(void);
double		sv_get_f64(void);
/* Mark the save being read corrupt (a value out of range), with a reason
 * for the log; the first reason is kept. */
void		sv_fail(const char *why);
int		sv_failed(void);

#endif
