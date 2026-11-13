# SPDX-License-Identifier: MIT
#
# Builds the GoldSrc probe game DLL (probe.so, 32-bit Linux) from a
# pinned Half Life 1 SDK checkout patched with hlsdk-hooks.patch plus the
# probe sources in probe/. The HLSDK checkout and all outputs live outside
# the repository (run.sh passes HLSDK_DIR and BUILD_DIR). Flags follow the
# HLSDK linux/Makefile (x87 maths, -fpermissive), so pm_shared in the probe
# computes as Valve's retail build does.
#
# Only 32-bit C runtime headers/libs are required (gcc-multilib); the
# 32-bit libstdc++ is linked by path. The DLL uses no C++ library code;
# public/minmax.h pulls <algorithm> for std::min/max only, so without
# g++-multilib the 64-bit bits/c++config.h is used as a fallback.

HLSDK_DIR ?= /scratch/hl64/tmp/oracle/hlsdk
BUILD_DIR ?= /scratch/hl64/builds/oracle
LIBSTDCXX32 ?= $(firstword $(wildcard /usr/lib32/libstdc++.so.6 /usr/lib/i386-linux-gnu/libstdc++.so.6))
PROBE_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))probe

CXX := g++
CC := gcc
ARCH := -m32 -march=pentium-m -mfpmath=387
DEFS := -DNDEBUG -DPOSIX -D_POSIX -DLINUX -D_LINUX -DGNUC -DHL1 -Dstricmp=strcasecmp -D_strnicmp=strncasecmp \
	-Dstrnicmp=strncasecmp -D_snprintf=snprintf -DQUIVER -DQUAKE2 -DVALVE_DLL -D_alloca=alloca \
	-D_vsnprintf=vsnprintf -DNO_MALLOC_OVERRIDE -DCLIENT_WEAPONS
COMMON := $(ARCH) -O2 -g -fPIC -fno-strict-aliasing -fno-delete-null-pointer-checks -fvisibility=hidden -w $(DEFS)
CXXFLAGS := $(COMMON) -std=gnu++11 -fpermissive -fno-gnu-unique -fno-exceptions
SKIP := AI_BaseNPC_Schedule Wxdebug glock mpstubb playermonster stats
DLL_SRC := $(filter-out $(addprefix $(HLSDK_DIR)/dlls/,$(addsuffix .cpp,$(SKIP))),$(wildcard $(HLSDK_DIR)/dlls/*.cpp)) \
	$(HLSDK_DIR)/dlls/wpn_shared/hl_wpn_glock.cpp $(HLSDK_DIR)/game_shared/voice_gamemgr.cpp
PM_SRC := $(HLSDK_DIR)/pm_shared/pm_shared.c $(HLSDK_DIR)/pm_shared/pm_math.c $(HLSDK_DIR)/pm_shared/pm_debug.c
PROBE_SRC := $(wildcard $(PROBE_DIR)/*.cpp)
INCS := -I$(HLSDK_DIR)/dlls -I$(HLSDK_DIR)/engine -I$(HLSDK_DIR)/common -I$(HLSDK_DIR)/pm_shared \
	-I$(HLSDK_DIR)/game_shared -I$(HLSDK_DIR)/public -I$(PROBE_DIR) \
	$(addprefix -idirafter ,$(wildcard /usr/include/x86_64-linux-gnu/c++/*/))

OBJ := $(addprefix $(BUILD_DIR)/obj/dll/,$(notdir $(DLL_SRC:.cpp=.o))) \
	$(addprefix $(BUILD_DIR)/obj/pm/,$(notdir $(PM_SRC:.c=.o))) \
	$(addprefix $(BUILD_DIR)/obj/probe/,$(notdir $(PROBE_SRC:.cpp=.o)))

vpath %.cpp $(HLSDK_DIR)/dlls $(HLSDK_DIR)/dlls/wpn_shared $(HLSDK_DIR)/game_shared
vpath %.c $(HLSDK_DIR)/pm_shared

all: $(BUILD_DIR)/probe.so

$(BUILD_DIR)/probe.so: $(OBJ)
	$(CXX) $(ARCH) -shared -nostdlib++ -o $@ $(OBJ) $(LIBSTDCXX32) -lm -ldl

$(BUILD_DIR)/obj/dll/%.o: %.cpp $(PROBE_DIR)/probe.h
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCS) -c $< -o $@

$(BUILD_DIR)/obj/pm/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(COMMON) -std=gnu89 -Wno-incompatible-pointer-types -I$(HLSDK_DIR)/common -I$(HLSDK_DIR)/public -I$(HLSDK_DIR)/engine -c $< -o $@

$(BUILD_DIR)/obj/probe/%.o: $(PROBE_DIR)/%.cpp $(PROBE_DIR)/probe.h $(PROBE_DIR)/probe_int.h
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCS) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR)/obj $(BUILD_DIR)/probe.so

.PHONY: all clean
