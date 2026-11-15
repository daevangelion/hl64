/* SPDX-License-Identifier: MIT */
/*
 * flash.c -- the cart's FlashRAM save chip behind sys.h's sys_flash_*
 * (docs/architecture.md 14, decision D4). The chip is 1 Mibit (128 KB):
 * 8 sectors of 16 KB, 128-byte pages, driven through PI domain 2 at
 * 0x08000000: array / status window there, command register at +0x10000
 * (32-bit words: opcode << 24 | page number), array data moved by DMA.
 *
 * The protocol and the chip table are libdragon's src/flashram.c (our
 * fork, pinned): silicon ID by DMA, a byte-indexed or a word-indexed array
 * by manufacturer / device, reads split at the 0x8000-byte DMA boundary.
 * What differs is blocking: libdragon's flashram_write / flashram_erase
 * wait hundreds of milliseconds for the chip. Here erase and program only
 * START the operation and return; sys_flash_poll reads the status register
 * the next frame (engine/save/flq.c). Nothing ever busy-waits on the chip.
 *
 * PI ownership: engine/io's chain launches ROM pieces from the PI interrupt
 * (plat/n64/cart.c). Every register access and DMA here runs with the chain
 * held (io_hold): a piece already in flight lands (libdragon's io_read /
 * io_write / dma_wait drain the PI queue and deliver its completion to the
 * chain's handler, which launches nothing while held), then the access
 * happens, then the hold is released and the chain resumes. The wait is one
 * piece (IO_PIECE, 0.4 ms) at most, and cannot deadlock: the handler only
 * retires the landed piece while held. A call holds the chain across its
 * few accesses only (sys_flash_read across its whole read).
 *
 * The chip modes: the array reads back only in read mode; erase and program
 * leave the chip in status mode (the status register reads at the window).
 * Polls just read it; when the operation ended the status latch is cleared
 * (status mode entered again, a 0 written at the window) and read mode is
 * restored, so the next read finds the array -- libdragon's sequence, which
 * is also the point where ares executes an erase / program. Commands go
 * through twice where libdragon does (status and silicon-ID mode: the
 * MX29L1100 needs both writes).
 */
#include <stdint.h>
#include <string.h>

#include <libdragon.h>

#include "engine/core/log.h"
#include "engine/core/sys.h"
#include "engine/io/io.h"

#define FL_BASE		0x08000000u	/* array / status window (PI domain 2) */
#define FL_CMD		(FL_BASE | 0x00010000u)

#define FL_TYPE_ID	0x11118001u	/* silicon ID word 0 of every known part */

#define FL_CMD_STATUS	0xD2000000u
#define FL_CMD_ID	0xE1000000u
#define FL_CMD_READ	0xF0000000u
#define FL_CMD_ERASE_AT	0x4B000000u	/* | page: arm the sector holding it */
#define FL_CMD_ERASE	0x78000000u	/* start the armed erase */
#define FL_CMD_LOAD	0xB4000000u	/* DMA the page buffer next */
#define FL_CMD_PROGRAM	0xA5000000u	/* | page: program the buffer into it */

#define FL_ST_PROG_BUSY	0x01
#define FL_ST_ERASE_BUSY 0x02
#define FL_ST_PROG_OK	0x04
#define FL_ST_ERASE_OK	0x08

#define FL_PAGE		128u
#define FL_SECTOR	(128u * FL_PAGE)
#define FL_SECTORS	8u
#define FL_BYTES	(FL_SECTORS * FL_SECTOR)
#define FL_DMA_SPAN	0x8000u		/* logical bytes one DMA must not cross */

/* PI domain 2 bus timings of libdragon's flashram_init: the page size is
 * left at its maximum so the PI never splits a DMA (the driver does,
 * word-indexed parts cannot express it). */
