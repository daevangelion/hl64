# SPDX-License-Identifier: MIT
#
# hl64 build (docs/architecture.md 18.1).
#
#   make rom      $(BUILD_DIR)/hl64.z64 (libdragon n64.mk + t3d.mk, banked libdragon)
#   make host     $(BUILD_DIR)/host/hl64-host (HOST_M32=1 default: -m32)
#   make cook     HL64_DATA (the player's .../Half-Life/valve) -> HL64_CONTENT
#                 [MAPS='hldemo* t0a0*'] [HL64_REF_PALETTE=<palette.lmp>], then
#                 restab; without HL64_DATA: restab over HL64_CONTENT only
#   make routes   test/routes/*.route -> $(BUILD_DIR)/routes/*.rbc
#   make test-host / test-ares / provcheck
#
# Variants: PROFILE=1, HL64_TEST=1 (rom). The host build is always a test
# build. BUILD_DIR may be anywhere (scratch on /scratch); nothing is
# written into the source tree.

BUILD_DIR ?= build
N64_INST ?= $(HOME)/sdks/n64-sdk
N64_GCCPREFIX ?= $(HOME)/sdks/mips64-toolchain
# n64sym and friends read both from the environment
export N64_INST N64_GCCPREFIX
T3D_INST ?= $(N64_INST)
HOST_M32 ?= 1
HL64_CONTENT ?=
# Test ROMs: a host-written debug command file (hl64-host --write-cmds),
# packed as dbg.cmd next to the content.
HL64_CMDS ?=
HL64_DATA ?=
HL64_REF_PALETTE ?=
MAPS ?=

# Sources. engine/ and plat/ build with the strict flags below.
ENGINE_SRC := \
	engine/core/dbg.c engine/core/eng.c engine/core/log.c engine/core/settings.c \
	engine/io/io.c \
	engine/mem/carve.c engine/mem/heap.c engine/mem/pool.c \
	engine/render/cam.c engine/render/dlight.c engine/render/world.c \
	engine/world/style.c engine/world/world.c
# engine/world/seam.c: campaign seams, crossings (M4)
ENGINE_SRC += engine/world/seam.c
# engine/coll: traces, contents, hitboxes, collision residency (M2)
ENGINE_SRC += engine/coll/coll.c engine/coll/colbench.c
# engine/ent + engine/phys: entity slots, linked boxes, spatial grid, touches (M2)
ENGINE_SRC += engine/ent/ent.c engine/ent/grid.c engine/ent/grid_bench.c engine/phys/touch.c
# input, player move host, view, route VM, rng (M2)
ENGINE_SRC += engine/core/rng.c engine/in/in.c engine/phys/pmove.c engine/test/route.c \
	engine/view/view.c
# the transcendentals every engine/ and game/ call site uses (host = cart)
ENGINE_SRC += engine/core/hlmath.c
# game/ (HLSDK licence, game/LICENSE-HLSDK) links into the same images:
# pm_shared and its adapter (D2), the player's pre-move think, the view
GAME_SRC := game/pm/pm_shared.c game/pm/pm_math.c game/pm/pm_host.c game/hl/player.c \
	game/hud/view.c
# M3 slices: each slice adds its own sources on its own line only
# (merges stay trivial; local://M3-contract.md).
ENGINE_SRC_ENT := engine/ent/heap.c engine/ent/pass.c engine/ent/priv.c
ENGINE_SRC_PHYS := engine/phys/move.c engine/phys/push.c engine/phys/walk.c
ENGINE_SRC_REC := engine/ent/rec.c engine/ent/rec_hle.c engine/res/res.c engine/res/rtb.c
ENGINE_SRC_STUDIO := engine/render/studio.c
ENGINE_SRC_SND := engine/snd/snd.c
ENGINE_SRC_FX := engine/render/fx.c engine/render/tent.c engine/render/part.c \
	engine/render/beam.c engine/render/decal.c engine/render/sprite.c engine/render/trans.c
ENGINE_SRC_D2 := engine/render/draw2d.c
GAME_SRC_CORE := game/hl/util.c game/hl/dispatch.c game/hl/world.c game/hl/triggers.c game/hl/logic.c \
	game/hl/doors.c game/hl/buttons.c game/hl/lights.c game/hl/ambient.c \
	game/hl/material.c game/hl/env.c game/hl/bmodels.c game/hl/plats.c \
	game/hl/breakable.c
