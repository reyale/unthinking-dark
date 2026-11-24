# Builds the browser replay player via Emscripten.
#
# Easiest: let CMake fetch every dependency (a pinned emsdk, Dear ImGui, xxHash, and a
# Python >= 3.10 via uv if the system has none) and run this file with their paths:
#   cmake -B ../build -DUD_WEB_VIEWER=ON && cmake --build ../build --target web_viewer
#
# By hand, with your own emsdk:
#   make -f Makefile.em EMSDK_DIR=~/emsdk PYTHON310=/path/to/python3.12
#   make -f Makefile.em serve    # build + serve at http://localhost:8080
#   make -f Makefile.em clean

OUTDIR := web

# emcc.py runs under a Python >= 3.10, which Ubuntu 20.04 doesn't ship, so find one or take it
# from the command line. em++, not emcc: recent Emscripten links the C++ runtime only for em++.
EMSDK_DIR ?= $(HOME)/emsdk
EMCC_PY   := $(EMSDK_DIR)/upstream/emscripten/em++.py
PYTHON310 ?= $(firstword $(shell which python3.12 2>/dev/null) \
                         $(shell which python3.11 2>/dev/null) \
                         $(shell which python3.10 2>/dev/null))
EMCC := $(PYTHON310) $(EMCC_PY)
# emcc runs helpers (the file packager, for the embedded shapes) with this Python too.
export EMSDK_PYTHON := $(PYTHON310)

# ---- Dependency locations ---------------------------------------------------
# FetchContent places them here after the first CMake build.

IMGUI_DIR  ?= $(firstword $(wildcard ../build/_deps/imgui-src)  $(wildcard imgui))
XXHASH_DIR ?= $(firstword $(wildcard ../build/_deps/xxhash-src) $(wildcard xxhash))
ENGINE_DIR := ../engine/src

# ---- Sources ----------------------------------------------------------------

ENGINE_SRCS := \
  $(ENGINE_DIR)/stats.cpp     \
  $(ENGINE_DIR)/fixed.cpp     \
  $(ENGINE_DIR)/rng.cpp       \
  $(ENGINE_DIR)/grid.cpp      \
  $(ENGINE_DIR)/world.cpp     \
  $(ENGINE_DIR)/snapshot.cpp  \
  $(ENGINE_DIR)/occupancy.cpp \
  $(ENGINE_DIR)/command.cpp   \
  $(ENGINE_DIR)/movement.cpp  \
  $(ENGINE_DIR)/combat.cpp    \
  $(ENGINE_DIR)/economy.cpp   \
  $(ENGINE_DIR)/territory.cpp \
  $(ENGINE_DIR)/wincheck.cpp  \
  $(ENGINE_DIR)/statehash.cpp \
  $(ENGINE_DIR)/tick.cpp      \
  $(ENGINE_DIR)/replay.cpp    \
  $(ENGINE_DIR)/replay_io.cpp \
  $(ENGINE_DIR)/frame.cpp     \
  $(ENGINE_DIR)/file_io.cpp

# The same isometric viewer as ud_viz_imgui, with the repo's shapes built in.
VIZ_SRCS := imgui_main.cpp web_version.cpp
SHAPES   := ../shapes.udshapes
VERSION  := $(shell git describe --tags --always --dirty 2>/dev/null || echo unknown)
# The version is baked in, so a new commit must rebuild even when no source changed: the stamp
# is rewritten only when the version differs, and index.html depends on it.
VERSION_STAMP := .web_version
$(shell echo '$(VERSION)' | cmp -s - $(VERSION_STAMP) || echo '$(VERSION)' > $(VERSION_STAMP))

IMGUI_SRCS := \
  $(IMGUI_DIR)/imgui.cpp                          \
  $(IMGUI_DIR)/imgui_draw.cpp                     \
  $(IMGUI_DIR)/imgui_widgets.cpp                  \
  $(IMGUI_DIR)/imgui_tables.cpp                   \
  $(IMGUI_DIR)/backends/imgui_impl_glfw.cpp       \
  $(IMGUI_DIR)/backends/imgui_impl_opengl3.cpp

