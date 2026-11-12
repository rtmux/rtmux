include ../../mk/def.mk
include ../../userland/lib/common.mk

PROG = $(BUILD_BASE)/lkl-fsd
OBJS = $(OBJDIR)/main.o $(OBJDIR)/pathbuild.o $(LKL_SERVER_OBJS) \
       $(OBJDIR)/host-selftest.o

# fsd links its own out-fsd LKL archive.  From source (LKL_SRC set) that is a
# net-off, filesystem-only build whose lkl.o drops the network stack for a
# ~1.4 MB smaller server; the --extract fallback lands the combined archive in
# out-fsd instead (a working link, no lean win).
LKL_OUT_NAME = out-fsd
LKL_PERSONALITY = fsd

all: $(PROG)

include ../../mk/lkl-server.mk

# After lkl-server.mk: it defines LKL_SERVER_OBJS, which OBJS expands.
$(eval $(call dep-include,$(OBJS)))

$(OBJDIR)/main.o: main.c $(LKL_RTMUX_LIB)
	$(lkl-cc)

$(OBJDIR)/host-selftest.o: ../../tests/lkl/test-host-selftest.c $(LKL_RTMUX_LIB)
	$(lkl-cc)

# pathbuild.c is pure libc (no LKL headers), also host-fuzzed via tools/host.
$(OBJDIR)/pathbuild.o: pathbuild.c pathbuild.h
	@mkdir -p $(@D)
	$(msg) CC $@
	$(Q)$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) -c -o $@ $<

$(PROG): $(OBJS) $(CRT0_O) $(LIBC_SO) $(LKL_RTMUX_LIB)
	@mkdir -p $(@D)
	$(msg) LD $@
	$(Q)$(CC) -fuse-ld=lld -Wl,-melf64lriscv -nostdlib -o $@ \
	    -Wl,--dynamic-linker=/lib/ld.so.1 \
	    $(CRT0_O) $(OBJS) $(LKL_RTMUX_LIB) $(LIBC_SO)

clean:
	$(Q)rm -rf $(BUILD_BASE)

.PHONY: all clean
