# SPDX-License-Identifier: MIT
#
# hl64 build (docs/architecture.md 18.1).
#
#   make rom      $(BUILD_DIR)/hl64.z64 (libdragon n64.mk + t3d.mk, banked libdragon)
#   make host     $(BUILD_DIR)/host/hl64-host (HOST_M32=1 default: -m32)
#   make cook     restab over the staged content (HL64_CONTENT)
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

# Sources. engine/ and plat/ build with the strict flags below.
ENGINE_SRC := \
	engine/core/dbg.c engine/core/eng.c engine/core/log.c engine/core/settings.c \
	engine/io/io.c \
	engine/mem/carve.c engine/mem/heap.c engine/mem/pool.c
N64_SRC := plat/n64/cart.c plat/n64/fpu_shim.c plat/n64/main.c plat/n64/sys.c plat/n64/vid.c
HOST_SRC := plat/host/heap_host.c plat/host/io_host.c plat/host/main.c plat/host/sys.c

# Directories built without -ffast-math on the cart (18.2): host and cart
# must agree bit for bit on collision and physics.
NOFASTMATH_DIRS := engine/coll engine/phys

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

.PHONY: all rom elf host cook routes test-host test-ares provcheck sdk-check clean
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

# Explicit ROM rule (n64.mk's %.z64 writes into the working directory).
$(ROM): N64_ROM_TITLE = "hl64"
$(ROM): $(ELF)
	@echo "    [Z64] $@"
	$(N64_SYM) --all $< $<.sym
	cp $< $<.stripped
	$(N64_STRIP) -s $<.stripped
	$(N64_ELFCOMPRESS) -o $(dir $<) -c $(N64_ROM_ELFCOMPRESS) $<.stripped
	@rm -f $@
	$(N64_TOOL) $(N64_TOOLFLAGS) --output $@.tmp --align 256 $<.stripped $<.sym $(N64_TOOLFILES)
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
HOST_LDFLAGS := $(HOST_ARCH) -Wl,--wrap=malloc,--wrap=free,--wrap=calloc,--wrap=realloc
HOST_OBJS := $(addprefix $(BUILD_DIR)/host/,$(ENGINE_SRC:.c=.o) $(HOST_SRC:.c=.o))

$(BUILD_DIR)/host/%.o: %.c
	@mkdir -p $(dir $@)
	@echo "    [HOSTCC] $<"
	$(HOST_CC) -c $(HOST_CFLAGS) -o $@ $<

$(HOST_BIN): $(HOST_OBJS)
	$(HOST_CC) -o $@ $^ $(HOST_LDFLAGS)

host: $(HOST_BIN)

# ---- content, routes, gates --------------------------------------------------

cook:
	@if [ -z "$(HL64_CONTENT)" ]; then echo "usage: make cook HL64_CONTENT=<staged content dir>" >&2; exit 2; fi
	python3 -m tools.restab.restab --content $(HL64_CONTENT) --defs game/defs -o $(HL64_CONTENT)/campaign.rtb

ROUTES := $(wildcard test/routes/*.route)
routes: cook
	@mkdir -p $(BUILD_DIR)/routes
	@for r in $(ROUTES); do \
		echo "    [ROUTE] $$r"; \
		python3 -m tools.routec.routec --rtb $(HL64_CONTENT)/campaign.rtb $$r \
			-o $(BUILD_DIR)/routes/$$(basename $$r .route).rbc || exit 1; \
	done

test-host:
	BUILD_DIR=$(BUILD_DIR) test/run/m0.sh
	python3 -m unittest tools.restab.test_restab tools.routec.test_routec

test-ares:
	BUILD_DIR=$(BUILD_DIR) test/run/m0.sh --ares

provcheck:
	python3 tools/provcheck/provcheck.py check

clean:
	rm -rf $(BUILD_DIR)

-include $(wildcard $(BUILD_DIR)/*/*/*.d $(BUILD_DIR)/host/*/*/*.d)
