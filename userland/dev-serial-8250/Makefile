#
# Makefile for userland/dev-serial-8250 — 16550a UART serial driver
#

include ../../mk/def.mk

INCDIR     = ../../include
ARCHINCDIR = ../../arch/$(ARCH)/include
LIBCDIR    = ../../lib/libc
LIBC_INTERNALDIR = $(LIBCDIR)/internal

CPPFLAGS += -I$(INCDIR) -I$(ARCHINCDIR) -I$(LIBCDIR) -I$(LIBC_INTERNALDIR) \
            -include $(LIBCDIR)/rtmux-libc-local.h
CFLAGS   += -std=gnu17 -g

PROG = $(BUILD_BASE)/dev-serial-8250

all: $(PROG)

$(PROG): $(OBJDIR)/main.o $(CRT0_O) $(LIBC_SO)
	@mkdir -p $(@D)
	$(msg) LD $@
	$(Q)$(LD) -nostdlib -o $@ $(CRT0_O) $(OBJDIR)/main.o $(LIBC_SO)

$(OBJDIR)/main.o: main.c
	@mkdir -p $(@D)
	$(msg) CC $@
	$(Q)$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

clean:
	$(Q)rm -rf $(BUILD_BASE)

.PHONY: all clean

# Auto-generated header deps (-MMD/-MP above) so a touched header — including
# the generated autoconf.h — rebuilds objects instead of leaving them stale.
-include $(OBJDIR)/main.d