GAME_SRC_MON := game/hl/monsters.c game/hl/schedule.c game/hl/defaultai.c game/hl/aicombat.c \
	game/hl/aisound.c game/hl/nodes.c game/hl/scripted.c game/hl/sentences.c \
	game/hl/talkmonster.c game/hl/scientist.c game/hl/monstermaker.c \
	game/hl/barney.c game/hl/generic.c game/hl/headcrab.c \
	game/hl/aisquad.c game/hl/houndeye.c game/hl/barnacle.c \
	game/hl/roach.c game/hl/grunt.c game/hl/garg.c \
	game/hl/islave.c game/hl/rat.c game/hl/turret.c
GAME_SRC_ITEMS := game/hl/weapons.c game/hl/weapon_crowbar.c game/hl/weapon_glock.c \
	game/hl/weapon_python.c game/hl/weapon_mp5.c game/hl/weapon_shotgun.c \
	game/hl/weapon_handgrenade.c game/hl/items.c game/hl/chargers.c game/hl/grenade.c \
	game/hl/tripmine.c game/hl/weapon_tripmine.c game/hud/hud.c
GAME_SRC_STUDIO := game/studio/pose.c game/studio/anim.c game/studio/event.c
# M4 GameLS: level state (dehydrate / rehydrate, twins, carry, cut refs)
GAME_SRC_LS := game/hl/levelstate.c
# M4 Stream: level-load model precache at HLE map open (game_map_opened)
GAME_SRC_CORE += game/hl/precache.c
ENGINE_SRC += $(ENGINE_SRC_ENT) $(ENGINE_SRC_PHYS) $(ENGINE_SRC_REC) $(ENGINE_SRC_STUDIO) \
	$(ENGINE_SRC_SND) $(ENGINE_SRC_FX) $(ENGINE_SRC_D2)
# M4 Gate: the seam-window profiler (engine/test/seamprof.c: PROFILE carts,
# every host build; empty otherwise)
ENGINE_SRC += engine/test/seamprof.c
GAME_SRC += $(GAME_SRC_CORE) $(GAME_SRC_MON) $(GAME_SRC_ITEMS) $(GAME_SRC_STUDIO) $(GAME_SRC_LS)
# M4 Stream: residency classes, target set, pins, prefetch, pop-in
ENGINE_SRC += engine/world/stream.c
# HLSDK sources kept as Valve wrote them: their unused locals, MSVC
# pragmas and array-parameter styles are not ours to fix; HLSDK builds
# them without strict aliasing (mathlib.h IS_NAN)
HLSDK_CFLAGS := -fno-strict-aliasing -Wno-unused-variable -Wno-unused-but-set-variable \
	-Wno-unknown-pragmas -Wno-array-parameter
ENGINE_SRC += $(GAME_SRC)
N64_SRC := plat/n64/cart.c plat/n64/fpu_shim.c plat/n64/gpu.c plat/n64/gpu_studio.c plat/n64/main.c \
	plat/n64/sys.c \
	plat/n64/vid.c
HOST_SRC := plat/host/gpu.c plat/host/gpu_studio.c plat/host/heap_host.c plat/host/io_host.c plat/host/main.c \
	plat/host/sys.c
# the Snd slice's audio backends (engine/snd/snd_hw.h)
N64_SRC += plat/n64/audio.c
HOST_SRC += plat/host/audio_host.c
# the FX slice's effects GPU seam (engine/render/gpu_fx.h)
N64_SRC += plat/n64/gpu_fx.c
HOST_SRC += plat/host/gpu_fx.c
N64_SRC += plat/n64/gpu_2d.c
HOST_SRC += plat/host/gpu_2d.c

# Directories built without -ffast-math on the cart (18.2): host and cart
# must agree bit for bit on collision and physics, on the world draw list
# (frustum, point -> leaf) the cook predicts, and on everything game logic
# decides (entities, records, AI, studio poses: a route must take the same
# path on both). engine/core (the random stream, hlmath, the frame clock),
# engine/in (the usercmd), engine/view (the drawn eye: residency input,
# flashlight) and engine/test (route conditions) feed the simulation too.
NOFASTMATH_DIRS := engine/coll engine/phys engine/render engine/world engine/ent engine/res \
	engine/snd engine/core engine/in engine/view engine/test game/pm game/hl game/studio game/hud

HL64_WARN := -Wall -Wextra -Werror
HL64_DEFS :=
ifdef PROFILE
HL64_DEFS += -DHL64_PROFILE=1
endif
ifdef HL64_TEST
HL64_DEFS += -DHL64_TEST=1
endif

