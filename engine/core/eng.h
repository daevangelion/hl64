/* SPDX-License-Identifier: MIT */
/*
 * eng.h -- engine boot and the frame loop (docs/architecture.md 3.1, 3.2).
 * The platform's main() initialises the hardware (or its host model),
 * calls eng_boot() once and eng_frame() forever; the loop leaves through
 * sys_exit (DBG_QUIT) or sys_error.
 */
#ifndef HL64_ENG_H
#define HL64_ENG_H

#include <stdint.h>

/* Boot steps after the platform's: carve, settings, io, debug channel.
 * Logs the carve table and "boot ok". */
void		eng_boot(const char *platform);
void		eng_frame(void);

#endif
