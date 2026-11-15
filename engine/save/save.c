/* SPDX-License-Identifier: MIT */
/*
 * save.c -- saved games and the settings block (include/hl64/save.h,
 * docs/architecture.md 14, decision D4).
 *
 * FlashRAM (128 KB, 8 sectors of 16 KB): sector 0 the settings block,
 * sectors 1..7 a pool. A save is a 128-byte header (magic, format, layout
 * id, logical slot, sequence, sectors, sizes, CRC-32 of the image, display
 * info, the open layers) and the LZ image of the sections every module
 * writes (engine/save/svmod.h order, then the game's); on the flash it
 * takes whole sectors of the pool, contiguous. A logical slot (the
 * autosave, the manual slots) is its newest copy whose header and image
 * check out; a new save goes to sectors no slot's current copy holds, so
 * the copy it replaces stays good until the new one is complete (a manual
 * slot with no such room writes over its own copy; an autosave never does:
 * it is dropped). The directory is read at boot and updated from the
 * read-back of every write.
 *
 * Save (save_write, save_autosave): the next frame boundary (save_frame,
 * first thing in eng_frame) settles what is in flight -- paused monster
 * thinks finish, a layer or HLE slot opening completes, a map's precache
 * hold lands -- in the run that saves as in every load of it, writes the
 * snapshot into the save region and hands it to the flash writer, which
 * programs it over the following frames.
 *
 * Load (save_load): the image is read and checked first (a bad one leaves
 * the session alone); then the session is dropped as at an end section,
 * the world's fenced frees retire over a few held frames, the saved layers
 * reopen in their saved slots, and every section is read back: entities
 * into their own slots with their serials, private blocks at their own
 * handles, HLE slots at their own blocks, the clock and the random stream
 * exactly. No spawn function runs. The game resumes at the next frame.
 */
#include <string.h>

#include "hl64/ent.h"
#include "hl64/fx.h"
#include "hl64/game.h"
#include "hl64/grid.h"
#include "hl64/rec.h"
#include "hl64/save.h"
#include "hl64/seam.h"
#include "hl64/settings.h"
#include "hl64/snd.h"
#include "hl64/studio.h"
#include "engine/core/dbg.h"
#include "engine/core/eng.h"
#include "engine/core/log.h"
#include "engine/core/sys.h"
#include "engine/ent/rec_int.h"
#include "engine/io/io.h"
#include "engine/mem/carve.h"
#include "engine/phys/phys.h"
#include "engine/phys/player.h"
#include "engine/render/ren.h"
#include "engine/save/flq.h"
#include "engine/save/sv.h"
#include "engine/save/sv_int.h"
#include "engine/save/svmod.h"
#include "engine/world/wld.h"
#include "engine/world/wld_int.h"

#define SAVE_FORMAT	3
#define SLOT_MAGIC	0x484C5356u	/* 'HLSV' */
#define SET_MAGIC	0x484C5354u	/* 'HLST' */
#define HDR_BYTES	128u
#define SET_BYTES	512u
#define FLASH_PAGE	128u
#define POOL_FIRST	1u		/* the pool's first sector */
#define FLASH_SLOTS	(1 + SAVE_FLASH_MANUAL)	/* logical slots on the flash, 0 the autosave */
#define SAVE_BYTES	(HDR_BYTES + SAVE_IMAGE_MAX)
#define DROP_FRAMES	3		/* world frames for the closed layers' fenced frees */
#define FLASH_BUDGET_US	4000u		/* flash writer time per frame */
#define CODE_BUDGET_US	8000u		/* coder / decoder time per frame */
#define PAK_SLOT	0xFFu		/* slot_hdr_t slot of a Pak note */
#define PAK_NOTES	16		/* note ids 0..15 (sys.h) */
#define PAK_STEP	256u		/* Pak bytes read / written per frame (ares: ~6 ms) */
#define PAK_ROOM_MIN	(16u * 1024u)	/* free bytes for the Pak's empty entry */

_Static_assert(SAVE_BYTES <= HL64_SAVE_KB * 1024u, "save region holds a save");
_Static_assert(SAVE_SECTOR_BYTES * (POOL_FIRST + SAVE_POOL_SECTORS) <= SAVE_FLASH_BYTES,
	"pool fits the flash");
_Static_assert(SAVE_BYTES <= SAVE_POOL_SECTORS * SAVE_SECTOR_BYTES, "a save fits the pool");

typedef struct {
	uint32_t	magic;
	uint32_t	hdr_crc;		/* bytes [8, HDR_BYTES) */
	uint16_t	format, hdr_bytes;
	uint32_t	layout;
	uint32_t	seq;
	uint32_t	raw_bytes, img_bytes, img_crc;
	float		game_time;
	uint8_t		skill, nlayers;
	uint8_t		slot;			/* logical slot (flash), PAK_SLOT on a Pak */
	uint8_t		nsect;			/* pool sectors it takes (flash), 0 on a Pak */
	uint16_t	layer_map[HL64_WORLD_LAYERS];	/* WLD_NO_MAP: closed */
	uint32_t	layer_crc[HL64_WORLD_LAYERS];	/* the package's directory CRC */
	char		map[16];
	char		title[24];
} slot_hdr_t;
_Static_assert(sizeof(slot_hdr_t) <= HDR_BYTES, "slot header");

enum {
	ST_IDLE, ST_SNAP, ST_WRITE, ST_SETTINGS, ST_LOAD_READ, ST_LOAD_DROP, ST_LOAD_OPEN,
	ST_PAK_WRITE, ST_PAK_READ, ST_CODE, ST_DECODE,
	ST_END		/* the operation ended last frame: its log line, then idle */
};

/* A directory entry: the slot's current copy. */
typedef struct {
	save_info_t	info;
	uint8_t		sect, nsect;		/* flash: pool sectors (sect from 0) */
	int8_t		note;			/* Pak: note id, -1 the empty entry */
	uint32_t	bytes;			/* Pak: the note's size */
} dirent_t;

