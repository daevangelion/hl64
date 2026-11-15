/* SPDX-License-Identifier: MIT */
/*
 * menu.c -- the front end (engine/ui/menu.h, docs/architecture.md 13).
 *
 * Look: the 1998 Half-Life menu recreated with our own drawing. Black
 * background (the frame's clear: nothing 3D is drawn while the menu is
 * up), the player's media/logo.avi banner at the top left (cooked to
 * gfx/logo.lg4 by tools/cook/bake_logo.py: 320 x 50 I4 frames at 24 fps,
 * one frame DMAed per change into the back one of two draw2d sheets), a
 * left column of dark buttons with orange labels (the highlighted one lit),
 * a description line for the highlighted item, and the menu sounds of the
 * player's sound/common (res.def Menu block). Labels are the player's
 * creditsfont (draw2d text).
 *
 * Screens: the title (New Game, Hazard Course, Load Game, Configuration),
 * the pause menu (Start in play: Resume Game, Save Game, Load Game,
 * Configuration, Quit to Title), New Game's difficulty (settings.skill,
 * which the game reads), Configuration (brightness, sound volume, music
 * volume, look sensitivity, invert look: applied as they change, stored through
 * settings_store when the screen is left), the save / load slot lists
 * (include/hl64/save.h) and the quit confirmation.
 *
 * Keys (engine/in in_keys): D-pad / stick move and change values, A
 * selects, B goes back, Start pauses and resumes. While the menu is up the
 * game is frozen (eng.c skips the simulation) and its sounds are paused
 * (snd_pause); menu sounds still play.
 */
#include <stdio.h>
#include <string.h>

#include "hl64/draw2d.h"
#include "hl64/rec.h"
#include "hl64/save.h"
#include "hl64/settings.h"
#include "hl64/snd.h"
#include "engine/core/eng.h"
#include "engine/core/log.h"
#include "engine/core/sys.h"
#include "engine/in/in.h"
#include "engine/io/io.h"
#include "engine/phys/player.h"
#include "engine/world/wld.h"
#include "engine/ui/menu.h"

/* res.def line ids: the menu's sounds */
enum {
#define RES(kind, id, name)	MR_##id,
#include "game/defs/res.def"
#undef RES
	MR_COUNT
};

/* ---- the logo banner (gfx/logo.lg4) ------------------------------------- */

#define LG4_MAGIC	0x4C473420u
#define LG4_HDR		32
#define LG4_MAX_W	320
#define LG4_MAX_H	64

static struct {
	uint32_t	rom;		/* 0: no logo */
	uint32_t	fb, ofs, len;	/* frame stride in the file, first frame, bytes read */
	uint16_t	frames, w, h, fps;
	uint8_t		*buf[2];
	int		sheet[2];
	int		have[2];	/* frame in each buffer, -1 none */
	int		show;		/* buffer drawn */
	int		load;		/* buffer the request fills, -1 none */
	int		loadf;
	io_req_t	req;
} logo;

static uint32_t be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint16_t be16(const uint8_t *p)
{
	return (uint16_t)(p[0] << 8 | p[1]);
}

static void logo_init(void)
{
	static uint8_t hdr[LG4_HDR] __attribute__((aligned(16)));
	uint32_t size, rom = sys_cart_addr("gfx/logo.lg4", &size), stride, fsize;
	int nbuf, i;

	memset(&logo, 0, sizeof(logo));
	logo.load = -1;
	if (!rom || size < LG4_HDR || (rom & 1))
	{
		log_printf("menu: no gfx/logo.lg4 (re-cook): no banner\n");
		return;
	}
	io_read_sync(hdr, rom, LG4_HDR, "menu");
	logo.frames = be16(hdr + 6);
	logo.w = be16(hdr + 8);
	logo.h = be16(hdr + 10);
	logo.fps = be16(hdr + 12);
	stride = be16(hdr + 14);
	logo.fb = be32(hdr + 16);
	logo.ofs = be32(hdr + 20);
	fsize = be32(hdr + 24);
	logo.len = (stride * logo.h + 1) & ~1u;
	if (be32(hdr) != LG4_MAGIC || be16(hdr + 4) != 1 || !logo.frames || !logo.w || !logo.h ||
		logo.w > LG4_MAX_W || logo.h > LG4_MAX_H || stride != ((uint32_t)logo.w + 15) / 16 * 8 ||
		logo.fb < logo.len || (logo.fb & 1) || (logo.ofs & 1) || logo.ofs < LG4_HDR ||
		fsize > size || logo.ofs + (uint64_t)logo.frames * logo.fb > fsize)
	{
		log_printf("menu: gfx/logo.lg4: bad header: no banner\n");
		return;
	}
	nbuf = logo.frames > 1 ? 2 : 1;
	for (i = 0; i < nbuf; i++)
	{
		logo.buf[i] = d2_region_alloc(logo.len);
		logo.sheet[i] = logo.buf[i] ? d2_dyn_sheet(logo.buf[i], logo.w, logo.h) : -1;
		logo.have[i] = -1;
		if (logo.sheet[i] < 0)
		{
			log_printf("menu: gfx/logo.lg4: no HUD room for %lu B: no banner\n",
				(unsigned long)logo.len);
			return;
		}
	}
	io_read_sync(logo.buf[0], rom + logo.ofs, logo.len, "menu");
	logo.have[0] = 0;
	logo.rom = rom;
	logo.req.prio = IO_PRIO_PREFETCH;
	log_printf("menu: logo %ux%u, %u frames at %u fps, %lu B a frame (%d buffers, %lu B HUD), "
		"%lu B on the cart\n", logo.w, logo.h, logo.frames, logo.fps, (unsigned long)logo.len,
		nbuf, (unsigned long)(nbuf * ((logo.len + 15) & ~15u)), (unsigned long)fsize);
}

