# SPDX-License-Identifier: MIT
# The SC64 interface test ROM (test/run/sc64.sh):
#   make -C test/sc64 BUILD_DIR=<dir>   ->  <dir>/sc64test.z64
# Same N64 SDK and toolchain as the game (the Makefile's LIBDRAGON_REV pin).
ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
BUILD_DIR ?= build
LIBDRAGON_REV := $(shell sed -n 's/^LIBDRAGON_REV := //p' $(ROOT)/Makefile)
N64_INST ?= $(HOME)/sdks/n64-sdk-hl64-$(shell printf %.12s $(LIBDRAGON_REV))
N64_GCCPREFIX ?= $(HOME)/sdks/mips64-toolchain
export N64_INST N64_GCCPREFIX
SOURCE_DIR := $(ROOT)/test/sc64
ROM := $(BUILD_DIR)/sc64test.z64
ELF := $(BUILD_DIR)/sc64test.elf

all: $(ROM)

include $(N64_INST)/include/n64.mk

N64_CFLAGS += -I$(ROOT) -Werror

$(ELF): $(BUILD_DIR)/sc64test.o

# explicit ROM rule (n64.mk's %.z64 writes into the working directory)
$(ROM): N64_ROM_TITLE = "hl64 sc64 test"
$(ROM): $(ELF)
	@echo "    [Z64] $@"
	$(N64_SYM) $< $<.sym
	cp $< $<.stripped
	$(N64_STRIP) -s $<.stripped
	@rm -f $@
	$(N64_TOOL) $(N64_TOOLFLAGS) --output $@ --align 256 $<.stripped $<.sym

clean:
	rm -f $(BUILD_DIR)/sc64test.*

.PHONY: all clean