static struct {
	uint8_t		chip;		/* flash present */
	uint8_t		st;
	int8_t		slot;		/* the operation's list index */
	int8_t		result;
	uint8_t		drop;		/* held frames left before the reopen */
	uint8_t		auto_ask;	/* an autosave waits for a quiet frame boundary */
	uint8_t		wsect, wnsect;	/* the write's pool run */
	uint32_t	seq;		/* newest sequence seen */
	uint8_t		*img;		/* SAVE region: header + image */
	dirent_t	flash[FLASH_SLOTS];
	/* Controller Pak (controller 1) */
	uint8_t		pak_acc;	/* SYS_PAK_* last seen, 0xFF before the first poll */
	uint8_t		pak_st;		/* SAVE_PAK_* */
	uint8_t		pak_mounted;
	uint8_t		npak;		/* entries listed */
	dirent_t	pak[SAVE_PAK_SLOTS];
	uint8_t		pak_gen;	/* + 1 at every Pak change (an operation on it fails) */
	uint8_t		pgen;		/* pak_gen at the operation's start */
	int8_t		pnew, pold;	/* the Pak note read / written, the one it replaces (-1) */
	uint32_t	poff, plen;	/* Pak transfer: next byte, bytes */
	slot_hdr_t	hdr;		/* the slot being loaded */
	/* the save region: image (SAVE_BYTES), coder work, staged state */
	uint8_t		*work, *raw;
	uint32_t	raw_cap, raw_len;
	uint32_t	us_max, us_sum, frames;	/* this operation's held / step frames */
	uint32_t	frame_max;	/* its longest whole frame (save_frame to save_frame) */
	uint32_t	check_us;	/* test builds: restore_check's time, left out of both */
	uint32_t	held_max;	/* its longest frame it held (snapshot, load): all ours */
	uint8_t		held;		/* save_frame held the frame now ending */
	uint64_t	last;		/* the previous save_frame call */
	const char	*what;		/* the ended operation, for its log line */
	uint64_t	t0;
} SV;

static uint8_t set_buf[SET_BYTES] __attribute__((aligned(16)));

static uint32_t sect_off(uint32_t sect)
{
	return SAVE_SECTOR_BYTES * (POOL_FIRST + sect);
}

static uint32_t fnv(uint32_t h, uint32_t v)
{
	int k;

	for (k = 0; k < 4; k++)
		h = (h ^ ((v >> (8 * k)) & 0xFFu)) * 16777619u;
	return h;
}

/* What a save depends on beside its own checks: the format, the engine's
 * component and table layouts, the game's (game_save_layout) and the
 * cooked id spaces. */
static uint32_t layout_id(void)
{
	uint32_t h = 2166136261u;
	int k;

	h = fnv(h, SAVE_FORMAT);
	h = fnv(h, HL64_MAX_ENTITIES);
	h = fnv(h, sizeof(col_ent_t));
	h = fnv(h, sizeof(ent_mot_t));
	h = fnv(h, sizeof(ent_phys_t));
	h = fnv(h, sizeof(ent_tm_t));
	h = fnv(h, sizeof(ent_rnd_t));
	h = fnv(h, sizeof(ent_id_t));
	h = fnv(h, sizeof(ent_player_t));
	h = fnv(h, REC_TABLE_BYTES);
	h = fnv(h, mem_region_bytes(MEM_COMPONENTS));
	h = fnv(h, mem_region_bytes(MEM_GAME_PRIVATE));
	h = fnv(h, mem_region_bytes(MEM_HLE_BLOBS));
	h = fnv(h, mem_region_bytes(MEM_DEHYDRATE));
	for (k = 0; k < 8; k++)
		h = fnv(h, (uint32_t)res_count(k));
	h = fnv(h, (uint32_t)res_classes());
	h = fnv(h, (uint32_t)res_patterns());
	h = fnv(h, (uint32_t)wld_map_count());
	return fnv(h, game_save_layout());
}

static void info_of(const slot_hdr_t *h, save_info_t *o, int state, int source, int flags)
{
	memset(o, 0, sizeof(*o));
	o->state = (uint8_t)state;
	o->source = (uint8_t)source;
	o->flags = (uint8_t)flags;
	if (state != SAVE_OK)
		return;
	o->skill = h->skill;
	o->seq = h->seq;
	o->game_time = h->game_time;
	memcpy(o->map, h->map, sizeof(o->map) - 1);
	memcpy(o->title, h->title, sizeof(o->title) - 1);
}

/* The header at buf: SAVE_OK, SAVE_CORRUPT (another format / build, a bad
 * header), SAVE_EMPTY (erased or no header at all). */
static int hdr_check(const uint8_t *buf, slot_hdr_t *h)
{
	memcpy(h, buf, sizeof(*h));
	if (h->magic != SLOT_MAGIC)
		return SAVE_EMPTY;
	if (h->format != SAVE_FORMAT || h->hdr_bytes != HDR_BYTES
		|| h->hdr_crc != sv_crc32(0, buf + 8, HDR_BYTES - 8)
		|| h->img_bytes == 0 || h->img_bytes > SAVE_IMAGE_MAX
		|| h->layout != layout_id())
		return SAVE_CORRUPT;
	return SAVE_OK;
}

/* The image after a good header, read into SV.img + HDR_BYTES. */
static int img_check(const slot_hdr_t *h)
{
	return sv_crc32(0, SV.img + HDR_BYTES, h->img_bytes) == h->img_crc ? SAVE_OK : SAVE_CORRUPT;
}

/* Read the save at pool sector sect into the save region and check it. */
static int read_flash(uint32_t sect, slot_hdr_t *h)
{
	int st;

	if (flq_read(SV.img, sect_off(sect), HDR_BYTES))
		return SAVE_EMPTY;
	if ((st = hdr_check(SV.img, h)) != SAVE_OK)
		return st;
	if (h->slot >= FLASH_SLOTS || h->nsect == 0 || sect + h->nsect > SAVE_POOL_SECTORS
		|| HDR_BYTES + h->img_bytes > h->nsect * SAVE_SECTOR_BYTES)
		return SAVE_CORRUPT;
	if (flq_read(SV.img + HDR_BYTES, sect_off(sect) + HDR_BYTES, (h->img_bytes + 1) & ~1u))
		return SAVE_CORRUPT;
	return img_check(h);
}

