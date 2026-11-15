/* SPDX-License-Identifier: MIT */
/*
 * pak.c -- the cart's Controller Pak backend for plat/pakfs.c (sys.h's
 * sys_pak_*): what libdragon's joypad module says is in controller 1, and
 * blocking raw transfers to the Pak in it (libdragon's cpak_read /
 * cpak_write over joybus, bank 0). The Pak moves 32-byte blocks; libdragon
 * reads the block around a partial write's edge first, so a write aligned
 * to 32 bytes costs exactly its blocks. No stdio, no buffering: a
 * transfer has reached the Pak when its call returns.
 */
#include <stdint.h>

#include <libdragon.h>

#include "engine/core/sys.h"
#include "plat/pakfs.h"

int sys_pak_accessory(void)
{
	switch (joypad_get_accessory_type(JOYPAD_PORT_1))
	{
	case JOYPAD_ACCESSORY_TYPE_NONE:
		return SYS_PAK_ABSENT;
	case JOYPAD_ACCESSORY_TYPE_CONTROLLER_PAK:
		return SYS_PAK_CPAK;
	case JOYPAD_ACCESSORY_TYPE_RUMBLE_PAK:
		return SYS_PAK_RUMBLE;
	default:
		return SYS_PAK_OTHER;
	}
}

int pakfs_dev_read(uint32_t addr, void *dst, uint32_t len)
{
	return cpak_read(JOYPAD_PORT_1, 0, (uint16_t)addr, dst, len) == (int)len ? 0 : -1;
}

int pakfs_dev_write(uint32_t addr, const void *src, uint32_t len)
{
	return cpak_write(JOYPAD_PORT_1, 0, (uint16_t)addr, src, len) == (int)len ? 0 : -1;
}
