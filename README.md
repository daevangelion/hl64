# hl64

A single-player Nintendo 64 engine for Half-Life content: one continuous,
streamed world with seamless level changes, cooked from the player's own
Half-Life data. The engine, renderer, streaming and tools are new code; the
game logic in `game/` is a C reimplementation of the Half Life 1 SDK game.
It currently runs the Uplink maps (`hldemo1`–`hldemo3`) and the Hazard
Course (`t0a0*`); the goal is the whole campaign.

- Design: [docs/architecture.md](docs/architecture.md); behaviour specs:
  [docs/specs/](docs/specs/README.md); cooked formats:
  [docs/formats/](docs/formats/).
- Clean-room rules: [docs/PROVENANCE.md](docs/PROVENANCE.md).
- Tests: [test/README.md](test/README.md).

No Half-Life game data is in this repository, and none is built into
anything you can download from it. You need your own copy of Half-Life.

## Licence

Everything outside `game/` is MIT ([LICENSE](LICENSE)). `game/` (and the
HLSDK-derived files in `tools/hlsdk/` and `test/oracle/`) is under the
Half Life 1 SDK licence ([game/LICENSE-HLSDK](game/LICENSE-HLSDK)): free
distribution only. See [NOTICE.md](NOTICE.md) and
[ATTRIBUTIONS.md](ATTRIBUTIONS.md).

## Requirements

- Linux on x86-64, bash, GNU make, git, curl.
- A host C compiler with 32-bit support (`gcc` plus `gcc-multilib` on
  Debian/Ubuntu, `lib32-glibc` on Arch) for the host build. The host
  build is `-m32` by default (`HOST_M32=1`) so its memory layout matches
  the cart's.
- Python 3 with `numpy` (the cook) and `Pillow` (some test scripts).
- The libdragon MIPS toolchain (GCC 16.2 is what hl64 is built with).
- Half-Life: the `valve` directory of the Steam release. Its `maps/` has
  `hldemo1`–`hldemo3` and `t0a0*`.

Paths below are examples; each one is a variable you can point anywhere.

## 1. Toolchain

Download DragonMinded's prebuilt toolchain and unpack it (or install the
package, which puts it in `/opt/libdragon`, and pass
`N64_GCCPREFIX=/opt/libdragon` to the steps below):

```sh
curl -LO https://github.com/DragonMinded/libdragon/releases/download/toolchain-continuous-prerelease/gcc-toolchain-mips64-amd64.deb
dpkg-deb -x gcc-toolchain-mips64-amd64.deb /tmp/n64-toolchain
mkdir -p ~/sdks && mv /tmp/n64-toolchain/opt/libdragon ~/sdks/mips64-toolchain
~/sdks/mips64-toolchain/bin/mips64-elf-gcc --version
```

(No `dpkg-deb`? `ar x` the `.deb` and untar `data.tar.*`.)

## 2. N64 SDK: libdragon fork + tiny3d

hl64 needs libdragon's `preview` branch plus a few patches (a banked RDRAM
allocator for the framebuffers, a smaller C++ debug runtime, a PI DMA
dispatch fix). They live on branch
[`hl64` of daevangelion/libdragon](https://github.com/daevangelion/libdragon/tree/hl64).
The `Makefile` pins the exact commits (`LIBDRAGON_REV`, `TINY3D_REV`);
`plat/n64/build_sdk.sh` builds both into one prefix named after the
libdragon commit.

```sh
git clone https://github.com/daevangelion/hl64.git
git clone -b hl64 https://github.com/daevangelion/libdragon.git ~/src/libdragon-hl64
git clone https://github.com/HailToDodongo/tiny3d.git ~/sdks/tiny3d

cd hl64
plat/n64/build_sdk.sh      # -> ~/sdks/n64-sdk-hl64-<rev12>
make sdk-check
```

`build_sdk.sh` reads `LIBDRAGON_GIT` (default `~/src/libdragon-hl64`),
`TINY3D_GIT` (default `~/sdks/tiny3d`), `N64_GCCPREFIX` (default
`~/sdks/mips64-toolchain`) and `PREFIX` (default
`~/sdks/n64-sdk-hl64-<rev12>`). If you change `PREFIX` or the toolchain
location, pass the same `N64_INST=<prefix>` / `N64_GCCPREFIX=<dir>` to
every `make` below.

## 3. Cook your Half-Life data

The cook converts maps, textures, models, sprites, sounds and music from
your `valve` directory into hl64's formats (it uses the SDK's
`audioconv64`, so build the SDK first):