static void flash_entry(int slot, const slot_hdr_t *h, int st, uint32_t sect)
{
	dirent_t *d = &SV.flash[slot];

	info_of(h, &d->info, st, SAVE_SRC_FLASH, slot == 0 ? SAVE_F_AUTO : 0);
	d->sect = (uint8_t)sect;
	d->nsect = st == SAVE_OK ? h->nsect : 0;
}

/* Boot: every pool sector that starts a save; per logical slot the newest
 * good copy (a slot with only bad ones shows SAVE_CORRUPT). */
static void flash_scan(void)
{
	slot_hdr_t h;
	uint32_t s;
	int k;

	for (k = 0; k < FLASH_SLOTS; k++)
		flash_entry(k, &h, SAVE_EMPTY, 0);
	for (s = 0; s < SAVE_POOL_SECTORS; s++)
	{
		int st = read_flash(s, &h);
		dirent_t *d;

		if (st == SAVE_EMPTY)
			continue;
		if (h.magic == SLOT_MAGIC && h.seq > SV.seq)
			SV.seq = h.seq;
		if (st != SAVE_OK)
		{
			if (h.slot < FLASH_SLOTS && SV.flash[h.slot].info.state == SAVE_EMPTY)
				flash_entry(h.slot, &h, SAVE_CORRUPT, s);
			continue;
		}
		d = &SV.flash[h.slot];
		if (d->info.state != SAVE_OK || h.seq > d->info.seq)
			flash_entry(h.slot, &h, SAVE_OK, s);
	}
}

/* A run of n pool sectors no slot's current copy holds (except slot's own
 * when own): its first sector, -1 none. */
static int flash_room(uint32_t n, int slot, int own)
{
	uint8_t used[SAVE_POOL_SECTORS];
	uint32_t s, k, run = 0;

	memset(used, 0, sizeof(used));
	for (k = 0; k < FLASH_SLOTS; k++)
	{
		const dirent_t *d = &SV.flash[k];

		if (d->info.state != SAVE_OK || ((int)k == slot && own))
			continue;
		for (s = d->sect; s < (uint32_t)d->sect + d->nsect; s++)
			used[s] = 1;
	}
	for (s = 0; s < SAVE_POOL_SECTORS; s++)
	{
		run = used[s] ? 0 : run + 1;
		if (run == n)
			return (int)(s + 1 - n);
	}
	return -1;
}

/* ---- Controller Pak directory --------------------------------------------------- */

/* Our notes (their headers only: the image is checked when it loads), then
 * the empty entry when a new note may fit. */
static void pak_scan(void)
{
	static uint8_t buf[HDR_BYTES] __attribute__((aligned(16)));
	uint8_t ids[PAK_NOTES];
	uint32_t bytes[PAK_NOTES], free_bytes = 0;
	int i, n, free_notes = 0;

	SV.npak = 0;
	n = sys_pak_notes(ids, bytes, PAK_NOTES);
	if (n < 0 || sys_pak_stat(&free_bytes, &free_notes))
	{
		SV.pak_st = SAVE_PAK_DAMAGED;
		return;
	}
	for (i = 0; i < n && SV.npak < SAVE_PAK_SLOTS; i++)
	{
		dirent_t *d = &SV.pak[SV.npak++];
		slot_hdr_t h;
		int st = SAVE_CORRUPT;

		memset(&h, 0, sizeof(h));
		if (bytes[i] >= HDR_BYTES && !sys_pak_read(ids[i], buf, 0, HDR_BYTES))
		{
			st = hdr_check(buf, &h);
			if (st == SAVE_EMPTY || (st == SAVE_OK && (h.slot != PAK_SLOT
				|| HDR_BYTES + h.img_bytes > bytes[i])))
				st = SAVE_CORRUPT;
			if (h.magic == SLOT_MAGIC && h.seq > SV.seq)
				SV.seq = h.seq;
		}
		info_of(&h, &d->info, st, SAVE_SRC_PAK, 0);
		d->note = (int8_t)ids[i];
		d->bytes = bytes[i];
	}
	if (SV.npak < SAVE_PAK_SLOTS && free_notes > 0 && free_bytes >= PAK_ROOM_MIN)
	{
		dirent_t *d = &SV.pak[SV.npak++];

		memset(d, 0, sizeof(*d));
		d->info.state = SAVE_EMPTY;
		d->info.source = SAVE_SRC_PAK;
		d->note = -1;
	}
	SV.pak_st = SV.npak ? SAVE_PAK_OK : SAVE_PAK_FULL;
	log_printf("save: Controller Pak: %d of our notes, %lu B and %d notes free%s\n", n,
		(unsigned long)free_bytes, free_notes,
		SV.pak_st == SAVE_PAK_FULL ? ": full" : "");
}

/* What is in controller 1: (un)mount on a change. Every frame; a change
 * costs the mount and the scan. */
static void pak_poll(void)
{
	int acc = sys_pak_accessory();

	if (acc == SV.pak_acc)
		return;
	SV.pak_acc = (uint8_t)acc;
	SV.pak_gen++;
	SV.npak = 0;
	if (SV.pak_mounted)
	{
		sys_pak_unmount();
		SV.pak_mounted = 0;
	}
	switch (acc)
	{
	case SYS_PAK_CPAK:
		if (sys_pak_mount())
		{
			SV.pak_st = SAVE_PAK_DAMAGED;
			log_printf("save: Controller Pak: unformatted or damaged\n");
			break;
		}
		SV.pak_mounted = 1;
		pak_scan();
		break;
	case SYS_PAK_ABSENT:
		SV.pak_st = SAVE_PAK_ABSENT;
		log_printf("save: Controller Pak: none\n");
		break;
	default:
		SV.pak_st = SAVE_PAK_RUMBLE;
		log_printf("save: controller 1: a Rumble Pak or another accessory\n");
		break;
	}
}

/* The lowest note id none of our notes has, -1 none. */
static int pak_free_id(void)
{
	uint8_t ids[PAK_NOTES];
	uint32_t bytes[PAK_NOTES];
	int id, i, n = sys_pak_notes(ids, bytes, PAK_NOTES);

	for (id = 0; n >= 0 && id < PAK_NOTES; id++)
	{
		for (i = 0; i < n && ids[i] != id; i++)
			;
		if (i == n)
			return id;
	}
	return -1;
}

