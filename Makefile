# ============================================================================
# Sluice — cross-platform build
#
#   make                       native build (static lib + shared lib + CLI)
#   make TARGET=linux          force Linux target
#   make TARGET=windows        cross-compile for Windows  (MinGW-w64)
#   make TARGET=macos          cross-compile for macOS    (osxcross)
#   make USE_ZIG=1 TARGET=...  cross-compile with `zig c++` (all 3 from one host)
#   make all-targets           build every target you have a toolchain for
#   make test                  build + run the CLI self-test (native only)
#   make bench                 build + run the benchmark      (native only)
#   make clean                 remove build/
#
# You can always force a compiler explicitly:  make TARGET=windows CXX=...
#
# Artifacts land in build/<target>/ :
#   lib<name>.a                       static library
#   lib<name>.so|.dylib|.dll          shared library (+ .dll.a import lib on Win)
#   <name>[.exe]                      command-line executable
# ============================================================================

NAME    := sluice
VERSION := 0.1.0

SRC_DIR := src
INC_DIR := include
LIB_SRC := $(SRC_DIR)/sluice.cpp
CLI_SRC := $(SRC_DIR)/cli.cpp

# ---- target selection ------------------------------------------------------
HOST_OS := $(shell uname -s)
ifeq ($(TARGET),)
  ifeq ($(HOST_OS),Darwin)
    TARGET := macos
  else ifneq (,$(findstring MINGW,$(HOST_OS)))
    TARGET := windows
  else ifneq (,$(findstring MSYS,$(HOST_OS)))
    TARGET := windows
  else
    TARGET := linux
  endif
endif

# ---- paths (defined BEFORE target blocks so recipes can use them) ----------
BUILD      := build/$(TARGET)
STATIC_LIB := $(BUILD)/lib$(NAME).a
SHARED_LIB := $(BUILD)/lib$(NAME)$(SO_EXT)
OBJ_SHARED := $(BUILD)/sluice.shared.o
OBJ_STATIC := $(BUILD)/sluice.static.o

COMMON_CXXFLAGS := -std=c++17 -O3 -Wall -Wextra -I$(INC_DIR) $(CXXFLAGS)

# ---- per-target toolchain, extensions, and flags ---------------------------
# Each block sets TARGET_CXX (the *default* compiler for that target) plus the
# platform-specific extensions and link flags. TARGET_CXX only becomes CXX if
# the user hasn't supplied their own (see the origin check below).
ifeq ($(TARGET),linux)
  TARGET_CXX     := g++
  SO_EXT         := .so
  EXE_EXT        :=
  FPIC           := -fPIC
  VIS            := -fvisibility=hidden
  SHARED_LDFLAGS  = -shared -Wl,-soname,lib$(NAME).so
endif

ifeq ($(TARGET),macos)
  TARGET_CXX     := o64-clang++
  SO_EXT         := .dylib
  EXE_EXT        :=
  FPIC           := -fPIC
  VIS            := -fvisibility=hidden
  SHARED_LDFLAGS  = -dynamiclib -install_name @rpath/lib$(NAME).dylib
endif

ifeq ($(TARGET),windows)
  TARGET_CXX     := x86_64-w64-mingw32-g++
  SO_EXT         := .dll
  EXE_EXT        := .exe
  FPIC           :=
  VIS            :=
  SHARED_LDFLAGS  = -shared -Wl,--out-implib,$(BUILD)/lib$(NAME).dll.a \
                    -static-libgcc -static-libstdc++
  EXE_LDFLAGS    := -static-libgcc -static-libstdc++
endif

# zig can target any platform from any host; opt in with USE_ZIG=1.
ifeq ($(USE_ZIG),1)
  ZIG_linux   := x86_64-linux-gnu
  ZIG_windows := x86_64-windows-gnu
  ZIG_macos   := x86_64-macos-none
  TARGET_CXX  := zig c++ -target $(ZIG_$(TARGET))
endif

# Honour a user-supplied CXX (command line / environment); otherwise use the
# target default. $(origin CXX)=="default" means it's only Make's built-in g++,
# which we must NOT use for cross builds.
ifeq ($(origin CXX),default)
  CXX := $(TARGET_CXX)
endif

# Recompute paths now that SO_EXT/EXE_EXT are known.
SHARED_LIB := $(BUILD)/lib$(NAME)$(SO_EXT)
EXE        := $(BUILD)/$(NAME)$(EXE_EXT)

AR ?= ar

.PHONY: all static shared exe test bench sanitize strict clean all-targets info
all: static shared exe
	@echo "==> $(TARGET): built static + shared + exe in $(BUILD)/  (CXX=$(CXX))"

static: $(STATIC_LIB)
shared: $(SHARED_LIB)
exe:    $(EXE)

$(BUILD):
	@mkdir -p $(BUILD)

# --- static library ---------------------------------------------------------
$(OBJ_STATIC): $(LIB_SRC) $(INC_DIR)/sluice.h | $(BUILD)
	$(CXX) $(COMMON_CXXFLAGS) -c $(LIB_SRC) -o $@

$(STATIC_LIB): $(OBJ_STATIC)
	$(AR) rcs $@ $^

# --- shared library ---------------------------------------------------------
$(OBJ_SHARED): $(LIB_SRC) $(INC_DIR)/sluice.h | $(BUILD)
	$(CXX) $(COMMON_CXXFLAGS) $(FPIC) $(VIS) -DSLUICE_BUILD_SHARED -c $(LIB_SRC) -o $@

$(SHARED_LIB): $(OBJ_SHARED)
	$(CXX) $(FPIC) $(SHARED_LDFLAGS) $^ -o $@

# --- executable (statically linked against the engine object) ---------------
$(EXE): $(CLI_SRC) $(OBJ_STATIC) $(INC_DIR)/sluice.h | $(BUILD)
	$(CXX) $(COMMON_CXXFLAGS) $(CLI_SRC) $(OBJ_STATIC) $(EXE_LDFLAGS) -o $@

# --- convenience ------------------------------------------------------------
test: exe
	@./$(EXE) --test
bench: exe
	@./$(EXE) --bench

# Build the CLI with ASan + UBSan (incl. float-cast-overflow) and run the
# self-test. Catches memory errors and undefined behaviour (e.g. bad float->int
# conversions) that a normal build silently tolerates.
sanitize:
	@mkdir -p $(BUILD)
	$(CXX) -std=c++17 -O1 -g -fsanitize=address,undefined,float-cast-overflow \
	  -fno-sanitize-recover=all -I$(INC_DIR) $(LIB_SRC) $(CLI_SRC) -o $(BUILD)/$(NAME)-san
	@$(BUILD)/$(NAME)-san --test

# Compile the library under strict warnings; fails the build on any warning.
strict:
	@mkdir -p $(BUILD)
	$(CXX) -std=c++17 -O2 -Wall -Wextra -Wconversion -Wstrict-aliasing=2 -Werror \
	  -I$(INC_DIR) -c $(LIB_SRC) -o $(BUILD)/sluice.strict.o && echo "strict: no warnings"

all-targets:
	@for t in linux windows macos; do \
	  $(MAKE) --no-print-directory TARGET=$$t all >/dev/null 2>&1 \
	    && echo "  [ok]   $$t" || echo "  [skip] $$t (no toolchain installed)"; \
	done

info:
	@echo "TARGET=$(TARGET)  CXX=$(CXX)  build=$(BUILD)"
	@echo "artifacts: $(STATIC_LIB)  $(SHARED_LIB)  $(EXE)"

clean:
	rm -rf build
