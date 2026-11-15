/* SPDX-License-Identifier: MIT */
/*
 * save.c -- saved games and the settings block (include/hl64/save.h,
 * docs/architecture.md 14, decision D4).
 *
 * FlashRAM (128 KB, 8 sectors of 16 KB): sector 0 the settings block,
 * then SAVE_SLOTS slots of SAVE_SLOT_SECTORS sectors. A slot is a 128-byte
 * header (magic, format, layout id, sequence, sizes, CRC-32 of the image,
 * display info, the open layers) and the LZ image of the sections every
 * module writes (engine/save/svmod.h order, then the game's).
 *
 * Save (save_write): the next frame boundary (save_frame, first thing in
 * eng_frame) settles what is in flight -- paused monster thinks finish, a
 * layer or HLE slot opening completes, a map's precache hold lands -- in
 * the run that saves as in every load of it, writes the snapshot into the
 * save region and hands it to the flash writer, which programs it over the
 * following frames.
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

#define SAVE_FORMAT	1
#define SLOT_MAGIC	0x484C5356u	/* 'HLSV' */
#define SET_MAGIC	0x484C5354u	/* 'HLST' */
#define HDR_BYTES	128u
#define SET_BYTES	512u
#define FLASH_PAGE	128u
#define DROP_FRAMES	3		/* world frames for the closed layers' fenced frees */
#define FLASH_BUDGET_US	4000u		/* flash writer time per frame */
#define SCRATCH_WORK	(64u * 1024u)	/* coder work in the scratch arena from here */

_Static_assert(SAVE_SLOT_BYTES <= HL64_SAVE_KB * 1024u, "save region holds a slot");
_Static_assert(SAVE_SECTOR_BYTES * (1 + SAVE_SLOTS * SAVE_SLOT_SECTORS) <= SAVE_FLASH_BYTES,
	"slots fit the flash");

typedef struct {
	uint32_t	magic;
	uint32_t	hdr_crc;		/* bytes [8, HDR_BYTES) */
	uint16_t	format, hdr_bytes;
	uint32_t	layout;
	uint32_t	seq;
	uint32_t	raw_bytes, img_bytes, img_crc;
	float		game_time;
	uint8_t		skill, nlayers, pad[2];
	uint16_t	layer_map[HL64_WORLD_LAYERS];	/* WLD_NO_MAP: closed */
	uint32_t	layer_crc[HL64_WORLD_LAYERS];	/* the package's directory CRC */
	char		map[16];
	char		title[24];
} slot_hdr_t;
_Static_assert(sizeof(slot_hdr_t) <= HDR_BYTES, "slot header");

enum { ST_IDLE, ST_SNAP, ST_WRITE, ST_SETTINGS, ST_LOAD_READ, ST_LOAD_DROP, ST_LOAD_OPEN };

static struct {
	uint8_t		chip;		/* flash present */
	uint8_t		st;
	int8_t		slot;
	int8_t		result;
	uint8_t		drop;		/* held frames left before the reopen */
	uint32_t	seq;		/* newest slot's sequence */
	uint8_t		*img;		/* SAVE region: header + image */
	save_info_t	dir[SAVE_SLOTS];
	slot_hdr_t	hdr;		/* the slot being loaded */
	uint32_t	us_max, us_sum, frames;	/* this operation's held / step frames */
	uint64_t	t0;
} SV;

static uint8_t set_buf[SET_BYTES] __attribute__((aligned(16)));

static uint32_t slot_off(int slot)
{
	return SAVE_SECTOR_BYTES * (1u + (uint32_t)slot * SAVE_SLOT_SECTORS);
}