/* Start the Pak write of the snapshot in SV.img to entry k: a new note
 * (the one it replaces goes when the new one is complete; with no room for
 * both, the old one goes first). */
static int pak_begin(int k)
{
	const slot_hdr_t *h = (const slot_hdr_t *)SV.img;
	uint32_t len = HDR_BYTES + h->img_bytes;
	int id;

	if (k >= SV.npak || SV.pak_st != SAVE_PAK_OK)
		return -1;
	id = pak_free_id();
	SV.pold = SV.pak[k].note;
	if (id < 0 || sys_pak_create(id, len))
	{
		if (SV.pold < 0 || sys_pak_delete(SV.pold) || sys_pak_create(id = SV.pold, len))
		{
			log_printf("save: Controller Pak: no room for %lu B\n", (unsigned long)len);
			SV.pak_acc = 0xFF;	/* list it again (a failed transfer unmounts) */
			return -1;
		}
		log_printf("save: Controller Pak: no room for a second note: over note %d\n", id);
		SV.pak[k].info.state = SAVE_EMPTY;
		SV.pold = -1;
	}
	SV.pnew = (int8_t)id;
	SV.poff = HDR_BYTES;	/* the image first, the header last */
	SV.plen = len;
	return 0;
}

/* One frame of the Pak write: 1 still running, 0 done, -1 failed. */
static int pak_write_step(void)
{
	uint32_t n;

	if (SV.poff < SV.plen)
	{
		n = SV.plen - SV.poff < PAK_STEP ? SV.plen - SV.poff : PAK_STEP;
		if (sys_pak_write(SV.img + SV.poff, SV.poff, n))
			return -1;
		SV.poff += n;
		return 1;
	}
	if (sys_pak_write(SV.img, 0, HDR_BYTES) || sys_pak_close())
		return -1;
	if (SV.pold >= 0 && sys_pak_delete(SV.pold))
		log_printf("save: Controller Pak: the replaced note %d stays\n", SV.pold);
	return 0;
}

/* One frame of a Pak load's read into SV.img: 1 running, 0 read and
 * checked, -1 failed. */
static int pak_read_step(void)
{
	uint32_t n;

	if (SV.poff == 0)
	{
		if (sys_pak_read(SV.pnew, SV.img, 0, HDR_BYTES) || hdr_check(SV.img, &SV.hdr) != SAVE_OK
			|| SV.hdr.slot != PAK_SLOT || HDR_BYTES + SV.hdr.img_bytes > SV.plen)
			return -1;
		SV.plen = HDR_BYTES + SV.hdr.img_bytes;
		SV.poff = HDR_BYTES;
		return 1;
	}
	n = SV.plen - SV.poff < PAK_STEP ? SV.plen - SV.poff : PAK_STEP;
	if (sys_pak_read(SV.pnew, SV.img + SV.poff, SV.poff, n))
		return -1;
	SV.poff += n;
	return SV.poff < SV.plen ? 1 : img_check(&SV.hdr) == SAVE_OK ? 0 : -1;
}

/* ---- settings block ----------------------------------------------------------- */

/* name hash -> value pairs of the persistent fields: a field the build
 * does not know is skipped, a new one keeps its default */
static uint32_t name_hash(const char *s)
{
	uint32_t h = 2166136261u;

	while (*s)
		h = (h ^ (uint8_t)*s++) * 16777619u;
	return h;
}

static float field_get(const settings_t *s, int id)
{
	const uint8_t *p = (const uint8_t *)s + settings_desc[id].offset;
	float f;

	if (settings_desc[id].type == SET_T_U8)
		return (float)*p;
	memcpy(&f, p, sizeof(f));
	return f;
}

static void field_set(settings_t *s, int id, float v)
{
	uint8_t *p = (uint8_t *)s + settings_desc[id].offset;

	if (!(v >= settings_desc[id].lo))
		v = settings_desc[id].lo;
	if (v > settings_desc[id].hi)
		v = settings_desc[id].hi;
	if (settings_desc[id].type == SET_T_U8)
		*p = (uint8_t)(v + 0.5f);
	else
		memcpy(p, &v, sizeof(v));
}

int settings_load(settings_t *s)
{
	uint32_t magic, n, crc, i;
	settings_t t;

	if (!SV.chip || flq_read(set_buf, 0, SET_BYTES))
		return -1;
	memcpy(&magic, set_buf, 4);
	memcpy(&n, set_buf + 4, 4);
	memcpy(&crc, set_buf + 8, 4);
	if (magic != SET_MAGIC || n > (SET_BYTES - 12) / 8
		|| crc != sv_crc32(0, set_buf + 12, n * 8))
		return -1;
	t = *s;
	for (i = 0; i < n; i++)
	{
		uint32_t h;
		float v;
		int id;

		memcpy(&h, set_buf + 12 + 8 * i, 4);
		memcpy(&v, set_buf + 16 + 8 * i, 4);
		for (id = 0; id < SET_COUNT; id++)
			if (settings_desc[id].persist && name_hash(settings_desc[id].name) == h)
				field_set(&t, id, v);
	}
	*s = t;
	return 0;
}

int settings_store(const settings_t *s)
{
	uint32_t n = 0, magic = SET_MAGIC, crc;
	int id;

	if (!SV.chip || SV.st != ST_IDLE)
		return -1;
	memset(set_buf, 0xFF, sizeof(set_buf));
	for (id = 0; id < SET_COUNT && n < (SET_BYTES - 12) / 8; id++)
	{
		uint32_t h;
		float v;

		if (!settings_desc[id].persist)
			continue;
		h = name_hash(settings_desc[id].name);
		v = field_get(s, id);
		memcpy(set_buf + 12 + 8 * n, &h, 4);
		memcpy(set_buf + 16 + 8 * n, &v, 4);
		n++;
	}
	crc = sv_crc32(0, set_buf + 12, n * 8);
	memcpy(set_buf, &magic, 4);
	memcpy(set_buf + 4, &n, 4);
	memcpy(set_buf + 8, &crc, 4);
	if (flq_write(0, set_buf, (12 + n * 8 + FLASH_PAGE - 1) & ~(FLASH_PAGE - 1)))
		return -1;
	SV.st = ST_SETTINGS;
	SV.slot = -1;
	SV.us_max = SV.us_sum = SV.frames = SV.frame_max = SV.held_max = 0;
	SV.t0 = sys_time_us();
	return 0;
}

