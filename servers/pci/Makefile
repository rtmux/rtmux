#
# Makefile for servers/pci — PCI configuration server (RTMux)
#
# Uses ECAM (Enhanced Configuration Access Mechanism) for PCIe config space.
# Primary target: SiFive Unmatched (FU740, PCIe Gen3 x8).
#
# SPDX-License-Identifier: Apache-2.0
#

include ../../mk/def.mk

INCDIR     = ../../include
ARCHINCDIR = ../../arch/$(ARCH)/include
LIBCDIR    = ../../lib/libc
LIBC_INTERNALDIR = $(LIBCDIR)/internal

CPPFLAGS += -I. -Iinclude -I$(INCDIR) -I$(ARCHINCDIR) -I$(LIBCDIR) \
            -I$(LIBC_INTERNALDIR) -include $(LIBCDIR)/rtmux-libc-local.h
CFLAGS   += -std=gnu17 -g

SRCS = hwres-client.c globals.c config.c scan.c enum.c resource.c \
       bridge.c client.c attach.c main.c ecam.c \
       dwmsi.c dw-atu.c
OBJS = $(SRCS:%.c=$(OBJDIR)/%.o)

TARGET = $(BUILD_BASE)/pci-server

all: $(TARGET)

$(OBJDIR)/%.o: %.c server.h pcidrvr.h globals.h slogcodes.h
	@mkdir -p $(@D)
	$(msg) CC $@
	$(Q)$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(TARGET): $(OBJS) $(CRT0_O) $(LIBC_SO)
	@mkdir -p $(@D)
	$(msg) LD $@
	$(Q)$(LD) -nostdlib -o $@ $(CRT0_O) $(OBJS) $(LIBC_SO)

clean:
	$(Q)rm -rf $(BUILD_BASE)

.PHONY: all clean