/* After sys_frame_try_begin (every earlier frame's RDP work is done, so
 * the buffer not drawn now is free): show the frame of time t, reading it
 * into the back buffer first. */
static void logo_draw(uint64_t t_us)
{
	int want;

	if (!logo.rom)
		return;
	if (logo.frames > 1)
	{
		want = (int)((t_us * logo.fps / 1000000u) % logo.frames);
		if (logo.load >= 0 && logo.req.state == IO_DONE)
		{
			logo.have[logo.load] = logo.loadf;
			logo.show = logo.load;
			logo.load = -1;
		}
		if (logo.load < 0 && logo.have[logo.show] != want)
		{
			int b = logo.show ^ 1;

			logo.have[b] = -1;
			logo.req.rom = logo.rom + logo.ofs + (uint32_t)want * logo.fb;
			logo.req.dst = logo.buf[b];
			logo.req.len = logo.len;
			io_submit(&logo.req);
			logo.load = b;
			logo.loadf = want;
		}
	}
	d2_pic(logo.sheet[logo.show], 0, 0, logo.w, logo.h, 0, 0, 255, 255, 255, 255, D2_NORMAL);
}

/* ---- screens ------------------------------------------------------------------ */

enum { SCR_MAIN, SCR_NEWGAME, SCR_CONFIG, SCR_SAVE, SCR_LOAD, SCR_QUIT, SCR_N };
static const char *const scr_name[SCR_N] = {
	"main", "newgame", "config", "save", "load", "quit"
};
static const char *const scr_title[SCR_N] = {
	NULL, "New Game", "Configuration", "Save Game", "Load Game", "Quit"
};

enum { A_SCREEN, A_RESUME, A_START, A_HAZARD, A_SLOT, A_PAK, A_QUIT, A_BACK, A_SET };
enum { SR_BRIGHT, SR_VOLUME, SR_SENS, SR_INVERT, SR_MUSIC };

typedef struct {
	const char	*label, *desc;
	uint8_t		act, arg;
} item_t;

#define MAX_ITEMS	(SAVE_SLOTS_MAX + 2)	/* Load Game: every slot, the Pak row, Cancel */
#define ROWS_VIS	5			/* buttons shown at once (longer lists scroll) */

enum { BUSY_NONE, BUSY_START, BUSY_SAVE, BUSY_LOAD };

static struct {
	int		on;		/* menu up */
	int		title;		/* playable: a title screen exists */
	int		scr;
	int		sel[SCR_N];
	int		top[SCR_N];	/* first item shown */
	int		busy;		/* BUSY_* */
	int		start_map;
	int		busy_frames;
	uint8_t		dirty;		/* settings changed on the Configuration screen */
	const char	*note;		/* replaces the description until note_end */
	uint64_t	note_end;
	uint64_t	t_us;		/* menu clock (the banner, notes) */
	save_info_t	slot[SAVE_SLOTS_MAX];
	int		nslots;
	int		pak;		/* save_pak_status() at the last listing */
	int		val_w;		/* Configuration value bar width (menu_init) */
	res_t		snd[6];		/* S_* sounds, resolved on first use */
} M;

enum { S_GLOW, S_SELECT, S_UP, S_DOWN, S_DENY, S_SLIDER };

static void sound(int s)
{
	static const int line[6] = { MR_UI_GLOW, MR_UI_SELECT, MR_UI_UPMENU, MR_UI_DNMENU,
		MR_UI_DENY, MR_UI_SLIDER };

	if (!M.snd[s])
		M.snd[s] = res_def(line[s]);
	if (M.snd[s])
		snd_ui(M.snd[s], 1.0f);
}