/* ---- the settings a save carries ---------------------------------------------- */

/* The player's preferences stay as they are when a save loads; every other
 * field (movevars, skill, the AI spread budget: they decide what the game
 * does) comes from the save. */
static int preference(int id)
{
	static const char *const pref[] = {
		"fov", "yawspeed", "pitchspeed", "vol_master", "vol_sfx", "invert_y",
		"stick_deadzone", "show_fps", "debug_overlay", "r_openseam", "r_dither",
		"brightness", "look_sens", "vol_music"
	};
	unsigned i;

	for (i = 0; i < sizeof(pref) / sizeof(pref[0]); i++)
		if (!strcmp(settings_desc[id].name, pref[i]))
			return 1;
	return 0;
}

static void settings_section_save(void)
{
	int id;

	sv_tag(SV_TAG('S', 'E', 'T', '1'));
	sv_u32(SET_COUNT);
	for (id = 0; id < SET_COUNT; id++)
	{
		sv_u32(name_hash(settings_desc[id].name));
		sv_f32(field_get(&settings, id));
	}
}

static int settings_section_load(void)
{
	uint32_t n, i;

	if (sv_expect(SV_TAG('S', 'E', 'T', '1')))
		return -1;
	n = sv_get_u32();
	if (n > 256)
	{
		sv_fail("settings count");
		return -1;
	}
	for (i = 0; i < n && !sv_failed(); i++)
	{
		uint32_t h = sv_get_u32();
		float v = sv_get_f32();
		int id;

		for (id = 0; id < SET_COUNT; id++)
			if (name_hash(settings_desc[id].name) == h && !preference(id))
				field_set(&settings, id, v);
	}
	return sv_failed() ? -1 : 0;
}

/* ---- snapshot ----------------------------------------------------------------- */

static const char *title_of(const char *map)
{
	return !strncmp(map, "t0a0", 4) ? "Hazard Course" : "Uplink";
}

/* Settle what is in flight (the same in the run that saves and goes on as
 * in every load of the save): monster thinks, layer and HLE opens, map
 * precache holds. */
static void settle(void)
{
	int s;

	game_save_settle();
	for (s = 0; s < HL64_WORLD_LAYERS; s++)
	{
		wld_layer_t *L = &wld_layers[s];

		if (L->open && L->ready == 0)
			wld_open(L->map);
	}
	rec_settle();
}

/* Every section in its order (sv_part: test builds' size table by part). */
static void sections_save(void)
{
	sv_part("eng clock rng settings");
	sv_tag(SV_TAG('H', 'L', '6', '4'));
	/* the clock first: eng_load_state takes the frame's time from it */
	eng_clock_save();
	eng_save_state();
	rng_save();
	settings_section_save();
	sv_part("rec");
	rec_save();
	sv_part("ent");
	ent_save();
	col_contents_save();	/* its own part: contents brushes */
	sv_part("player");
	phy_player_save();
	sv_part("in");
	in_save();
	sv_part("conv");
	phy_conv_save();
	sv_part("trans");
	trans_save();
	sv_part("seam");
	seam_save();
	sv_part("style");
	wld_style_save();
	sv_part("snd");
	snd_save();
	sv_part("game");
	sv_tag(SV_TAG('G', 'A', 'M', 'E'));
	game_save_globals();
	sv_part("route end");
	rt_save();
	sv_tag(SV_TAG('E', 'N', 'D', '!'));
}

static int sections_load(void)
{
	if (sv_expect(SV_TAG('H', 'L', '6', '4')) || eng_clock_load() || eng_load_state()
		|| rng_load() || settings_section_load() || rec_load() || ent_load()
		|| col_contents_load() || phy_player_load() || in_load() || phy_conv_load()
		|| trans_load() || seam_load() || wld_style_load() || snd_load()
		|| sv_expect(SV_TAG('G', 'A', 'M', 'E')) || game_load_globals() || rt_load_state()
		|| sv_expect(SV_TAG('E', 'N', 'D', '!')) || ent_priv_fixup())
		return -1;
	return 0;
}

/* The snapshot frame: settle, then every section staged (plain copies);
 * the coder runs over it in the following frames (ST_CODE). */
static int snapshot(int slot)
{
	uint64_t t0 = sys_time_us(), t1;
	uint32_t raw;

	settle();
	t1 = sys_time_us();
	sv_write_begin(SV.raw, SV.raw_cap);
	sections_save();
	raw = sv_write_end();
	if (!raw)
	{
		log_printf("save: slot %d: %s: %lu B of state (staging %lu B)\n", slot,
			sv_fail_reason() ? sv_fail_reason() : "too big", (unsigned long)sv_write_pos(),
			(unsigned long)SV.raw_cap);
		return -1;
	}
	SV.raw_len = raw;
	log_printf("save: slot %d snapshot: settle %lu us, staging %lu us\n", slot,
		(unsigned long)(t1 - t0), (unsigned long)(sys_time_us() - t1));
	memset(SV.img, 0, HDR_BYTES);
	sv_code_begin(SV.img + HDR_BYTES, SAVE_IMAGE_MAX, SV.work);
	return 0;
}