#define PI_DOM2_LAT	((volatile uint32_t *)0xA4600024)
#define PI_DOM2_PWD	((volatile uint32_t *)0xA4600028)
#define PI_DOM2_PGS	((volatile uint32_t *)0xA460002C)
#define PI_DOM2_RLS	((volatile uint32_t *)0xA4600030)

typedef struct {
	uint16_t	mfr, dev;
	uint8_t		word;		/* array addressed in 16-bit words */
	const char	*name;
} model_t;

/* libdragon's table: the addressing is a property of the silicon. */
static const model_t models[] = {
	{ 0x00C2, 0x0000, 1, "MX29L0000" },
	{ 0x00C2, 0x0001, 1, "MX29L0001" },
	{ 0x00C2, 0x001E, 1, "MX29L1100" },
	{ 0x00C2, 0x001D, 0, "MX29L1101_A" },
	{ 0x00C2, 0x0084, 0, "MX29L1101_B" },
	{ 0x00C2, 0x008E, 0, "MX29L1101_C" },
	{ 0x0032, 0x00F1, 0, "MN63F8MPN" },
};

enum { OP_NONE, OP_ERASE, OP_PROGRAM };

static struct {
	uint8_t		present;
	uint8_t		word;		/* the array is word-indexed (address >> 1) */
	uint8_t		op;		/* operation in flight */
} fl;

static uint32_t id_buf[4] __attribute__((aligned(16)));

static void fl_cmd(uint32_t cmd)
{
	io_write(FL_CMD, cmd);
}

/* Status and silicon-ID mode need the command twice on some parts. */
static void fl_cmd2(uint32_t cmd)
{
	io_write(FL_CMD, cmd);
	io_write(FL_CMD, cmd);
}

/* libdragon's PI queue starts a DMA at enqueue only while the PI is idle,
 * else it waits for a completion interrupt -- and the PI raises none when a
 * CPU write to the bus (a command above) ends. A DMA enqueued while that
 * write is still in flight would sit in the queue for good (the next
 * command's dma_wait spins on it; seen on ares, which holds IO_BUSY for
 * 400 cycles). So every DMA here starts after the PI has settled. */
#define FL_PI_STATUS	((volatile uint32_t *)0xA4600010)
#define FL_PI_BUSY	0x3u		/* DMA busy | IO busy */

static void fl_settle(void)
{
	while (*FL_PI_STATUS & FL_PI_BUSY)
		;
}

static uint32_t fl_pi(uint32_t off)
{
	return FL_BASE + (off >> fl.word);
}

int sys_flash_init(void)
{
	uint32_t type, mfr, dev;
	const char *name = "unknown";
	unsigned i;

	if (fl.present)
		return 1;
	memset(&fl, 0, sizeof(fl));
	io_hold(1);
	dma_wait();
	disable_interrupts();
	*PI_DOM2_LAT = 0x05;
	*PI_DOM2_PWD = 0x0C;
	*PI_DOM2_PGS = 0x0F;
	*PI_DOM2_RLS = 0x02;
	enable_interrupts();

	/* the ID needs a DMA: a CPU read of the window returns word 0 twice */
	fl_cmd2(FL_CMD_ID);
	memset(id_buf, 0, sizeof(id_buf));
	data_cache_hit_writeback_invalidate(id_buf, sizeof(id_buf));
	fl_settle();
	dma_read_raw_async(id_buf, FL_BASE, 8);
	dma_wait();
	fl_cmd(FL_CMD_READ);
	io_hold(0);

	type = id_buf[0];
	mfr = id_buf[1] >> 16;
	dev = id_buf[1] & 0xFFFFu;
	if (type != FL_TYPE_ID)
	{
		log_printf("flash: no chip (id word %08x %08x)\n", (unsigned)id_buf[0], (unsigned)id_buf[1]);
		return 0;
	}
	for (i = 0; i < sizeof(models) / sizeof(models[0]); i++)
		if (models[i].mfr == mfr && models[i].dev == dev)
		{
			name = models[i].name;
			fl.word = models[i].word;
			break;
		}
	fl.present = 1;
	log_printf("flash: chip %s mfr %04x dev %04x, %s-indexed, %u KB\n", name, (unsigned)mfr,
		(unsigned)dev, fl.word ? "word" : "byte", FL_BYTES / 1024);
	return 1;
}