/* the notes that replace the description line for a while */
enum { N_LOADING, N_NOSAVE, N_SAVING, N_SAVED, N_SAVEFAIL, N_LOADFAIL, N_PAKBIG, N_TOOBIG, N_COUNT };
static const char *const note_text[N_COUNT] = {
	"Loading...", "The game cannot be saved now.", "Saving...", "Game saved.",
	"The save failed.", "The game could not be loaded.",
	"Save too large for Controller Pak.", "Save too large."
};

/* a save / load slot's description: SD_* */
enum { SD_SAVE_EMPTY, SD_SAVE_OVER, SD_SAVE_NOTE, SD_LOAD_OK, SD_LOAD_AUTO, SD_LOAD_BAD,
	SD_LOAD_EMPTY, SD_LOAD_NOAUTO, SD_COUNT };
static const char *const slot_desc[SD_COUNT] = {
	"Save the game here.", "Save the game here, over this save.",
	"Save to a new note on the Controller Pak.", "Load this saved game.",
	"Load the last automatic save.", "This save is damaged.", "Empty slot.",
	"No autosave yet."
};

/* the Controller Pak row (save_pak_status other than SAVE_PAK_OK), by SAVE_PAK_* */
#define PAK_N	(SAVE_PAK_DAMAGED + 1)
static const char *const pak_value[PAK_N] = { "", "None", "Full", "Rumble Pak", "Damaged" };
static const char *const pak_desc[PAK_N] = {
	"",
	"Insert a Controller Pak for more slots.",
	"The Controller Pak has no room for a save.",
	"Swap the Rumble Pak for a Controller Pak.",
	"Controller Pak damaged or not formatted."
};

static void note(int n, uint32_t us)
{
	M.note = note_text[n];
	M.note_end = M.t_us + us;
}

static int in_game(void)
{
	return phy_player_active();
}

static const char *const flash_label[SAVE_FLASH_MANUAL] = { "Slot 1", "Slot 2" };
static const char *const pak_label[SAVE_PAK_SLOTS] = { "Pak 1", "Pak 2", "Pak 3", "Pak 4" };

/* slot i's button label: Autosave, Slot N (Flash), Pak N (Controller Pak) */
static const char *slot_name(int i)
{
	int k, nth = 0;

	if (M.slot[i].flags & SAVE_F_AUTO)
		return "Autosave";
	for (k = 0; k < i; k++)
		if (M.slot[k].source == M.slot[i].source && !(M.slot[k].flags & SAVE_F_AUTO))
			nth++;
	if (M.slot[i].source == SAVE_SRC_PAK)
		return nth < SAVE_PAK_SLOTS ? pak_label[nth] : "Pak";
	return nth < SAVE_FLASH_MANUAL ? flash_label[nth] : "Slot";
}

static int slot_desc_of(int scr, int i)
{
	const save_info_t *s = &M.slot[i];

	if (scr == SCR_SAVE)
		return s->state == SAVE_EMPTY ? (s->source == SAVE_SRC_PAK ? SD_SAVE_NOTE : SD_SAVE_EMPTY)
			: SD_SAVE_OVER;
	if (s->state == SAVE_OK)
		return (s->flags & SAVE_F_AUTO) ? SD_LOAD_AUTO : SD_LOAD_OK;
	if (s->state == SAVE_CORRUPT)
		return SD_LOAD_BAD;
	return (s->flags & SAVE_F_AUTO) ? SD_LOAD_NOAUTO : SD_LOAD_EMPTY;
}