/* The image is coded: its header, then the write to list index slot. */
static int seal(int slot)
{
	slot_hdr_t *h = (slot_hdr_t *)SV.img;
	const ent_player_t *pl = phy_player();
	uint32_t raw = SV.raw_len, n = sv_code_end(), nsect;
	int s, nl = 0, at;

	if (!n)
	{
		log_printf("save: slot %d: too big: %lu B of state -> %lu B image (at most %lu B)\n",
			slot, (unsigned long)raw, (unsigned long)sv_code_need(),
			(unsigned long)SAVE_IMAGE_MAX);
		return -1;
	}
	nsect = (HDR_BYTES + n + SAVE_SECTOR_BYTES - 1) / SAVE_SECTOR_BYTES;
	at = slot < FLASH_SLOTS ? flash_room(nsect, slot, 0) : 0;
	if (at < 0 && slot != 0)
	{
		/* a manual slot with no free run: over its own copy */
		at = flash_room(nsect, slot, 1);
		if (at >= 0)
		{
			log_printf("save: slot %d: no free run of %lu sectors: over its own copy\n", slot,
				(unsigned long)nsect);
			SV.flash[slot].info.state = SAVE_EMPTY;
		}
	}
	if (at < 0)
	{
		log_printf("save: slot %d: no free run of %lu sectors\n", slot, (unsigned long)nsect);
		return -1;
	}
	h->magic = SLOT_MAGIC;
	h->format = SAVE_FORMAT;
	h->hdr_bytes = HDR_BYTES;
	h->layout = layout_id();
	h->seq = ++SV.seq;
	h->raw_bytes = raw;
	h->img_bytes = n;
	h->img_crc = sv_crc32(0, SV.img + HDR_BYTES, n);
	h->game_time = (float)eng_clock_time();
	h->skill = settings.skill;
	h->slot = (uint8_t)(slot < FLASH_SLOTS ? (uint32_t)slot : PAK_SLOT);
	h->nsect = (uint8_t)(slot < FLASH_SLOTS ? nsect : 0);
	for (s = 0; s < HL64_WORLD_LAYERS; s++)
	{
		const wld_layer_t *L = wld_layer(s);

		h->layer_map[s] = L ? L->map : WLD_NO_MAP;
		h->layer_crc[s] = L ? L->hdr.dir_crc : 0;
		nl += L != NULL;
	}
	h->nlayers = (uint8_t)nl;
	strncpy(h->map, wld_map_name(phy_player_map()), sizeof(h->map) - 1);
	strncpy(h->title, title_of(h->map), sizeof(h->title) - 1);
	h->hdr_crc = sv_crc32(0, SV.img + 8, HDR_BYTES - 8);
	log_printf("save: slot %d seq %lu: %lu B state -> %lu B image (%lu%%, crc %08lx), %s %d+%lu, "
		"%s t=%.3f org %.1f %.1f %.1f\n", slot, (unsigned long)h->seq, (unsigned long)raw,
		(unsigned long)n, (unsigned long)(100u * n / (raw ? raw : 1)), (unsigned long)h->img_crc,
		slot < FLASH_SLOTS ? "sectors" : "Pak entry",
		slot < FLASH_SLOTS ? at + (int)POOL_FIRST : slot - FLASH_SLOTS,
		(unsigned long)(slot < FLASH_SLOTS ? nsect : 1), h->map, (double)h->game_time,
		(double)pl->origin[0], (double)pl->origin[1], (double)pl->origin[2]);
	if (slot >= FLASH_SLOTS)
		return pak_begin(slot - FLASH_SLOTS);
	SV.wsect = (uint8_t)at;
	SV.wnsect = (uint8_t)nsect;
	return flq_write(sect_off((uint32_t)at), SV.img, HDR_BYTES + n);
}

/* The write ended: read it back; the slot's directory entry is the new
 * copy only if that checks out. */
static int write_done(void)
{
	slot_hdr_t h;
	uint32_t seq = SV.seq;

	if (flq_result())
		return -1;
	if (read_flash(SV.wsect, &h) != SAVE_OK || h.seq != seq || h.slot != (uint32_t)SV.slot)
	{
		log_printf("save: slot %d: read-back of sector %u does not check out\n", SV.slot,
			(unsigned)(SV.wsect + POOL_FIRST));
		return -1;
	}
	flash_entry(SV.slot, &h, SAVE_OK, SV.wsect);
	return 0;
}

/* ---- load --------------------------------------------------------------------- */

/* The session goes, as at an end section (eng.c eng_disconnect): sounds,
 * effects, lights, entities, the player, HLE slots, layers, residency. */
static void drop_session(void)
{
	int m;

	snd_stop_all();
	fx_init();
	ren_dlight_reset();
	ent_reset();
	phy_player_reset();
	studio_new_game();
	rec_reset();
	for (m = 0; m < wld_map_count(); m++)
		wld_close(wld_layer_of_map(m));
	wld_stream_new_game();
	ren_cam.map = -1;
}

#ifdef HL64_TEST
/* Test builds: the restored game saved again at once (the snapshot's
 * sections, no settle: the saved state was settled) must be the loaded
 * state byte for byte (test/save/determinism.sh). Its time is left out of
 * the load's step and frame figures. */
static void restore_check(void)
{
	uint64_t t0 = sys_time_us();
	uint32_t off = (SV.raw_len + 15u) & ~15u, n, i = 0;
	const uint8_t *a = SV.raw, *b = SV.raw + off;

	if (SV.raw_cap - off < SV.raw_len)
	{
		log_printf("load: slot %d re-save: no room to stage it (%lu B)\n", SV.slot,
			(unsigned long)SV.raw_len);
		return;
	}
	sv_write_begin(SV.raw + off, SV.raw_cap - off);
	sections_save();
	n = sv_write_end();
	while (i < n && i < SV.raw_len && a[i] == b[i])
		i++;
	if (n == SV.raw_len && i == n)
		log_printf("load: slot %d re-save: equal, %lu B state crc %08lx\n", SV.slot,
			(unsigned long)n, (unsigned long)sv_crc32(0, b, n));
	else
	{
		uint32_t k, lo = i & ~3u;

		log_printf("load: slot %d re-save: DIFFERS: %lu B state, %lu B loaded, first difference "
			"at %lu\n", SV.slot, (unsigned long)n, (unsigned long)SV.raw_len, (unsigned long)i);
		for (k = lo; k + 4 <= lo + 32 && k + 4 <= n && k + 4 <= SV.raw_len; k += 4)
			log_printf("load: re-save at %lu: loaded %02x%02x%02x%02x, re-saved %02x%02x%02x%02x\n",
				(unsigned long)k, a[k], a[k + 1], a[k + 2], a[k + 3], b[k], b[k + 1], b[k + 2],
				b[k + 3]);
	}
	SV.check_us = (uint32_t)(sys_time_us() - t0);
}
#endif

