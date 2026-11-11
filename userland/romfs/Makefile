#
# Makefile for romfs - Unix filesystem I/O service (RTMux)
#

include ../../mk/def.mk

INCDIR     = ../../include
ARCHINCDIR = ../../arch/$(ARCH)/include
LIBCDIR    = ../../lib/libc
LIBC_INTERNALDIR = $(LIBCDIR)/internal

CPPFLAGS += -I$(INCDIR) -I$(ARCHINCDIR) -I$(LIBCDIR) -I$(LIBC_INTERNALDIR) \
            -include $(LIBCDIR)/rtmux-libc-local.h
CFLAGS   += -std=gnu17 -g

SRCS   = main.c fs.c bio.c
OBJS   = $(SRCS:%.c=$(OBJDIR)/%.o)
TARGET = $(BUILD_BASE)/romfs

all: $(TARGET)

$(OBJDIR)/%.o: %.c
	@mkdir -p $(@D)
	$(msg) CC $@
	$(Q)$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(TARGET): $(OBJS) $(CRT0_O) $(LIBC_SO)
	@mkdir -p $(@D)
	$(msg) LD $@
	$(Q)$(LD) -nostdlib -o $@ $(CRT0_O) $(OBJS) $(LIBC_SO)

clean:
	$(Q)rm -rf $(BUILD_BASE)

.PHONY: all clean

# Auto-generated header deps (-MMD/-MP above) so a touched header — including
# the generated autoconf.h — rebuilds objects instead of leaving them stale.
-include $(OBJS:.o=.d)
