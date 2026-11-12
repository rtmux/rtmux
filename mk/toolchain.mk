# mk/toolchain.mk — cross-toolchain detection, arch dispatch, common flags.
#
# Included by mk/def.mk (which is in turn included by every sub-Makefile),
# so anything declared here is available throughout the tree.

ifndef _MK_TOOLCHAIN_INCLUDED
_MK_TOOLCHAIN_INCLUDED := 1

# Repo root resolved from this file's location: mk/.. = repo root.
DEFMK_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
TOPDIR    := $(DEFMK_DIR)..

# Pick up the Kconfig-generated .config (CONFIG_FOO=y → Make variables).
-include $(TOPDIR)/.config

# Architecture selected by Kconfig.
_PI3_GOALS := pi3-bringup check-pi3-bringup
_PI3_ONLY := $(if $(filter $(_PI3_GOALS),$(MAKECMDGOALS)),$(if $(filter-out $(_PI3_GOALS),$(MAKECMDGOALS)),,yes))
ifneq ($(filter defconfig menuconfig guiconfig oldconfig savedefconfig,$(MAKECMDGOALS))$(_PI3_ONLY),)
  # Kconfig and the standalone Pi3 bootstrap do not need RISC-V arch setup.
  ARCH = none
  CROSS_COMPILE ?= none-
else
  export ARCH = $(if $(filter y,$(CONFIG_ARCH_AARCH64)),aarch64,riscv)
  CROSS_COMPILE ?= riscv-none-elf-
endif
export CROSS_COMPILE

# Quiet build (V=1 for verbose).
#
# The non-verbose `msg` definition wraps printf in a shell function so the
# build banner shows the displayed path relative to $(TOPDIR), not the absolute
# /Users/.../rtmux/build/... path that $(OUT) actually carries.  Call sites
# stay simple: `$(msg) CC $@` still works, the function strips the prefix.
ifeq ($(V),1)
  Q =
  msg = @:
else
  Q = @
  _TOPDIR_PREFIX := $(abspath $(TOPDIR))/
  msg = @__msg() { printf '  %-8s %s\n' "$$1" "$${2\#$(_TOPDIR_PREFIX)}"; }; __msg
  # -s silences sub-make "Nothing to be done" / command-echo noise.  $(msg)
  # printf output reaches stdout directly so banners still appear; errors
  # are not affected by -s either.
  MAKEFLAGS += --no-print-directory -s
endif

# Tool macros.
CC      = $(CROSS_COMPILE)gcc
AS      = $(CROSS_COMPILE)gas
LD      = $(CROSS_COMPILE)ld
AR      = $(CROSS_COMPILE)ar
OBJCOPY = $(CROSS_COMPILE)objcopy
OBJDUMP = $(CROSS_COMPILE)objdump
READELF = $(CROSS_COMPILE)readelf
STRIP   = $(CROSS_COMPILE)strip
NM      = $(CROSS_COMPILE)nm
SIZE    = $(CROSS_COMPILE)size

# Freestanding headers shipped with the cross compiler (stdint.h, stddef.h, ...).
# Asking gcc itself keeps the path correct across toolchains (xPack, distro,
# hand-built) and architectures without hardcoding distro layout.
GCC_INC := $(shell $(CROSS_COMPILE)gcc -print-file-name=include 2>/dev/null)

# Shared-object linker recipe. xPack's bundled riscv-none-elf-ld is built
# with --disable-shared, so it cannot link .so / .so outputs. Use LLD via
# gcc's driver; override LDSHARED if your toolchain's bundled ld supports
# -shared (e.g. riscv64-linux-gnu-gcc on Debian).
#
# The explicit -Wl,-melf64lriscv is needed because the gcc driver doesn't
# forward an arch implication to lld when call sites omit -march/-mabi
# from LDSHARED — lld would otherwise default to elf32lriscv and reject
# every rv64 input as incompatible.
LDSHARED ?= $(CC) -fuse-ld=lld -Wl,-melf64lriscv -shared
export LDSHARED

# AArch64 uses LLVM's freestanding cross target; no Linux sysroot is involved.
ifeq ($(ARCH),aarch64)
LLVM_PREFIX ?= $(if $(shell command -v clang-18 2>/dev/null),$(shell dirname $$(readlink -f $$(command -v clang-18)))/,)
CC = $(LLVM_PREFIX)clang --target=aarch64-none-elf
AS = $(CC)
LD = $(LLVM_PREFIX)ld.lld --dynamic-linker=/lib/ld.so.1 -z max-page-size=4096
AR = $(LLVM_PREFIX)llvm-ar
OBJCOPY = $(LLVM_PREFIX)llvm-objcopy
OBJDUMP = $(LLVM_PREFIX)llvm-objdump
READELF = $(LLVM_PREFIX)llvm-readelf
STRIP = $(LLVM_PREFIX)llvm-strip
NM = $(LLVM_PREFIX)llvm-nm
SIZE = $(LLVM_PREFIX)llvm-size
GCC_INC := $(shell $(LLVM_PREFIX)clang -print-resource-dir)/include
LDSHARED = $(CC) -fuse-ld=lld -Wl,-shared
endif