/* screen scr's items (ingame: the pause menu's, else the title's) */
static int items(int scr, int ingame, item_t *it)
{
	int n = 0, i;

	switch (scr)
	{
	case SCR_MAIN:
		if (ingame)
		{
			it[n++] = (item_t){ "Resume Game", "Return to the game.", A_RESUME, 0 };
			it[n++] = (item_t){ "Save Game", "Save the game in progress.", A_SCREEN, SCR_SAVE };
		}
		else
		{
			it[n++] = (item_t){ "New Game", "Start a new game from the beginning.",
				A_SCREEN, SCR_NEWGAME };
			if (wld_training_map() >= 0)
				it[n++] = (item_t){ "Hazard Course",
					"Learn to play in the Hazard Course.", A_HAZARD, 0 };
		}
		it[n++] = (item_t){ "Load Game", "Continue from a saved game.", A_SCREEN, SCR_LOAD };
		it[n++] = (item_t){ "Configuration", "Brightness, volumes and look controls.",
			A_SCREEN, SCR_CONFIG };
		if (ingame)
			it[n++] = (item_t){ "Quit to Title",
				"Leave this game for the title screen.", A_SCREEN, SCR_QUIT };
		break;
	case SCR_NEWGAME:
		it[n++] = (item_t){ "Easy", "Enemies are weaker and do less damage.", A_START, 1 };
		it[n++] = (item_t){ "Medium", "The standard challenge.", A_START, 2 };
		it[n++] = (item_t){ "Difficult", "Enemies are tougher and hit harder.", A_START, 3 };
		it[n++] = (item_t){ "Cancel", "Back to the title screen.", A_BACK, 0 };
		break;
	case SCR_CONFIG:
		it[n++] = (item_t){ "Brightness", "Lifts dark areas. Left / right to change.",
			A_SET, SR_BRIGHT };
		it[n++] = (item_t){ "Sound Volume", "Sound volume. Left / right to change.",
			A_SET, SR_VOLUME };
		it[n++] = (item_t){ "Music Volume", "Music volume. Left / right to change.",
			A_SET, SR_MUSIC };
		it[n++] = (item_t){ "Look Speed", "Stick and C-button turning speed.",
			A_SET, SR_SENS };
		it[n++] = (item_t){ "Invert Look", "C-up looks down, C-down looks up.",
			A_SET, SR_INVERT };
		it[n++] = (item_t){ "Done", "Keep these settings.", A_BACK, 0 };
		break;
	case SCR_SAVE:
	case SCR_LOAD:
		/* Load: every slot (the autosave first); Save: the manual ones */
		for (i = 0; i < M.nslots && n < MAX_ITEMS - 2; i++)
			if (scr == SCR_LOAD || !(M.slot[i].flags & SAVE_F_AUTO))
				it[n++] = (item_t){ slot_name(i), slot_desc[slot_desc_of(scr, i)], A_SLOT,
					(uint8_t)i };
		if (M.pak != SAVE_PAK_OK && M.pak > 0 && M.pak < PAK_N)
			it[n++] = (item_t){ "Controller Pak", pak_desc[M.pak], A_PAK, 0 };
		it[n++] = (item_t){ "Cancel", "Go back.", A_BACK, 0 };
		break;
	case SCR_QUIT:
		it[n++] = (item_t){ "Quit to Title", "Progress since the last save is lost.",
			A_QUIT, 0 };
		it[n++] = (item_t){ "Cancel", "Return to the menu.", A_BACK, 0 };
		break;
	}
	return n;
}

/* the slot list and the Pak's state (save.h: list again whenever save_busy
 * dropped or the Pak may have changed: every frame of these screens) */
static void slots_refresh(void)
{
	M.nslots = save_slots(M.slot, SAVE_SLOTS_MAX);
	if (M.nslots < 0)
		M.nslots = 0;
	M.pak = save_pak_status();
}

static void open_screen(int scr)
{
	M.scr = scr;
	if (scr == SCR_SAVE || scr == SCR_LOAD)
		slots_refresh();
	if (scr == SCR_CONFIG)
		M.dirty = 0;
	log_printf("menu: screen %s\n", scr_name[scr]);
}

/* leave the current screen for the main one (Configuration stores) */
static void back(void)
{
	if (M.scr == SCR_CONFIG && M.dirty)
	{
		int r = settings_store(&settings);

		log_printf("menu: settings store %s\n", r == 0 ? "started" : "failed");
		M.dirty = 0;
	}
	if (M.scr != SCR_MAIN)
		open_screen(SCR_MAIN);
}

static void close_menu(const char *why)
{
	back();
	M.on = 0;
	M.busy = BUSY_NONE;
	snd_pause(0);
	in_hold();
	log_printf("menu: %s\n", why);
}

static void set_change(int row, int dir)
{
	int v;

	switch (row)
	{
	case SR_BRIGHT:
		v = settings.brightness + 10 * dir;
		settings_set(SET_BRIGHTNESS, (float)v);
		break;
	case SR_VOLUME:
		v = settings.vol_sfx + 10 * dir;
		settings_set(SET_VOL_SFX, (float)v);
		break;
	case SR_MUSIC:
		v = settings.vol_music + 10 * dir;
		settings_set(SET_VOL_MUSIC, (float)v);
		break;
	case SR_SENS:
		v = settings.look_sens + 25 * dir;
		settings_set(SET_LOOK_SENS, (float)v);
		break;
	case SR_INVERT:
		settings.invert_y = (uint8_t)!settings.invert_y;
		break;
	}
	M.dirty = 1;
	sound(S_SLIDER);
	log_printf("menu: set brightness=%u volume=%u look=%u invert=%u music=%u\n",
		settings.brightness, settings.vol_sfx, settings.look_sens, settings.invert_y,
		settings.vol_music);
}

