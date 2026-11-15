#
# Makefile for roottask -- the initial process/memory/path/system server
#
# Produces roottask: an ET_DYN userland binary, dynamically linked
# against libc.so.  Loaded at boot by the kernel-resident B1 ELF
# loader (see kernel/ker_start_roottask_umode.c) into pid 1's private
# as at base address 0x40000000 (L2[1]).  After roottask is up,
# all subsequent process spawns go through roottask's own loader.
#

include ../../mk/def.mk

INCDIR = ../../include
ARCHINCDIR = ../../arch/$(ARCH)/include

CPPFLAGS += -I$(INCDIR) -I$(ARCHINCDIR) -Isys/$(ARCH) -Imem -Imem/$(ARCH) \
            -I../../kernel/include -Iinclude
# Static, fixed-VA build: roottask is an ET_EXEC linked against libc.a
# at base 0xC0000000 (matches RTMUX_ROOTTASK_UADDR_BASE / SV39 L2[3]).
# No -fPIC — every symbol resolves to its final VA at link time, so
# libc.a's data (environ, errno, allocator state) lives inside
# roottask's own image and is reachable U-mode without any kernel-heap
# PTE_U=1 trick.
CFLAGS += -std=gnu17 -g

# --- sys/ objects ---
SYS_OBJECTS = \
	sys/misc.o \
	sys/init.o \
	sys/sysconf.o \
	sys/command.o \
	sys/sync.o \
	sys/console.o \
	sys/support.o \
	sys/root-process.o \
	sys/main.o \
	sys/heap.o \
	sys/message.o \
	sys/proc-loader.o \
	sys/proc-read.o \
	sys/loader-elf.o \
	sys/bootstrap-manifest.o \
	sys/bootimage-init.o \
	sys/hwres-init.o \
	sys/hwres-list.o \
	sys/hwres-api.o \
	sys/hwres-cmd.o \
	sys/hwres-seed.o \
	sys/devno.o \
	sys/root-log.o \
	sys/cpio.o \
	sys/walk-asinfo.o

# --- proc/ objects ---
PROC_OBJECTS = \
	proc/kext-wrappers.o \
	proc/kcalls.o \
	proc/dispatch.o \
	proc/loader-context.o \
	proc/ids.o \
	proc/session.o \
	proc/group.o \
	proc/umask.o \
	proc/event.o \
	proc/resource.o \
	proc/spawn.o \
	proc/posix-spawn.o \
	proc/wait.o \
	proc/system-channel.o \
	proc/detach.o \
	proc/signal.o \
	proc/jobctl.o \
	proc/exit.o \
	proc/coredump.o \
	proc/stack.o

# --- mem/ objects (platform-independent) ---
MEM_OBJECTS = \
	mem/vm-init.o \
	mem/vm-ctrl.o \
	mem/vm-fd.o \
	mem/vm-info.o \
	mem/vm-map.o \
	mem/vm-offset.o \
	mem/vm-shmem.o \
	mem/vm-support.o \
	mem/vm-tymem.o \
	mem/vm-debug-info.o \
	mem/memcap.o \
	mem/cap-table.o \
	mem/mm-anmem.o \
	mem/mm-map.o \
	mem/mm-memobj.o \
	mem/mm-memref.o \
	mem/mm-pte.o \
	mem/mm-reference.o \
	mem/mm-rlimit.o \
	mem/$(ARCH)/pte-temp-map.o \
	mem/pa.o \
	mem/as/debuginfo.o \
	mem/as/dup.o \
	mem/as/madvise.o \
	mem/as/mapinfo.o \
	mem/as/mcreate.o \
	mem/as/memobj-phys.o \
	mem/as/mmap.o \
	mem/as/mprotect.o \
	mem/as/msync.o \
	mem/as/munmap.o \
	mem/as/resize.o \
	mem/as/vaddrinfo.o \
	mem/as/vaddr-to-memobj.o \
	mem/as/validate.o \
	mem/$(ARCH)/cpu-as.o \
	mem/$(ARCH)/as-init-mem.o

# --- path/ objects ---
PATH_OBJECTS = \
	path/init.o \
	path/link.o \
	path/node.o \
	path/object.o \
	path/open.o \
	path/resolve.o \
	path/accounting.o \
	path/stats.o \
	path/sync.o \
	path/devmem.o \
	path/devnull.o \
	path/devstd.o \
	path/devtext.o \
	path/devtty.o \
	path/devzero.o \
	path/procfs.o \
	path/cpiofs.o \
	path/capfs.o \
	path/sysfs.o \
	path/namedsem.o

# --- namesrv/ objects ---
NAMESRV_OBJECTS = \
	namesrv/resolve.o

OBJECTS = $(addprefix $(OBJDIR)/, \
	$(SYS_OBJECTS) $(PROC_OBJECTS) $(MEM_OBJECTS) $(PATH_OBJECTS) \
	$(NAMESRV_OBJECTS))

# --- Targets ---
#
# Static roottask build at fixed VA 0xC0000000.  Replaces the earlier
# ET_DYN + libc.so arrangement, which made roottask depend on a kernel-side
# shared-object loader. With static linking, every libc symbol lives inside
# roottask's own image at 0xC0000000+ and is naturally PTE_U=1 in roottask's
# pgdir only.
all: $(ROOTTASK_BIN)

$(ROOTTASK_BIN): $(OBJECTS) $(CRT0_O) $(LIBC_A)
	@mkdir -p $(@D)
	$(msg) LD $@
	$(Q)$(LD) -nostdlib -static -Ttext-segment=0xC0000000 \
	    -o $@ $(CRT0_O) $(OBJECTS) --start-group $(LIBC_A) --end-group
	$(msg) SYM $@.sym
	$(Q)$(OBJDUMP) -t $@ | sed '1,/SYMBOL TABLE/d; s/ .* / /; /^$$/d' | sort > $@.sym

$(OBJDIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(msg) CC $@
	$(Q)$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) -c -o $@ $<

# Stateless leaf utilities shared with the kernel.  Both binaries
# compile their own copy of these source files; the parser/walker is
# pure read-only code (no allocator, no globals, no #ifdef-driven
# divergence) so the two .o files behave identically and there's no
# risk of cross-binary state.
#  * cpio.c       — CPIO newc parser (kernel uses it at boot to find
#                   roottask in the initramfs; roottask uses it for cpiofs).
#  * walk-asinfo.c — iterates _syspage_ptr->asinfo[] (kernel asinfo
#                   queries; roottask uses it to locate the ramdisk
#                   and sysram regions).
$(OBJDIR)/sys/cpio.o: ../../kernel/startup/lib/cpio.c
	@mkdir -p $(dir $@)
	$(msg) CC $@
	$(Q)$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) -c -o $@ $<

$(OBJDIR)/sys/walk-asinfo.o: ../../kernel/lib/walk-asinfo.c
	@mkdir -p $(dir $@)
	$(msg) CC $@
	$(Q)$(CC) $(CPPFLAGS) $(CFLAGS) -include root-log.h $(DEPFLAGS) -c -o $@ $<

$(OBJDIR)/mem/cap-table.o: ../../kernel/cap/table.c
	@mkdir -p $(dir $@)
	$(msg) CC $@
	$(Q)$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) -c -o $@ $<

clean:
	$(Q)rm -rf $(BUILD_BASE)

.PHONY: all clean

$(eval $(call dep-include,$(OBJECTS)))