# Common flags.
CPPFLAGS  = -Wall -nostdinc
CPPFLAGS += $(if $(GCC_INC),-isystem $(GCC_INC))
CPPFLAGS += -I$(TOPDIR)/internal/include
CPPFLAGS += -I$(TOPDIR)/lib/common
# The LKL servers need the lkl-floor libc surface.  Refuse to build them under an
# effectively-smaller profile even from a stale/hand-edited .config that bypasses
# Kconfig's `depends on` -- so the impossible pairing never reaches link.  minimal
# or realtime wins the trim header's #if/#elif, so either being set is the fault;
# lkl/full (or no profile line) resolve to the full-enough surface.
ifeq ($(CONFIG_LKL_SERVERS),y)
ifneq ($(filter y,$(CONFIG_POSIX_PROFILE_MINIMAL) $(CONFIG_POSIX_PROFILE_REALTIME)),)
$(error CONFIG_LKL_SERVERS=y requires the lkl or full POSIX profile; minimal/realtime cannot host the LKL servers. Select POSIX_PROFILE_LKL/FULL or unset CONFIG_LKL_SERVERS)
endif
endif
# Surface the active POSIX profile to <sys/posix-profile-trim.h>.  full (the
# default) defines nothing, so headers advertise the whole option surface.
ifeq ($(CONFIG_POSIX_PROFILE_MINIMAL),y)
CPPFLAGS += -DCONFIG_POSIX_PROFILE_MINIMAL=1
endif
ifeq ($(CONFIG_POSIX_PROFILE_REALTIME),y)
CPPFLAGS += -DCONFIG_POSIX_PROFILE_REALTIME=1
endif
ifeq ($(CONFIG_POSIX_PROFILE_LKL),y)
CPPFLAGS += -DCONFIG_POSIX_PROFILE_LKL=1
endif
ifeq ($(CONFIG_ULOCK_DEBUG),y)
CPPFLAGS += -DCONFIG_ULOCK_DEBUG=1
endif
ifeq ($(CONFIG_SERVER_POOL_CANARY),y)
CPPFLAGS += -DCONFIG_SERVER_POOL_CANARY=1
endif
ASFLAGS   = -D__ASSEMBLY__ -fno-pie
CFLAGS    = -O2 -Wconversion -fno-strict-aliasing
CFLAGS   += -ffreestanding -fno-common -nostdlib
CFLAGS   += -fno-pie -fno-builtin

# Arch-specific flags shared by every target.
# -mabi=lp64d (hard-float, double precision) and -m elf64lriscv are
# required when the cross-toolchain is multilib (e.g. xPack
# riscv-none-elf-gcc, which defaults to rv32/ilp32 for both gcc and ld);
# the flags are harmless when not multilib. lp64d matches the ABI used
# by the previous Debian riscv64-linux-gnu- toolchain (its built-in
# default), so the resulting binaries are runtime-compatible with the
# rest of the RTMux port. -march=rv64g already enables the D extension.
# ASFLAGS picks the same -march/-mabi so .S files (assembled via $(CC))
# don't fall back to gcc's default baseline and reject 64-bit opcodes.
# -mno-strict-align lets gcc emit unaligned loads/stores. xPack gcc 15
# defaults to -mstrict-align, which produces byte-by-byte access sequences
# that RTMux's syscall and IPC fast paths cannot tolerate.
ifeq ($(ARCH),aarch64)
ARCH_FLAGS = -mcpu=cortex-a53 -mno-outline-atomics
else
ARCH_FLAGS = -march=rv64g -mabi=lp64d -mcmodel=medany -mno-relax -mno-strict-align
endif
CFLAGS  += $(ARCH_FLAGS)
ASFLAGS += $(ARCH_FLAGS)
ifneq ($(ARCH),aarch64)
LD      += -m elf64lriscv
endif

# Extra flags for kernel/startup code only (not libc or userland).
# Picked up by kernel/Makefile, kernel/startup/Makefile, and
# arch/$(ARCH)/Makefile. Userland and libc must NOT inherit these.
#
# -ffixed-tp reserves the tp register for the kernel. In RTMux's S-mode,
# tp holds the per-CPU pcpu pointer; the compiler must never spill to it.
# User-space libc uses tp for its own thread TLS, so the flag stays out of
# userland builds.
ifeq ($(ARCH),aarch64)
KERNEL_CFLAGS = -mgeneral-regs-only
else
KERNEL_CFLAGS = -ffixed-tp
endif

endif # _MK_TOOLCHAIN_INCLUDED