static void *work(void)
{
	return (uint8_t *)mem_region(MEM_SCRATCH) + SCRATCH_WORK;
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

static void info_of(const slot_hdr_t *h, save_info_t *o, int state)
{
	memset(o, 0, sizeof(*o));
	o->state = (uint8_t)state;
	if (state != SAVE_OK)
		return;
	o->skill = h->skill;
	o->seq = h->seq;
	o->game_time = h->game_time;
	memcpy(o->map, h->map, sizeof(o->map) - 1);
	memcpy(o->title, h->title, sizeof(o->title) - 1);
}

/* Read slot's header and image into the save region and check them;
 * SAVE_EMPTY / SAVE_OK / SAVE_CORRUPT. */
static int read_slot(int slot, slot_hdr_t *h)
{
	uint32_t off = slot_off(slot);

	if (flq_read(SV.img, off, HDR_BYTES))
		return SAVE_EMPTY;
	memcpy(h, SV.img, sizeof(*h));
	if (h->magic == 0xFFFFFFFFu)
		return SAVE_EMPTY;
	if (h->magic != SLOT_MAGIC || h->format != SAVE_FORMAT || h->hdr_bytes != HDR_BYTES
		|| h->hdr_crc != sv_crc32(0, SV.img + 8, HDR_BYTES - 8)
		|| h->img_bytes == 0 || h->img_bytes > SAVE_SLOT_BYTES - HDR_BYTES
		|| h->layout != layout_id())
		return SAVE_CORRUPT;
	if (flq_read(SV.img + HDR_BYTES, off + HDR_BYTES, (h->img_bytes + 1) & ~1u)
		|| sv_crc32(0, SV.img + HDR_BYTES, h->img_bytes) != h->img_crc)
		return SAVE_CORRUPT;
	return SAVE_OK;
}

static void dir_refresh(int slot)
{
	slot_hdr_t h;
	int st = read_slot(slot, &h);

	info_of(&h, &SV.dir[slot], st);
	if (st == SAVE_OK && h.seq > SV.seq)
		SV.seq = h.seq;
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
	SV.us_max = SV.us_sum = SV.frames = 0;
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

/* Test builds: each section's state bytes and (about: the coder lags by
 * its lookahead) its image bytes. */
static uint32_t sec_raw, sec_img;

static void sec(const char *name)
{
#ifdef HL64_TEST
	uint32_t r, c;

	sv_write_pos(&r, &c);
	log_printf("save: section %s %lu B -> %lu B\n", name, (unsigned long)(r - sec_raw),
		(unsigned long)(c - sec_img));
	sec_raw = r;
	sec_img = c;
#else
	(void)name;
#endif
}

static void sections_save(void)
{
	sec_raw = sec_img = 0;
	sv_tag(SV_TAG('H', 'L', '6', '4'));
	eng_save_state();
	eng_clock_save();
	rng_save();
	settings_section_save();
	sec("eng clock rng settings");
	rec_save();
	sec("rec");
	ent_save();
	sec("ent");
	col_contents_save();
	phy_player_save();
	in_save();
	phy_conv_save();
	trans_save();
	seam_save();
	wld_style_save();
	snd_save();
	sec("player in conv trans seam style snd");
	sv_tag(SV_TAG('G', 'A', 'M', 'E'));
	game_save_globals();
	sec("game");
	rt_save();
	sv_tag(SV_TAG('E', 'N', 'D', '!'));
}

static int sections_load(void)
{
	if (sv_expect(SV_TAG('H', 'L', '6', '4')) || eng_load_state() || eng_clock_load()
		|| rng_load() || settings_section_load() || rec_load() || ent_load()
		|| col_contents_load() || phy_player_load() || in_load() || phy_conv_load()
		|| trans_load() || seam_load() || wld_style_load() || snd_load()
		|| sv_expect(SV_TAG('G', 'A', 'M', 'E')) || game_load_globals() || rt_load_state()
		|| sv_expect(SV_TAG('E', 'N', 'D', '!')) || ent_priv_fixup())
		return -1;
	return 0;
}

static int snapshot(int slot)
{
	slot_hdr_t *h = (slot_hdr_t *)SV.img;
	const ent_player_t *pl = phy_player();
	uint32_t raw, n;
	int s, nl = 0;

	settle();
	memset(SV.img, 0, HDR_BYTES);
	sv_write_begin(SV.img + HDR_BYTES, SAVE_SLOT_BYTES - HDR_BYTES, work());
	sections_save();
	n = sv_write_end(&raw);
	if (!n)
	{
		uint32_t r, c;

		sv_write_pos(&r, &c);
		log_printf("save: slot %d: %s: %lu B of state -> %lu B image (slot %lu B)\n", slot,
			sv_fail_reason() ? sv_fail_reason() : "too big", (unsigned long)raw,
			(unsigned long)c, (unsigned long)(SAVE_SLOT_BYTES - HDR_BYTES));
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
	log_printf("save: slot %d seq %lu: %lu B state -> %lu B image (%lu%%), %s t=%.3f "
		"org %.1f %.1f %.1f\n", slot, (unsigned long)h->seq, (unsigned long)raw,
		(unsigned long)n, (unsigned long)(100u * n / (raw ? raw : 1)), h->map,
		(double)h->game_time, (double)pl->origin[0], (double)pl->origin[1],
		(double)pl->origin[2]);
	return flq_write(slot_off(slot), SV.img, HDR_BYTES + n);
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

static int restore(void)
{
	const slot_hdr_t *h = &SV.hdr;
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
	sv_read_begin(SV.img + HDR_BYTES, h->img_bytes, work());
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
	io_set_load(1);
	studio_preload();
	io_set_load(0);
	game_load_done();
	return 0;
}

/* ---- API ---------------------------------------------------------------------- */

int save_slots(save_info_t *out, int max)
{
	int i, n = max < SAVE_SLOTS ? max : SAVE_SLOTS;

	for (i = 0; i < n; i++)
		out[i] = SV.dir[i];
	return n < 0 ? 0 : n;
}

static void op_begin(int st, int slot)
{
	SV.st = (uint8_t)st;
	SV.slot = (int8_t)slot;
	SV.us_max = SV.us_sum = SV.frames = 0;
	SV.t0 = sys_time_us();
}

int save_write(int slot)
{
	if (!SV.chip || SV.st != ST_IDLE || slot < 0 || slot >= SAVE_SLOTS || !phy_player_active())
		return -1;
	op_begin(ST_SNAP, slot);
	return 0;
}

int save_load(int slot)
{
	if (!SV.chip || SV.st != ST_IDLE || slot < 0 || slot >= SAVE_SLOTS
		|| SV.dir[slot].state != SAVE_OK)
		return -1;
	op_begin(ST_LOAD_READ, slot);
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

static void op_end(int result, const char *what)
{
	SV.result = (int8_t)result;
	log_printf("save: %s slot %d %s: %lu frames, worst frame %lu us, %lu ms\n", what, SV.slot,
		result ? "FAILED" : "ok", (unsigned long)SV.frames, (unsigned long)SV.us_max,
		(unsigned long)((sys_time_us() - SV.t0) / 1000u));
	SV.st = ST_IDLE;
}

int save_frame(void)
{
	uint64_t t0 = sys_time_us();
	uint32_t us;
	int hold = 0;

	switch (SV.st)
	{
	case ST_IDLE:
		return 0;
	case ST_SNAP:
		hold = 1;
		if (snapshot(SV.slot))
			op_end(-1, "save");
		else
			SV.st = ST_WRITE;
		break;
	case ST_WRITE:
	case ST_SETTINGS:
		flq_step(FLASH_BUDGET_US);
		if (!flq_busy())
		{
			if (SV.st == ST_WRITE)
				dir_refresh(SV.slot);
			op_end(flq_result(), SV.st == ST_WRITE ? "save" : "settings");
		}
		break;
	case ST_LOAD_READ:
		hold = 1;
		if (read_slot(SV.slot, &SV.hdr) != SAVE_OK)
		{
			SV.dir[SV.slot].state = SAVE_CORRUPT;
			op_end(-1, "load");
			hold = 0;
			break;
		}
		log_printf("load: slot %d seq %lu: %s t=%.3f, %lu B image\n", SV.slot,
			(unsigned long)SV.hdr.seq, SV.hdr.map, (double)SV.hdr.game_time,
			(unsigned long)SV.hdr.img_bytes);
		drop_session();
		SV.drop = DROP_FRAMES;
		SV.st = ST_LOAD_DROP;
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
	us = (uint32_t)(sys_time_us() - t0);
	if (SV.st != ST_IDLE || hold)
	{
		SV.frames++;
		SV.us_sum += us;
		if (us > SV.us_max)
			SV.us_max = us;
	}
	return hold;
}

static int cmd_save(const dbg_cmd_t *c)
{
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
	SV.img = mem_region(MEM_SAVE);
	if (mem_region_bytes(MEM_SCRATCH) < SCRATCH_WORK + SV_WORK_BYTES)
		sys_error("save: scratch arena too small for the coder");
	dbg_handler_set(DBG_SAVE, cmd_save);
	dbg_handler_set(DBG_LOAD, cmd_load);
	dbg_handler_set(DBG_FLASH, cmd_flash);
	SV.chip = (uint8_t)sys_flash_init();
	if (!SV.chip)
	{
		log_printf("save: no FlashRAM: saving off\n");
		return;
	}
	for (s = 0; s < SAVE_SLOTS; s++)
		dir_refresh(s);
	log_printf("save: FlashRAM: %d slots of %u KB:", SAVE_SLOTS, SAVE_SLOT_BYTES / 1024u);
	for (s = 0; s < SAVE_SLOTS; s++)
		log_printf(" %s", SV.dir[s].state == SAVE_OK ? SV.dir[s].map
			: SV.dir[s].state == SAVE_EMPTY ? "empty" : "corrupt");
	log_printf("\n");
}