static int restore(void)
{
	const slot_hdr_t *h = &SV.hdr;
	uint64_t t0 = sys_time_us(), t1, t2, t3;
	int s;

	/* the New Game shell: the game's campaign state at its start */
	game_new_campaign();
	for (s = 0; s < HL64_WORLD_LAYERS; s++)
	{
		wld_layer_t *L;

		if (h->layer_map[s] == WLD_NO_MAP)
			continue;
		if (h->layer_map[s] >= wld_map_count()
			|| !(L = wld_open_in(h->layer_map[s], s)))
		{
			log_printf("load: layer %d (map %u) does not open\n", s, (unsigned)h->layer_map[s]);
			return -1;
		}
		if (L->hdr.dir_crc != h->layer_crc[s])
		{
			log_printf("load: %s was cooked again since the save\n", wld_map_name(L->map));
			return -1;
		}
	}
	t1 = sys_time_us();
	sv_read_begin(SV.raw, SV.raw_len);
	if (sections_load())
	{
		log_printf("load: slot %d: %s (at %lu B of state)\n", SV.slot,
			sv_fail_reason() ? sv_fail_reason() : "section", (unsigned long)sv_read_pos());
		return -1;
	}
	if (sv_read_pos() != h->raw_bytes)
	{
		log_printf("load: slot %d: %lu B read, %lu B saved\n", SV.slot,
			(unsigned long)sv_read_pos(), (unsigned long)h->raw_bytes);
		return -1;
	}
	/* every model with interest: its infos now (load I/O), as New Game */
	t2 = sys_time_us();
	io_set_load(1);
	studio_preload();
	io_set_load(0);
	t3 = sys_time_us();
	game_load_done();
	log_printf("load: slot %d restore: layers %lu us, sections %lu us, preload %lu us, game %lu us\n",
		SV.slot, (unsigned long)(t1 - t0), (unsigned long)(t2 - t1), (unsigned long)(t3 - t2),
		(unsigned long)(sys_time_us() - t3));
#ifdef HL64_TEST
	restore_check();
#endif
	return 0;
}

/* ---- API ---------------------------------------------------------------------- */

int save_slots(save_info_t *out, int max)
{
	int i, n = 0;

	for (i = 0; i < FLASH_SLOTS && n < max; i++)
		out[n++] = SV.flash[i].info;
	for (i = 0; i < SV.npak && n < max; i++)
		out[n++] = SV.pak[i].info;
	return n;
}

static void op_begin(int st, int slot)
{
	SV.st = (uint8_t)st;
	SV.slot = (int8_t)slot;
	SV.pgen = SV.pak_gen;
	SV.us_max = SV.us_sum = SV.frames = SV.frame_max = SV.held_max = 0;
	SV.t0 = sys_time_us();
}

/* The entry behind list index slot, NULL none. */
static dirent_t *entry(int slot)
{
	if (slot >= 0 && slot < FLASH_SLOTS)
		return SV.chip ? &SV.flash[slot] : NULL;
	slot -= FLASH_SLOTS;
	return slot >= 0 && slot < SV.npak && SV.pak_st == SAVE_PAK_OK ? &SV.pak[slot] : NULL;
}

int save_write(int slot)
{
	if (SV.st != ST_IDLE || slot < 1 || !entry(slot) || !phy_player_active())
		return -1;
	op_begin(ST_SNAP, slot);
	return 0;
}

void save_autosave(void)
{
	if (SV.chip && phy_player_active())
		SV.auto_ask = 1;
}

int save_load(int slot)
{
	const dirent_t *d = entry(slot);

	if (SV.st != ST_IDLE || !d || d->info.state != SAVE_OK)
		return -1;
	op_begin(slot < FLASH_SLOTS ? ST_LOAD_READ : ST_PAK_READ, slot);
	SV.pnew = d->note;
	SV.poff = 0;
	SV.plen = d->bytes;
	return 0;
}

int save_busy(void)
{
	return SV.st != ST_IDLE;
}

int save_result(void)
{
	return SV.result;
}

int save_pak_status(void)
{
	return SV.pak_st;
}

/* The operation ends; its log line comes at the next frame boundary, with
 * the whole of this last frame measured (save_busy stays 1 until then). */
static void op_end(int result, const char *what)
{
	SV.result = (int8_t)result;
	SV.what = what;
	SV.st = ST_END;
}

/* An asked autosave starts at a quiet frame boundary: a player, no seam
 * window or crossing. */
static int autosave_due(void)
{
	if (!SV.auto_ask)
		return 0;
	if (!phy_player_active())
	{
		SV.auto_ask = 0;
		return 0;
	}
	if (!seam_quiet())
		return 0;
	SV.auto_ask = 0;
	op_begin(ST_SNAP, 0);
	return 1;
}

/* The image in SV.img read and checked: decode it over the next frames. */
static void load_begin(void)
{
	sv_decode_begin(SV.img + HDR_BYTES, SV.hdr.img_bytes, SV.raw, SV.raw_cap);
	SV.st = ST_DECODE;
}

/* The state decoded: drop the session, reopen after the held frames. */
static void load_drop(void)
{
	log_printf("load: slot %d seq %lu: %s t=%.3f, %lu B image, %lu B state\n", SV.slot,
		(unsigned long)SV.hdr.seq, SV.hdr.map, (double)SV.hdr.game_time,
		(unsigned long)SV.hdr.img_bytes, (unsigned long)SV.raw_len);
	{
		uint64_t t0 = sys_time_us();

		drop_session();
		log_printf("load: slot %d drop: %lu us\n", SV.slot, (unsigned long)(sys_time_us() - t0));
	}
	SV.drop = DROP_FRAMES;
	SV.st = ST_LOAD_DROP;
}

