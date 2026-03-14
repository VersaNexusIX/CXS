# =============================================================================
# Makefile - CXS: Code eXecution Scrambler
# Auto-detects OS + Arch, auto-cleans stale objects if platform changes.
# Supports: Linux, macOS, Windows (MinGW), BSD, UNIX, Android (Termux/NDK)
# =============================================================================

PROJECT := cxs
VERSION := 4.0.0

# ---- Directories -------------------------------------------------------------
SRCDIR   := src
ASMDIR   := asm
INCDIR   := include
BUILDDIR := build

# =============================================================================
# Step 1: Detect OS and Architecture
# =============================================================================
OS   := $(shell uname -s 2>/dev/null || echo Windows_NT)
ARCH := $(shell uname -m 2>/dev/null || echo x86_64)

# --- Normalize OS ---
ifeq ($(OS), Darwin)
  PLATFORM := macos
else ifeq ($(OS), Linux)
  ifneq ($(wildcard /data/data/com.termux),)
    PLATFORM := android
  else
    PLATFORM := linux
  endif
else ifeq ($(findstring BSD,$(OS)),BSD)
  PLATFORM := bsd
else ifeq ($(OS), Windows_NT)
  PLATFORM := windows
else
  PLATFORM := unix
endif

# NDK cross-compile overrides platform
ifneq ($(NDK_TOOLCHAIN),)
  PLATFORM := android
endif

# --- Normalize Arch ---
ifeq ($(ARCH),x86_64)
  ARCHNAME := x86_64
else ifeq ($(ARCH),amd64)
  ARCHNAME := x86_64
else ifeq ($(ARCH),aarch64)
  ARCHNAME := arm64
else ifeq ($(ARCH),arm64)
  ARCHNAME := arm64
else ifeq ($(ARCH),armv7l)
  ARCHNAME := arm32
else ifeq ($(ARCH),i386)
  ARCHNAME := x86
else ifeq ($(ARCH),i686)
  ARCHNAME := x86
else
  ARCHNAME := generic
endif

# =============================================================================
# Step 2: Toolchain per platform
# =============================================================================
CC      := cc
CFLAGS  := -Wall -Wextra -std=c99 -I$(INCDIR) -DCXS_VERSION=\"$(VERSION)\"
LDFLAGS :=
TARGET  := $(PROJECT)

ifeq ($(DEBUG),1)
  CFLAGS += -g -O0 -DDEBUG
else
  CFLAGS += -O2 -DNDEBUG
endif

ifeq ($(PLATFORM),macos)
  CC      := clang
  CFLAGS  += -DCXS_MACOS
  LDFLAGS += -lm
else ifeq ($(PLATFORM),linux)
  CFLAGS  += -DCXS_LINUX
  LDFLAGS += -lm
else ifeq ($(PLATFORM),android)
  CFLAGS  += -DCXS_ANDROID
  ifneq ($(NDK_TOOLCHAIN),)
    CC := $(NDK_TOOLCHAIN)/aarch64-linux-android21-clang
  endif
  LDFLAGS += -lm
else ifeq ($(PLATFORM),bsd)
  CFLAGS  += -DCXS_BSD
  LDFLAGS += -lm
else ifeq ($(PLATFORM),windows)
  CC      := x86_64-w64-mingw32-gcc
  CFLAGS  += -DCXS_WIN
  LDFLAGS += -lm
  TARGET  := $(PROJECT).exe
else
  CFLAGS  += -DCXS_UNIX
  LDFLAGS += -lm
endif

# Explicit arch flag so cxs.h can resolve register name tables correctly.
# This must come AFTER the platform block (Android is always ARM64).
ifeq ($(ARCHNAME),arm64)
  CFLAGS += -DCXS_ARCH_ARM64
else ifeq ($(ARCHNAME),x86_64)
  CFLAGS += -DCXS_ARCH_X86_64
else
  CFLAGS += -DCXS_ARCH_GENERIC
endif

# =============================================================================
# Step 3: Pick the correct ASM file for this platform+arch
#
# T6 (instruction overlap bytes) REQUIRES the assembled ASM file because
# the engine reads pattern bytes from real assembled function symbols at
# runtime.  If no assembler is available, we compile a pure-C stub that
# provides zero-length patterns (T6 is silently disabled).
# =============================================================================
NASM := $(shell which nasm 2>/dev/null)
GAS  := $(shell which as   2>/dev/null)

ASM_SRC  :=
ASM_OBJ  :=
ASM_FMT  :=
ASM_TOOL :=

ifeq ($(ARCHNAME),arm64)
  # ARM64: GAS/Clang assembles the .S directly via $(CC) -c
  ASM_SRC  := $(ASMDIR)/cxs_asm_arm64.S
  ASM_OBJ  := $(BUILDDIR)/cxs_asm_arm64.o
  ASM_TOOL := cc
else ifeq ($(ARCHNAME),x86_64)
  ifeq ($(PLATFORM),macos)
    ASM_SRC  := $(ASMDIR)/cxs_asm_x86_64.S
    ASM_OBJ  := $(BUILDDIR)/cxs_asm_x86_64.o
    ASM_TOOL := cc
  else ifeq ($(PLATFORM),windows)
    ifneq ($(NASM),)
      ASM_SRC  := $(ASMDIR)/cxs_asm_win64.asm
      ASM_OBJ  := $(BUILDDIR)/cxs_asm_win64.o
      ASM_FMT  := win64
      ASM_TOOL := nasm
    else
      $(warning [CXS] nasm not found — T6 patterns disabled on Windows)
    endif
  else
    # Linux / BSD / UNIX x86-64 — use GAS .S (no nasm needed)
    ASM_SRC  := $(ASMDIR)/cxs_asm_x86_64.S
    ASM_OBJ  := $(BUILDDIR)/cxs_asm_x86_64.o
    ASM_TOOL := cc
  endif