ALL_SRCS := $(ENGINE_SRCS) $(VIZ_SRCS) $(IMGUI_SRCS)

# ---- Flags ------------------------------------------------------------------

# 32-bit draw indices: a big map's isometric draw list passes 65536 vertices, which the
# desktop build gets past with glDrawElementsBaseVertex, missing from WebGL.
CXXFLAGS := \
  -std=c++20 -O2                 \
  -I$(ENGINE_DIR)                \
  -I$(IMGUI_DIR)                 \
  -I$(IMGUI_DIR)/backends        \
  -I$(XXHASH_DIR)                \
  -DXXH_INLINE_ALL               \
  '-DImDrawIdx=unsigned int'     \
  -DUD_DEFAULT_SHAPES_PATH=\"/shapes.udshapes\" \
  -DUD_WEB_VERSION=\"$(VERSION)\" \
  -fexceptions                   \
  -ffp-contract=off -fno-fast-math \
  -w

LDFLAGS := \
  -s USE_GLFW=3                                              \
  -s FULL_ES3=1                                              \
  -s ALLOW_MEMORY_GROWTH=1                                   \
  -s EXPORTED_FUNCTIONS=_main,_load_replay_from_fs,_viewer_status,_viewer_control \
  -s EXPORTED_RUNTIME_METHODS=FS,UTF8ToString                \
  -fexceptions                                               \
  --embed-file $(SHAPES)@/shapes.udshapes                    \
  --shell-file shell.html

# ---- Targets ----------------------------------------------------------------

.PHONY: all clean serve check-deps

all: check-deps $(OUTDIR)/index.html

$(OUTDIR)/index.html: $(ALL_SRCS) $(wildcard *.hpp) $(SHAPES) shell.html Makefile.em \
                      $(VERSION_STAMP) | $(OUTDIR)
	$(EMCC) $(CXXFLAGS) $(LDFLAGS) $(ALL_SRCS) -o $@
	@echo ""
	@echo "Built: $(OUTDIR)/index.html"
	@echo "Serve: make -f Makefile.em serve"

$(OUTDIR):
	mkdir -p $(OUTDIR)

check-deps:
	@test -f "$(EMCC_PY)" || { \
	  echo ""; \
	  echo "error: emsdk not found at '$(EMSDK_DIR)'."; \
	  echo "Configure with -DUD_WEB_VIEWER=ON to fetch it, or pass EMSDK_DIR=<dir>."; \
	  echo ""; \
	  exit 1; }
	@test -n "$(PYTHON310)" || { \
	  echo ""; \
	  echo "error: Python 3.10+ not found (emscripten requires >= 3.10)."; \
	  echo "Configure with -DUD_WEB_VIEWER=ON (fetches one with uv), or pass PYTHON310=<python>."; \
	  echo ""; \
	  exit 1; }
	@test -f "$(IMGUI_DIR)/imgui.cpp" || { \
	  echo ""; \
	  echo "error: Dear ImGui not found at '$(IMGUI_DIR)'."; \
	  echo "Configure with -DUD_WEB_VIEWER=ON to fetch it, or pass IMGUI_DIR=<dir>."; \
	  echo ""; \
	  exit 1; }
	@test -f "$(XXHASH_DIR)/xxhash.h" || { \
	  echo ""; \
	  echo "error: xxHash not found at '$(XXHASH_DIR)'."; \
	  echo "Run this first to fetch dependencies:"; \
	  echo "  cmake -B ../build && cmake --build ../build"; \
	  echo ""; \
	  exit 1; }

serve: $(OUTDIR)/index.html
	@echo "Serving at http://localhost:8080 — press Ctrl-C to stop"
	cd $(OUTDIR) && python3 -m http.server 8080

clean:
	rm -rf $(OUTDIR) $(VERSION_STAMP)
