# AllGenURecomp build
# Windows: MSYS2 UCRT64 (clang, lld, make; see README.md). Linux: clang (planned).
#
#   make gen GAME=<folder with exefs/ and romfs/>   recompile the game code from your own dump into generated/
#   make -j4                                        build build/allgenurecomp(.exe) and the game code library, with its
#                                                   DLLs and the game files (build/data): start build/allgenurecomp.exe
# build/ holds only the game folder (what a player copies); the compiler's files go to obj/.
UNAME := $(shell uname -s)
ifneq ($(filter MINGW% MSYS% CYGWIN% Windows_NT,$(UNAME) $(OS)),)
PLATFORM := windows
else
PLATFORM := linux
endif

GAME     ?= ../mhgu/game
GEN      ?= generated
B        ?= build
O        ?= obj
CC       := clang
CXX      := clang++
GENARCH  ?= -march=x86-64-v3  # AVX2 + FMA (Haswell or newer)
# [AllGenU] LTO=1 (default): thin LTO for the runtime (render executor, Vulkan backend, memory management).
# The recompiled game code keeps GENFLAGS as-is (-O1 on generated C with missing returns would be risky at -O2+,
# and thin-LTO over thousands of generated files makes the link extremely slow). LTO=0 builds without LTO.
LTO      ?= 1
# [AllGenU] the runtime's optimisation level (make OPT=2 for a build with less aggressive inlining / vectorisation)
OPT      ?= 3
GENFLAGS := -O1 -fno-strict-aliasing -ffp-contract=off -fno-math-errno -Wno-return-type -Wno-unused-label \
            -Wno-parentheses-equality -Wno-unused-value -fvisibility=hidden -Iruntime $(GENARCH)

GEN_SRC  := $(wildcard $(GEN)/main/*.c) $(wildcard $(GEN)/sdk/*.c)
GEN_OBJ  := $(patsubst $(GEN)/%.c,$(O)/gen/%.o,$(GEN_SRC)) $(O)/gen/tools/gamelib_entry.o
RT_SRC   := $(wildcard runtime/*.c) $(wildcard runtime/hle/*.c) $(wildcard runtime/services/*.c) $(wildcard runtime/net/*.c) \
            runtime/nvn/objects.c runtime/nvn/cmdbuf.c runtime/nvn/nvn_args.c runtime/nvn/tegra.c runtime/nvn/formats.c \
            runtime/nvn/backend_null.c runtime/nvn/astc.c runtime/nvn/maxwell/translate.c $(wildcard runtime/nvn/vulkan/*.c) \
            runtime/platform/$(PLATFORM).c
# (simd_int.c / neon.c, helpers of the recompiled code, live in the executable: the game code library imports them,
# and runtime/interp.c uses them too)
RT_HDRS  := $(wildcard runtime/*.h) $(wildcard runtime/platform/*.h) $(wildcard runtime/nvn/*.h) $(wildcard runtime/nvn/*/*.h)

ifeq ($(PLATFORM),windows)
EXE      := $(B)/allgenurecomp.exe
OSFLAGS  := -D_FILE_OFFSET_BITS=64 -DWIN32_LEAN_AND_MEAN -DNOMINMAX -D_CRT_RAND_S -D_POSIX_THREAD_SAFE_FUNCTIONS
GENFLAGS += -D_FILE_OFFSET_BITS=64
LDFLAGS  := -fuse-ld=lld -Wl,--stack,16777216 -lSDL3 -lavformat -lavcodec -lavutil -lvulkan-1 -lshaderc_shared -lwinmm -lole32 -lshell32 -luuid -lws2_32 -lcomdlg32 \
            -Wl,--export-all-symbols -Wl,--out-implib,$(O)/allgenurecomp.dll.a
GAMELIB  := $(B)/allgenugame.dll
GAMELIB_LDFLAGS := -shared -fuse-ld=lld $(O)/allgenurecomp.dll.a
else
EXE      := $(B)/allgenurecomp
OSFLAGS  :=
GENFLAGS += -fPIC
LDFLAGS  := -fuse-ld=lld -rdynamic -lSDL3 -lavformat -lavcodec -lavutil -lvulkan -lshaderc_shared -lpthread -ldl -lm
GAMELIB  := $(B)/allgenugame.so
GAMELIB_LDFLAGS := -shared -fuse-ld=lld
endif

# [AllGenU] link the runtime objects with thin LTO when enabled (the game code library is not built with it)
ifeq ($(LTO),1)
LDFLAGS  += -flto=thin
endif

