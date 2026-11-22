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
        $(BUILD_BASE)/ticket-lock-test $(BUILD_BASE)/tty-ldisc-test $(BUILD_BASE)/spawn-search-test $(BUILD_BASE)/cpio-parser-test $(BUILD_BASE)/spawn-fd-policy-test $(BUILD_BASE)/loader-end-test \
        $(BUILD_BASE)/serial-reply-test $(BUILD_BASE)/open-bind-test $(BUILD_BASE)/outcomes-test \
        $(BUILD_BASE)/sv39-lookup-test $(BUILD_BASE)/sv39-manip-test \
        $(BUILD_BASE)/strftime-test $(BUILD_BASE)/sh-scan-test $(BUILD_BASE)/tz-test \
        $(BUILD_BASE)/malloc-stats-test $(BUILD_BASE)/pci-args-test \
        $(BUILD_BASE)/libc-algorithms-test $(BUILD_BASE)/pgrp-table-test $(BUILD_BASE)/process-slot-test \
        $(BUILD_BASE)/sysfs-ocb-test \
        $(BUILD_BASE)/heap-arena-test $(BUILD_BASE)/kernel-alloc-test $(BUILD_BASE)/iodir-test

# Each tool compiles and links its sources in one command, so -MMD cannot give a
# per-tool dependency file (with several sources it writes one named after each
# source, into the source directory). These tools are cheap to build, so every
# one depends on every header in the trees they include from: a header edit
# rebuilds them, and no header can be forgotten from a hand-written list.
HOST_HDRS = $(wildcard $(TOPDIR)/include/*.h $(TOPDIR)/include/*/*.h \
                       $(TOPDIR)/internal/include/*.h $(TOPDIR)/internal/include/*/*.h \
                       $(TOPDIR)/kernel/include/*.h $(TOPDIR)/kernel/include/*/*.h \
                       $(TOPDIR)/servers/roottask/include/*.h \
                       $(TOPDIR)/lib/common/*.h $(TOPDIR)/lib/ldso/*.h \
                       $(TOPDIR)/lib/ldso/*/*.h $(TOPDIR)/userland/romfs/*.h \
                       $(TOPDIR)/userland/dev-serial-common/*.h \
                       $(TOPDIR)/tests/userland/outcome.h)

all: $(PROGS)

$(BUILD_BASE)/tty-ldisc-test: $(TEST_DIR)/test-tty-ldisc.c ../../lib/libc/ioserv/tty-ldisc.c \
                              ../../internal/include/sys/ioserv-ldisc.h
	@mkdir -p $(@D)
	$(msg) HOSTCC $(notdir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -idirafter ../../internal/include -o $@ $(filter %.c,$^)

$(BUILD_BASE)/spawn-search-test: $(TEST_DIR)/test-spawn-search.c ../../lib/libc/spawn/posix-spawnp.c \
                                ../../internal/include/exec-path.h ../../lib/libc/internal/exec-path-default.h
	@mkdir -p $(@D)
	$(msg) HOSTCC $(notdir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -D__hidden= -I../../lib/libc/internal -idirafter ../../internal/include \
	    -o $@ $(TEST_DIR)/test-spawn-search.c

$(BUILD_BASE)/ticket-lock-test: ../../tests/userland/lock-rank-stress/tickets.c \
                               ../../arch/riscv/include/ticket-lock.h ../../internal/include/sys/spinlock.h
	@mkdir -p $(@D)
	$(msg) HOSTCC $(notdir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -DTICKET_HOST_TEST -idirafter ../../internal/include \
	    -I../../arch/riscv/include -pthread -o $@ $<

$(BUILD_BASE)/spawn-fd-policy-test: $(TEST_DIR)/test-spawn-fd-policy.c \
                                    ../../servers/roottask/include/spawn-fd-policy.h
	@mkdir -p $(@D)
	$(msg) HOSTCC $(notdir $@)
	$(Q)$(HOST_CC) $(HOST_CPPFLAGS) -I../../internal/include -I../../include $(HOST_CFLAGS) -o $@ \
	    $(TEST_DIR)/test-spawn-fd-policy.c

$(BUILD_BASE)/loader-end-test: $(TEST_DIR)/test-loader-end.c \
                               ../../servers/roottask/include/loader-end.h
	@mkdir -p $(@D)
	$(msg) HOSTCC $(notdir $@)
	$(Q)$(HOST_CC) -I../../servers/roottask/include $(HOST_CFLAGS) -o $@ $(TEST_DIR)/test-loader-end.c

$(BUILD_BASE)/pgrp-table-test: $(TEST_DIR)/test-pgrp-table.c \
                               ../../servers/roottask/proc/pgrp-table.c \
                               ../../servers/roottask/include/pgrp-table.h
	@mkdir -p $(@D)
	$(msg) HOSTCC $(notdir $@)
	$(Q)$(HOST_CC) $(HOST_CPPFLAGS) -I../../servers/roottask/include $(HOST_CFLAGS) -o $@ \
	    $(TEST_DIR)/test-pgrp-table.c ../../servers/roottask/proc/pgrp-table.c

$(BUILD_BASE)/process-slot-test: $(TEST_DIR)/test-process-slot.c ../../servers/roottask/sys/root-process.c
	@mkdir -p $(@D)
	$(msg) HOSTCC $(notdir $@)
	$(Q)$(HOST_CC) -I../../servers/roottask/include $(HOST_CFLAGS) -pthread -o $@ $<

# roottask's /sys OCB pool, from several threads at once.
$(BUILD_BASE)/sysfs-ocb-test: $(TEST_DIR)/test-sysfs-ocb.c ../../servers/roottask/path/sysfs-ocb.c \
                              ../../servers/roottask/include/root-mutex.h \
                              ../../servers/roottask/path/sysfs-ocb.h
	@mkdir -p $(@D)
	$(msg) HOSTCC $(notdir $@)
	$(Q)$(HOST_CC) -I../../servers/roottask/path -I../../servers/roottask/include \
	    -idirafter ../../internal/include \
	    -DCONFIG_SERVER_POOL_CANARY $(HOST_CFLAGS) -pthread -o $@ \
	    $(TEST_DIR)/test-sysfs-ocb.c ../../servers/roottask/path/sysfs-ocb.c

# Outcome accounting (pass/fail/gap/skip) shared with the guest suite.
$(BUILD_BASE)/outcomes-test: $(TEST_DIR)/test-outcomes.c
	@mkdir -p $(@D)
	$(msg) HOSTCC $(notdir $@)
	$(Q)$(HOST_CC) -I../../tests/userland $(HOST_CFLAGS) -o $@ $(TEST_DIR)/test-outcomes.c

# The guest libc suites with no system dependency, linked against the RTMux
# sources, whose definitions take precedence over the host libc's. _GNU_SOURCE
# makes the host <stdlib.h> declare qsort_r, which glibc gives the POSIX.1-2024
# signature. libc-host-shim.h routes their allocations through a counting
# allocator; the host headers mark the getenv family nonnull, which the RTMux
# NULL checks would otherwise warn about. Environment strings and vectors are
# never freed by design, so the sanitizer run suppresses leaks from getenv.c
# alone (libc-algorithms.lsan).
LIBC_ALGO_SRCS = ../../tests/userland/report.c ../../tests/userland/qsort-test.c \
                 ../../lib/libc/stdlib/qsort.c ../../tests/userland/getopt-test.c \
                 ../../lib/libc/stdlib/getopt.c ../../tests/userland/env-test.c \
                 ../../lib/libc/stdlib/getenv.c
LIBC_ALGO_FLAGS = -I../../tests/userland -I$(TEST_DIR) -idirafter ../../internal/include \
                  -idirafter ../../lib/libc/internal -D_GNU_SOURCE -DLIBC_HOST_TEST \
                  -include $(TEST_DIR)/libc-host-shim.h -pthread -Wno-nonnull-compare
$(BUILD_BASE)/libc-algorithms-test: $(TEST_DIR)/test-libc-algorithms.c $(LIBC_ALGO_SRCS) \
                                     ../../tests/userland/test.h $(TEST_DIR)/libc-host-shim.h \
                                     ../../lib/libc/internal/env-lock.h
	@mkdir -p $(@D)
	$(msg) HOSTCC $(notdir $@)
	$(Q)$(HOST_CC) $(LIBC_ALGO_FLAGS) $(HOST_CFLAGS) -o $@ \
	    $(TEST_DIR)/test-libc-algorithms.c $(LIBC_ALGO_SRCS)

# The PCI server's command line, with the libc getopt_long it runs on the guest.
$(BUILD_BASE)/pci-args-test: $(TEST_DIR)/test-pci-args.c ../../servers/pci/args.c \
                             ../../servers/pci/args.h ../../lib/libc/stdlib/getopt.c
	@mkdir -p $(@D)
	$(msg) HOSTCC $(notdir $@)
	$(Q)$(HOST_CC) -I../../servers/pci -idirafter ../../internal/include \
	    -idirafter ../../lib/libc/internal -D_GNU_SOURCE -DLIBC_HOST_TEST \
	    -pthread $(HOST_CFLAGS) -o $@ $(TEST_DIR)/test-pci-args.c \
	    ../../servers/pci/args.c ../../lib/libc/stdlib/getopt.c

# The shared serial service is compiled against the tree's own headers with the
# IPC calls stubbed (the host libc has no ssp/string.h, so fortify is off).
SERIAL_DIR = ../../userland/dev-serial-common
NOTIFY_LIST = ../../lib/libc/ioserv/notify-list.c ../../lib/libc/ioserv/tty-ldisc.c
$(BUILD_BASE)/serial-reply-test: $(TEST_DIR)/test-serial-reply.c $(SERIAL_DIR)/serial-service.h \
                                 $(NOTIFY_LIST)
	@mkdir -p $(@D)
	$(msg) HOSTCC $(notdir $@)
	$(Q)$(HOST_CC) -I$(SERIAL_DIR) -I../../include -I../../internal/include $(HOST_CFLAGS) \
	    -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -o $@ $(TEST_DIR)/test-serial-reply.c \
	    $(NOTIFY_LIST)

IOSERV_DIR = ../../lib/libc/ioserv
$(BUILD_BASE)/open-bind-test: $(TEST_DIR)/test-open-bind.c $(IOSERV_DIR)/open-bind.c \
                             $(IOSERV_DIR)/api.h $(IOSERV_DIR)/internal.h
	@mkdir -p $(@D)
	$(msg) HOSTCC $(notdir $@)
	$(Q)$(HOST_CC) -I$(IOSERV_DIR) -I../../lib/libc/internal -I../../include \
	    -I../../internal/include $(HOST_CFLAGS) -Dcalloc=fail_binding_alloc -o $@ $(filter %.c,$^)

# libc strftime against the host headers, renamed so the host copy is never
# called. When the host compiler supports AddressSanitizer a second build runs
# with it, so a byte touched outside the arguments fails host-tests.
STRFTIME_SRCS = ../../lib/libc/posix/strftime.c ../../lib/libc/posix/time-conv.c
STRFTIME_FLAGS = -I../../lib/libc/internal -I../../tests/userland -Dstrftime=rtmux_strftime
STRFTIME_DEPS = $(TEST_DIR)/test-strftime.c $(STRFTIME_SRCS) \
                ../../lib/libc/internal/time-conv.h ../../tests/userland/strftime-cases.h
$(BUILD_BASE)/strftime-test: $(STRFTIME_DEPS)
	@mkdir -p $(@D)
	$(msg) HOSTCC $(notdir $@)
	$(Q)$(HOST_CC) $(STRFTIME_FLAGS) $(HOST_CFLAGS) -o $@ \
	    $(TEST_DIR)/test-strftime.c $(STRFTIME_SRCS)

# libc localtime, mktime and the TZif reader against the host libc as reference
# (Europe/Berlin over 10^5 times, plus fixture zone files). The time functions
# are renamed, in one translation unit each, so the host copies are never
# called; with AddressSanitizer a second build also traps stray accesses.
TZ_DIR_SRC = ../../lib/libc/posix
TZ_RENAME = -Dgmtime_r=rtmux_gmtime_r -Dgmtime=rtmux_gmtime \
            -Dlocaltime_r=rtmux_localtime_r -Dlocaltime=rtmux_localtime \
            -Dmktime=rtmux_mktime -Dtimegm=rtmux_timegm -Dtzset=rtmux_tzset \
            -Dtzname=rtmux_tzname -Dtimezone=rtmux_timezone \
            -Ddaylight=rtmux_daylight -Ddifftime=rtmux_difftime \
            -Dasctime_r=rtmux_asctime_r -Dasctime=rtmux_asctime \
            -Dctime_r=rtmux_ctime_r -Dctime=rtmux_ctime
TZ_CFLAGS = -I../../lib/libc/internal -D__hidden= -DLIBC_HOST_TEST
TZ_DEPS = $(TEST_DIR)/test-tz.c $(TZ_DIR_SRC)/localtime.c $(TZ_DIR_SRC)/tzfile.c \
          $(TZ_DIR_SRC)/time-conv.c ../../lib/libc/internal/tzfile.h \
          ../../lib/libc/internal/time-conv.h
# $(1): sanitizer flags, $(2): output.
define build-tz-test
	$(Q)$(HOST_CC) $(TZ_CFLAGS) $(TZ_RENAME) $(HOST_CFLAGS) $(1) -c \
	    $(TZ_DIR_SRC)/localtime.c -o $(2).localtime.o
	$(Q)$(HOST_CC) $(TZ_CFLAGS) $(HOST_CFLAGS) $(1) -c \
	    $(TZ_DIR_SRC)/tzfile.c -o $(2).tzfile.o
	$(Q)$(HOST_CC) $(TZ_CFLAGS) $(HOST_CFLAGS) $(1) -c \
	    $(TZ_DIR_SRC)/time-conv.c -o $(2).time-conv.o
	$(Q)$(HOST_CC) $(HOST_CFLAGS) $(1) -o $(2) $(TEST_DIR)/test-tz.c \
	    $(2).localtime.o $(2).tzfile.o $(2).time-conv.o
	$(Q)rm -f $(2).localtime.o $(2).tzfile.o $(2).time-conv.o
endef
# The zone files the image ships, compiled once for the tests to read.
TZ_ZONES = $(BUILD_BASE)/zoneinfo
$(TZ_ZONES)/.stamp: ../../scripts/gen-zoneinfo.sh ../../scripts/zoneinfo-2025b.zi
	@mkdir -p $(@D)
	$(msg) ZONEINFO $(notdir $(TZ_ZONES))
	$(Q)../../scripts/gen-zoneinfo.sh $(TZ_ZONES) && touch $@

$(BUILD_BASE)/tz-test: $(TZ_DEPS)
	@mkdir -p $(@D)
	$(msg) HOSTCC $(notdir $@)
	$(call build-tz-test,,$@)

# Without sanitizer support the "binary" is a script that reports the skip.
$(BUILD_BASE)/tz-asan-test: $(TZ_DEPS)
	@mkdir -p $(@D)
	$(msg) HOSTCC $(notdir $@)
	$(Q)if echo 'int main(void){return 0;}' | \
	    $(HOST_CC) $(SAN_FLAGS) -x c - -o $@.probe 2>/dev/null; then \
	    rm -f $@.probe; $(MAKE) --no-print-directory tz-asan-real; \
	else \
	    rm -f $@.probe; \
	    printf '#!/bin/sh\necho "tz-test: SKIP ASan (unsupported)"\n' > $@; \
	    chmod +x $@; \
	fi

.PHONY: tz-asan-real
tz-asan-real:
	$(call build-tz-test,$(SAN_FLAGS),$(BUILD_BASE)/tz-asan-test)

# The allocator's counters, with the real malloc-core.c linked in. The same source
# without the counters (MALLOC_NO_STATS) is the baseline of host-bench-malloc.
MALLOC_FLAGS = -I../../lib/common -I../../lib/libc/alloc -idirafter ../../include \
               -D__hidden= -DMALLOC_HOST_TEST -pthread
MALLOC_DEPS = $(TEST_DIR)/test-malloc-stats.c ../../lib/libc/alloc/malloc-core.c \
              ../../lib/libc/alloc/malloc-core.h ../../include/sys/mallocstats.h
$(BUILD_BASE)/malloc-stats-test: $(MALLOC_DEPS)
	@mkdir -p $(@D)
	$(msg) HOSTCC $(notdir $@)
	$(Q)$(HOST_CC) $(MALLOC_FLAGS) $(HOST_CFLAGS) -o $@ \
	    $(TEST_DIR)/test-malloc-stats.c ../../lib/libc/alloc/malloc-core.c

# Build the allocator against the host mmap ABI and the directory client
# against RTMux's on-wire ABI. Only the client's allocators are redirected.
$(BUILD_BASE)/iodir-alloc.o: ../../lib/libc/alloc/malloc-core.c ../../lib/libc/alloc/malloc-core.h
	@mkdir -p $(@D)
	$(Q)$(HOST_CC) $(MALLOC_FLAGS) $(HOST_CFLAGS) -c $< -o $@

IODIR_FLAGS = -I../../lib/libc/internal -I../../lib/libc/alloc -I../../include \
              -I../../internal/include -I../../lib/common $(HOST_CFLAGS) -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0
$(BUILD_BASE)/iodir-test: $(TEST_DIR)/test-iodir.c ../../lib/libc/posix/iodir.c \
                          $(BUILD_BASE)/iodir-alloc.o ../../lib/libc/internal/connect.h
	@mkdir -p $(@D)
	$(msg) HOSTCC $(notdir $@)
	$(Q)$(HOST_CC) $(IODIR_FLAGS) -include ../../lib/libc/rtmux-libc-local.h \
	    -Dmalloc=__crt_malloc -Dcalloc=__crt_calloc -Dfree=__crt_free \
	    -Dstrdup=dir_test_strdup -c ../../lib/libc/posix/iodir.c -o $@.o
	$(Q)$(HOST_CC) $(IODIR_FLAGS) -o $@ $< $@.o $(BUILD_BASE)/iodir-alloc.o -pthread

# Roottask's malloc arena (servers/roottask/sys/heap.c) under the real allocator.
HEAP_DEPS = $(TEST_DIR)/test-heap-arena.c ../../lib/libc/alloc/malloc-core.c \
            ../../servers/roottask/sys/heap.c \
            ../../servers/roottask/include/heap-stats.h ../../servers/roottask/include/root-mutex.h
$(BUILD_BASE)/kernel-alloc-test: $(TEST_DIR)/test-kernel-alloc.c ../../kernel/object/alloc.c
	@mkdir -p $(@D)
	$(msg) HOSTCC $(notdir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -std=c11 -o $@ $<

$(BUILD_BASE)/heap-arena-test: $(HEAP_DEPS)
	@mkdir -p $(@D)
	$(msg) HOSTCC $(notdir $@)
	$(Q)$(HOST_CC) $(MALLOC_FLAGS) -I../../servers/roottask/include \
	    -DCONFIG_ROOTTASK_HEAP_SIZE=4194304 $(HOST_CFLAGS) -Wl,--wrap=pthread_mutex_lock -o $@ \
	    $(TEST_DIR)/test-heap-arena.c ../../lib/libc/alloc/malloc-core.c \
	    ../../servers/roottask/sys/heap.c

$(BUILD_BASE)/malloc-nostats-bench: $(MALLOC_DEPS)
	@mkdir -p $(@D)
	$(msg) HOSTCC $(notdir $@)
	$(Q)$(HOST_CC) $(MALLOC_FLAGS) -DMALLOC_NO_STATS $(HOST_CFLAGS) -o $@ \
	    $(TEST_DIR)/test-malloc-stats.c ../../lib/libc/alloc/malloc-core.c

# Alternating runs of the two builds, each the best of 7 passes of 2e6 malloc/free
# pairs, and a verdict against the noise of the samples (scripts/bench-malloc.sh).
host-bench-malloc: $(BUILD_BASE)/malloc-stats-test $(BUILD_BASE)/malloc-nostats-bench
	@../../scripts/bench-malloc.sh $(BUILD_BASE)/malloc-stats-test \
	    $(BUILD_BASE)/malloc-nostats-bench
.PHONY: host-bench-malloc

$(BUILD_BASE)/strftime-asan-test: $(STRFTIME_DEPS)
	@mkdir -p $(@D)
	$(msg) HOSTCC $(notdir $@)
	$(Q)if echo 'int main(void){return 0;}' | \
	    $(HOST_CC) $(SAN_FLAGS) -x c - -o $@.probe 2>/dev/null; then ok=1; else ok=0; fi; \
	rm -f $@.probe; \
	if [ $$ok = 1 ]; then \
	    $(HOST_CC) $(STRFTIME_FLAGS) $(HOST_CFLAGS) $(SAN_FLAGS) -o $@ \
	        $(TEST_DIR)/test-strftime.c $(STRFTIME_SRCS); \
	else \
	    printf '#!/bin/sh\necho "strftime-test: SKIP ASan (unsupported)"\n' > $@; \
	    chmod +x $@; \
	fi

# The toolbox shell's quote and substitution scanner, against a table of quoting
# cases whose fields were made by a reference shell (gen-sh-quote-cases.py).
SH_DIR = ../../userland/utils
$(BUILD_BASE)/sh-scan-test: $(TEST_DIR)/test-sh-scan.c $(SH_DIR)/sh-scan.c $(SH_DIR)/sh-scan.h \
                            $(TEST_DIR)/sh-quote-cases.h
	@mkdir -p $(@D)
	$(msg) HOSTCC $(notdir $@)
	$(Q)$(HOST_CC) -I$(SH_DIR) -I$(TEST_DIR) $(HOST_CFLAGS) -o $@ \
	    $(TEST_DIR)/test-sh-scan.c $(SH_DIR)/sh-scan.c

# The shell itself as a host program, built with the hook that lets a test send
# it a signal at the one moment a read could lose it (tests/host/sh-host-shim.c).
# Subshells run this same binary. The format-truncation warning is about the
# line editor's path buffers, which the target compiler does not report.
$(BUILD_BASE)/sh-host: $(TEST_DIR)/sh-host-shim.c $(SH_DIR)/sh.c $(SH_DIR)/sh-scan.c $(SH_DIR)/sh-parse.c \
                       $(SH_DIR)/sh-scan.h $(SH_DIR)/sh-parse.h ../../userland/include/toolbox.h
	@mkdir -p $(@D)
	$(msg) HOSTCC $(notdir $@)
	$(Q)$(HOST_CC) -std=gnu11 -D_GNU_SOURCE -DEOK=0 \
	    -DSH_TEST_AWAIT_WINDOW=sh_test_await_window \
	    '-DSH_SELF_PATH="$(abspath $@)"' -I../../userland/include -I$(SH_DIR) \
	    $(HOST_CFLAGS) -Wno-format-truncation -o $@ \
	    $(TEST_DIR)/sh-host-shim.c $(SH_DIR)/sh.c $(SH_DIR)/sh-scan.c $(SH_DIR)/sh-parse.c

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

# Sv39 leaf walk and superpage address arithmetic, from the kernel's header.
$(BUILD_BASE)/sv39-lookup-test: $(TEST_DIR)/test-sv39-lookup.c \
                              ../../internal/include/pte-op.h \
                              ../../arch/riscv/include/sv39-lookup.h \
                              ../../arch/riscv/include/cpu-paging.h
	@mkdir -p $(@D)
	$(msg) HOSTCC $(notdir $@)
	$(Q)$(HOST_CC) -I../../arch/riscv/include -iquote ../../internal/include $(HOST_CFLAGS) -o $@ \
	    $(TEST_DIR)/test-sv39-lookup.c

# The loop behind cpu_pte_manipulate: error mapping and the progress it leaves.
$(BUILD_BASE)/sv39-manip-test: $(TEST_DIR)/test-sv39-manip.c \
                              ../../internal/include/pte-op.h \
                              ../../arch/riscv/include/sv39-lookup.h \
                              ../../arch/riscv/include/cpu-paging.h
	@mkdir -p $(@D)
	$(msg) HOSTCC $(notdir $@)
	$(Q)$(HOST_CC) -I../../arch/riscv/include -iquote ../../internal/include $(HOST_CFLAGS) -o $@ \
	    $(TEST_DIR)/test-sv39-manip.c

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
            $(BUILD_BASE)/ticket-lock-test $(BUILD_BASE)/tty-ldisc-test $(BUILD_BASE)/spawn-search-test $(BUILD_BASE)/cpio-parser-test $(BUILD_BASE)/spawn-fd-policy-test $(BUILD_BASE)/loader-end-test \
            $(BUILD_BASE)/serial-reply-test $(BUILD_BASE)/open-bind-test $(BUILD_BASE)/outcomes-test \
            $(BUILD_BASE)/sv39-lookup-test $(BUILD_BASE)/sv39-manip-test \
            $(BUILD_BASE)/strftime-test $(BUILD_BASE)/strftime-asan-test \
            $(BUILD_BASE)/sh-scan-test $(BUILD_BASE)/libc-algorithms-test \
            $(BUILD_BASE)/tz-test $(BUILD_BASE)/tz-asan-test $(TZ_ZONES)/.stamp \
            $(BUILD_BASE)/malloc-stats-test $(BUILD_BASE)/pci-args-test \
            $(BUILD_BASE)/pgrp-table-test $(BUILD_BASE)/process-slot-test $(BUILD_BASE)/heap-arena-test $(BUILD_BASE)/kernel-alloc-test \
            $(BUILD_BASE)/sysfs-ocb-test $(BUILD_BASE)/sh-host $(BUILD_BASE)/iodir-test
	$(Q)$<
	$(Q)$(BUILD_BASE)/iodir-test
	$(Q)$(BUILD_BASE)/cap-table-test
	$(Q)$(BUILD_BASE)/bootstrap-manifest-test
	$(Q)$(BUILD_BASE)/elf-range-test
	$(Q)$(BUILD_BASE)/ldso-hash-test
	$(Q)$(BUILD_BASE)/ldso-page-test
	$(Q)$(BUILD_BASE)/ldso-parse-test
	$(Q)$(BUILD_BASE)/ldso-path-test
	$(Q)$(BUILD_BASE)/cpio-parser-test
	$(Q)$(BUILD_BASE)/tty-ldisc-test
	$(Q)$(BUILD_BASE)/spawn-search-test
	$(Q)$(BUILD_BASE)/ticket-lock-test
	$(Q)$(BUILD_BASE)/spawn-fd-policy-test
	$(Q)$(BUILD_BASE)/loader-end-test
	$(Q)$(BUILD_BASE)/pgrp-table-test
	$(Q)$(BUILD_BASE)/process-slot-test
	$(Q)$(BUILD_BASE)/sysfs-ocb-test
	$(Q)$(BUILD_BASE)/serial-reply-test
	$(Q)$(BUILD_BASE)/open-bind-test
	$(Q)$(BUILD_BASE)/outcomes-test >/dev/null
	$(Q)$(BUILD_BASE)/sv39-lookup-test
	$(Q)$(BUILD_BASE)/sv39-manip-test
	$(Q)$(BUILD_BASE)/strftime-test
	$(Q)$(BUILD_BASE)/strftime-asan-test
	$(Q)$(BUILD_BASE)/sh-scan-test
	$(Q)if command -v dash >/dev/null; then \
	    $(PYTHON) $(TEST_DIR)/gen-sh-quote-cases.py --check; fi
	$(Q)$(PYTHON) $(TEST_DIR)/test-sh-host.py $(abspath $(BUILD_BASE)/sh-host) \
	    $(abspath ../../tests/userland/sh-posix/gaps.sh)
	$(Q)if command -v dash >/dev/null; then \
	    SH=dash dash ../../tests/userland/sh-posix/gaps.sh >/dev/null; fi
	$(Q)RTMUX_TZDIR=$(abspath $(TZ_ZONES))/ $(BUILD_BASE)/tz-test
	$(Q)RTMUX_TZDIR=$(abspath $(TZ_ZONES))/ $(BUILD_BASE)/tz-asan-test
	$(Q)$(BUILD_BASE)/malloc-stats-test
	$(Q)$(BUILD_BASE)/heap-arena-test
	$(Q)$(BUILD_BASE)/kernel-alloc-test
	$(Q)$(BUILD_BASE)/pci-args-test
	$(Q)out=$$($(BUILD_BASE)/libc-algorithms-test) || { printf '%s\n' "$$out"; exit 1; }
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
# tuple loop. STDIO_DEBUG turns on vfprintf's queued-buffer assertion.
VFP_STDIO   = ../../lib/libc/stdio/vfprintf.c ../../lib/libc/stdio/vsnprintf.c \
              ../../lib/libc/stdio/fvwrite.c
VFP_BUILTIN := $(shell $(CLANG) -print-file-name=include)
VFP_NS      = -nostdinc -isystem $(VFP_BUILTIN) -DSTDIO_DEBUG \
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
	printf '  %-8s %s\n' SANITIZE libc-algorithms; \
	$(CLANG) $(SAN_FLAGS) $(LIBC_ALGO_FLAGS) -o $(FUZZ_OUT)/san-libc-algorithms \
	    $(TEST_DIR)/test-libc-algorithms.c $(LIBC_ALGO_SRCS); \
	LSAN_OPTIONS=suppressions=$(TEST_DIR)/libc-algorithms.lsan \
	    $(FUZZ_OUT)/san-libc-algorithms >/dev/null; \
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
	$(Q)rm -f $(PROGS) $(BUILD_BASE)/strftime-asan-test $(BUILD_BASE)/tz-asan-test \
	    $(BUILD_BASE)/sh-host $(BUILD_BASE)/malloc-nostats-bench
	$(Q)rm -rf $(TZ_ZONES)
	$(Q)rm -rf $(FUZZ_OUT)

.PHONY: all clean host-tests cap-table-test host-sanitize host-fuzz

# After every recipe rule, so these extra prerequisites follow the sources and $< in
# the recipes above still names the first source file.
$(PROGS): $(HOST_HDRS)
