# mk/build.mk - Out-of-source build directory.
#
# All generated files (object files, binaries, libraries, the initramfs
# tree, the cpio archive, the romfs/nvme images) land under $(OUT),
# mirroring the source-tree layout.
#
# MODDIR auto-derives from $(CURDIR) -- the sub-Makefile at
# $(TOPDIR)/foo/bar gets MODDIR = foo/bar and BUILD_BASE =
# $(OUT)/foo/bar. Object files conventionally go under $(BUILD_BASE).
#
# Recipes that create files under $(BUILD_BASE) should mkdir -p $(@D)
# themselves (cheap, idempotent) rather than depending on a directory
# target, because GNU make's order-only prereqs and pattern rules don't
# play well with the late MODDIR assignment.

ifndef _MK_BUILD_INCLUDED
_MK_BUILD_INCLUDED := 1

# TOPDIR comes from mk/toolchain.mk; default to a co-located build/ dir.
# Canonicalize relative OUT values (e.g. `make OUT=out`) against TOPDIR so
# every sub-make resolves to the same absolute path.
# $(abspath out) from inside arch/riscv would re-root at the sub-make's
# CURDIR and produce arch/riscv/out -- not what the
# user meant.  Anchoring at $(TOPDIR) gives the same answer everywhere.
OUT ?= $(abspath $(TOPDIR))/build
ifeq ($(filter /%,$(OUT)),)
override OUT := $(abspath $(TOPDIR)/$(OUT))
endif
BUILD_DIR := $(OUT)
export OUT BUILD_DIR

# Per-component output base. Auto-derives from $(CURDIR); a sub-Makefile
# may override MODDIR before including build.mk if it bypasses mk/def.mk
# (host-side tools that skip the cross toolchain do this).
#
# Use $(realpath ...) on both sides so a symlinked checkout, a sub-make
# invoked through a symlink, or a CURDIR that includes a different
# symlink spelling than TOPDIR all canonicalize identically.  Three
# cases need separate handling:
#   1. CURDIR == TOPDIR (top-level Makefile)     -> MODDIR is empty,
#                                                   BUILD_BASE == OUT.
#   2. CURDIR under TOPDIR (any sub-Makefile)    -> MODDIR is the
#                                                   relative path.
#   3. CURDIR neither (build invoked from /tmp,  -> error: silently
#      stale tree, missing realpath target)        leaking outputs into
#                                                  $(OUT)/<abs>
#                                                  is worse than failing.
_TOPDIR_REAL := $(realpath $(TOPDIR))
_CURDIR_REAL := $(realpath $(CURDIR))
ifeq ($(_CURDIR_REAL),$(_TOPDIR_REAL))
  MODDIR ?=
else
  MODDIR ?= $(patsubst $(_TOPDIR_REAL)/%,%,$(_CURDIR_REAL))
  ifeq ($(MODDIR),$(_CURDIR_REAL))
    $(error mk/build.mk: $(_CURDIR_REAL) is not under $(_TOPDIR_REAL); \
      set MODDIR explicitly or invoke from inside the source tree)
  endif
endif
BUILD_BASE = $(if $(MODDIR),$(OUT)/$(MODDIR),$(OUT))
OBJDIR     = $(BUILD_BASE)

# ---------------------------------------------------------------------
# Conventional build-output paths for common dependencies. Sub-Makefiles
# link/include against these names instead of redefining the paths.
# ---------------------------------------------------------------------

# libc
LIBC_SRC    = $(abspath $(TOPDIR))/lib/libc
LIBC_BUILD  = $(OUT)/lib/libc
LIBC_A      = $(LIBC_BUILD)/libc.a
LIBC_SO    = $(LIBC_BUILD)/libc.so
CRT0_O      = $(LIBC_BUILD)/crt0.o

# ldso
LDSO_BUILD = $(OUT)/lib/ldso
LDSO_SO    = $(LDSO_BUILD)/ld-rtmux.so

# kernel binary (the linked ELF lives next to its object tree)
KERNEL_BIN = $(OUT)/arch/$(ARCH)/rtmux-kernel

# roottask server binary
ROOTTASK_BIN = $(OUT)/servers/roottask/roottask

# Initramfs tree + cpio archive
INITRAMFS_DIR  = $(OUT)/initramfs
INITRAMFS_CPIO = $(OUT)/initramfs.cpio

endif # _MK_BUILD_INCLUDED
