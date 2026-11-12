include ../../mk/def.mk
include ../lib/common.mk

PROG = $(BUILD_BASE)/ramblk
OBJS = $(OBJDIR)/main.o

all: $(PROG)
$(OBJDIR)/%.o: %.c
	@mkdir -p $(@D)
	$(msg) CC $@
	$(Q)$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<
$(PROG): $(OBJS) $(CRT0_O) $(LIBC_SO)
	$(msg) LD $@
	$(Q)$(LD) -nostdlib -o $@ $(CRT0_O) $(OBJS) $(LIBC_SO)
clean:
	$(Q)rm -rf $(BUILD_BASE)
.PHONY: all clean
