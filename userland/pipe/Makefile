#
# Makefile for userland/pipe - POSIX pipe/FIFO I/O service
#

include ../../mk/def.mk

INCDIR     = ../../include
ARCHINCDIR = ../../arch/$(ARCH)/include
LIBCDIR    = ../../lib/libc
LIBC_INTERNALDIR = $(LIBCDIR)/internal

CPPFLAGS += -I$(INCDIR) -I$(ARCHINCDIR) -I$(LIBCDIR) -I$(LIBC_INTERNALDIR) \
            -include $(LIBCDIR)/rtmux-libc-local.h
CFLAGS   += -std=gnu17 -g

PROG = $(BUILD_BASE)/pipe

all: $(PROG)

$(PROG): $(OBJDIR)/main.o $(CRT0_O) $(LIBC_SO)
	@mkdir -p $(@D)
	$(msg) LD $@
	$(Q)$(LD) -nostdlib -o $@ $(CRT0_O) $(OBJDIR)/main.o $(LIBC_SO)

$(OBJDIR)/main.o: main.c
	@mkdir -p $(@D)
	$(msg) CC $@
	$(Q)$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) -c -o $@ $<

clean:
	$(Q)rm -rf $(BUILD_BASE)

.PHONY: all clean

$(eval $(call dep-include,$(OBJDIR)/main.o))
