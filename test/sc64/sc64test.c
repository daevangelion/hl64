/* SPDX-License-Identifier: MIT */
/*
 * sc64test.c -- the SummerCart64 N64-side interface on the patched ares
 * (test/run/sc64.sh). Written from the SC64 documentation (docs/
 * 01_memory_map.md, 02_n64_commands.md, 04_config_options.md): registers at
 * 0x1FFF0000, KEY unlock, SCR/DATA0/DATA1 command flow, CMD_IRQ_REQUEST.
 *
 * The SD image test/sc64/mkimage.py writes holds the big-endian word
 * pat_in(w) = w * 0x9E3779B1 ^ 0x5C64C0DE at every word w of the image. The
 * ROM checks, logging one `sc64: ok ...` / `sc64: FAIL ...` line each:
 *   - locked: IDENTIFIER does not read SCv2; KEY unlock: it does
 *   - IDENTIFIER_GET, VERSION_GET, CONFIG_GET ROM_WRITE_ENABLE (default 0)
 *   - SD_CARD_OP init + status (inserted, initialized)
 *   - SD_SECTOR_SET + SD_READ of 64 sectors into ROM space, PI DMA back,
 *     every word the image's; CMD_BUSY seen for >= 90 % of 64 * SECTOR_US
 *     (sc64.sh runs ares with ARES_SC64_SECTOR_US = SECTOR_US)
 *   - the same with CMD_IRQ_REQUEST: one cart interrupt, CMD_IRQ_PENDING,
 *     cleared through IRQ CMD_CLEAR
 *   - ROM_WRITE_ENABLE 0: PI writes to ROM space are ignored; 1: they land
 *   - SD_WRITE of 16 sectors (pat_out) from ROM space to WRITE_LBA, read back
 *     with SD_READ elsewhere and compared (sc64.sh also checks the image file)
 *   - a read past the card's end fails with CMD_ERROR
 *   - libdragon's libcart: cart_init (SC64), cart_card_init, cart_card_rd_dram
 *     and cart_card_rd_cart against the image
 * then `sc64: PASS` / `sc64: FAIL n` and the harness exit (0 / 1).
 */
#include <libdragon.h>
#include <libcart/cart.h>
#include <stdint.h>
#include <string.h>

#include "plat/n64/harness.h"

#define SC64_SCR        0x1FFF0000
#define SC64_DATA0      0x1FFF0004
#define SC64_DATA1      0x1FFF0008
#define SC64_IDENTIFIER 0x1FFF000C
#define SC64_KEY        0x1FFF0010
#define SC64_IRQ        0x1FFF0014

#define SCR_CMD_BUSY        (1u << 31)
#define SCR_CMD_ERROR       (1u << 30)
#define SCR_CMD_IRQ_PENDING (1u << 27)
#define SCR_CMD_IRQ_REQUEST (1u << 8)
#define IRQ_CMD_CLEAR       (1u << 30)

#define ID_SCV2 0x53437632u /* "SCv2" */

#define CFG_ROM_WRITE_ENABLE 1
#define SD_OP_INIT       1
#define SD_OP_GET_STATUS 2

#define SECTOR      512
#define CARD_SECTORS 2048 /* mkimage.py: a 1 MiB image */
#define SECTOR_US   100   /* sc64.sh: ARES_SC64_SECTOR_US */
#define READ_LBA    100
#define READ_N      64
#define WRITE_LBA   1024
#define WRITE_N     16

/* ROM-space (cart SDRAM) addresses the test uses, well past this ROM */
#define CART_READ  0x13000000u
#define CART_IRQ   0x13080000u
#define CART_WRITE 0x13100000u
#define CART_BACK  0x13200000u
#define CART_LIBC  0x13300000u

static int fails;
static volatile int irqs;
static volatile uint32_t irq_scr;

static uint32_t pat_in(uint32_t w) { return w * 0x9E3779B1u ^ 0x5C64C0DEu; }
static uint32_t pat_out(uint32_t w) { return ~(w * 0x85EBCA6Bu); }

static void check(int ok, const char *what)
{
	debugf("sc64: %s %s\n", ok ? "ok" : "FAIL", what);
	if (!ok)
		fails++;
}

/* one command, polled: CMD_BUSY low; returns SCR, the results in d0/d1;
 * *busy_us: the time CMD_BUSY stayed set */
static uint32_t cmd(uint8_t id, uint32_t a0, uint32_t a1, uint32_t *d0, uint32_t *d1, uint32_t *busy_us)
{
	io_write(SC64_DATA0, a0);
	io_write(SC64_DATA1, a1);
	uint32_t t0 = get_ticks();
	io_write(SC64_SCR, id);
	uint32_t scr;
	while ((scr = io_read(SC64_SCR)) & SCR_CMD_BUSY)
		;
	if (busy_us)
		*busy_us = TICKS_TO_US(get_ticks() - t0);
	if (d0)
		*d0 = io_read(SC64_DATA0);
	if (d1)
		*d1 = io_read(SC64_DATA1);
	return scr;
}

