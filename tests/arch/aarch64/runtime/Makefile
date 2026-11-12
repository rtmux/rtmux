.DEFAULT_GOAL := all
include ../../../../mk/def.mk
include ../../../../userland/lib/common.mk
PROG = $(BUILD_BASE)/pi3-runtime-test
OBJS = $(BUILD_BASE)/runtime.o $(BUILD_BASE)/simd.o
all: $(PROG)
$(BUILD_BASE)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<
$(BUILD_BASE)/%.o: %.S
	@mkdir -p $(@D)
	$(CC) $(ASFLAGS) -c -o $@ $<
$(PROG): $(OBJS) $(CRT0_O) $(LIBC_SO)
	$(LD) -nostdlib -o $@ $(CRT0_O) $(OBJS) $(LIBC_SO)
install: all
	cp $(PROG) $(INITRAMFS_DIR)/bin/
	cp ../../../userland/toolbox/test.sh $(INITRAMFS_DIR)/bin/toolbox-test
.PHONY: all install
