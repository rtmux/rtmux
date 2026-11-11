#
# Makefile for RTMux kernel (startup + arch + kernel, linked together)
#
# Single-pass link.
#

include ../../mk/def.mk
include ../../kernel/objects.mk
include ../../kernel/startup/objects.mk

CPPFLAGS += -Iinclude -I../../include -I../../kernel/include
# Match kernel/Makefile — arch sources are kernel-private and need the
# full struct bodies (process_t, thread_t, channel_t, ...) gated behind
# KERNEL_INTERNAL in include/objects.h.
CPPFLAGS += -DKERNEL_INTERNAL
CFLAGS += -g
CFLAGS += $(KERNEL_CFLAGS)
LDFLAGS = -z max-page-size=4096

# Out-of-source paths. Three object trees feed the final link:
#   - $(STARTUP_OBJDIR) — arch-specific startup objects compiled here
#   - $(STARTUP_COMMON_OBJDIR) — arch-independent startup compiled by ../../startup/
#   - $(KER_OBJDIR) — kernel core objects compiled by ../../
STARTUP_OBJDIR        = $(BUILD_BASE)/startup
KERDIR                = ../../kernel
STARTUP_COMMON_DIR    = ../../kernel/startup
STARTUP_COMMON_OBJDIR = $(OUT)/kernel/startup
KER_OBJDIR            = $(OUT)/kernel

# --- Arch-specific startup objects (remain here) ---

STARTUP_SRC = head entry board init-asinfo \
              print-syspage mmu debug arch-init init-cpu

# Platform-dependent
PLAT = platform/qemu_virt
PLAT_SRC = mem

OBJECTS = $(addprefix $(STARTUP_OBJDIR)/, $(addsuffix .o, $(STARTUP_SRC))) \
          $(addprefix $(STARTUP_OBJDIR)/plat/, $(addsuffix .o, $(PLAT_SRC)))

# --- Startup common objects (compiled by startup/Makefile) ---

STARTUP_COMMON_OBJECTS = $(addprefix $(STARTUP_COMMON_OBJDIR)/, \
                           $(addsuffix .o, $(STARTUP_ALL_SRC)))

# --- RISC-V arch objects (kernel CPU-specific code) ---

ARCH_SRC = cpu-misc cpu-elf-reloc trap-handler exit entry kernel-thread \
           cpu-pgtable plic trap \
           xfer-jmp fpcontext calls as cpu-pte
ARCH_OBJECTS = $(addprefix $(STARTUP_OBJDIR)/arch/, $(addsuffix .o, $(ARCH_SRC)))

# --- Kernel objects (compiled by kernel/Makefile) ---

KERFLAGS = $(CPPFLAGS) $(CFLAGS) -I$(KERDIR) -I$(KERDIR)/include \
           -I$(KERDIR)/../servers/roottask/mem \
           -I$(KERDIR)/../servers/roottask/mem/riscv \
           -I$(KERDIR)/../servers/roottask/include
KER_OBJECTS = $(addprefix $(KER_OBJDIR)/, $(addsuffix .o, $(KER_ALL_SRC)))

# All objects for linking
ALL_LINK_OBJS = $(OBJECTS) $(STARTUP_COMMON_OBJECTS) $(ARCH_OBJECTS) $(KER_OBJECTS)

# --- Build rules ---

# Re-enter make after the recursive object builds so it reads their new timestamps
# before deciding whether the kernel needs relinking.
all: startup_common kernel_objects
	$(Q)$(MAKE) $(KERNEL_BIN)

# Keep the version header as a link prerequisite because header dependencies are
# not tracked automatically; callers force a rebuild after header changes.
$(KERNEL_BIN): $(ALL_LINK_OBJS) rtmux_kernel.ld $(abspath $(TOPDIR))/internal/include/rtmux-version.h
	@mkdir -p $(@D)
	$(msg) LD $@
	$(Q)$(LD) $(LDFLAGS) -T rtmux_kernel.ld -o $@ $(ALL_LINK_OBJS)
	$(msg) SYM $@.sym
	$(Q)$(OBJDUMP) -t $@ | sed '1,/SYMBOL TABLE/d; s/ .* / /; /^$$/d' > $@.sym
	$(msg) BIN $@.bin
	$(Q)$(OBJCOPY) -O binary $@ $@.bin

startup_common:
	$(Q)$(MAKE) -C $(STARTUP_COMMON_DIR)

kernel_objects:
	$(Q)$(MAKE) -C $(KERDIR)

# Startup objects
$(STARTUP_OBJDIR)/%.o: startup/%.c
	@mkdir -p $(dir $@)
	$(msg) CC $@
	$(Q)$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(STARTUP_OBJDIR)/%.o: startup/%.S
	@mkdir -p $(dir $@)
	$(msg) AS $@
	$(Q)$(CC) $(CPPFLAGS) $(ASFLAGS) $(CFLAGS) -c -o $@ $<

# Platform objects
$(STARTUP_OBJDIR)/plat/%.o: $(PLAT)/%.c
	@mkdir -p $(dir $@)
	$(msg) CC $@
	$(Q)$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

# Arch objects
$(STARTUP_OBJDIR)/arch/%.o: %.c
	@mkdir -p $(dir $@)
	$(msg) CC $@
	$(Q)$(CC) $(KERFLAGS) -c -o $@ $<

$(STARTUP_OBJDIR)/arch/%.o: %.S
	@mkdir -p $(dir $@)
	$(msg) AS $@
	$(Q)$(CC) $(CPPFLAGS) $(ASFLAGS) $(CFLAGS) -c -o $@ $<

clean:
	$(Q)rm -rf $(BUILD_BASE)
	$(Q)$(MAKE) -C $(KERDIR) clean

.PHONY: all clean kernel_objects startup_common
