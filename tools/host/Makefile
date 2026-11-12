#
# Makefile for tools/host/ - utilities that run on the build host.
#
# Compiled with the HOST compiler (cc), not the cross-compiler.
# Pull in mk/build.mk directly (skip toolchain.mk) so we don't inherit
# the cross-compile -nostdinc / -ffreestanding flags.
#

TOPDIR := ../..
include $(TOPDIR)/mk/build.mk

MODDIR := tools/host

# Quiet by default; V=1 enables full command echo.
ifeq ($(V),1)
  Q :=
  msg = @:
else
  Q := @
  msg = @printf '  %-8s %s\n'
endif

HOST_CC      ?= cc
PYTHON       ?= python3
ROMFS_DIR     = ../../userland/romfs
TEST_DIR      = ../../tests/host
HOST_CFLAGS   = -Wall -Wextra -O2
HOST_CPPFLAGS = -I$(ROMFS_DIR) -I../../servers/roottask/include -I../../kernel/include
LDSO_CPPFLAGS = -I../../lib/common -I../../lib/ldso -I../../lib/ldso/riscv \
                -I../../lib/ldso/machine

PATH_NORMALIZE = ../../servers/roottask/path/normalize.c
PROGS = $(BUILD_BASE)/mkasmoff $(BUILD_BASE)/mkromfs $(BUILD_BASE)/treeromfs \
        $(BUILD_BASE)/path-normalize-test $(BUILD_BASE)/cap-table-test \
        $(BUILD_BASE)/bootstrap-manifest-test $(BUILD_BASE)/elf-range-test \
        $(BUILD_BASE)/ldso-hash-test $(BUILD_BASE)/ldso-page-test \
        $(BUILD_BASE)/ldso-parse-test $(BUILD_BASE)/ldso-path-test \
        $(BUILD_BASE)/cpio-parser-test $(BUILD_BASE)/spawn-fd-policy-test \
        $(BUILD_BASE)/serial-reply-test

all: $(PROGS)

$(BUILD_BASE)/spawn-fd-policy-test: $(TEST_DIR)/test-spawn-fd-policy.c \
                                    ../../servers/roottask/include/spawn-fd-policy.h
	@mkdir -p $(@D)
	$(msg) HOSTCC $(notdir $@)
	$(Q)$(HOST_CC) $(HOST_CPPFLAGS) -I../../internal/include -I../../include $(HOST_CFLAGS) -o $@ \
	    $(TEST_DIR)/test-spawn-fd-policy.c

# The shared serial service is compiled against the tree's own headers with the
# IPC calls stubbed (the host libc has no ssp/string.h, so fortify is off).
SERIAL_DIR = ../../userland/dev-serial-common
$(BUILD_BASE)/serial-reply-test: $(TEST_DIR)/test-serial-reply.c $(SERIAL_DIR)/serial-service.h
	@mkdir -p $(@D)
	$(msg) HOSTCC $(notdir $@)
	$(Q)$(HOST_CC) -I$(SERIAL_DIR) -I../../include -I../../internal/include $(HOST_CFLAGS) \
	    -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -o $@ $(TEST_DIR)/test-serial-reply.c

cap-table-test: $(BUILD_BASE)/cap-table-test
	$(Q)$<

