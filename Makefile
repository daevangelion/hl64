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
# engine/coll: traces, contents, hitboxes, collision residency (M2)
ENGINE_SRC += engine/coll/coll.c engine/coll/colbench.c
# engine/ent + engine/phys: entity slots, linked boxes, spatial grid, touches (M2)
ENGINE_SRC += engine/ent/ent.c engine/ent/grid.c engine/ent/grid_bench.c engine/phys/touch.c
# input, player move host, view, route VM, rng (M2)
ENGINE_SRC += engine/core/rng.c engine/in/in.c engine/phys/pmove.c engine/test/route.c \
	engine/view/view.c
# game/ (HLSDK licence, game/LICENSE-HLSDK) links into the same images:
# pm_shared and its adapter (D2), the player's pre-move think, the view
GAME_SRC := game/pm/pm_shared.c game/pm/pm_math.c game/pm/pm_host.c game/hl/player.c \
	game/hud/view.c
# M3 slices: each slice adds its own sources on its own line only
# (merges stay trivial; local://M3-contract.md).
ENGINE_SRC_ENT := engine/ent/heap.c engine/ent/pass.c engine/ent/priv.c
ENGINE_SRC_PHYS :=
ENGINE_SRC_REC := engine/ent/rec.c engine/ent/rec_hle.c engine/res/res.c engine/res/rtb.c
ENGINE_SRC_STUDIO := engine/render/studio.c
ENGINE_SRC_SND :=
ENGINE_SRC_FX :=
ENGINE_SRC_D2 :=
GAME_SRC_CORE :=
GAME_SRC_MON :=
GAME_SRC_ITEMS :=
GAME_SRC_STUDIO := game/studio/pose.c game/studio/anim.c game/studio/event.c
ENGINE_SRC += $(ENGINE_SRC_ENT) $(ENGINE_SRC_PHYS) $(ENGINE_SRC_REC) $(ENGINE_SRC_STUDIO) \
	$(ENGINE_SRC_SND) $(ENGINE_SRC_FX) $(ENGINE_SRC_D2)
GAME_SRC += $(GAME_SRC_CORE) $(GAME_SRC_MON) $(GAME_SRC_ITEMS) $(GAME_SRC_STUDIO)
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

# Directories built without -ffast-math on the cart (18.2): host and cart
# must agree bit for bit on collision and physics, on the world draw list
# (frustum, point -> leaf) the cook predicts, and on everything game logic
# decides (entities, records, AI, studio poses: a route must take the same
# path on both).
NOFASTMATH_DIRS := engine/coll engine/phys engine/render engine/world engine/ent engine/res \
	engine/snd game/pm game/hl game/studio game/hud

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

.PHONY: all rom elf host unit colltest studiotest area pmtest enttest rectest cook routes test-host test-ares provcheck sdk-check clean
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

# Player move driver (test/m2/pm.c, run by test/m2/pmbot.py and test/run/m2.sh).
PM_BIN := $(BUILD_DIR)/host/hl64-pm
PM_OBJS := $(filter-out $(BUILD_DIR)/host/plat/host/main.o,$(HOST_OBJS)) \
	$(BUILD_DIR)/host/test/m2/pm.o
$(PM_BIN): $(PM_OBJS)
	$(HOST_CC) -o $@ $^ $(HOST_LDFLAGS)
pmtest: $(PM_BIN)

# Records / resources driver (test/m3/rec_test.c, run by test/m3/rec.py).
REC_BIN := $(BUILD_DIR)/host/hl64-rec
REC_OBJS := $(filter-out $(BUILD_DIR)/host/plat/host/main.o,$(HOST_OBJS)) \
	$(BUILD_DIR)/host/test/m3/rec_test.o
$(REC_BIN): $(REC_OBJS)
	$(HOST_CC) -o $@ $^ $(HOST_LDFLAGS)
rectest: $(REC_BIN)

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