static void cart_irq(void)
{
	uint32_t scr = io_read(SC64_SCR);
	if (scr & SCR_CMD_IRQ_PENDING) {
		irq_scr = scr;
		io_write(SC64_IRQ, IRQ_CMD_CLEAR);
		irqs++;
	}
}

/* every word of n sectors from lba at PI address pi is pat_in's */
static int image_words(uint32_t *buf, uint32_t pi, uint32_t lba, uint32_t n)
{
	dma_read(buf, pi, n * SECTOR);
	for (uint32_t i = 0; i < n * SECTOR / 4; i++)
		if (buf[i] != pat_in(lba * (SECTOR / 4) + i)) {
			debugf("sc64: word %lu at 0x%08lx: %08lx want %08lx\n", (unsigned long)i,
				(unsigned long)pi, (unsigned long)buf[i],
				(unsigned long)pat_in(lba * (SECTOR / 4) + i));
			return 0;
		}
	return 1;
}

int main(void)
{
	debug_init_isviewer();
	uint32_t *buf = malloc_uncached(READ_N * SECTOR);
	uint32_t d0, d1, us, scr;
	char line[96];

	/* locked out of reset; KEY unlocks */
	check(io_read(SC64_IDENTIFIER) != ID_SCV2, "locked: IDENTIFIER is not SCv2");
	io_write(SC64_KEY, 0x00000000);
	io_write(SC64_KEY, 0x5F554E4C);
	io_write(SC64_KEY, 0x4F434B5F);
	check(io_read(SC64_IDENTIFIER) == ID_SCV2, "unlocked: IDENTIFIER SCv2");

	scr = cmd('v', 0, 0, &d0, NULL, NULL);
	check(!(scr & SCR_CMD_ERROR) && d0 == ID_SCV2, "IDENTIFIER_GET SCv2");
	scr = cmd('V', 0, 0, &d0, &d1, NULL);
	snprintf(line, sizeof line, "VERSION_GET %lu.%lu.%lu", (unsigned long)(d0 >> 16),
		(unsigned long)(d0 & 0xffff), (unsigned long)d1);
	check(!(scr & SCR_CMD_ERROR), line);
	scr = cmd('c', CFG_ROM_WRITE_ENABLE, 0, NULL, &d1, NULL);
	check(!(scr & SCR_CMD_ERROR) && d1 == 0, "CONFIG_GET ROM_WRITE_ENABLE = 0");

	/* the card */
	scr = cmd('i', 0, SD_OP_INIT, NULL, NULL, NULL);
	check(!(scr & SCR_CMD_ERROR), "SD_CARD_OP init");
	scr = cmd('i', 0, SD_OP_GET_STATUS, NULL, &d1, NULL);
	snprintf(line, sizeof line, "SD_CARD_OP status 0x%02lx (inserted, initialized)", (unsigned long)d1);
	check(!(scr & SCR_CMD_ERROR) && (d1 & 3) == 3, line);

	/* SD_READ into ROM space, polled, busy for the modelled time */
	scr = cmd('I', READ_LBA, 0, NULL, NULL, NULL);
	check(!(scr & SCR_CMD_ERROR), "SD_SECTOR_SET");
	scr = cmd('s', CART_READ, READ_N, &d0, NULL, &us);
	snprintf(line, sizeof line, "SD_READ %d sectors: busy %lu us (want >= %d)", READ_N,
		(unsigned long)us, READ_N * SECTOR_US * 9 / 10);
	check(!(scr & SCR_CMD_ERROR) && us >= READ_N * SECTOR_US * 9 / 10, line);
	check(image_words(buf, CART_READ, READ_LBA, READ_N), "SD_READ data (PI DMA from ROM space)");

	/* the same with CMD_IRQ_REQUEST: completion through the cart interrupt */
	register_CART_handler(cart_irq);
	set_CART_interrupt(1);
	cmd('I', READ_LBA + READ_N, 0, NULL, NULL, NULL);
	io_write(SC64_DATA0, CART_IRQ);
	io_write(SC64_DATA1, READ_N);
	uint32_t t0 = get_ticks();
	io_write(SC64_SCR, SCR_CMD_IRQ_REQUEST | 's');
	while (!irqs && TICKS_TO_US(get_ticks() - t0) < 1000000)
		;
	us = TICKS_TO_US(get_ticks() - t0);
	scr = io_read(SC64_SCR);
	snprintf(line, sizeof line, "CMD_IRQ_REQUEST: %d cart interrupt(s) after %lu us, pending cleared",
		irqs, (unsigned long)us);
	check(irqs == 1 && (irq_scr & SCR_CMD_IRQ_PENDING) && !(scr & (SCR_CMD_BUSY | SCR_CMD_IRQ_PENDING | SCR_CMD_ERROR)), line);
	set_CART_interrupt(0);
	unregister_CART_handler(cart_irq);
	check(image_words(buf, CART_IRQ, READ_LBA + READ_N, READ_N), "SD_READ data (interrupt run)");

	/* ROM_WRITE_ENABLE: off, PI writes are ignored; on, they land */
	uint32_t before = io_read(CART_WRITE);
	io_write(CART_WRITE, ~before);
	check(io_read(CART_WRITE) == before, "ROM_WRITE_ENABLE 0: PI write ignored");
	scr = cmd('C', CFG_ROM_WRITE_ENABLE, 1, NULL, &d1, NULL);
	check(!(scr & SCR_CMD_ERROR) && d1 == 0, "CONFIG_SET ROM_WRITE_ENABLE 1 (previous 0)");
	for (uint32_t i = 0; i < WRITE_N * SECTOR / 4; i++)
		buf[i] = pat_out(i);
	dma_write(buf, CART_WRITE, WRITE_N * SECTOR);
	memset(buf, 0, WRITE_N * SECTOR);
	dma_read(buf, CART_WRITE, WRITE_N * SECTOR);
	int same = 1;
	for (uint32_t i = 0; i < WRITE_N * SECTOR / 4; i++)
		same &= buf[i] == pat_out(i);
	check(same, "ROM_WRITE_ENABLE 1: PI DMA write lands in ROM space");

	/* SD_WRITE from ROM space, read back elsewhere */
	cmd('I', WRITE_LBA, 0, NULL, NULL, NULL);
	scr = cmd('S', CART_WRITE, WRITE_N, NULL, NULL, &us);
	snprintf(line, sizeof line, "SD_WRITE %d sectors: busy %lu us", WRITE_N, (unsigned long)us);
	check(!(scr & SCR_CMD_ERROR) && us >= WRITE_N * SECTOR_US * 9 / 10, line);
	cmd('I', WRITE_LBA, 0, NULL, NULL, NULL);
	scr = cmd('s', CART_BACK, WRITE_N, NULL, NULL, NULL);
	memset(buf, 0, WRITE_N * SECTOR);
	dma_read(buf, CART_BACK, WRITE_N * SECTOR);
	same = !(scr & SCR_CMD_ERROR);
	for (uint32_t i = 0; i < WRITE_N * SECTOR / 4; i++)
		same &= buf[i] == pat_out(i);
	check(same, "SD_WRITE round trip (SD_READ back into ROM space)");

	scr = cmd('C', CFG_ROM_WRITE_ENABLE, 0, NULL, &d1, NULL);
	check(!(scr & SCR_CMD_ERROR) && d1 == 1, "CONFIG_SET ROM_WRITE_ENABLE 0 (previous 1)");
	before = io_read(CART_WRITE);
	io_write(CART_WRITE, ~before);
	check(io_read(CART_WRITE) == before, "ROM_WRITE_ENABLE 0 again: PI write ignored");

	/* errors: past the card's end */
	cmd('I', CARD_SECTORS - 1, 0, NULL, NULL, NULL);
	scr = cmd('s', CART_BACK, 2, &d0, NULL, NULL);
	snprintf(line, sizeof line, "SD_READ past the end: CMD_ERROR, DATA0 0x%08lx", (unsigned long)d0);
	check((scr & SCR_CMD_ERROR) && d0 != 0, line);

	/* libdragon's libcart on the same card */
	int r = cart_init();
	snprintf(line, sizeof line, "libcart cart_init %d type %d (SC64 %d)", r, cart_type, CART_SC);
	check(r == 0 && cart_type == CART_SC, line);
	r = cart_card_init();
	snprintf(line, sizeof line, "libcart cart_card_init %d", r);
	check(r == 0, line);
	/* a cached buffer: libcart writes back / invalidates its lines itself */
	static uint32_t cbuf[8 * SECTOR / 4] __attribute__((aligned(16)));
	r = cart_card_rd_dram(cbuf, 7, 8);
	same = r == 0;
	for (uint32_t i = 0; i < 8 * SECTOR / 4; i++)
		same &= cbuf[i] == pat_in(7 * (SECTOR / 4) + i);
	check(same, "libcart cart_card_rd_dram (through the data buffer)");
	r = cart_card_rd_cart(CART_LIBC, 300, 32);
	check(r == 0 && image_words(buf, CART_LIBC, 300, 32), "libcart cart_card_rd_cart");

	if (fails)
		debugf("sc64: FAIL %d\n", fails);
	else
		debugf("sc64: PASS\n");
	th_exit(fails ? 1 : 0);
	for (;;)
		;
}
