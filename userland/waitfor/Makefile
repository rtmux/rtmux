#
# Makefile for userland/waitfor — small early-boot path wait helper.
#

include ../../mk/def.mk
include ../lib/common.mk

PROG = $(BUILD_BASE)/waitfor
OBJS = $(OBJDIR)/waitfor.o

all: $(PROG)

$(OBJDIR)/waitfor.o: main.c
	@mkdir -p $(@D)
	$(msg) CC $@
	$(Q)$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) -c -o $@ $<

$(PROG): $(OBJS) $(CRT0_O) $(LIBC_SO)
	@mkdir -p $(@D)
	$(msg) LD $@
	$(Q)$(LD) -nostdlib -o $@ $(CRT0_O) $(OBJS) $(LIBC_SO)

clean:
	$(Q)rm -f $(PROG) $(OBJS) $(OBJS:.o=.d)

.PHONY: all clean

$(eval $(call dep-include,$(OBJS)))
