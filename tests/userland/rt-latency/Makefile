#
# Realtime validation probes.
#

include ../../../mk/def.mk
include ../../../userland/lib/common.mk

CPPFLAGS += -I../../../servers/pci/include

PROGS = $(BUILD_BASE)/rt-latency $(BUILD_BASE)/timer-jitter \
        $(BUILD_BASE)/pi-inversion-xfail $(BUILD_BASE)/lkl-gate \
        $(BUILD_BASE)/lkl-measure $(BUILD_BASE)/lkl-native-test \
        $(BUILD_BASE)/lkl-multi-test $(BUILD_BASE)/lkl-fsd-read-test \
        $(BUILD_BASE)/lkl-supervisor $(BUILD_BASE)/lkl-stale-handle-test \
        $(BUILD_BASE)/lkl-contain-test $(BUILD_BASE)/lkl-runmask \
        $(BUILD_BASE)/lkl-backpressure-test $(BUILD_BASE)/lkl-iommu-probe \
        $(BUILD_BASE)/tracelogger $(BUILD_BASE)/rcu-lookup-measure \
        $(BUILD_BASE)/revoke-stress $(BUILD_BASE)/ipc-roundtrip \
        $(BUILD_BASE)/ipc-priority-donation $(BUILD_BASE)/mem-pressure \
        $(BUILD_BASE)/irq-notify $(BUILD_BASE)/tickless-idle \
        $(BUILD_BASE)/monotonic-time \
        $(BUILD_BASE)/rr-fairness \
        $(BUILD_BASE)/sporadic-budget \
        $(BUILD_BASE)/logger-restart \
        $(BUILD_BASE)/preempt-latency \
        $(BUILD_BASE)/mutex-wake \
        $(BUILD_BASE)/mutex-uncontended \
        $(BUILD_BASE)/ipc-contention \
        $(BUILD_BASE)/ist-drain-load \
        $(BUILD_BASE)/irq-latency \
        $(BUILD_BASE)/gpio-latency
SRCS = latency.c timer-jitter.c pi-inversion-xfail.c lkl-gate.c lkl-measure.c \
       lkl-native-test.c lkl-multi-test.c lkl-fsd-read-test.c lkl-supervisor.c \
       lkl-stale-handle-test.c lkl-contain-test.c lkl-runmask.c \
       lkl-backpressure-test.c lkl-iommu-probe.c tracelogger.c \
       rcu-lookup-measure.c revoke-stress.c ipc-roundtrip.c \
       ipc-priority-donation.c mem-pressure.c irq-notify.c tickless-idle.c \
       monotonic-time.c rr-fairness.c sporadic-budget.c logger-restart.c \
       preempt-latency.c mutex-wake.c mutex-uncontended.c ipc-contention.c \
       ist-drain-load.c \
       irq-latency.c gpio-latency.c
OBJS = $(SRCS:%.c=$(OBJDIR)/%.o)

all: $(PROGS)

$(OBJDIR)/%.o: %.c
	@mkdir -p $(@D)
	$(msg) CC $@
	$(Q)$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(BUILD_BASE)/rt-latency: $(OBJDIR)/latency.o $(CRT0_O) $(LIBC_SO)
	@mkdir -p $(@D)
	$(msg) LD $@
	$(Q)$(LD) -nostdlib -o $@ $(CRT0_O) $< $(LIBC_SO)

$(BUILD_BASE)/timer-jitter: $(OBJDIR)/timer-jitter.o $(CRT0_O) $(LIBC_SO)
	@mkdir -p $(@D)
	$(msg) LD $@
	$(Q)$(LD) -nostdlib -o $@ $(CRT0_O) $< $(LIBC_SO)

$(BUILD_BASE)/pi-inversion-xfail: $(OBJDIR)/pi-inversion-xfail.o $(CRT0_O) $(LIBC_SO)
	@mkdir -p $(@D)
	$(msg) LD $@
	$(Q)$(LD) -nostdlib -o $@ $(CRT0_O) $< $(LIBC_SO)

$(BUILD_BASE)/lkl-gate: $(OBJDIR)/lkl-gate.o $(CRT0_O) $(LIBC_SO)
	@mkdir -p $(@D)
	$(msg) LD $@
	$(Q)$(LD) -nostdlib -o $@ $(CRT0_O) $< $(LIBC_SO)

$(BUILD_BASE)/lkl-measure: $(OBJDIR)/lkl-measure.o $(CRT0_O) $(LIBC_SO)
	@mkdir -p $(@D)
	$(msg) LD $@
	$(Q)$(LD) -nostdlib -o $@ $(CRT0_O) $< $(LIBC_SO)

