#
# SMP lock-order stress probe.
#

include ../../../mk/def.mk
include ../../../userland/lib/common.mk

PROG = $(BUILD_BASE)/lock-rank-stress
OBJS = $(OBJDIR)/stress.o

all: $(PROG)

$(OBJDIR)/%.o: %.c
	@mkdir -p $(@D)
	$(msg) CC $@
	$(Q)$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) -c -o $@ $<

$(PROG): $(OBJS) $(CRT0_O) $(LIBC_SO)
	@mkdir -p $(@D)
	$(msg) LD $@
	$(Q)$(LD) -nostdlib -o $@ $(CRT0_O) $(OBJS) $(LIBC_SO)

clean:
	$(Q)rm -rf $(BUILD_BASE)

.PHONY: all clean

$(eval $(call dep-include,$(OBJS)))
