/* SPDX-License-Identifier: MIT */
/*
 * menu.h -- the front end (docs/architecture.md 13): the title screen,
 * the pause menu (Start in play), New Game, Configuration and the save /
 * load screens, drawn in the style of the 1998 Half-Life menu (black
 * background, the player's animated logo.avi banner, a column of dark
 * buttons with orange labels, a description line) through draw2d.
 *
 * Its own unit (engine/ui) so that it can move into an overlay (7.4); it
 * keeps no code addresses in anything saved.
 */
#ifndef HL64_MENU_H
#define HL64_MENU_H

#include <stdint.h>

/* menu_frame: what the rest of the frame does */
enum {
	MENU_PLAY,	/* the game runs */
	MENU_PAUSE,	/* a game exists and is frozen: draw the menu only */
	MENU_TITLE	/* no game: draw the menu only */
};

/* Boot, after d2_init and snd_init: the logo, the sounds; title = boot to
 * the title screen (playable builds; also loads the stored settings),
 * else the menu stays closed until Start. */
void	menu_init(int title);
/* Step 2, every frame: the pad's menu keys (Start pauses in play), the
 * chosen action; returns MENU_*. */
int	menu_frame(uint32_t dt_us);
/* The frame's 2D (between d2_begin and d2_end) while not MENU_PLAY. */
void	menu_draw(void);
/* The session ended (end of section, playable builds): back to the title. */
void	menu_to_title(void);

#endif