endif

# =============================================================================
# Step 4: Auto-clean if platform/arch fingerprint changed
# =============================================================================
FINGERPRINT      := $(PLATFORM)-$(ARCHNAME)
FINGERPRINT_FILE := $(BUILDDIR)/.cxs_fingerprint

STORED_FP := $(shell cat $(FINGERPRINT_FILE) 2>/dev/null)

ifneq ($(STORED_FP),$(FINGERPRINT))
  $(info [CXS] Platform changed: '$(STORED_FP)' -> '$(FINGERPRINT)' — auto-clean...)
  DUMMY := $(shell rm -rf $(BUILDDIR))
endif

# =============================================================================
# Step 5: Source lists
# =============================================================================
C_SRCS   := $(SRCDIR)/main.c \
             $(SRCDIR)/engine.c \
             $(SRCDIR)/loader.c \
             $(SRCDIR)/transform.c \
             $(SRCDIR)/transform2.c \
             $(SRCDIR)/emit.c

C_OBJS   := $(patsubst $(SRCDIR)/%.c,$(BUILDDIR)/%.o,$(C_SRCS))
ALL_OBJS := $(C_OBJS) $(ASM_OBJ)

$(info [CXS] platform=$(PLATFORM) arch=$(ARCHNAME) cc=$(CC) asm=$(if $(ASM_SRC),$(ASM_SRC),none))

# =============================================================================
# Build rules
# =============================================================================
.PHONY: all clean help info test stress

all: $(BUILDDIR) $(FINGERPRINT_FILE) $(TARGET)
	@echo ""
	@echo "  [CXS] Build complete -> ./$(TARGET)"
	@echo "  Run demo  : ./$(TARGET)"
	@echo "  Stress    : ./$(TARGET) --stress 20"
	@echo ""

# --- Linker ------------------------------------------------------------------
$(TARGET): $(ALL_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# --- C compilation -----------------------------------------------------------
$(BUILDDIR)/%.o: $(SRCDIR)/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

# --- ASM: ARM64 GAS (Linux/Android/macOS Apple Silicon/iOS) ------------------
$(BUILDDIR)/cxs_asm_arm64.o: $(ASMDIR)/cxs_asm_arm64.S | $(BUILDDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

# --- ASM: x86_64 Linux/BSD/UNIX ELF64 (NASM) --------------------------------
$(BUILDDIR)/cxs_asm_x86_64.o: $(ASMDIR)/cxs_asm_x86_64.S | $(BUILDDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

# --- ASM: macOS x86_64 Mach-O64 (NASM) --------------------------------------
$(BUILDDIR)/cxs_asm_macos.o: $(ASMDIR)/cxs_asm_macos.asm | $(BUILDDIR)
	$(NASM) -f macho64 -o $@ $<

# --- ASM: Windows x64 PE/COFF (NASM) -----------------------------------------
$(BUILDDIR)/cxs_asm_win64.o: $(ASMDIR)/cxs_asm_win64.asm | $(BUILDDIR)
	$(NASM) -f win64 -o $@ $<

# --- Build dir ---------------------------------------------------------------
$(BUILDDIR):
	@mkdir -p $(BUILDDIR)

# --- Write fingerprint after build dir exists --------------------------------
$(FINGERPRINT_FILE): | $(BUILDDIR)
	@echo $(FINGERPRINT) > $@

# =============================================================================
# Utility targets
# =============================================================================
clean:
	rm -rf $(BUILDDIR) $(PROJECT) $(PROJECT).exe
	@echo "  [CXS] Cleaned."

info:
	@echo "Project   : $(PROJECT) v$(VERSION)"
	@echo "Platform  : $(PLATFORM)"
	@echo "Arch      : $(ARCHNAME)"
	@echo "Compiler  : $(CC)"
	@echo "CFLAGS    : $(CFLAGS)"
	@echo "LDFLAGS   : $(LDFLAGS)"
	@echo "ASM src   : $(if $(ASM_SRC),$(ASM_SRC),<none - pure C fallback>)"
	@echo "ASM fmt   : $(if $(ASM_FMT),$(ASM_FMT),<none>)"
	@echo "Target    : $(TARGET)"

test: $(TARGET)
	@echo "=== CXS Demo ==="
	./$(TARGET)
	@echo ""
	@echo "=== Stress test (10 cycles) ==="
	./$(TARGET) --stress 10

stress: $(TARGET)
	./$(TARGET) --stress 50

help:
	@echo "Usage: make [target] [options]"
	@echo ""
	@echo "Targets:"
	@echo "  make            Build (auto-detects platform + arch)"
	@echo "  make test       Demo + 10-cycle equivalence test"
	@echo "  make stress     50-cycle stress test"
	@echo "  make clean      Remove all build artifacts"
	@echo "  make info       Show build configuration"
	@echo ""
	@echo "Options:"
	@echo "  DEBUG=1         Enable debug symbols"
	@echo ""
	@echo "Cross-compile:"
	@echo "  Android NDK:  make NDK_TOOLCHAIN=/path/to/ndk/.../bin"
	@echo "  Windows:      OS=Windows_NT make"
