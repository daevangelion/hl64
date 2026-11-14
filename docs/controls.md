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
| D-up | flashlight on / off | `impulse 100` |
| D-left | previous weapon | `invprev` (hl64 impulse 7) |
| D-right | next weapon | `invnext` (hl64 impulse 6) |
| Start | last weapon | `lastinv` (hl64 impulse 8) |

- Held buttons hold their command button for as long as they are down.
- D-up, D-left, D-right and Start send their impulse once per press (the
  frame the button goes down), never while held. Presses in the same
  frame go out one per frame in the table's order; a route / debug
  `impulse` goes first.
- The game handles impulses as HLSDK `ImpulseCommands`
  (`game/hl/player.c`): they wait while the weapon's attack delay runs,
  and the flashlight needs the HEV suit.
- Weapon order for previous / next is the HUD's: bucket, then position,
  usable weapons only, wrapping.
- There is no walk (`+speed`) button: walking speed comes from the stick.
- Start is last weapon until a pause menu exists. A pause that freezes the
  simulation has to hold the clock, the timers, the active set, sound and
  the streaming / seam pumps together (`eng_frame` steps 3-10), which is
  more than a binding change.

Before the player exists (no map, or the free camera of a world-only
boot) the pad drives the debug camera instead (`engine/render/cam.c`).

## Tests

- Host: `make BUILD_DIR=… intest && …/host/hl64-in` (`test/m5/in_test.c`,
  also a suite of `test/run/m5.sh`): every held binding, one impulse per
  press with no repeat while held, same-frame presses queued, route
  impulse first.
- ares (by hand): `BUILD_DIR=… [HL64_ARES_HOST=ares-host] test/m5/controls_ares.sh` runs
  `test/m5/controls.route` on an HL64_TEST ROM and presses the pad through
  the patched ares' `ARES_TEST_INPUT` file; it checks the cart logs
  `flashlight: on` / `off`, next / previous / last weapon deploys and
  `weapon: reload` in order.
