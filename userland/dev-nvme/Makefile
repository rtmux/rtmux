#
# Makefile for userland/dev-nvme — NVMe block driver (read-only, v0.24).
#

include ../../mk/def.mk

INCDIR     = ../../include
ARCHINCDIR = ../../arch/$(ARCH)/include
LIBCDIR    = ../../lib/libc
LIBC_INTERNALDIR = $(LIBCDIR)/internal

PCIDIR     = ../../servers/pci/include

CPPFLAGS += -I$(PCIDIR) -I$(INCDIR) -I$(ARCHINCDIR) -I$(LIBCDIR) \
            -I$(LIBC_INTERNALDIR) -include $(LIBCDIR)/rtmux-libc-local.h
CFLAGS   += -std=gnu17 -g
ifeq ($(CONFIG_MEASURE_IST_DRAIN),y)
CFLAGS   += -DCONFIG_MEASURE_IST_DRAIN=1
endif

SRCS = main.c nvme-ctrl.c nvme-admin.c nvme-io.c nvme-service.c nvme-ist.c \
       gpt.c pci-client.c pci-strerror.c
OBJS = $(SRCS:%.c=$(OBJDIR)/%.o)
PROG = $(BUILD_BASE)/dev-nvme

all: $(PROG)

$(PROG): $(OBJS) $(CRT0_O) $(LIBC_SO)
	@mkdir -p $(@D)
	$(msg) LD $@
	$(Q)$(LD) -nostdlib -o $@ $(CRT0_O) $(OBJS) $(LIBC_SO)

gpt-test: $(BUILD_BASE)/gpt-test
	$(BUILD_BASE)/gpt-test

$(BUILD_BASE)/gpt-test: gpt-test.c gpt.c gpt.h crc32.h
	@mkdir -p $(@D)
	$(msg) HOSTCC gpt-test
	$(Q)cc -std=gnu17 -Wall -Wextra -g -O0 -I. -o $@ gpt-test.c gpt.c

$(OBJDIR)/%.o: %.c
	@mkdir -p $(@D)
	$(msg) CC $@
	$(Q)$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) -c -o $@ $<

clean:
	$(Q)rm -rf $(BUILD_BASE)

.PHONY: all clean gpt-test

$(eval $(call dep-include,$(OBJS)))
