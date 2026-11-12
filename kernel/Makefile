#
# Kernel Makefile -- compiles kernel objects only (linking done by arch/).
#

include ../mk/def.mk
include objects.mk

CPPFLAGS += -I. -I../arch/$(ARCH)/include -I../include -Iinclude
# The kext_*.c bodies that were relocated from servers/roottask/mem/ need
# access to the roottask-side mm subsystem headers.
CPPFLAGS += -I../servers/roottask/mem -I../servers/roottask/mem/$(ARCH) -I../servers/roottask/include
# KERNEL_INTERNAL gates the bodies of kernel-private struct definitions
# (process_t, thread_t, channel_t, ...) in include/objects.h.  Kernel sources
# see the full struct; roottask sees only the typedef as an opaque type
# and is forbidden from dereferencing kernel pointers it gets back from
# query_object / similar kexts.
CPPFLAGS += -DKERNEL_INTERNAL
CFLAGS += -g -ffunction-sections -fdata-sections
CFLAGS += $(KERNEL_CFLAGS)

OBJECTS = $(addprefix $(OBJDIR)/, $(addsuffix .o, $(KER_ALL_SRC)))

all: $(OBJECTS)

$(OBJDIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(msg) CC $@
	$(Q)$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) -c -o $@ $<

clean:
	$(Q)rm -rf $(BUILD_BASE)

.PHONY: all clean

$(eval $(call dep-include,$(OBJECTS)))
