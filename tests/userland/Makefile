#
# Makefile for tests/userland — Syscall Conformance Test Suite
#

.DEFAULT_GOAL := all

include ../../mk/def.mk
include ../../mk/recurse.mk
include ../../userland/lib/common.mk

SRCS = main.c timers.c sync.c printf-test.c setjmp-test.c signals-test.c \
       notifications.c vfs.c posix-options.c capgate.c trace-negative.c \
       user-ptr-negative.c serial-unblock.c leak.c
OBJS = $(SRCS:%.c=$(OBJDIR)/%.o)
ifeq ($(ARCH),aarch64)
OBJS += $(OBJDIR)/fp-aarch64.o
endif
PROG = $(BUILD_BASE)/syscall_testing
CRYPTTEST = $(BUILD_BASE)/crypttest

# Recurse into rt-latency/ as a real phony prerequisite so the jobserver
# can schedule it in parallel with $(PROG).  The wildcard guard keeps a
# stale checkout that lacks the subdir from breaking `make` and `make clean`.
ifneq ($(wildcard rt-latency/Makefile),)
$(call RTMUX_RECURSE_DIRS,rt-latency)
RT_LATENCY_TARGETS = rt-latency
RT_LATENCY_CLEAN   = clean-rt-latency
endif
ifneq ($(wildcard lock-rank-stress/Makefile),)
$(call RTMUX_RECURSE_DIRS,lock-rank-stress)
LOCK_RANK_STRESS_TARGETS = lock-rank-stress
LOCK_RANK_STRESS_CLEAN   = clean-lock-rank-stress
endif
ifneq ($(wildcard namespace-roundtrip/Makefile),)
$(call RTMUX_RECURSE_DIRS,namespace-roundtrip)
NAMESPACE_ROUNDTRIP_TARGETS = namespace-roundtrip
NAMESPACE_ROUNDTRIP_CLEAN   = clean-namespace-roundtrip
endif

SYNC_IGNORE = $(BUILD_BASE)/sync-ignore
SLOW_SERVER = $(BUILD_BASE)/slow-server

PROBE = $(BUILD_BASE)/setuid-probe
PROBE_PLAIN = $(BUILD_BASE)/setuid-probe-plain
PROBE_SETGID = $(BUILD_BASE)/setgid-probe
PRELOAD_PROBE = $(BUILD_BASE)/preload-probe.so

all: $(PROG) $(CRYPTTEST) $(PROBE) $(PROBE_PLAIN) $(PROBE_SETGID) $(PRELOAD_PROBE) \
     $(SYNC_IGNORE) $(SLOW_SERVER) \
     $(RT_LATENCY_TARGETS) \
     $(LOCK_RANK_STRESS_TARGETS) $(NAMESPACE_ROUNDTRIP_TARGETS)

$(OBJDIR)/%.o: %.c test.h
	@mkdir -p $(@D)
	$(msg) CC $@
	$(Q)$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/%.o: %.S
	@mkdir -p $(@D)
	$(msg) AS $@
	$(Q)$(CC) $(ASFLAGS) -c -o $@ $<

$(PROG): $(OBJS) $(CRT0_O) $(LIBC_SO)
	@mkdir -p $(@D)
	$(msg) LD $@
	$(Q)$(LD) -nostdlib -o $@ $(CRT0_O) $(OBJS) $(LIBC_SO)

$(CRYPTTEST): $(OBJDIR)/crypttest.o $(CRT0_O) $(LIBC_SO)
	@mkdir -p $(@D)
	$(msg) LD $@
	$(Q)$(LD) -nostdlib -o $@ $(CRT0_O) $(OBJDIR)/crypttest.o $(LIBC_SO)

# One source, three binaries: the install step gives setuid-probe the set-uid
# bit, setgid-probe the set-gid bit, and the plain twin neither.
$(PROBE) $(PROBE_PLAIN) $(PROBE_SETGID): \
        $(OBJDIR)/setuid-probe.o $(CRT0_O) $(LIBC_SO)
	@mkdir -p $(@D)
	$(msg) LD $@
	$(Q)$(LD) -nostdlib -o $@ $(CRT0_O) $(OBJDIR)/setuid-probe.o $(LIBC_SO)