$(BUILD_BASE)/lkl-native-test: $(OBJDIR)/lkl-native-test.o $(CRT0_O) $(LIBC_SO)
	@mkdir -p $(@D)
	$(msg) LD $@
	$(Q)$(LD) -nostdlib -o $@ $(CRT0_O) $< $(LIBC_SO)

$(BUILD_BASE)/lkl-multi-test: $(OBJDIR)/lkl-multi-test.o $(CRT0_O) $(LIBC_SO)
	@mkdir -p $(@D)
	$(msg) LD $@
	$(Q)$(LD) -nostdlib -o $@ $(CRT0_O) $< $(LIBC_SO)

$(BUILD_BASE)/lkl-fsd-read-test: $(OBJDIR)/lkl-fsd-read-test.o $(CRT0_O) $(LIBC_SO)
	@mkdir -p $(@D)
	$(msg) LD $@
	$(Q)$(LD) -nostdlib -o $@ $(CRT0_O) $< $(LIBC_SO)

$(BUILD_BASE)/lkl-supervisor: $(OBJDIR)/lkl-supervisor.o $(CRT0_O) $(LIBC_SO)
	@mkdir -p $(@D)
	$(msg) LD $@
	$(Q)$(LD) -nostdlib -o $@ $(CRT0_O) $< $(LIBC_SO)

$(BUILD_BASE)/lkl-stale-handle-test: $(OBJDIR)/lkl-stale-handle-test.o $(CRT0_O) $(LIBC_SO)
	@mkdir -p $(@D)
	$(msg) LD $@
	$(Q)$(LD) -nostdlib -o $@ $(CRT0_O) $< $(LIBC_SO)

$(BUILD_BASE)/lkl-contain-test: $(OBJDIR)/lkl-contain-test.o $(CRT0_O) $(LIBC_SO)
	@mkdir -p $(@D)
	$(msg) LD $@
	$(Q)$(LD) -nostdlib -o $@ $(CRT0_O) $< $(LIBC_SO)

$(BUILD_BASE)/lkl-runmask: $(OBJDIR)/lkl-runmask.o $(CRT0_O) $(LIBC_SO)
	@mkdir -p $(@D)
	$(msg) LD $@
	$(Q)$(LD) -nostdlib -o $@ $(CRT0_O) $< $(LIBC_SO)

$(BUILD_BASE)/lkl-backpressure-test: $(OBJDIR)/lkl-backpressure-test.o $(CRT0_O) $(LIBC_SO)
	@mkdir -p $(@D)
	$(msg) LD $@
	$(Q)$(LD) -nostdlib -o $@ $(CRT0_O) $< $(LIBC_SO)

$(BUILD_BASE)/lkl-iommu-probe: $(OBJDIR)/lkl-iommu-probe.o $(CRT0_O) $(LIBC_SO)
	@mkdir -p $(@D)
	$(msg) LD $@
	$(Q)$(LD) -nostdlib -o $@ $(CRT0_O) $< $(LIBC_SO)

$(BUILD_BASE)/tracelogger: $(OBJDIR)/tracelogger.o $(CRT0_O) $(LIBC_SO)
	@mkdir -p $(@D)
	$(msg) LD $@
	$(Q)$(LD) -nostdlib -o $@ $(CRT0_O) $< $(LIBC_SO)

$(BUILD_BASE)/rcu-lookup-measure: $(OBJDIR)/rcu-lookup-measure.o $(CRT0_O) $(LIBC_SO)
	@mkdir -p $(@D)
	$(msg) LD $@
	$(Q)$(LD) -nostdlib -o $@ $(CRT0_O) $< $(LIBC_SO)

$(BUILD_BASE)/revoke-stress: $(OBJDIR)/revoke-stress.o $(CRT0_O) $(LIBC_SO)
	@mkdir -p $(@D)
	$(msg) LD $@
	$(Q)$(LD) -nostdlib -o $@ $(CRT0_O) $< $(LIBC_SO)

$(BUILD_BASE)/ipc-roundtrip: $(OBJDIR)/ipc-roundtrip.o $(CRT0_O) $(LIBC_SO)
	@mkdir -p $(@D)
	$(msg) LD $@
	$(Q)$(LD) -nostdlib -o $@ $(CRT0_O) $< $(LIBC_SO)

$(BUILD_BASE)/ipc-priority-donation: $(OBJDIR)/ipc-priority-donation.o $(CRT0_O) $(LIBC_SO)
	@mkdir -p $(@D)
	$(msg) LD $@
	$(Q)$(LD) -nostdlib -o $@ $(CRT0_O) $< $(LIBC_SO)

