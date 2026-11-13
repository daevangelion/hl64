/* SPDX-License-Identifier: MIT */
/*
 * host.h -- functions shared between the plat/host files. The engine sees
 * only engine/core/sys.h.
 */
#ifndef HL64_PLAT_HOST_H
#define HL64_PLAT_HOST_H

/* io_host.c: cart root (the DFS image source); NULL keeps ".". Reads
 * HL64_HOST_PI_US_PER_KB. */
void	host_io_init(const char *root);

#endif
