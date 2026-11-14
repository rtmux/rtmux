#
# Makefile for servers/logger — system event logger (RTMux)
#
# Provides /dev/slog — circular buffer for system log messages.
# Drivers write slog event records directly to /dev/slog.
#
# SPDX-License-Identifier: Apache-2.0
#

include ../../mk/def.mk

INCDIR     = ../../include
ARCHINCDIR = ../../arch/$(ARCH)/include
LIBCDIR    = ../../lib/libc
LIBC_INTERNALDIR = $(LIBCDIR)/internal

CPPFLAGS += -I. -I$(INCDIR) -I$(ARCHINCDIR) -I$(LIBCDIR) -I$(LIBC_INTERNALDIR) \
            -include $(LIBCDIR)/rtmux-libc-local.h
CFLAGS   += -std=gnu17 -g

SRCS = main.c logger.c externs.c \
       io-read.c io-write.c io-notify.c io-unblock.c io-msg.c
OBJS = $(SRCS:%.c=$(OBJDIR)/%.o)

TARGET = $(BUILD_BASE)/logger

all: $(TARGET)

$(OBJDIR)/%.o: %.c externs.h struct.h service.h
	@mkdir -p $(@D)
	$(msg) CC $@
	$(Q)$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) -c -o $@ $<

$(TARGET): $(OBJS) $(CRT0_O) $(LIBC_SO)
	@mkdir -p $(@D)
	$(msg) LD $@
	$(Q)$(LD) -nostdlib -o $@ $(CRT0_O) $(OBJS) $(LIBC_SO)

clean:
	$(Q)rm -rf $(BUILD_BASE)

.PHONY: all clean

$(eval $(call dep-include,$(OBJS)))