int sys_flash_read(void *dst, uint32_t off, uint32_t len)
{
	uint8_t bounce[16] __attribute__((aligned(16)));
	uint8_t *out = dst;
	uint32_t done = 0;

	if (!fl.present || fl.op != OP_NONE)
		return -1;
	if ((off | len) & 1u || off > FL_BYTES || len > FL_BYTES - off || ((uintptr_t)dst & 15u))
		return -1;
	if (!len)
		return 0;

	io_hold(1);
	fl_cmd(FL_CMD_READ);
	while (done < len)
	{
		uint32_t abs = off + done;
		uint32_t chunk = len - done;
		uint32_t span = FL_DMA_SPAN - (abs & (FL_DMA_SPAN - 1));
		uint32_t direct;

		if (chunk > span)
			chunk = span;
		direct = chunk & ~15u;
		if (direct)
		{
			/* whole cache lines: straight into the caller's buffer */
			data_cache_hit_writeback_invalidate(out + done, direct);
			fl_settle();
			dma_read_raw_async(out + done, fl_pi(abs), direct);
			dma_wait();
			done += direct;
			continue;
		}
		/* the tail under 16 bytes: a line of its own, then copied (the
		 * invalidate must not reach the caller's neighbouring data) */
		data_cache_hit_writeback_invalidate(bounce, sizeof(bounce));
		fl_settle();
		dma_read_raw_async(bounce, fl_pi(abs), chunk);
		dma_wait();
		memcpy(out + done, bounce, chunk);
		done += chunk;
	}
	io_hold(0);
	return 0;
}

int sys_flash_erase(uint32_t sector)
{
	if (!fl.present || fl.op != OP_NONE || sector >= FL_SECTORS)
		return -1;
	io_hold(1);
	fl_cmd(FL_CMD_ERASE_AT | (sector * (FL_SECTOR / FL_PAGE)));
	fl_cmd(FL_CMD_ERASE);
	io_hold(0);
	fl.op = OP_ERASE;
	return 0;
}

int sys_flash_program(uint32_t page, const void *src)
{
	if (!fl.present || fl.op != OP_NONE || page >= FL_BYTES / FL_PAGE || ((uintptr_t)src & 15u))
		return -1;
	io_hold(1);
	fl_cmd(FL_CMD_LOAD);
	data_cache_hit_writeback(src, FL_PAGE);
	fl_settle();
	dma_write_raw_async(src, FL_BASE, FL_PAGE);
	/* the command write drains the DMA queue first: the page buffer is
	 * loaded when it goes out */
	fl_cmd(FL_CMD_PROGRAM | page);
	io_hold(0);
	fl.op = OP_PROGRAM;
	return 0;
}

int sys_flash_poll(void)
{
	uint8_t busy, ok, st;

	if (fl.op == OP_NONE)
		return 0;
	busy = fl.op == OP_ERASE ? FL_ST_ERASE_BUSY : FL_ST_PROG_BUSY;
	ok = fl.op == OP_ERASE ? FL_ST_ERASE_OK : FL_ST_PROG_OK;

	io_hold(1);
	st = (uint8_t)io_read(FL_BASE);
	if (st & busy)
	{
		io_hold(0);
		return 1;
	}
	/* ended: clear the OK latch, back to read mode for the next read */
	fl_cmd2(FL_CMD_STATUS);
	io_write(FL_BASE, 0);
	fl_cmd(FL_CMD_READ);
	io_hold(0);
	fl.op = OP_NONE;
	return (st & ok) == ok ? 0 : -1;
}