static void start(int map, const char *what)
{
	if (map < 0)
	{
		sound(S_DENY);
		return;
	}
	M.busy = BUSY_START;
	M.start_map = map;
	M.busy_frames = 0;
	note(N_LOADING, 60000000u);
	log_printf("menu: %s on %s, skill %u\n", what, wld_map_name(map), settings.skill);
}

static void activate(const item_t *it)
{
	switch (it->act)
	{
	case A_SCREEN:
		sound(S_UP);
		open_screen(it->arg);
		if (it->arg == SCR_QUIT)
			M.sel[SCR_QUIT] = 1;
		break;
	case A_RESUME:
		sound(S_SELECT);
		close_menu("resume");
		break;
	case A_START:
		sound(S_SELECT);
		settings.skill = it->arg;
		start(wld_start_map(), "new game");
		break;
	case A_HAZARD:
		sound(S_SELECT);
		start(wld_training_map(), "hazard course");
		break;
	case A_SLOT:
		if (M.scr == SCR_SAVE)
		{
			if (save_write(it->arg) != 0)
			{
				sound(S_DENY);
				note(N_NOSAVE, 3000000u);
				break;
			}
			sound(S_SELECT);
			M.busy = BUSY_SAVE;
			note(N_SAVING, 60000000u);
			log_printf("menu: save slot %u\n", it->arg);
			break;
		}
		if (M.slot[it->arg].state != SAVE_OK || save_load(it->arg) != 0)
		{
			sound(S_DENY);
			break;
		}
		sound(S_SELECT);
		M.busy = BUSY_LOAD;
		note(N_LOADING, 60000000u);
		log_printf("menu: load slot %u\n", it->arg);
		break;
	case A_QUIT:
		sound(S_SELECT);
		eng_to_title();
		M.sel[SCR_MAIN] = 0;
		open_screen(SCR_MAIN);
		log_printf("menu: quit to title\n");
		break;
	case A_PAK:
		sound(S_DENY);
		break;
	case A_BACK:
		sound(S_DOWN);
		back();
		break;
	case A_SET:
		set_change(it->arg, 1);
		break;
	}
}

/* a running save / load / start: 1 while the menu waits for it */
static int busy_step(void)
{
	switch (M.busy)
	{
	case BUSY_START:
		/* one frame of "Loading..." on screen before the blocking load */
		if (M.busy_frames++ < 1 || !eng_can_start())
			return 1;
		M.busy = BUSY_NONE;
		eng_start_game(M.start_map);
		M.note_end = 0;
		close_menu("play");
		return 1;
	case BUSY_SAVE:
		if (save_busy())
			return 1;
		M.busy = BUSY_NONE;
		slots_refresh();
		note(save_result() == 0 ? N_SAVED : save_result() == SAVE_ERR_PAK_SIZE ? N_PAKBIG
			: save_result() == SAVE_ERR_SIZE ? N_TOOBIG : N_SAVEFAIL, 3000000u);
		log_printf("menu: save %s\n", save_result() == 0 ? "done" : "failed");
		if (save_result() == 0)
			open_screen(SCR_MAIN);
		return 1;
	case BUSY_LOAD:
		if (save_busy())
			return 1;
		M.busy = BUSY_NONE;
		if (save_result() == 0 && in_game())
		{
			M.note_end = 0;
			log_printf("menu: load done\n");
			close_menu("play");
			return 1;
		}
		note(N_LOADFAIL, 3000000u);
		log_printf("menu: load failed\n");
		return 1;
	}
	return 0;
}