```sh
make cook HL64_DATA=/path/to/Half-Life/valve HL64_CONTENT=~/hl64-content \
          MAPS='hldemo* t0a0*' BUILD_DIR=~/hl64-build
```

`HL64_CONTENT` (about 35 MB for these maps) is your own derived copy of
Valve's data: keep it private.

## 4. Build the ROM

```sh
make rom BUILD_DIR=~/hl64-build HL64_CONTENT=~/hl64-content -j$(nproc)
```

The ROM is `~/hl64-build/hl64.z64` (it carries the cooked content, so it is
private too). `BUILD_DIR` defaults to `build/` in the tree; nothing else is
written into the source tree. Variants: `PROFILE=1` (frame profiler),
`HL64_TEST=1` (test-harness ROM for the scripts below).

## 5. Host build and tests

```sh
make host BUILD_DIR=~/hl64-build      # ~/hl64-build/host/hl64-host
make test-host BUILD_DIR=~/hl64-build # unit tests + the M0 boot gate
make provcheck                        # clean-room gate (docs/PROVENANCE.md)
```

The milestone gates `test/run/m0.sh` .. `m5.sh` run the host build over
whole routes (`test/routes/*.route`): for example
`BUILD_DIR=~/hl64-build HL64_CONTENT=~/hl64-content test/run/m5.sh`.
Their environment variables are listed at the top of each script:
`HL64_CONTENT` (cooked content), `HL64_DATA` (your `valve` directory),
`HL64_FIXTURES` (GoldSrc oracle fixtures made by `test/oracle/run.sh`, which
needs a dedicated server: [test/oracle/README.md](test/oracle/README.md)).
The scripts' default paths are under `/scratch`; set the variables
instead. The gates hard-link the cooked content into `BUILD_DIR`, so keep
`HL64_CONTENT` and `BUILD_DIR` on the same filesystem.

## 6. Emulator (ares) harness

`--ares` runs of the gates and `test/run/scene.sh --ares` need a patched
ares: `test/run/ares-build.sh` fetches
[meeq/ares](https://github.com/meeq/ares) (ares with SummerCart64
emulation) at the commit it pins (`ARES_PIN`), applies `test/ares-*.patch`
(test harness device, ISViewer log to stdout at any ROM size, pad input
and taps, screenshots, FlashRAM / Controller Pak / SD card image files, SD
command timing) and builds it into `~/hl64-ares-sc64` (or the directory
given as its argument; needs cmake, ninja, a C++20 compiler and ares'
build dependencies such as SDL2 and the Vulkan headers). The scripts find
it there (or at `ARES_BIN`) and run it under a private Xvfb.

```sh
test/run/ares-build.sh
BUILD_DIR=~/hl64-build test/run/m0.sh --ares
```

`HL64_ARES_HOST=<ssh host>` runs ares on another machine instead (see
`test/run/ares-remote.sh`).

## SummerCart64

Streaming the whole campaign from an SC64 flashcart's SD card is being
designed in [docs/sc64-stream.md](docs/sc64-stream.md). `test/run/sc64.sh`
builds a small test ROM (`test/sc64/`) that exercises the SC64 interface
(registers, SD reads and writes, libdragon's libcart) on the patched ares
against an SD card image.