int save_frame(void)
{
	uint64_t t0 = sys_time_us();
	uint32_t us, frame = SV.last ? (uint32_t)(t0 - SV.last) - SV.check_us : 0;
	int hold = 0, r;

	/* the frame that just ended belonged to the operation running now */
	SV.last = t0;
	SV.check_us = 0;
	if (SV.st != ST_IDLE && frame > SV.frame_max)
		SV.frame_max = frame;
	/* a frame this module held (no game step): all of it is ours */
	if (SV.held && frame > SV.held_max)
		SV.held_max = frame;
	pak_poll();
	switch (SV.st)
	{
	case ST_END:
		log_printf("save: %s slot %d %s: %lu frames, worst step %lu us, longest held frame %lu us, "
			"longest frame %lu us, %lu ms\n", SV.what, SV.slot, SV.result ? "FAILED" : "ok",
			(unsigned long)SV.frames, (unsigned long)SV.us_max, (unsigned long)SV.held_max,
			(unsigned long)SV.frame_max, (unsigned long)((t0 - SV.t0) / 1000u));
		SV.st = ST_IDLE;
		/* fall through */
	case ST_IDLE:
		if (!autosave_due())
			return 0;
		/* fall through */
	case ST_SNAP:
		hold = 1;
		if (snapshot(SV.slot))
			op_end(-1, "save");
		else
			SV.st = ST_CODE;
		break;
	case ST_CODE:
		if (sv_code_step(CODE_BUDGET_US))
			break;
		if (seal(SV.slot))
			op_end(-1, "save");
		else
			SV.st = SV.slot < FLASH_SLOTS ? ST_WRITE : ST_PAK_WRITE;
		break;
	case ST_DECODE:
		r = sv_decode_step(CODE_BUDGET_US);
		if (r > 0)
			break;
		SV.raw_len = sv_decode_end();
		if (r < 0 || SV.raw_len != SV.hdr.raw_bytes)
		{
			log_printf("load: slot %d: the image decodes to %lu B, %lu B saved\n", SV.slot,
				(unsigned long)SV.raw_len, (unsigned long)SV.hdr.raw_bytes);
			op_end(-1, "load");
			break;
		}
		hold = 1;
		load_drop();
		break;
	case ST_WRITE:
	case ST_SETTINGS:
		flq_step(FLASH_BUDGET_US);
		if (!flq_busy())
			op_end(SV.st == ST_WRITE ? write_done() : flq_result(),
				SV.st == ST_WRITE ? "save" : "settings");
		break;
	case ST_PAK_WRITE:
		r = SV.pgen == SV.pak_gen ? pak_write_step() : -1;
		if (r == 0)
			pak_scan();
		else if (r < 0)
			SV.pak_acc = 0xFF;	/* a failed transfer unmounts: mount again */
		if (r <= 0)
			op_end(r, "save");
		break;
	case ST_LOAD_READ:
		hold = 1;
		SV.auto_ask = 0;
		if (read_flash(SV.flash[SV.slot].sect, &SV.hdr) != SAVE_OK)
		{
			SV.flash[SV.slot].info.state = SAVE_CORRUPT;
			op_end(-1, "load");
			hold = 0;
			break;
		}
		load_begin();
		break;
	case ST_PAK_READ:
		SV.auto_ask = 0;
		r = SV.pgen == SV.pak_gen ? pak_read_step() : -1;
		if (r < 0)
		{
			SV.pak_acc = 0xFF;
			op_end(-1, "load");
		}
		else if (r == 0)
			load_begin();
		break;
	case ST_LOAD_DROP:
		hold = 1;
		/* the world's own frame count goes on: fenced frees retire */
		wld_frame(wld_frame_no);
		if (--SV.drop == 0)
			SV.st = ST_LOAD_OPEN;
		break;
	case ST_LOAD_OPEN:
		hold = 1;
		if (restore())
		{
			drop_session();
			op_end(-1, "load");
		}
		else
			op_end(0, "load");
		break;
	}
	us = (uint32_t)(sys_time_us() - t0) - SV.check_us;
	if (SV.st != ST_IDLE || hold)
	{
		SV.frames++;
		SV.us_sum += us;
		if (us > SV.us_max)
			SV.us_max = us;
	}
	SV.held = (uint8_t)hold;
	return hold;
}

/* `save N`: list index N; 0 asks for an autosave. An operation in flight
 * finishes first: the command comes back next frame (as a quit). */
static int cmd_save(const dbg_cmd_t *c)
{
	if (c->arg[0].u == 0)
	{
		save_autosave();
		return SV.auto_ask ? 0 : 1;
	}
	if (save_busy())
	{
		dbg_cmd_t q = *c;

		q.frame = dbg_trace_frame() + 1;
		return dbg_push(&q) ? 1 : 0;
	}
	return save_write((int)c->arg[0].u) ? 1 : 0;
}

static int cmd_load(const dbg_cmd_t *c)
{
	return save_load((int)c->arg[0].u) ? 1 : 0;
}

static int cmd_flash(const dbg_cmd_t *c)
{
	return flq_dbg_flash(c->arg[0].u) ? 1 : 0;
}

void save_init(void)
{
	int s;

	memset(&SV, 0, sizeof(SV));
	SV.slot = -1;
	SV.pak_acc = 0xFF;
	SV.pak_st = SAVE_PAK_ABSENT;
	SV.img = mem_region(MEM_SAVE);
	SV.work = SV.img + ((SAVE_BYTES + 15u) & ~15u);
	SV.raw = SV.work + SV_WORK_BYTES;
	SV.raw_cap = (uint32_t)(SV.img + mem_region_bytes(MEM_SAVE) - SV.raw);
	dbg_handler_set(DBG_SAVE, cmd_save);
	dbg_handler_set(DBG_LOAD, cmd_load);
	dbg_handler_set(DBG_FLASH, cmd_flash);
	SV.chip = (uint8_t)sys_flash_init();
	if (SV.chip)
	{
		flash_scan();
		log_printf("save: FlashRAM: autosave + %d slots, %d pool sectors:", SAVE_FLASH_MANUAL,
			SAVE_POOL_SECTORS);
		for (s = 0; s < FLASH_SLOTS; s++)
			log_printf(" %s", SV.flash[s].info.state == SAVE_OK ? SV.flash[s].info.map
				: SV.flash[s].info.state == SAVE_EMPTY ? "empty" : "corrupt");
		log_printf("\n");
	}
	else
		log_printf("save: no FlashRAM: Flash slots and autosaves off\n");
	pak_poll();
}