# A real LD_PRELOAD object, linked with lld like libc.so.
$(OBJDIR)/preload-probe.o: preload-probe.c
	@mkdir -p $(@D)
	$(msg) CC $@
	$(Q)$(CC) $(CPPFLAGS) $(CFLAGS) -fPIC -c -o $@ $<

$(PRELOAD_PROBE): $(OBJDIR)/preload-probe.o
	@mkdir -p $(@D)
	$(msg) LD $@
	$(Q)$(LDSHARED) -nostdlib -Wl,-soname,preload-probe.so -o $@ $<

$(SYNC_IGNORE): $(OBJDIR)/sync-ignore.o $(CRT0_O) $(LIBC_SO)
	@mkdir -p $(@D)
	$(msg) LD $@
	$(Q)$(LD) -nostdlib -o $@ $(CRT0_O) $(OBJDIR)/sync-ignore.o $(LIBC_SO)

$(SLOW_SERVER): $(OBJDIR)/slow-server.o $(CRT0_O) $(LIBC_SO)
	@mkdir -p $(@D)
	$(msg) LD $@
	$(Q)$(LD) -nostdlib -o $@ $(CRT0_O) $(OBJDIR)/slow-server.o $(LIBC_SO)

# Each entry is a path under $(BUILD_BASE) to a built binary; the install dst in
# $(INITRAMFS_DIR)/bin/ is just the basename. crypttest is built but not shipped.
INSTALL_BIN = syscall_testing \
              setuid-probe \
              setuid-probe-plain \
              setgid-probe \
              sync-ignore \
              slow-server \
              rt-latency/rt-latency \
              rt-latency/timer-jitter \
              rt-latency/pi-inversion-xfail \
              rt-latency/lkl-gate \
              rt-latency/lkl-measure \
              rt-latency/lkl-native-test \
              rt-latency/lkl-multi-test \
              rt-latency/lkl-fsd-read-test \
              rt-latency/lkl-supervisor \
              rt-latency/lkl-stale-handle-test \
              rt-latency/lkl-contain-test \
              rt-latency/lkl-runmask \
              rt-latency/lkl-backpressure-test \
              rt-latency/lkl-iommu-probe \
              rt-latency/rcu-lookup-measure \
              rt-latency/revoke-stress \
              rt-latency/tracelogger \
              rt-latency/ipc-roundtrip \
              rt-latency/ipc-priority-donation \
              rt-latency/mem-pressure \
              rt-latency/irq-notify \
              rt-latency/irq-latency \
              rt-latency/gpio-latency \
              rt-latency/tickless-idle \
              rt-latency/monotonic-time \
              rt-latency/rr-fairness \
              rt-latency/sporadic-budget \
              rt-latency/logger-restart \
              rt-latency/preempt-latency \
              rt-latency/mutex-wake \
              rt-latency/mutex-uncontended \
              rt-latency/ipc-contention \
              rt-latency/ist-drain-load \
              lock-rank-stress/lock-rank-stress \
              namespace-roundtrip/namespace-roundtrip

install: all
	$(Q)mkdir -p $(INITRAMFS_DIR)/bin
	$(Q)for src in $(INSTALL_BIN); do \
		dst=$(INITRAMFS_DIR)/bin/$${src##*/}; \
		cp $(BUILD_BASE)/$$src $$dst || exit 1; \
		$(STRIP) --strip-unneeded $$dst || exit 1; \
	done
	$(Q)chmod 4755 $(INITRAMFS_DIR)/bin/setuid-probe
	$(Q)chmod 2755 $(INITRAMFS_DIR)/bin/setgid-probe
	$(Q)mkdir -p $(INITRAMFS_DIR)/lib
	$(Q)cp $(PRELOAD_PROBE) $(INITRAMFS_DIR)/lib/preload-probe.so
	$(Q)$(STRIP) --strip-unneeded $(INITRAMFS_DIR)/lib/preload-probe.so
	$(Q)cp sh-posix/test.sh $(INITRAMFS_DIR)/bin/sh-posix-test
	$(Q)cp toolbox/test.sh $(INITRAMFS_DIR)/bin/toolbox-test

clean: $(RT_LATENCY_CLEAN) $(LOCK_RANK_STRESS_CLEAN) $(NAMESPACE_ROUNDTRIP_CLEAN)
	$(Q)rm -rf $(BUILD_BASE)

.PHONY: all install clean