int menu_frame(uint32_t dt_us)
{
	uint16_t k = in_keys(dt_us);
	item_t it[MAX_ITEMS];
	int n, *sel;

	M.t_us += dt_us;
	if (!M.on)
	{
		if (in_game() && (k & IN_KEY_START))
		{
			M.on = 1;
			M.sel[SCR_MAIN] = 0;
			M.note_end = 0;
			open_screen(SCR_MAIN);
			snd_pause(1);
			log_printf("menu: pause\n");
			return MENU_PAUSE;
		}
		if (in_game() || !M.title)
			return MENU_PLAY;
		/* no game and a title: the title screen */
		M.on = 1;
		open_screen(SCR_MAIN);
	}
	if (busy_step())
		return M.on ? (in_game() ? MENU_PAUSE : MENU_TITLE) : MENU_PLAY;

	if (M.scr == SCR_SAVE || M.scr == SCR_LOAD)
		slots_refresh();
	n = items(M.scr, in_game(), it);
	sel = &M.sel[M.scr];
	if (*sel >= n)
		*sel = n - 1;
	if (*sel < 0)
		*sel = 0;
	if (k & (IN_KEY_UP | IN_KEY_DOWN))
	{
		*sel = (*sel + ((k & IN_KEY_DOWN) ? 1 : n - 1)) % n;
		M.note_end = 0;
		sound(S_GLOW);
		log_printf("menu: select %s %s\n", scr_name[M.scr], it[*sel].label);
	}
	else if ((k & (IN_KEY_LEFT | IN_KEY_RIGHT)) && it[*sel].act == A_SET)
		set_change(it[*sel].arg, (k & IN_KEY_RIGHT) ? 1 : -1);
	else if (k & IN_KEY_START)
	{
		if (in_game())
		{
			sound(S_SELECT);
			close_menu("resume");
		}
		else
			activate(&it[*sel]);
	}
	else if (k & IN_KEY_A)
		activate(&it[*sel]);
	else if (k & IN_KEY_B)
	{
		if (M.scr != SCR_MAIN)
		{
			sound(S_DOWN);
			back();
		}
		else if (in_game())
		{
			sound(S_DOWN);
			close_menu("resume");
		}
	}
	if (!M.on)
		return MENU_PLAY;
	return in_game() ? MENU_PAUSE : MENU_TITLE;
}

/* ---- drawing ------------------------------------------------------------------- */

/* Layout in framebuffer pixels (320 x 240). The VI scans out every row and
 * column of it inside libdragon's CRT-safe area (plat/n64/main.c
 * VI_CRT_MARGIN: the picture is scaled into the screen less a 5% border,
 * not cropped), so on screen means inside the framebuffer; layout_check
 * proves every string fits at boot. */
#define SCR_W		320
#define SCR_H		240
#define COL_X		16
#define RIGHT_X		(SCR_W - COL_X)	/* right edge of values and the description */
#define HEAD_Y		54
#define BTN_Y		78
#define BTN_W		136
#define BTN_H		21
#define BTN_PITCH	24
#define VAL_X		166
#define VAL_GAP		6		/* bar to its value */
#define VAL_WIDEST	"200%"		/* the widest value text (Look Speed) */
#define DESC_Y		214

static void frame_box(int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b)
{
	d2_fill(x, y, w, 1, r, g, b, 255, D2_NORMAL);
	d2_fill(x, y + h - 1, w, 1, r, g, b, 255, D2_NORMAL);
	d2_fill(x, y + 1, 1, h - 2, r, g, b, 255, D2_NORMAL);
	d2_fill(x + w - 1, y + 1, 1, h - 2, r, g, b, 255, D2_NORMAL);
}

static void button(int y, const char *label, int hi, int dim)
{
	int th = d2_text_height();

	if (hi)
	{
		d2_fill(COL_X + 1, y + 1, BTN_W - 2, BTN_H - 2, 72, 44, 8, 255, D2_NORMAL);
		frame_box(COL_X, y, BTN_W, BTN_H, 255, 176, 48);
	}
	else
	{
		d2_fill(COL_X + 1, y + 1, BTN_W - 2, BTN_H - 2, 18, 16, 12, 255, D2_NORMAL);
		frame_box(COL_X, y, BTN_W, BTN_H, 92, 60, 16);
	}
	if (dim)
		d2_text(COL_X + 8, y + (BTN_H - th) / 2, label, 96, 88, 72, 255);
	else if (hi)
		d2_text(COL_X + 8, y + (BTN_H - th) / 2, label, 255, 210, 120, 255);
	else
		d2_text(COL_X + 8, y + (BTN_H - th) / 2, label, 220, 128, 24, 255);
}

/* value bar of a Configuration row: v of [lo, hi]; the text right-aligned
 * at RIGHT_X, the bar up to VAL_GAP before the widest value (M.val_w) */
static void bar(int y, int v, int lo, int hi, const char *text)
{
	int fill = (v - lo) * (M.val_w - 4) / (hi - lo);

	frame_box(VAL_X, y + 5, M.val_w, 11, 92, 60, 16);
	if (fill > 0)
		d2_fill(VAL_X + 2, y + 7, fill, 7, 220, 128, 24, 255, D2_NORMAL);
	d2_text(RIGHT_X - d2_text_width(text), y + (BTN_H - d2_text_height()) / 2, text,
		200, 190, 170, 255);
}

/* Brightness preview: eight grey steps as the 3D passes draw them at this
 * setting (cooked value + lift, settings.h SETTINGS_BRIGHT_LIFT) */
