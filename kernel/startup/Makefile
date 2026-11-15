#
# Makefile for arch-independent startup code.
# Linked by arch/riscv/Makefile.
#

include ../../mk/def.mk
include objects.mk

CPPFLAGS += -I../../arch/$(ARCH)/include -I../../include -I../include
# Startup is part of the kernel proper — same -DKERNEL_INTERNAL gate
# (see kernel/Makefile / arch/$(ARCH)/Makefile).
CPPFLAGS += -DKERNEL_INTERNAL
CFLAGS += -g -ffunction-sections -fdata-sections
CFLAGS += $(KERNEL_CFLAGS)

OBJECTS = $(addprefix $(OBJDIR)/, $(addsuffix .o, $(STARTUP_ALL_SRC)))

all: $(OBJECTS)

$(OBJDIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(msg) CC $@
	$(Q)$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) -c -o $@ $<

clean:
	$(Q)rm -rf $(BUILD_BASE)

.PHONY: all clean

$(eval $(call dep-include,$(OBJECTS)))
