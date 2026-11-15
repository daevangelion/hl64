/* SPDX-License-Identifier: MIT */
/*
 * pakfs.h -- the Controller Pak file system behind sys.h's sys_pak_*
 * (docs/architecture.md 14, decision D4), shared by the cart
 * (plat/n64/pak.c: the Pak in controller 1 over joybus) and the host
 * (plat/host/pak_host.c: a 32 KB image file), so an image written by one
 * mounts on the other (and in ares: test/ares-pak.patch).
 *
 * The layout is the Pak's own (one 32 KB bank of 128 pages of 256 B, as
 * libdragon's cpakfs and libultra's pfs read it):
 *   page 0      ID area; the ID block (serial, device id, bank count,
 *               two checksums) at 0x20, copies at 0x60, 0x80, 0xC0
 *   page 1      the FAT: 128 big-endian 2-byte entries, one per page;
 *               byte 1 is the sum of the bytes of entries 5..127.
 *               An entry is the next page of its note's chain, 0x0001
 *               (the chain ends), 0x0003 (free) or 0x0000 (reserved)
 *   page 2      the FAT's backup copy (used when page 1's sum is wrong)
 *   pages 3-4   16 notes of 32 B: game code (4), publisher code (2),
 *               first page (2), status (bit 1: occupied), libdragon's
 *               padding of the last page (1) and CRC-16 (2) marking it,
 *               extension (4), file name (16), names in the Pak's codepage
 *   pages 5-127 data
 *
 * Ours: game code "HL64", publisher "HL", name "HL64SAVE", extension the
 * id in decimal ("0".."15"). Nothing else on the Pak is ever written:
 *   - a page is ours to allocate only when the FAT says free AND no
 *     occupied note's chain (walked at mount, loops bounded) reaches it;
 *     a note entry only when both libdragon (status bit clear) and libultra
 *     (game or publisher code zero) call it empty;
 *   - create writes the FAT (main copy, then backup) and only then the
 *     note; delete clears the note and only then frees the pages in the
 *     FAT. Power lost between the two leaves an unreferenced chain (lost
 *     space that fsck tools reclaim), never a note pointing at free pages.
 *     A FAT copy cut mid-write fails its sum and the other copy is read;
 *   - sys_pak_write touches only data pages of the note being written,
 *     exactly its bytes (no FS metadata), so every call leaves the FS
 *     consistent;
 *   - the ID area is never written; a Pak whose ID or FAT copies all fail
 *     their checksums, or with more than one bank, does not mount.
 * A failed transfer while writing unmounts (the cached FAT may no longer
 * be the Pak's): the caller mounts again.
 *
 * The backend supplies the transfers (addresses in the 32 KB bank).
 */
#ifndef HL64_PLAT_PAKFS_H
#define HL64_PLAT_PAKFS_H

#include <stdint.h>

#define PAKFS_GAME	"HL64"
#define PAKFS_PUB	"HL"
#define PAKFS_NAME	"HL64SAVE"
#define PAKFS_BYTES	32768u
#define PAKFS_PAGE	256u
#define PAKFS_PAGES	128u
#define PAKFS_NOTES	16

/* The backend: blocking transfers of [addr, addr + len) in bank 0; 0 ok,
 * -1 failed (no Pak, wire error). */
int	pakfs_dev_read(uint32_t addr, void *dst, uint32_t len);
int	pakfs_dev_write(uint32_t addr, const void *src, uint32_t len);

/* A freshly formatted Pak image (libdragon's cpakfs_format layout; serial
 * fixed so host runs are deterministic). */
void	pakfs_format(uint8_t img[PAKFS_BYTES]);

#endif