$(BUILD_BASE)/mem-pressure: $(OBJDIR)/mem-pressure.o $(CRT0_O) $(LIBC_SO)
	@mkdir -p $(@D)
	$(msg) LD $@
	$(Q)$(LD) -nostdlib -o $@ $(CRT0_O) $< $(LIBC_SO)

$(OBJDIR)/irq-notify.o $(OBJDIR)/irq-latency.o: rtc-probe.h alarm-device.h
$(OBJDIR)/preempt-latency.o: alarm-device.h

$(BUILD_BASE)/irq-notify: $(OBJDIR)/irq-notify.o $(CRT0_O) $(LIBC_SO)
	@mkdir -p $(@D)
	$(msg) LD $@
	$(Q)$(LD) -nostdlib -o $@ $(CRT0_O) $< $(LIBC_SO)

$(BUILD_BASE)/irq-latency: $(OBJDIR)/irq-latency.o $(CRT0_O) $(LIBC_SO)
	@mkdir -p $(@D)
	$(msg) LD $@
	$(Q)$(LD) -nostdlib -o $@ $(CRT0_O) $< $(LIBC_SO)

$(BUILD_BASE)/gpio-latency: $(OBJDIR)/gpio-latency.o $(CRT0_O) $(LIBC_SO)
	@mkdir -p $(@D)
	$(msg) LD $@
	$(Q)$(LD) -nostdlib -o $@ $(CRT0_O) $< $(LIBC_SO)

$(BUILD_BASE)/tickless-idle: $(OBJDIR)/tickless-idle.o $(CRT0_O) $(LIBC_SO)
	@mkdir -p $(@D)
	$(msg) LD $@
	$(Q)$(LD) -nostdlib -o $@ $(CRT0_O) $< $(LIBC_SO)

$(BUILD_BASE)/monotonic-time: $(OBJDIR)/monotonic-time.o $(CRT0_O) $(LIBC_SO)
	@mkdir -p $(@D)
	$(msg) LD $@
	$(Q)$(LD) -nostdlib -o $@ $(CRT0_O) $< $(LIBC_SO)

$(BUILD_BASE)/rr-fairness: $(OBJDIR)/rr-fairness.o $(CRT0_O) $(LIBC_SO)
	@mkdir -p $(@D)
	$(msg) LD $@
	$(Q)$(LD) -nostdlib -o $@ $(CRT0_O) $< $(LIBC_SO)

$(BUILD_BASE)/sporadic-budget: $(OBJDIR)/sporadic-budget.o $(CRT0_O) $(LIBC_SO)
	@mkdir -p $(@D)
	$(msg) LD $@
	$(Q)$(LD) -nostdlib -o $@ $(CRT0_O) $< $(LIBC_SO)

$(BUILD_BASE)/logger-restart: $(OBJDIR)/logger-restart.o $(CRT0_O) $(LIBC_SO)
	@mkdir -p $(@D)
	$(msg) LD $@
	$(Q)$(LD) -nostdlib -o $@ $(CRT0_O) $< $(LIBC_SO)

$(BUILD_BASE)/preempt-latency: $(OBJDIR)/preempt-latency.o $(CRT0_O) $(LIBC_SO)
	@mkdir -p $(@D)
	$(msg) LD $@
	$(Q)$(LD) -nostdlib -o $@ $(CRT0_O) $< $(LIBC_SO)

$(BUILD_BASE)/mutex-wake: $(OBJDIR)/mutex-wake.o $(CRT0_O) $(LIBC_SO)
	@mkdir -p $(@D)
	$(msg) LD $@
	$(Q)$(LD) -nostdlib -o $@ $(CRT0_O) $< $(LIBC_SO)

$(BUILD_BASE)/mutex-uncontended: $(OBJDIR)/mutex-uncontended.o $(CRT0_O) $(LIBC_SO)
	@mkdir -p $(@D)
	$(msg) LD $@
	$(Q)$(LD) -nostdlib -o $@ $(CRT0_O) $< $(LIBC_SO)

$(BUILD_BASE)/ipc-contention: $(OBJDIR)/ipc-contention.o $(CRT0_O) $(LIBC_SO)
	@mkdir -p $(@D)
	$(msg) LD $@
	$(Q)$(LD) -nostdlib -o $@ $(CRT0_O) $< $(LIBC_SO)

$(BUILD_BASE)/ist-drain-load: $(OBJDIR)/ist-drain-load.o $(CRT0_O) $(LIBC_SO)
	@mkdir -p $(@D)
	$(msg) LD $@
	$(Q)$(LD) -nostdlib -o $@ $(CRT0_O) $< $(LIBC_SO)

clean:
	$(Q)rm -rf $(BUILD_BASE)

.PHONY: all clean