static void bright_ramp(int y)
{
	int i, lift = SETTINGS_BRIGHT_LIFT(settings.brightness);

	for (i = 0; i < 8; i++)
	{
		int c = i * 255 / 7 * 3 / 8 + lift;

		if (c > 255)
			c = 255;
		d2_fill(VAL_X + i * 15, y, 15, 12, (uint8_t)c, (uint8_t)c, (uint8_t)c, 255, D2_NORMAL);
	}
}

static void row_value(const item_t *it, int y, int hi)
{
	char t[40];

	if (it->act == A_SET)
	{
		switch (it->arg)
		{
		case SR_BRIGHT:
			snprintf(t, sizeof(t), "%u", settings.brightness);
			bar(y, settings.brightness, 0, 100, t);
			if (hi)
				bright_ramp(DESC_Y - 18);
			break;
		case SR_VOLUME:
			snprintf(t, sizeof(t), "%u", settings.vol_sfx);
			bar(y, settings.vol_sfx, 0, 100, t);
			break;
		case SR_MUSIC:
			snprintf(t, sizeof(t), "%u", settings.vol_music);
			bar(y, settings.vol_music, 0, 100, t);
			break;
		case SR_SENS:
			snprintf(t, sizeof(t), "%u%%", settings.look_sens);
			bar(y, settings.look_sens, 25, 200, t);
			break;
		case SR_INVERT:
			d2_text(VAL_X, y + (BTN_H - d2_text_height()) / 2, settings.invert_y ? "On" : "Off",
				200, 190, 170, 255);
			break;
		}
	}
	else if (it->act == A_SLOT)
	{
		const save_info_t *s = &M.slot[it->arg];
		uint32_t sec = s->game_time > 0 ? (uint32_t)s->game_time : 0;

		if (s->state == SAVE_OK)
		{
			int k;

			/* the title shortened until the line ends by RIGHT_X */
			for (k = 12; k >= 0; k--)
			{
				snprintf(t, sizeof(t), "%.*s %lu:%02lu", k, s->title[0] ? s->title : s->map,
					(unsigned long)(sec / 60), (unsigned long)(sec % 60));
				if (VAL_X + d2_text_width(t) <= RIGHT_X)
					break;
			}
		}
		else
			snprintf(t, sizeof(t), "%s", s->state == SAVE_CORRUPT ? "Damaged" : "Empty");
		d2_text(VAL_X, y + (BTN_H - d2_text_height()) / 2, t, 200, 190, 170, 255);
	}
	else if (it->act == A_PAK)
		d2_text(VAL_X, y + (BTN_H - d2_text_height()) / 2, pak_value[M.pak], 96, 88, 72, 255);
}

/* a 7-px scroll marker beside the button column: more items above (up) or
 * below */
static void more_marker(int y, int up)
{
	int r;

	for (r = 0; r < 4; r++)
	{
		int w = up ? 1 + 2 * r : 7 - 2 * r;

		d2_fill(COL_X + BTN_W + 4 + (7 - w) / 2, y + r, w, 1, 220, 128, 24, 255, D2_NORMAL);
	}
}

void menu_draw(void)
{
	item_t it[MAX_ITEMS];
	int n = items(M.scr, in_game(), it), i, sel = M.sel[M.scr], *top = &M.top[M.scr];
	const char *desc;

	if (!M.on)
		return;
	logo_draw(M.t_us);
	if (scr_title[M.scr])
		d2_text(COL_X, HEAD_Y, scr_title[M.scr], 255, 255, 255, 255);
	else if (in_game())
		d2_text(COL_X, HEAD_Y, "Paused", 255, 255, 255, 255);
	/* at most ROWS_VIS buttons: the window follows the highlight */
	if (sel < *top)
		*top = sel;
	if (sel >= *top + ROWS_VIS)
		*top = sel - ROWS_VIS + 1;
	if (*top > n - ROWS_VIS)
		*top = n - ROWS_VIS;
	if (*top < 0)
		*top = 0;
	for (i = *top; i < n && i < *top + ROWS_VIS; i++)
	{
		int y = BTN_Y + (i - *top) * BTN_PITCH;
		int dim = (M.scr == SCR_LOAD && it[i].act == A_SLOT && M.slot[it[i].arg].state != SAVE_OK)
			|| it[i].act == A_PAK;

		button(y, it[i].label, i == sel && M.busy == BUSY_NONE, dim);
		row_value(&it[i], y, i == sel);
	}
	if (*top > 0)
		more_marker(BTN_Y + 2, 1);
	if (*top + ROWS_VIS < n)
		more_marker(BTN_Y + (ROWS_VIS - 1) * BTN_PITCH + BTN_H - 6, 0);
	desc = M.note_end > M.t_us ? M.note : sel < n ? it[sel].desc : "";
	d2_fill(COL_X, DESC_Y - 3, RIGHT_X - COL_X, 1, 92, 60, 16, 255, D2_NORMAL);
	d2_text(COL_X, DESC_Y, desc, 200, 190, 170, 255);
}

