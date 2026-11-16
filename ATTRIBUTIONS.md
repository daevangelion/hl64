# Attributions

## Included in this repository

| component | where | licence | copyright |
|---|---|---|---|
| hl64 engine, renderer, streaming, tools, tests | everything outside the rows below | MIT ([LICENSE](LICENSE)) | 2026 daevangelion |
| Half-Life game logic, reimplemented in C from the Half Life 1 SDK | `game/` | Half Life 1 SDK licence ([game/LICENSE-HLSDK](game/LICENSE-HLSDK)) | Valve LLC (1996-2002) |
| HLSDK structures and tables used by the cook tools | `tools/hlsdk/` | Half Life 1 SDK licence ([tools/hlsdk/LICENSE-HLSDK](tools/hlsdk/LICENSE-HLSDK)) | Valve LLC (1996-2002) |
| GoldSrc oracle hooks for the HLSDK game (`hlsdk-hooks.patch`, `probe/`) | `test/oracle/` | Half Life 1 SDK licence ([test/oracle/LICENSE-HLSDK](test/oracle/LICENSE-HLSDK)) | Valve LLC (1996-2002) |
| ares patches (test harness device, save files, SD card timing, pad taps) | `test/ares-*.patch` | ISC, as ares | ares team; changes 2026 daevangelion |
| provcheck fingerprint database: hashes of k-grams, comment word n-grams and identifiers of the Quake, Quake 2, Xash3D FWGS and HLSDK sources (no source text) | `tools/provcheck/db/` | MIT | 2026 daevangelion |

## Fetched at build time (not included)

| component | used for | licence |
|---|---|---|
| [libdragon](https://github.com/DragonMinded/libdragon), fork branch [`daevangelion/libdragon` `hl64`](https://github.com/daevangelion/libdragon/tree/hl64) | N64 SDK (`plat/n64/build_sdk.sh`) | Unlicense (public domain) |
| [tiny3d](https://github.com/HailToDodongo/tiny3d) | N64 3D microcode and library | MIT, Copyright (c) 2023 Max Bebök |
| libdragon MIPS toolchain (GCC, binutils, newlib) | cross compiler | GPL / LGPL / BSD-style (their own licences) |
| [ares](https://github.com/ares-emulator/ares), [meeq/ares](https://github.com/meeq/ares) fork with SummerCart64 emulation | test harness emulator (`test/run/ares-build.sh`) | ISC |
| [Half-Life SDK](https://github.com/ValveSoftware/halflife) and a Half-Life dedicated server | GoldSrc oracle fixtures (`test/oracle/run.sh`) | Half Life 1 SDK licence; Steam Subscriber Agreement |

## Game data

Half-Life is a trademark of Valve Corporation. hl64 is not affiliated with
or endorsed by Valve. No Half-Life game data is included; players cook
their own copy, and the cooked content and the ROMs built from it are for
their own use.