$(BUILD_BASE)/mkasmoff: mkasmoff.c
	@mkdir -p $(@D)
	$(msg) HOSTCC $(notdir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -o $@ $<

$(BUILD_BASE)/mkromfs: mkromfs.c $(ROMFS_DIR)/fs.h
	@mkdir -p $(@D)
	$(msg) HOSTCC $(notdir $@)
	$(Q)$(HOST_CC) $(HOST_CPPFLAGS) $(HOST_CFLAGS) -o $@ $<

$(BUILD_BASE)/treeromfs: treeromfs.c $(ROMFS_DIR)/fs.h
	@mkdir -p $(@D)
	$(msg) HOSTCC $(notdir $@)
	$(Q)$(HOST_CC) $(HOST_CPPFLAGS) $(HOST_CFLAGS) -o $@ $<

$(BUILD_BASE)/path-normalize-test: $(TEST_DIR)/test-path-normalize.c $(PATH_NORMALIZE) \
                                   ../../servers/roottask/include/path-normalize.h
	@mkdir -p $(@D)
	$(msg) HOSTCC $(notdir $@)
	$(Q)$(HOST_CC) $(HOST_CPPFLAGS) $(HOST_CFLAGS) -o $@ \
	    $(TEST_DIR)/test-path-normalize.c $(PATH_NORMALIZE)

$(BUILD_BASE)/cap-table-test: $(TEST_DIR)/test-cap-table.c ../../kernel/cap/table.c \
                              ../../kernel/include/cap-table.h
	@mkdir -p $(@D)
	$(msg) HOSTCC $(notdir $@)
	$(Q)$(HOST_CC) $(HOST_CPPFLAGS) $(HOST_CFLAGS) -pthread -o $@ \
	    $(TEST_DIR)/test-cap-table.c ../../kernel/cap/table.c

$(BUILD_BASE)/bootstrap-manifest-test: $(TEST_DIR)/test-bootstrap-manifest.c \
                                       ../../servers/roottask/sys/bootstrap-manifest.c \
                                       ../../servers/roottask/include/bootstrap-manifest.h
	@mkdir -p $(@D)
	$(msg) HOSTCC $(notdir $@)
	$(Q)$(HOST_CC) $(HOST_CPPFLAGS) $(HOST_CFLAGS) \
	    -DRTMUX_BOOTSTRAP_MANIFEST_TEST -I../../servers/roottask/include \
	    -o $@ $(TEST_DIR)/test-bootstrap-manifest.c \
	    ../../servers/roottask/sys/bootstrap-manifest.c

$(BUILD_BASE)/elf-range-test: $(TEST_DIR)/test-elf-range.c ../../kernel/include/elf-range.h
	@mkdir -p $(@D)
	$(msg) HOSTCC $(notdir $@)
	$(Q)$(HOST_CC) $(HOST_CPPFLAGS) $(HOST_CFLAGS) -o $@ \
	    $(TEST_DIR)/test-elf-range.c

$(BUILD_BASE)/ldso-hash-test: $(TEST_DIR)/test-ldso-hash.c ../../lib/ldso/hash.c \
                              ../../lib/ldso/hash.h
	@mkdir -p $(@D)
	$(msg) HOSTCC $(notdir $@)
	$(Q)$(HOST_CC) $(LDSO_CPPFLAGS) $(HOST_CFLAGS) -o $@ \
	    $(TEST_DIR)/test-ldso-hash.c ../../lib/ldso/hash.c

$(BUILD_BASE)/ldso-page-test: $(TEST_DIR)/test-ldso-page.c ../../lib/ldso/page.h \
                              ../../lib/common/align.h
	@mkdir -p $(@D)
	$(msg) HOSTCC $(notdir $@)
	$(Q)$(HOST_CC) $(LDSO_CPPFLAGS) $(HOST_CFLAGS) -o $@ \
	    $(TEST_DIR)/test-ldso-page.c

$(BUILD_BASE)/ldso-parse-test: $(TEST_DIR)/test-ldso-parse.c ../../lib/ldso/parse.c \
                               ../../lib/ldso/parse.h
	@mkdir -p $(@D)
	$(msg) HOSTCC $(notdir $@)
	$(Q)$(HOST_CC) $(LDSO_CPPFLAGS) $(HOST_CFLAGS) -o $@ \
	    $(TEST_DIR)/test-ldso-parse.c ../../lib/ldso/parse.c

$(BUILD_BASE)/ldso-path-test: $(TEST_DIR)/test-ldso-path.c ../../lib/ldso/path.c \
                              ../../lib/ldso/ldso-path.h
	@mkdir -p $(@D)
	$(msg) HOSTCC $(notdir $@)
	$(Q)$(HOST_CC) $(LDSO_CPPFLAGS) $(HOST_CFLAGS) -o $@ \
	    $(TEST_DIR)/test-ldso-path.c ../../lib/ldso/path.c

$(BUILD_BASE)/cpio-parser-test: $(TEST_DIR)/test-cpio-parser.c ../../kernel/startup/lib/cpio.c \
                                ../../internal/include/sys/cpio.h
	@mkdir -p $(@D)
	$(msg) HOSTCC $(notdir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -idirafter ../../include \
	    -idirafter ../../internal/include -o $@ \
	    $(TEST_DIR)/test-cpio-parser.c ../../kernel/startup/lib/cpio.c

host-tests: $(BUILD_BASE)/path-normalize-test $(BUILD_BASE)/cap-table-test \
            $(BUILD_BASE)/bootstrap-manifest-test $(BUILD_BASE)/elf-range-test \
            $(BUILD_BASE)/ldso-hash-test $(BUILD_BASE)/ldso-page-test \
            $(BUILD_BASE)/ldso-parse-test $(BUILD_BASE)/ldso-path-test \
            $(BUILD_BASE)/cpio-parser-test $(BUILD_BASE)/spawn-fd-policy-test \
            $(BUILD_BASE)/serial-reply-test
	$(Q)$<
	$(Q)$(BUILD_BASE)/cap-table-test
	$(Q)$(BUILD_BASE)/bootstrap-manifest-test
	$(Q)$(BUILD_BASE)/elf-range-test
	$(Q)$(BUILD_BASE)/ldso-hash-test
	$(Q)$(BUILD_BASE)/ldso-page-test
	$(Q)$(BUILD_BASE)/ldso-parse-test
	$(Q)$(BUILD_BASE)/ldso-path-test
	$(Q)$(BUILD_BASE)/cpio-parser-test
	$(Q)$(BUILD_BASE)/spawn-fd-policy-test
	$(Q)$(BUILD_BASE)/serial-reply-test
	$(Q)HOST_CC="$(HOST_CC)" $(PYTHON) $(TEST_DIR)/test-ipc-deferred-error.py
	$(Q)$(PYTHON) $(TEST_DIR)/test-capability-namespace-doc.py
	$(Q)$(PYTHON) $(TEST_DIR)/test-management-ipc-lock.py
	$(Q)HOST_CC="$(HOST_CC)" $(PYTHON) -I $(TEST_DIR)/test-lkl-iommu-contract.py
	$(Q)! find ../../kernel/include -path '*+*' | grep .

# Standalone cross-assembly check of the LKL virtio fence helpers (fence w,o /
# fence o,o). Deliberately not part of host-tests, which must run without a
# RISC-V compiler. RISCV_CC has no default and the target fails if it is unset or
# the compiler fails, e.g.:
#   make host-lkl-fence-asm RISCV_CC='riscv-none-elf-gcc -march=rv64g -mabi=lp64d'
host-lkl-fence-asm:
	$(Q)RISCV_CC="$(RISCV_CC)" $(PYTHON) -I $(TEST_DIR)/test-lkl-iommu-contract.py --riscv-asm
.PHONY: host-lkl-fence-asm

# Fuzz and sanitizer targets over the pure parsers reachable from untrusted input
# (the boot initramfs CPIO, client paths, and the on-disk GPT). They are optional:
# host-tests stays portable under plain cc, while these light up only when clang
# is present (for host-sanitize) and the libFuzzer runtime links (for host-fuzz),
# and skip cleanly otherwise. Each harness links either the standalone driver (for
# corpus replay under the sanitizers) or libFuzzer, never both.
CLANG        ?= clang
FUZZ_DIR      = $(TEST_DIR)/fuzz
FUZZ_CORPUS   = $(FUZZ_DIR)/corpus
FUZZ_SECONDS ?= 10
FUZZ_OUT      = $(BUILD_BASE)/fuzz
SAN_FLAGS     = -g -O1 -fno-omit-frame-pointer -fsanitize=address,undefined \
                -fno-sanitize-recover=all
FUZZ_FLAGS    = -g -O1 -fsanitize=fuzzer,address,undefined \
                -fno-sanitize-recover=all

CPIO_SRC = ../../kernel/startup/lib/cpio.c
CPIO_INC = -idirafter ../../include -idirafter ../../internal/include
PATH_SRC = ../../servers/roottask/path/normalize.c
PATH_INC = -I../../servers/roottask/include
GPT_SRC  = ../../userland/dev-nvme/gpt.c
GPT_INC  = -I../../userland/dev-nvme
XFER_SRC = ../../kernel/xfer/check.c
XFER_INC = -I../../tests/host/fuzz/xfer-stubs
# xferlen builds with NDEBUG so its debug-only address-space assertion, which
# reaches into per-thread and per-CPU state, drops out and only the length-sum
# path under test remains against the stub.
XFERLEN_SRC = ../../kernel/xfer/length.c
XFERLEN_INC = -I../../tests/host/fuzz/xfer-stubs -DNDEBUG
# elf-validate.c is the paging-free half of the initramfs ELF loader, so it
# builds host-side against the real kernel and public headers with no stubs; the
# harness supplies the single cpu_elf_machine_valid external.
ELF_SRC = ../../kernel/module/elf-validate.c
# elf-stubs holds a sys/platform.h that shadows the RTMux LP64 typedefs with host
# stdint plus a sys/elf.h shim that force-includes the real RTMux header (glibc
# ships its own sys/elf.h that -idirafter would otherwise let win). The real
# kernel and public headers then come in via -idirafter so host libc still wins
# for everything the stubs do not cover.
# -fno-sanitize=alignment: the validator reads Elf64 fields at image-supplied
# file offsets, so a fuzzed e_shoff yields unaligned but in-bounds loads (a
# loader-wide property the RV64 target defines and emulates); alignment UB is
# orthogonal to the bounds logic and AddressSanitizer still catches real OOB.
ELF_INC = -I../../tests/host/fuzz/elf-stubs \
          -idirafter ../../kernel/include -idirafter ../../include \
          -idirafter ../../internal/include -fno-sanitize=alignment
# romfs fs.c is a self-contained userland decoder over plain host headers; the
# harness supplies its one I/O seam (bread/brelse/bio_init), so only the romfs
# source directory is on the include path.
ROMFS_SRC = ../../userland/romfs/fs.c
ROMFS_INC = -I../../userland/romfs
# build_lkl_path was split into its own pure-libc leaf so it builds host-side
# against plain headers; the harness supplies the mount prefix.
BUILDPATH_SRC = ../../servers/lkl-fsd/pathbuild.c
BUILDPATH_INC = -I../../servers/lkl-fsd
# connectpath.c reads only struct _io_connect; the connectpath-stubs sys/iomsg.h
# mirrors that layout on host types so the leaf builds without the RTMux message
# headers.
CONNECTPATH_SRC = ../../servers/lkl/connectpath.c
CONNECTPATH_INC = -I../../tests/host/fuzz/connectpath-stubs

# The libc pure parsers build against the host standard headers (the harness
# links the RTMux implementation under test, so the host libc copy is never
# pulled in), hence no include flags.
FNMATCH_SRC   = ../../lib/libc/stdlib/fnmatch.c
STRTOX_SRC    = ../../lib/libc/stdlib/strtoul.c ../../lib/libc/stdlib/strtol.c
GETSUBOPT_SRC = ../../lib/libc/stdlib/getsubopt.c

# __vfprintf pulls in the RTMux FILE layout and its stdio internals, which
# collide with the host stdio headers, so its three sources build in a pure
# RTMux header world (-nostdinc plus the RTMux include dirs and the libc build
# prelude) and link against the host-world harness by plain vsnprintf prototype.
# The vfprintf target is therefore built on its own below, not through the shared
# tuple loop.
VFP_STDIO   = ../../lib/libc/stdio/vfprintf.c ../../lib/libc/stdio/vsnprintf.c \
              ../../lib/libc/stdio/fvwrite.c
VFP_BUILTIN := $(shell $(CLANG) -print-file-name=include)
VFP_NS      = -nostdinc -isystem $(VFP_BUILTIN) \
              -include ../../lib/libc/rtmux-libc-local.h \
              -I../../include -I../../internal/include -I../../lib/libc/stdio

# One "name incflags src" tuple per parser. The quotes survive make expansion,
# so each recipe re-splits a tuple back into a name plus its argument words.
FUZZ_TARGETS = "cpio $(CPIO_INC) $(CPIO_SRC)" \
               "path $(PATH_INC) $(PATH_SRC)" \
               "gpt $(GPT_INC) $(GPT_SRC)" \
               "xfer $(XFER_INC) $(XFER_SRC)" \
               "xferlen $(XFERLEN_INC) $(XFERLEN_SRC)" \
               "elf $(ELF_INC) $(ELF_SRC)" \
               "romfs $(ROMFS_INC) $(ROMFS_SRC)" \
               "buildpath $(BUILDPATH_INC) $(BUILDPATH_SRC)" \
               "connectpath $(CONNECTPATH_INC) $(CONNECTPATH_SRC)" \
               "fnmatch $(FNMATCH_SRC)" \
               "strtox $(STRTOX_SRC)" \
               "getsubopt $(GETSUBOPT_SRC)"

host-sanitize:
	@if ! command -v $(CLANG) >/dev/null 2>&1; then \
	    printf '  %-8s %s\n' SKIP "host-sanitize (no $(CLANG))"; exit 0; fi; \
	set -e; mkdir -p $(FUZZ_OUT); \
	for t in $(FUZZ_TARGETS); do \
	    set -- $$t; name=$$1; shift; \
	    printf '  %-8s %s\n' SANITIZE $$name; \
	    $(CLANG) $(SAN_FLAGS) $$@ -o $(FUZZ_OUT)/san-$$name \
	        $(FUZZ_DIR)/fuzz-$$name.c $(FUZZ_DIR)/standalone-main.c; \
	    $(FUZZ_OUT)/san-$$name $(FUZZ_CORPUS)/$$name/*; \
	done; \
	printf '  %-8s %s\n' SANITIZE vfprintf; \
	for s in $(VFP_STDIO); do \
	    $(CLANG) $(SAN_FLAGS) $(VFP_NS) -c $$s \
	        -o $(FUZZ_OUT)/vfp-san-`basename $$s .c`.o; \
	done; \
	$(CLANG) $(SAN_FLAGS) -o $(FUZZ_OUT)/san-vfprintf \
	    $(FUZZ_DIR)/fuzz-vfprintf.c $(FUZZ_DIR)/standalone-main.c \
	    $(FUZZ_OUT)/vfp-san-*.o; \
	$(FUZZ_OUT)/san-vfprintf $(FUZZ_CORPUS)/vfprintf/*; \
	printf '  %-8s %s\n' SANITIZE oob-selftest; \
	$(CLANG) $(SAN_FLAGS) -o $(FUZZ_OUT)/san-oob \
	    $(FUZZ_DIR)/fuzz-oob-selftest.c $(FUZZ_DIR)/standalone-main.c; \
	if $(FUZZ_OUT)/san-oob $(FUZZ_CORPUS)/path/plain >/dev/null 2>&1; then \
	    echo "host-sanitize: planted OOB was not caught by the sanitizer" >&2; \
	    exit 1; \
	fi; \
	printf '  %-8s %s\n' OK "planted OOB trapped by the sanitizer"

host-fuzz:
	@if ! command -v $(CLANG) >/dev/null 2>&1; then \
	    printf '  %-8s %s\n' SKIP "host-fuzz (no $(CLANG))"; exit 0; fi; \
	mkdir -p $(FUZZ_OUT); \
	printf 'int LLVMFuzzerTestOneInput(const unsigned char*d,unsigned long s){return 0;}\n' \
	    > $(FUZZ_OUT)/fzprobe.c; \
	if ! $(CLANG) -fsanitize=fuzzer -o $(FUZZ_OUT)/fzprobe $(FUZZ_OUT)/fzprobe.c \
	     >/dev/null 2>&1; then \
	    printf '  %-8s %s\n' SKIP "host-fuzz (no libFuzzer runtime)"; exit 0; fi; \
	set -e; \
	for t in $(FUZZ_TARGETS); do \
	    set -- $$t; name=$$1; shift; \
	    printf '  %-8s %s\n' FUZZ "$$name ($(FUZZ_SECONDS)s)"; \
	    $(CLANG) $(FUZZ_FLAGS) $$@ -o $(FUZZ_OUT)/fuzz-$$name \
	        $(FUZZ_DIR)/fuzz-$$name.c; \
	    $(FUZZ_OUT)/fuzz-$$name -max_total_time=$(FUZZ_SECONDS) \
	        -print_final_stats=1 $(FUZZ_CORPUS)/$$name; \
	done; \
	printf '  %-8s %s\n' FUZZ "vfprintf ($(FUZZ_SECONDS)s)"; \
	for s in $(VFP_STDIO); do \
	    $(CLANG) $(FUZZ_FLAGS) $(VFP_NS) -c $$s \
	        -o $(FUZZ_OUT)/vfp-fz-`basename $$s .c`.o; \
	done; \
	$(CLANG) $(FUZZ_FLAGS) -o $(FUZZ_OUT)/fuzz-vfprintf \
	    $(FUZZ_DIR)/fuzz-vfprintf.c $(FUZZ_OUT)/vfp-fz-*.o; \
	$(FUZZ_OUT)/fuzz-vfprintf -max_total_time=$(FUZZ_SECONDS) \
	    -print_final_stats=1 $(FUZZ_CORPUS)/vfprintf

clean:
	$(Q)rm -f $(PROGS)
	$(Q)rm -rf $(FUZZ_OUT)

.PHONY: all clean host-tests cap-table-test host-sanitize host-fuzz