# GAMELIB_ID: what the game code library is compiled against. A library with another id is refused at startup.
GAMELIB_SRCS := tools/gen.py tools/lifter.py tools/neon.py tools/nso.py tools/gamelib_entry.c \
                runtime/hle/hle_symbols.txt runtime/cpu.h runtime/vfp.h runtime/neon_rt.h runtime/gamelib.h
GAMELIB_ID := $(shell (cat $(GAMELIB_SRCS); echo '$(GENFLAGS)') | sha256sum | cut -c1-16)

# [AllGenU] KZMUI (runtime/kzmui.cpp): the port's own interface, drawn by overlay.cpp with ImGui's C++ API. Not built
# with NULL_GPU=1 (it drives the renderer's settings: vk_set_msaa / vk_set_vsync live in the Vulkan backend).
KZMUI_OBJ := $(if $(filter 1,$(NULL_GPU)),,$(O)/rt/kzmui.o)
RT_OBJ   = $(patsubst runtime/%.c,$(O)/rt/%.o,$(RT_SRC)) $(RT_CXX) $(O)/rt/overlay.o $(KZMUI_OBJ) $(IMGUI_OBJ)
# Dear ImGui with cimgui's C API (third_party/cimgui: Forge plugins' ImGui, runtime/forge_imgui.c, overlay.cpp)
IMGUI    := third_party/cimgui
IMGUI_SRC := $(IMGUI)/cimgui.cpp $(addprefix $(IMGUI)/imgui/,imgui.cpp imgui_draw.cpp imgui_tables.cpp imgui_widgets.cpp imgui_demo.cpp)
IMGUI_SRC += $(if $(filter 1,$(NULL_GPU)),,$(IMGUI)/imgui/backends/imgui_impl_vulkan.cpp)
IMGUI_OBJ := $(patsubst $(IMGUI)/%.cpp,$(O)/imgui/%.o,$(IMGUI_SRC))
IMGUIFLAGS := -O3 -g -DCIMGUI_NO_EXPORT -I$(IMGUI) -I$(IMGUI)/imgui
# (vendored VulkanMemoryAllocator fallback: -I on a missing dir is ignored, so MSYS2 builds
# with the system vulkan-memory-allocator package are unaffected)
# [AllGenU] -O3 (vectorisation, wider inlining) and the same baseline as the game code library (x86-64-v3: the
# executable already needs an AVX2 CPU through allgenugame.dll, so this raises no requirement) + optional thin LTO.
RTFLAGS  := -O$(OPT) -g -fno-strict-aliasing -Wall -Wno-unused-function -Iruntime -Ithird_party/vk_mem_alloc -I$(IMGUI) $(GENARCH) $(if $(filter 1,$(LTO)),-flto=thin) $(OSFLAGS)
# NULL_GPU=1: no renderer (runtime/nvn/backend_null.c: the game runs, nothing is drawn)
ifeq ($(NULL_GPU),1)
RTFLAGS  += -DMHGU_NULL_BACKEND
RT_SRC   := $(filter-out runtime/nvn/vulkan/%,$(RT_SRC))
RT_CXX   :=
else
RT_CXX   := $(O)/rt/nvn/vulkan/vma.o
endif

all: $(EXE) $(if $(GEN_SRC),$(GAMELIB)) $(if $(filter windows,$(PLATFORM)),bundle) data

$(O)/gen/%.o: $(GEN)/%.c runtime/cpu.h runtime/vfp.h runtime/neon_rt.h
	@mkdir -p $(dir $@)
	@$(CC) $(GENFLAGS) -c $< -o $@


# the id as a file, rewritten only when it changes: both sides of the library interface rebuild together
$(O)/gamelib.id: FORCE
	@mkdir -p $(O); [ "$$(cat $@ 2>/dev/null)" = '$(GAMELIB_ID)' ] || echo '$(GAMELIB_ID)' > $@

$(O)/gen/tools/gamelib_entry.o: tools/gamelib_entry.c $(O)/gamelib.id
	@mkdir -p $(dir $@)
	$(CC) $(GENFLAGS) -DGAMELIB_ID='"$(GAMELIB_ID)"' -c $< -o $@
$(O)/rt/gamelib.o: $(O)/gamelib.id
$(O)/rt/gamelib.o: RTFLAGS += -DGAMELIB_ID='"$(GAMELIB_ID)"'

