# Controls

The N64 pad (controller port 1) in play. The binding is one table,
`bind[]` in `engine/in/in.c`; this page and that table change together.

| Pad | Action | HLSDK equivalent |
|---|---|---|
| Stick left / right | turn | `+left` / `+right` (absolute, scaled by frame time and `yawspeed`) |
| Stick up / down | walk forward / back (speed follows the stick) | `+forward` / `+back` |
| C-up / C-down | look up / down | `+lookup` / `+lookdown` (`invert_y` flips) |
| C-left / C-right | strafe left / right | `+moveleft` / `+moveright` |
| Z | fire | `+attack` |
| R | alternate fire | `+attack2` |
| A | jump | `+jump` |
| B | use | `+use` |
| L | duck | `+duck` |
| D-down | reload (held) | `+reload` |
| D-up | night vision on / off (the flashlight) | `impulse 100` |
| D-left | previous weapon | `invprev` (hl64 impulse 7) |
| D-right | next weapon | `invnext` (hl64 impulse 6) |
| Start | pause menu (Start again resumes) | Esc (the menu) |

- Held buttons hold their command button for as long as they are down.
- D-up, D-left and D-right send their impulse once per press (the frame
  the button goes down), never while held. Presses in the same frame go
  out one per frame in the table's order; a route / debug `impulse` goes
  first.
- The game handles impulses as HLSDK `ImpulseCommands`
  (`game/hl/player.c`): they wait while the weapon's attack delay runs,
  and the flashlight needs the HEV suit.
- The flashlight is night vision (user decision D12, architecture.md 19;
  a deliberate departure from GoldSrc): D-up toggles it like Half-Life's
  flashlight (same battery drain and recharge, HUD icon and sound), but
  instead of a light beam the view turns green Opposing Force-style
  goggle vision, fading to black with distance (docs/specs/dlights.md 2).
- Weapon order for previous / next is the HUD's: bucket, then position,
  usable weapons only, wrapping.
- There is no walk (`+speed`) button: walking speed comes from the stick.
- The look speed setting scales stick turning and C-up / C-down looking.
- Mounted guns (`func_tank` with a `func_tankcontrols`, hldemo2's
  biggun): B on the controls takes the gun, the stick and C-up / C-down
  aim it (it follows the view), Z fires. Walking 30 u away lets it go;
  as in HLSDK, B does not, and while on the gun the weapon, its HUD and
  the D-pad impulses are off (`game/hl/tanks.c`, `player.c` PostThink).

## Menus

Playable ROMs boot to the title menu (engine/ui/menu.c); Start in play
opens the pause menu, which freezes the game and pauses its sounds.

| Pad | Menu |
|---|---|
| D-pad / stick up, down | move the highlight (held: repeats) |
| D-pad / stick left, right | change a Configuration value |
| A | select |
| B | back (pause menu: resume) |
| Start | resume (title: select) |

Buttons still held when the menu closes do nothing until released (A on
Resume Game does not jump). Lists longer than five buttons (Load Game:
the autosave, the Flash slots, the Controller Pak's) scroll with the
highlight; small arrows beside the buttons show more above or below.

Before the player exists (no map, or the free camera of a world-only
boot) the pad drives the debug camera instead (`engine/render/cam.c`).

## Tests

- Host: `make BUILD_DIR=… intest && …/host/hl64-in` (`test/m5/in_test.c`,
  also a suite of `test/run/m5.sh`): every held binding, one impulse per
  press with no repeat while held, same-frame presses queued, route
  impulse first, the menu keys (auto-repeat, held-on-close), look speed.
- Host: `test/m6/menu_host.sh` (a suite of `test/run/m5.sh`): the menu
  driven by `pad` commands on `hl64-host --title` (architecture.md 13).
- ares (by hand): `BUILD_DIR=… [HL64_ARES_HOST=ares-host] test/m5/controls_ares.sh` runs
  `test/m5/controls.route` on an HL64_TEST ROM and presses the pad through
  the patched ares' `ARES_TEST_INPUT` file; it checks the cart logs
  `flashlight: on` / `off`, next / previous weapon deploys, `menu: pause`
  / `menu: resume` (Start) and `weapon: reload` in order.
- ares (by hand): `[HL64_ARES_HOST=ares-host] test/run/menu.sh [ROM]` walks
  a playable ROM's menus with the pad (architecture.md 13).