ROM := $(BUILD_DIR)/hl64.z64
ELF := $(BUILD_DIR)/hl64.elf
HOST_BIN := $(BUILD_DIR)/host/hl64-host

.PHONY: all rom elf host unit colltest studiotest area pmtest enttest fxtest gametest montest itemstest rectest rec4test sndtest gamelstest seamproftest mathtest seamtest streamtest cook routes test-host test-ares provcheck sdk-check clean
all: rom

# ---- cart -------------------------------------------------------------------

ifeq ($(wildcard $(N64_INST)/include/n64.mk),)
rom elf sdk-check:
	@echo "N64 SDK not found at N64_INST=$(N64_INST)" >&2; exit 2
else
include $(N64_INST)/include/n64.mk
include $(T3D_INST)/include/t3d.mk
SOURCE_DIR := .

# Banked libdragon (FB / Z / stream in separate RDRAM banks): the SDK's
# libdragon rev plus the banked overlay and plat/n64/libdragon-patches,
# built into its own prefix. Its MANIFEST must name the SDK's rev and the
# sha256 of our patches; it replaces both SDK archives.
SDK_REV := $(shell sed -n 's/.*"libdragon_rev": *"\([0-9a-f]*\)".*/\1/p' $(N64_INST)/EMERGENCE_SDK_MANIFEST.json)
LIBDRAGON_BANKED ?= $(HOME)/sdks/libdragon-banked-$(shell printf %.12s "$(SDK_REV)")
BANKED_REV := $(shell sed -n 's/.*"libdragon_rev": *"\([0-9a-f]*\)".*/\1/p' $(LIBDRAGON_BANKED)/MANIFEST.json 2>/dev/null)
BANKED_PATCHES := $(shell sed -n 's/.*"port_patches_sha256": *"\([0-9a-f]*\)".*/\1/p' $(LIBDRAGON_BANKED)/MANIFEST.json 2>/dev/null)
PATCHES_SHA := $(shell cat plat/n64/libdragon-patches/*.patch | sha256sum | cut -d' ' -f1)

sdk-check:
	@if [ -z "$(SDK_REV)" ] || [ "$(BANKED_REV)" != "$(SDK_REV)" ]; then \
		echo "libdragon-banked at $(LIBDRAGON_BANKED) has rev '$(BANKED_REV)', SDK wants '$(SDK_REV)'" >&2; exit 2; fi
	@if [ "$(BANKED_PATCHES)" != "$(PATCHES_SHA)" ]; then \
		echo "libdragon-banked at $(LIBDRAGON_BANKED) lacks plat/n64/libdragon-patches (sha $(PATCHES_SHA))" >&2; exit 2; fi

N64_LDFLAGS := $(patsubst -ldragon,$(LIBDRAGON_BANKED)/lib/libdragon.a,\
	$(patsubst -ldragonsys,$(LIBDRAGON_BANKED)/lib/libdragonsys.a,$(N64_LDFLAGS)))
# libdragon's headers are system headers to us: their own warnings are not
# ours to fix (-isystem wins over n64.mk's -I for the same directory).
N64_CFLAGS += -isystem $(N64_INCLUDEDIR) -isystem $(LIBDRAGON_BANKED)/include -I. -Iinclude
# hl64 rules, after n64.mk's: every warning an error (n64.mk demotes the
# unused-* family; promote it back), no auto-var pattern fill. engine/ is
# C99; plat/n64 keeps n64.mk's -std=gnu17: libdragon's headers #error
# without GNU extensions (pputils.h) and tiny3d's use C11 static_assert.
N64_CFLAGS += -O2 $(HL64_WARN) -ftrivial-auto-var-init=uninitialized \
	-Werror=unused-variable -Werror=unused-but-set-variable -Werror=unused-function \
	-Werror=unused-parameter -Werror=unused-but-set-parameter -Werror=unused-label \
	-Werror=unused-local-typedefs -Werror=unused-const-variable \
	-Werror=deprecated-declarations $(HL64_DEFS)
# engine/mem/heap.c counts every heap block; plat/n64/sys.c reports RSP
# crashes without allocating (only libdragon's crash handler calls these).
N64_LDFLAGS += --wrap=malloc --wrap=free --wrap=calloc --wrap=realloc \
	--wrap=memalign --wrap=malloc_uncached --wrap=malloc_uncached_aligned \
	--wrap=console_init --wrap=console_render
ifdef PROFILE
N64_SRC += plat/n64/iostat.c
N64_LDFLAGS += --wrap=fread --wrap=fseek
endif

ROM_OBJS := $(addprefix $(BUILD_DIR)/,$(ENGINE_SRC:.c=.o) $(N64_SRC:.c=.o))

$(foreach d,$(NOFASTMATH_DIRS),$(eval $(BUILD_DIR)/$(d)/%.o: CFLAGS += -fno-fast-math))
$(BUILD_DIR)/engine/%.o: CFLAGS += -std=c99
$(addprefix $(BUILD_DIR)/,game/pm/pm_shared.o game/pm/pm_math.o): CFLAGS += $(HLSDK_CFLAGS)
# game/hl private blocks are HLSDK class chains as flat C structs used
# through base-class pointers (game/hl/cbase.h)
$(BUILD_DIR)/game/hl/%.o: CFLAGS += -fno-strict-aliasing

# n64.mk sets the toolchain for %.z64 targets (and so for everything $(ROM)
# depends on); `make elf` gets the same so it alone is a compile/link check.
elf: CC=$(N64_CC)
elf: CXX=$(N64_CXX)
elf: AS=$(N64_AS)
elf: LD=$(N64_LD)
elf: CFLAGS+=$(N64_CFLAGS)
elf: ASFLAGS+=$(N64_ASFLAGS)
elf: LDFLAGS+=$(N64_LDFLAGS)
$(ELF): $(ROM_OBJS) | sdk-check

# Player content as the ROM's DFS (HL64_CONTENT; the cook's restab-only
# inputs maps/*.ent stay out), plus HL64_CMDS as dbg.cmd. The tree is
# staged with hard links into BUILD_DIR and re-packed on every ROM build.
DFS := $(if $(HL64_CONTENT),$(BUILD_DIR)/hl64.dfs)
.PHONY: FORCE
$(BUILD_DIR)/hl64.dfs: FORCE
	@echo "    [DFS] $@"
	@rm -rf $(BUILD_DIR)/dfs-root && mkdir -p $(BUILD_DIR)/dfs-root
	cd '$(HL64_CONTENT)' && find . -type f ! -name '*.ent' | cpio -pdml --quiet '$(abspath $(BUILD_DIR))/dfs-root'
	$(if $(HL64_CMDS),cp '$(HL64_CMDS)' $(BUILD_DIR)/dfs-root/dbg.cmd)
	$(N64_MKDFS) $@.tmp $(BUILD_DIR)/dfs-root >/dev/null && mv $@.tmp $@

# Explicit ROM rule (n64.mk's %.z64 writes into the working directory).
$(ROM): N64_ROM_TITLE = "hl64"
$(ROM): $(ELF) $(DFS)
	@echo "    [Z64] $@"
	$(N64_SYM) --all $< $<.sym
	cp $< $<.stripped
	$(N64_STRIP) -s $<.stripped
	$(N64_ELFCOMPRESS) -o $(dir $<) -c $(N64_ROM_ELFCOMPRESS) $<.stripped
	@rm -f $@
	$(N64_TOOL) $(N64_TOOLFLAGS) --output $@.tmp --align 256 $<.stripped $<.sym $(DFS) \
		$(N64_TOOLFILES)
	@mv $@.tmp $@

rom: $(ROM)
elf: $(ELF)
endif

# ---- host -------------------------------------------------------------------

HOST_CC ?= cc
HOST_ARCH := $(if $(filter 1,$(HOST_M32)),-m32,)
HOST_CFLAGS := $(HOST_ARCH) -std=c99 -O2 $(HL64_WARN) -g -MMD \
	-ffp-contract=off -msse2 -mfpmath=sse \
	-D_POSIX_C_SOURCE=200809L -DHL64_HOST=1 -DHL64_TEST=1 \
	$(filter -DHL64_PROFILE=1,$(HL64_DEFS)) -I. -Iinclude -Iplat/host
HOST_LDFLAGS := $(HOST_ARCH) -Wl,--wrap=malloc,--wrap=free,--wrap=calloc,--wrap=realloc -lm
HOST_OBJS := $(addprefix $(BUILD_DIR)/host/,$(ENGINE_SRC:.c=.o) $(HOST_SRC:.c=.o))

$(BUILD_DIR)/host/%.o: %.c
	@mkdir -p $(dir $@)
	@echo "    [HOSTCC] $<"
	$(HOST_CC) -c $(HOST_CFLAGS) -o $@ $<

$(addprefix $(BUILD_DIR)/host/,game/pm/pm_shared.o game/pm/pm_math.o): HOST_CFLAGS += $(HLSDK_CFLAGS)
$(BUILD_DIR)/host/game/hl/%.o: HOST_CFLAGS += -fno-strict-aliasing

# The host heap is the heap an HL64_TEST cart has left after its boot carve
# (plat/host/heap_host.c, include/hl64/limits.h HL64_CART_*), which starts
# where that cart's image ends: link the HL64_TEST ELF (the host's PROFILE
# setting) in $(BUILD_DIR)/host-cart and take its __bss_end. Needs the N64
# SDK; without it the host build fails here.
HOST_CART_DIR := $(BUILD_DIR)/host-cart
HOST_CART_H := $(BUILD_DIR)/host/cart_image.h
.PHONY: FORCE
$(HOST_CART_H): FORCE
	@mkdir -p $(dir $@)
	$(MAKE) --no-print-directory BUILD_DIR=$(HOST_CART_DIR) HL64_TEST=1 HL64_CONTENT= HL64_CMDS= elf
	@e=$$($(N64_NM) $(HOST_CART_DIR)/hl64.elf | sed -n 's/^\([0-9a-f]*\) [A-Za-z] __bss_end$$/\1/p'); \
	[ -n "$$e" ] || { echo "no __bss_end in $(HOST_CART_DIR)/hl64.elf" >&2; exit 1; }; \
	printf '/* generated: __bss_end of %s */\n#define HL64_CART_BSS_END 0x%su\n' \
		'$(HOST_CART_DIR)/hl64.elf' "$$e" >$@.tmp; \
	if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv $@.tmp $@; fi
$(BUILD_DIR)/host/plat/host/heap_host.o: $(HOST_CART_H)
$(BUILD_DIR)/host/plat/host/heap_host.o: HOST_CFLAGS += -I$(BUILD_DIR)/host

$(HOST_BIN): $(HOST_OBJS)
	$(HOST_CC) -o $@ $^ $(HOST_LDFLAGS)

host: $(HOST_BIN)

# Host unit tests of the M1 modules (test/m1/unit.c): the host objects
# without main.
UNIT_BIN := $(BUILD_DIR)/host/hl64-unit
UNIT_OBJS := $(filter-out $(BUILD_DIR)/host/plat/host/main.o,$(HOST_OBJS)) \
	$(BUILD_DIR)/host/test/m1/unit.o
$(UNIT_BIN): $(UNIT_OBJS)
	$(HOST_CC) -o $@ $^ $(HOST_LDFLAGS)
unit: $(UNIT_BIN)

# Collision test driver (test/coll/colltest.c, run by test/coll/trace_tests.py).
COLLTEST_BIN := $(BUILD_DIR)/host/hl64-colltest
COLLTEST_OBJS := $(filter-out $(BUILD_DIR)/host/plat/host/main.o $(BUILD_DIR)/host/engine/core/eng.o,\
	$(HOST_OBJS)) \
	$(BUILD_DIR)/host/test/coll/colltest.o $(BUILD_DIR)/host/test/m3/studio_doubles.o
$(COLLTEST_BIN): $(COLLTEST_OBJS)
	$(HOST_CC) -o $@ $^ $(HOST_LDFLAGS)
colltest: $(COLLTEST_BIN)

# Studio residency + animation API driver (test/m3/studio.c, run by
# test/m3/studio_test.py).
STUDIOTEST_BIN := $(BUILD_DIR)/host/hl64-studiotest
STUDIOTEST_OBJS := $(filter-out $(BUILD_DIR)/host/plat/host/main.o $(BUILD_DIR)/host/engine/core/eng.o,\
	$(HOST_OBJS)) \
	$(BUILD_DIR)/host/test/m3/studio.o $(BUILD_DIR)/host/test/m3/studio_doubles.o
$(STUDIOTEST_BIN): $(STUDIOTEST_OBJS)
	$(HOST_CC) -o $@ $^ $(HOST_LDFLAGS)
studiotest: $(STUDIOTEST_BIN)

# Area links / touches driver (test/m2/area.c, run by test/m2/area.py).
AREA_BIN := $(BUILD_DIR)/host/hl64-area
AREA_OBJS := $(filter-out $(BUILD_DIR)/host/plat/host/main.o,$(HOST_OBJS)) \
	$(BUILD_DIR)/host/test/m2/area.o
$(AREA_BIN): $(AREA_OBJS)
	$(HOST_CC) -o $@ $^ $(HOST_LDFLAGS)
area: $(AREA_BIN)

# Entity layer driver (test/m3/ent.c, run by test/m3/ent.py: K1-K3, K6-K9).
ENTTEST_BIN := $(BUILD_DIR)/host/hl64-ent
ENTTEST_OBJS := $(filter-out $(BUILD_DIR)/host/plat/host/main.o,$(HOST_OBJS)) \
	$(BUILD_DIR)/host/test/m3/ent.o
$(ENTTEST_BIN): $(ENTTEST_OBJS)
	$(HOST_CC) -o $@ $^ $(HOST_LDFLAGS)
enttest: $(ENTTEST_BIN)

# Effects driver (test/m3/fx.c, run by test/m3/fx.py: X1-X6, D1/D2/D4,
# L1/L2, model light terms).
FXTEST_BIN := $(BUILD_DIR)/host/hl64-fx
FXTEST_OBJS := $(filter-out $(BUILD_DIR)/host/plat/host/main.o,$(HOST_OBJS)) \
	$(BUILD_DIR)/host/test/m3/fx.o
$(FXTEST_BIN): $(FXTEST_OBJS)
	$(HOST_CC) -o $@ $^ $(HOST_LDFLAGS)
fxtest: $(FXTEST_BIN)

# Game spawn-all driver (test/m3/game.c, run by test/m3/game.py).
GAMETEST_BIN := $(BUILD_DIR)/host/hl64-game
GAMETEST_OBJS := $(filter-out $(BUILD_DIR)/host/plat/host/main.o,$(HOST_OBJS)) \
	$(BUILD_DIR)/host/test/m3/game.o
$(GAMETEST_BIN): $(GAMETEST_OBJS)
	$(HOST_CC) -o $@ $^ $(HOST_LDFLAGS)
gametest: $(GAMETEST_BIN)

# Monster sweep driver (test/m3/mon.c, run by test/m3/mon_sweep.py).
MONTEST_BIN := $(BUILD_DIR)/host/hl64-mon
MONTEST_OBJS := $(filter-out $(BUILD_DIR)/host/plat/host/main.o,$(HOST_OBJS)) \
	$(BUILD_DIR)/host/test/m3/mon.o
$(MONTEST_BIN): $(MONTEST_OBJS)
	$(HOST_CC) -o $@ $^ $(HOST_LDFLAGS)
montest: $(MONTEST_BIN)

# Items driver (test/m3/items.c, run by test/m3/items.py: tripmine,
# weaponbox map ammo, long jump).
ITEMSTEST_BIN := $(BUILD_DIR)/host/hl64-items
ITEMSTEST_OBJS := $(filter-out $(BUILD_DIR)/host/plat/host/main.o,$(HOST_OBJS)) \
	$(BUILD_DIR)/host/test/m3/items.o
$(ITEMSTEST_BIN): $(ITEMSTEST_OBJS)
	$(HOST_CC) -o $@ $^ $(HOST_LDFLAGS)
itemstest: $(ITEMSTEST_BIN)

# Physics driver (test/m3/phys.c, run by test/m3/phys.py: P1-P15, K4).
PHYS_BIN := $(BUILD_DIR)/host/hl64-phys
PHYS_OBJS := $(filter-out $(BUILD_DIR)/host/plat/host/main.o,$(HOST_OBJS)) \
	$(BUILD_DIR)/host/test/m3/phys.o
$(PHYS_BIN): $(PHYS_OBJS)
	$(HOST_CC) -o $@ $^ $(HOST_LDFLAGS)
phystest: $(PHYS_BIN)

# Player move driver (test/m2/pm.c, run by test/m2/pmbot.py and test/run/m2.sh).
PM_BIN := $(BUILD_DIR)/host/hl64-pm
PM_OBJS := $(filter-out $(BUILD_DIR)/host/plat/host/main.o,$(HOST_OBJS)) \
	$(BUILD_DIR)/host/test/m2/pm.o
$(PM_BIN): $(PM_OBJS)
	$(HOST_CC) -o $@ $^ $(HOST_LDFLAGS)
pmtest: $(PM_BIN)

# Records / resources driver (test/m3/rec_test.c, run by test/m3/rec.py).
REC_BIN := $(BUILD_DIR)/host/hl64-rec
# The driver's test game takes the record dispatch rec.c calls (--wrap);
# the rest of game/ links as usual.
REC_WRAP := game_spawn_record game_spawn_logic game_use game_logic_use game_logic_timer \
	game_logic_restored game_map_opened game_map_closing game_dehydrate game_rehydrate \
	game_cut_refs wld_layer_ready wld_layer_of_map
REC_OBJS := $(filter-out $(BUILD_DIR)/host/plat/host/main.o,$(HOST_OBJS)) \
	$(BUILD_DIR)/host/test/m3/rec_test.o
$(REC_BIN): $(REC_OBJS)
	$(HOST_CC) -o $@ $^ $(HOST_LDFLAGS) $(foreach f,$(REC_WRAP),-Wl,--wrap=$(f))
rectest: $(REC_BIN)

# M4 Stream residency driver (test/m4/stream.c, run by test/m4/stream.sh).
STREAM_BIN := $(BUILD_DIR)/host/hl64-stream
STREAM_OBJS := $(filter-out $(BUILD_DIR)/host/plat/host/main.o,$(HOST_OBJS)) \
	$(BUILD_DIR)/host/test/m4/stream.o
$(STREAM_BIN): $(STREAM_OBJS)
	$(HOST_CC) -o $@ $^ $(HOST_LDFLAGS)
streamtest: $(STREAM_BIN)
# M4 level-state driver (test/m4/rec4_test.c, run by test/m4/rec.sh): async
# open, release / rehydrate, frozen maps, twins, deferred uses, runtime ids
# with a test game (--wrap as above, plus runtime spawns).
REC4_BIN := $(BUILD_DIR)/host/hl64-rec4
REC4_OBJS := $(filter-out $(BUILD_DIR)/host/plat/host/main.o,$(HOST_OBJS)) \
	$(BUILD_DIR)/host/test/m4/rec4_test.o
$(REC4_BIN): $(REC4_OBJS)
	$(HOST_CC) -o $@ $^ $(HOST_LDFLAGS) $(foreach f,$(REC_WRAP) game_spawn_class,-Wl,--wrap=$(f))
rec4test: $(REC4_BIN)

# Sound driver (test/m3/snd.c, run by test/m3/snd.sh: S1-S8 host parts).
SND_BIN := $(BUILD_DIR)/host/hl64-snd
SND_OBJS := $(filter-out $(BUILD_DIR)/host/plat/host/main.o,$(HOST_OBJS)) \
	$(BUILD_DIR)/host/test/m3/snd.o
$(SND_BIN): $(SND_OBJS)
	$(HOST_CC) -o $@ $^ $(HOST_LDFLAGS)
sndtest: $(SND_BIN)

# Seam-window profiler driver (test/m4/gate_seamprof.c, run by
# test/m4/gate_seamprof.py and test/run/m4.sh).
SEAMPROF_BIN := $(BUILD_DIR)/host/hl64-seamprof
SEAMPROF_OBJS := $(filter-out $(BUILD_DIR)/host/plat/host/main.o,$(HOST_OBJS)) \
	$(BUILD_DIR)/host/test/m4/gate_seamprof.o
$(SEAMPROF_BIN): $(SEAMPROF_OBJS)
	$(HOST_CC) -o $@ $^ $(HOST_LDFLAGS)
seamproftest: $(SEAMPROF_BIN)

# Transcendentals accuracy driver (test/m4/hlmath.c, run by test/m4/hlmath.py).
MATHTEST_BIN := $(BUILD_DIR)/host/hl64-mathtest
$(MATHTEST_BIN): $(BUILD_DIR)/host/engine/core/hlmath.o $(BUILD_DIR)/host/test/m4/hlmath.o
	$(HOST_CC) -o $@ $^ $(HOST_LDFLAGS)
mathtest: $(MATHTEST_BIN)

# Seam driver (test/m4/seam_test.c, run by test/m4/seam.sh): the seam
# runtime over the real campaign parse, world layers, player re-base and
# entities; the sibling M4 services it drives are the driver's doubles.
SEAM_BIN := $(BUILD_DIR)/host/hl64-seam
SEAM_WRAP := wld_open_async wld_layer_ready wld_layer_of_map wld_pin_seam wld_pin_seam_window \
	wld_unpin_seam wld_stream_rebase rec_map_open rec_map_ready rec_map_close rec_slot_live \
	rec_release rec_twin_alias rec_twin_clear rec_rehome rec_entity rec_state rec_kill rec_wake \
	rec_slot_held rec_origin \
	rec_name_find rec_fire rec_fire_at rec_key_f game_twin_copy game_carry
SEAM_OBJS := $(filter-out $(BUILD_DIR)/host/plat/host/main.o,$(HOST_OBJS)) \
	$(BUILD_DIR)/host/test/m4/seam_test.o
$(SEAM_BIN): $(SEAM_OBJS)
	$(HOST_CC) -o $@ $^ $(HOST_LDFLAGS) $(foreach f,$(SEAM_WRAP),-Wl,--wrap=$(f))
seamtest: $(SEAM_BIN)
# Level-state driver (test/m4/gamels.c, run by test/m4/gamels.py: dehydrate
# / rehydrate round trips, quiescence, twins, carry, cut refs). The record
# layer's canonical-reference calls and seam_changelevel are the driver's.
GAMELS_BIN := $(BUILD_DIR)/host/hl64-gamels
GAMELS_WRAP := rec_runtime_id rec_ref_entity rec_twin_of rec_slot_closing seam_changelevel
GAMELS_OBJS := $(filter-out $(BUILD_DIR)/host/plat/host/main.o,$(HOST_OBJS)) \
	$(BUILD_DIR)/host/test/m4/gamels.o
$(GAMELS_BIN): $(GAMELS_OBJS)
	$(HOST_CC) -o $@ $^ $(HOST_LDFLAGS) $(foreach f,$(GAMELS_WRAP),-Wl,--wrap=$(f))
gamelstest: $(GAMELS_BIN)

# ---- content, routes, gates --------------------------------------------------

# Host cook tool (native, not HOST_M32): BSP30 entity lump -> HLE1 + HLN1.
HLECOOK := $(BUILD_DIR)/tools/hlecook
$(HLECOOK): tools/w64c/hlecook.c tools/w64c/hl_entcook.h tools/w64c/hl_nodecook.h \
		tools/hlsdk/hl_nodegraph.h
	@mkdir -p $(@D)
	$(HOST_CC) -std=c99 -O2 -Wall -Wextra -Werror -ffp-contract=off -o $@ tools/w64c/hlecook.c -lm

cook: $(HLECOOK)
	@if [ -z "$(HL64_CONTENT)" ]; then echo "usage: make cook HL64_DATA=<.../Half-Life/valve> HL64_CONTENT=<dir> [MAPS='glob ...'] [HL64_REF_PALETTE=<palette.lmp>]" >&2; exit 2; fi
	$(if $(HL64_DATA),python3 -m tools.cook.stage --data '$(HL64_DATA)' --content '$(HL64_CONTENT)' \
		--hlecook $(HLECOOK) $(if $(MAPS),--maps $(foreach m,$(MAPS),'$(m)')) \
		$(if $(HL64_REF_PALETTE),--ref-palette '$(HL64_REF_PALETTE)'))
	python3 -m tools.restab.restab --content $(HL64_CONTENT) --defs game/defs -o $(HL64_CONTENT)/campaign.rtb
	$(if $(HL64_DATA),python3 -m tools.w64c.hleids --check $(HL64_CONTENT) --defs game/defs)
	python3 -m tools.cook.layerdirs $(HL64_CONTENT)

ROUTES := $(wildcard test/routes/*.route)
routes: cook
	@mkdir -p $(BUILD_DIR)/routes
	@for r in $(ROUTES); do \
		echo "    [ROUTE] $$r"; \
		python3 -m tools.routec.routec --rtb $(HL64_CONTENT)/campaign.rtb $$r \
			-o $(BUILD_DIR)/routes/$$(basename $$r .route).rbc || exit 1; \
	done

test-host: $(HLECOOK)
	BUILD_DIR=$(BUILD_DIR) test/run/m0.sh
	python3 -m unittest tools.restab.test_restab tools.routec.test_routec tools.studio.test_studiocook
	HL64_HLECOOK=$(HLECOOK) python3 -m unittest tools.cook.test_cook tools.cook.test_hl_campaign \
		tools.w64c.test_w64c tools.w64c.test_w64check tools.w64c.test_w64seam \
		tools.w64c.test_hlecook tools.w64c.test_hleids

test-ares:
	BUILD_DIR=$(BUILD_DIR) test/run/m0.sh --ares

provcheck:
	python3 tools/provcheck/provcheck.py check

clean:
	rm -rf $(BUILD_DIR)

-include $(wildcard $(BUILD_DIR)/*/*/*.d $(BUILD_DIR)/host/*/*/*.d)
