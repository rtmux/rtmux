include ../../mk/def.mk
include ../../userland/lib/common.mk

PROG = $(BUILD_BASE)/lkl-netd
OBJS = $(OBJDIR)/main.o $(LKL_SERVER_OBJS)

# netd links its own out-netd LKL archive.  From source (LKL_SRC set) that build
# compiles the ext4 driver out of lkl.o (~0.6 MB; netd's cmdline has no root= and
# it never calls lkl_disk_add); the fs glue objects stay (utils.o/virtio.o need
# lkl_mount_fs), so this is a config trim, not an object drop.  The --extract
# fallback lands the combined archive in out-netd instead (a working link).
LKL_OUT_NAME = out-netd
LKL_PERSONALITY = netd

all: $(PROG)

include ../../mk/lkl-server.mk

# After lkl-server.mk: it defines LKL_SERVER_OBJS, which OBJS expands.
$(eval $(call dep-include,$(OBJS)))

$(OBJDIR)/%.o: %.c $(LKL_RTMUX_LIB)
	$(lkl-cc)

$(PROG): $(OBJS) $(CRT0_O) $(LIBC_SO) $(LKL_RTMUX_LIB)
	@mkdir -p $(@D)
	$(msg) LD $@
	$(Q)$(CC) -fuse-ld=lld -Wl,-melf64lriscv -nostdlib -o $@ \
	    -Wl,--dynamic-linker=/lib/ld.so.1 \
	    $(CRT0_O) $(OBJS) $(LKL_RTMUX_LIB) $(LIBC_SO)

clean:
	$(Q)rm -rf $(BUILD_BASE)

.PHONY: all clean
