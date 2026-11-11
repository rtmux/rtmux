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
	$(Q)$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

# These rules have no automatic header tracking.  hardware-init.o bakes
# RTMUX_BUILD_ID into the syspage, so it must follow the generated header, or a
# new commit would leave a stale kernel id and roottask would report a false
# "version skew".
$(OBJDIR)/hardware-init.o: $(abspath $(TOPDIR))/internal/include/rtmux-version.h

clean:
	$(Q)rm -rf $(BUILD_BASE)

.PHONY: all clean