/* one string of the layout: logs and counts it when it ends past x1 */
static int fits(const char *s, int x0, int x1)
{
	int w = d2_text_width(s);

	if (x0 + w <= x1)
		return 0;
	log_printf("menu: layout: \"%s\" ends at x %d, past %d\n", s, x0 + w, x1);
	return 1;
}

/* Boot: every label, value, description and note inside the framebuffer
 * (the VI shows all of it, SCR_W x SCR_H above); logs `menu: layout ok` or
 * each string that does not fit. */
static void layout_check(void)
{
	static const char *const values[] = { "100", VAL_WIDEST, "Off", "On", "Damaged", "Empty" };
	item_t it[MAX_ITEMS];
	int bad = 0, scr, g, i, n, th = d2_text_height();

	/* ROWS_VIS buttons (longer lists scroll) end above the Configuration
	 * brightness ramp (DESC_Y - 18) and the description rule below it */
	if (BTN_Y + (ROWS_VIS - 1) * BTN_PITCH + BTN_H > DESC_Y - 18)
	{
		log_printf("menu: layout: %d rows run into the description\n", ROWS_VIS);
		bad++;
	}
	for (scr = 0; scr < SCR_N; scr++)
		for (g = 0; g < 2; g++)
		{
			n = items(scr, g, it);
			for (i = 0; i < n; i++)
			{
				bad += fits(it[i].label, COL_X + 8, COL_X + BTN_W - 4);
				bad += fits(it[i].desc, COL_X, RIGHT_X);
			}
			if (scr_title[scr])
				bad += fits(scr_title[scr], COL_X, RIGHT_X);
		}
	bad += fits("Paused", COL_X, RIGHT_X);
	/* the slot screens' rows (no slots listed at boot) */
	bad += fits("Autosave", COL_X + 8, COL_X + BTN_W - 4);
	bad += fits("Controller Pak", COL_X + 8, COL_X + BTN_W - 4);
	for (i = 0; i < SAVE_FLASH_MANUAL; i++)
		bad += fits(flash_label[i], COL_X + 8, COL_X + BTN_W - 4);
	for (i = 0; i < SAVE_PAK_SLOTS; i++)
		bad += fits(pak_label[i], COL_X + 8, COL_X + BTN_W - 4);
	for (i = 1; i < PAK_N; i++)
	{
		bad += fits(pak_desc[i], COL_X, RIGHT_X);
		bad += fits(pak_value[i], VAL_X, RIGHT_X);
	}
	for (i = 0; i < SD_COUNT; i++)
		bad += fits(slot_desc[i], COL_X, RIGHT_X);
	for (i = 0; i < N_COUNT; i++)
		bad += fits(note_text[i], COL_X, RIGHT_X);
	for (i = 0; i < (int)(sizeof(values) / sizeof(values[0])); i++)
		bad += fits(values[i], VAL_X, RIGHT_X);
	if (M.val_w < 32)
	{
		log_printf("menu: layout: value bar %d px\n", M.val_w);
		bad++;
	}
	if (DESC_Y + th > SCR_H)
	{
		log_printf("menu: layout: description rows %d..%d past %d\n", DESC_Y, DESC_Y + th - 1,
			SCR_H - 1);
		bad++;
	}
	if (bad)
		log_printf("menu: layout: %d strings / rows do not fit\n", bad);
	else
		log_printf("menu: layout ok (description rows %d..%d of %d, value bar %d px)\n", DESC_Y,
			DESC_Y + th - 1, SCR_H, M.val_w);
}

void menu_to_title(void)
{
	M.on = M.title;
	M.busy = BUSY_NONE;
	M.sel[SCR_MAIN] = 0;
	M.scr = SCR_MAIN;
	snd_pause(0);
	log_printf("menu: title\n");
}

void menu_init(int title)
{
	memset(&M, 0, sizeof(M));
	M.title = title;
	M.val_w = RIGHT_X - VAL_GAP - d2_text_width(VAL_WIDEST) - VAL_X;
	layout_check();
	logo_init();
	if (title && settings_load(&settings) == 0)
		log_printf("menu: settings loaded: brightness=%u volume=%u look=%u invert=%u music=%u\n",
			settings.brightness, settings.vol_sfx, settings.look_sens, settings.invert_y,
			settings.vol_music);
	if (title)
		log_printf("menu: title\n");
}