$(O)/rt/%.o: runtime/%.c $(RT_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(RTFLAGS) -c $< -o $@

$(O)/rt/forge_imgui.o: runtime/forge_imgui_thunks.inc $(IMGUI)/cimgui.h
$(O)/rt/forge_imgui.o: RTFLAGS += -DCIMGUI_NO_EXPORT -Wno-unused-variable

$(O)/rt/overlay.o: runtime/overlay.cpp runtime/overlay.h runtime/forge.h runtime/kzmui.h
	@mkdir -p $(dir $@)
	$(CXX) $(IMGUIFLAGS) $(if $(filter 1,$(NULL_GPU)),-DMHGU_NULL_BACKEND) -Wall -Iruntime -c $< -o $@

# [AllGenU] KZMUI: the menu itself (runtime/kzmui.cpp, C++ with ImGui's C++ API, as overlay.cpp)
$(O)/rt/kzmui.o: runtime/kzmui.cpp runtime/kzmui.h runtime/config.h runtime/forge.h runtime/mods.h
	@mkdir -p $(dir $@)
	$(CXX) $(IMGUIFLAGS) -Wall -Iruntime -c $< -o $@

$(O)/imgui/%.o: $(IMGUI)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(IMGUIFLAGS) -w -c $< -o $@

$(O)/rt/nvn/vulkan/vma.o: runtime/nvn/vulkan/vma.cpp
	@mkdir -p $(dir $@)
	$(CXX) -O3 -w -Ithird_party/vk_mem_alloc -c $< -o $@

$(EXE): $(RT_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) -o $@ $^ $(LDFLAGS)

$(GAMELIB): $(GEN_OBJ) | $(EXE)
	@mkdir -p $(O)
	$(CC) $(GAMELIB_LDFLAGS) -o $(O)/gamelib.tmp $^
	mv -f $(O)/gamelib.tmp $@

PY := $(firstword $(wildcard tools/venv/Scripts/python.exe) $(wildcard tools/venv/bin/python) python3)

# The build folder is the game folder: allgenurecomp.exe starts from it on its own (runtime/main.c), with
# - bundle (Windows): the MSYS2 DLLs it needs beside it (tools/bundle_dlls.py; not the GPU driver's vulkan-1.dll)
# - data: the game files from $(GAME) in $(B)/data/exefs and $(B)/data/romfs, once (hard links where the file system
#   allows them - no second copy on disk, and deleting them leaves $(GAME) alone - else copies).
# Nothing else: a player copies it as a whole into their game folder (or over it: their settings, caches, mods/ and
# log.txt are written only when the game runs). run-game.bat runs it with those in run/ (MHGU_HOME), not here.
bundle: $(EXE) $(if $(GEN_SRC),$(GAMELIB))
	@$(PY) tools/bundle_dlls.py "$$(cygpath -m "$$(dirname "$$(command -v $(CC))")")" $(B) $^

data: $(B)/data/.complete
$(B)/data/.complete:
	@if [ ! -f "$(GAME)/exefs/main" ] || [ ! -d "$(GAME)/romfs" ]; then \
	    echo "data: no game files in $(GAME) (make GAME=<folder with exefs/ and romfs/>): $(B)/data left empty"; \
	else \
	    mkdir -p $(B)/data && for d in exefs romfs; do \
	        rm -rf "$(B)/data/$$d.tmp"; \
	        cp -rl "$(GAME)/$$d" "$(B)/data/$$d.tmp" 2>/dev/null || { rm -rf "$(B)/data/$$d.tmp"; \
	            echo "data: no hard links here, copying $$d"; cp -r "$(GAME)/$$d" "$(B)/data/$$d.tmp"; } || exit 1; \
	        rm -rf "$(B)/data/$$d" && mv "$(B)/data/$$d.tmp" "$(B)/data/$$d" || exit 1; \
	    done && touch $@ && echo "data: game files in $(B)/data"; \
	fi

gen:
	cd tools && ../$(PY) gen.py sdk ../$(GAME)/exefs/sdk 0x02400000 ../$(GEN)/sdk --hle ../runtime/hle/hle_symbols.txt
	cd tools && ../$(PY) gen.py main ../$(GAME)/exefs/main 0x00200000 ../$(GEN)/main

# differential test of the recompiler against Unicorn (tools/test_lift.py)
test-lift:
	cd tools && ../$(PY) test_lift.py ../$(GAME)/exefs/main 0x00200000 --tests 6000
	cd tools && ../$(PY) test_lift.py ../$(GAME)/exefs/sdk 0x02400000 --tests 6000

runtime-objs: $(RT_OBJ)

clean:  # (keeps $(B)/data, and the settings, caches and saves wherever the game ran)
	rm -rf $(O) $(B)/*.dll $(B)/*.so $(EXE) $(B)/vkprobe.exe

.PHONY: all gen test-lift runtime-objs clean bundle data FORCE
