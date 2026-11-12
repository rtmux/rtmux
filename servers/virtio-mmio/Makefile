include ../../mk/def.mk

INCDIR     = ../../include
ARCHINCDIR = ../../arch/$(ARCH)/include
LIBCDIR    = ../../lib/libc
LIBC_INTERNALDIR = $(LIBCDIR)/internal

CPPFLAGS += -I. -I$(INCDIR) -I$(ARCHINCDIR) -I$(LIBCDIR) \
            -I$(LIBC_INTERNALDIR) -include $(LIBCDIR)/rtmux-libc-local.h
CFLAGS   += -std=gnu17 -g

TARGET = $(BUILD_BASE)/virtio-mmio
OBJS = $(OBJDIR)/main.o

all: $(TARGET)

$(OBJDIR)/%.o: %.c
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
