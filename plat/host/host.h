/* SPDX-License-Identifier: MIT */
/*
 * host.h -- functions shared between the plat/host files. The engine sees
 * only engine/core/sys.h.
 */
#ifndef HL64_PLAT_HOST_H
#define HL64_PLAT_HOST_H

/* io_host.c: cart root (the DFS image source); NULL keeps "." and mounts
 * nothing (a ROM without content). With a root, mounts it as the cart's
 * libdragon mounts its DFS: the lookup table (8 + 16 B per file) and the
 * prefix block from the heap, for the files the Makefile packs (every
 * regular file but *.ent) plus extra_files the ROM also carries
 * (hl64-host: dbg.cmd, route.rbc). Reads HL64_HOST_PI_US_PER_KB. */
void	host_io_init(const char *root);
void	host_io_init_files(const char *root, unsigned extra_files);

/* flash_host.c: the FlashRAM image file (hl64-host --save); before
 * sys_flash_init. Without it: hl64.fla beside the hl64-host binary. */
void	host_flash_set_path(const char *path);

/* pak_host.c: what is in controller 1 (hl64-host --pak): "rumble" a
 * Rumble Pak, else a Controller Pak whose 32 KB is that image file
 * (formatted when missing). Without it: nothing. */
void	host_pak_set(const char *arg);

/* gpu.c (the draw-list recorder): how many times the last finished frame
 * (gpu_end) bound this page (gpu_bind_page; host tests). */
int	host_gpu_bound(const void *page);

#endif
