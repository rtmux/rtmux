#
# Makefile for RTMux dynamic linker (ld-rtmux.so)
#
# Based on FreeBSD's ldso, adapted for RTMux RISC-V.
#

include ../../mk/def.mk

INCDIR     = ../../include
INTERNALINCDIR = ../../internal/include
ARCHINCDIR = ../../arch/$(ARCH)/include
LIBCDIR    = ../libc
LIBC_INTERNALDIR = $(LIBCDIR)/internal

CPPFLAGS += -I$(INTERNALINCDIR) -I$(INCDIR) -I$(ARCHINCDIR) -I. -I$(ARCH) \
            -I$(LIBCDIR) -I$(LIBC_INTERNALDIR) -I$(LIBCDIR)/alloc \
            -include $(LIBCDIR)/rtmux-libc-local.h \
            -include local.h \
            -DIN_LDSO -DNO_LD_DEBUG
# LDSO_VERBOSE=1 enables trace output (ecall to kernel console)
ifdef LDSO_VERBOSE
CPPFLAGS += -DLDSO_VERBOSE
endif
CFLAGS   += -std=gnu17 -g -O2 -fpic -DPIC -ffreestanding \
            -ffunction-sections -fdata-sections
ASFLAGS  += -nostdinc -fpic -DPIC -I$(INTERNALINCDIR) -I$(INCDIR) \
            -I$(ARCHINCDIR) -I. -I$(ARCH) -DIN_LDSO
ifdef LDSO_VERBOSE
ASFLAGS  += -DLDSO_VERBOSE
endif

# Core dynlink sources (from FreeBSD)
DYNLINK_SRCS = dynlink.c hash.c map-object.c parse.c path.c printf.c \
            lock.c malloc.c debug.c xmalloc.c

# RTMux adaptation sources
RTMUX_SRCS = fd.c vm.c process.c signal.c rtmux-ldso-stubs.c

# RISC-V arch sources
ARCH_SRCS = $(ARCH)/reloc.c
ARCH_ASM  = $(ARCH)/start.S

# Minimal libc sources compiled into ldso (with -fpic).
#
# The dynamic linker must remain self-contained before libc.so has been
# mapped, so it links only the string routines, tiny conversion helpers,
# RTMux syscall glue, and path-open/mmap helpers that ldso actually references.
LIBC_STRING_SRCS = memcpy.c memmove.c memset.c memcmp.c memchr.c \
    strcmp.c strcpy.c strncpy.c strcat.c strlen.c strncmp.c strdup.c \
    strchr.c strrchr.c strstr.c strlcpy.c strlcat.c \
    strspn.c strcspn.c strpbrk.c strtok.c
LIBC_STDLIB_SRCS = strtol.c strtoul.c
LIBC_RTMUX_SRCS = sigfillset.c posix-id.c
LIBC_MMAN_SRCS = mmap.c
# ldso redirects sigprocmask / sigaction calls via libc.h to
# rtmux_ldso_* in signal.c, so the libc implementations of these
# functions (which now reach into the signal table in signal-state.c)
# are not needed here and would introduce unresolved PLT entries that
# break self-contained relocation.
LIBC_SUPPORT_SRCS = _vopen.c _connect.c _connect-entry.c _connect-ctrl.c
LIBC_USER_SRCS = syscalls.c

DYNLINK_OBJS = $(addprefix $(OBJDIR)/, $(notdir $(DYNLINK_SRCS:.c=.o)))
RTMUX_LDSO_OBJS  = $(addprefix $(OBJDIR)/, $(notdir $(RTMUX_SRCS:.c=.o)))
ARCH_OBJS = $(addprefix $(OBJDIR)/, $(notdir $(ARCH_SRCS:.c=.o)))
ASM_OBJS  = $(addprefix $(OBJDIR)/, $(notdir $(ARCH_ASM:.S=.o)))
LIBC_OBJS = $(addprefix $(OBJDIR)/lc_, \
    $(notdir $(LIBC_STRING_SRCS:.c=.o)) \
    $(notdir $(LIBC_STDLIB_SRCS:.c=.o)) \
    $(notdir $(LIBC_RTMUX_SRCS:.c=.o)) \
    $(notdir $(LIBC_MMAN_SRCS:.c=.o)) \
    $(notdir $(LIBC_SUPPORT_SRCS:.c=.o)) \
    $(notdir $(LIBC_USER_SRCS:.c=.o)))

ALL_OBJS = $(ASM_OBJS) $(DYNLINK_OBJS) $(RTMUX_LDSO_OBJS) $(ARCH_OBJS) $(LIBC_OBJS)

all: $(LDSO_SO)

$(LDSO_SO): $(ALL_OBJS) Symbol.map
	@mkdir -p $(@D)
	$(msg) LD $@
	$(Q)$(LDSHARED) -nostdlib \
	    -Wl,-Bsymbolic,--version-script=Symbol.map \
	    -e .ldso_start -o $@ $(ALL_OBJS)

$(OBJDIR)/%.o: %.c | $(OBJDIR)
	$(msg) CC $@
	$(Q)$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/%.o: $(ARCH)/%.c | $(OBJDIR)
	$(msg) CC $@
	$(Q)$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/%.o: $(ARCH)/%.S | $(OBJDIR)
	$(msg) AS $@
	$(Q)$(CC) $(ASFLAGS) -c -o $@ $<

# libc sources compiled with PIC and an lc_ prefix.
# Use libc's own CPPFLAGS (no local.h)
LIBC_CPPFLAGS = -Wall -nostdinc \
    $(if $(GCC_INC),-isystem $(GCC_INC)) \
    -I../../lib/common -I$(INTERNALINCDIR) -I$(INCDIR) -I$(ARCHINCDIR) \
    -I$(LIBCDIR) -I$(LIBC_INTERNALDIR) -I$(LIBCDIR)/alloc \
    -include $(LIBCDIR)/rtmux-libc-local.h \
    -DLDSO_MINIMAL_CONNECT
LIBC_CFLAGS = -std=gnu17 -g -fpic -DPIC -ffreestanding \
    $(ARCH_FLAGS) \
    -ffunction-sections -fdata-sections

$(OBJDIR)/lc_%.o: $(LIBCDIR)/string/%.c | $(OBJDIR)
	$(msg) CC $@
	$(Q)$(CC) $(LIBC_CPPFLAGS) $(LIBC_CFLAGS) -c -o $@ $<

$(OBJDIR)/lc_%.o: $(LIBCDIR)/stdlib/%.c | $(OBJDIR)
	$(msg) CC $@
	$(Q)$(CC) $(LIBC_CPPFLAGS) $(LIBC_CFLAGS) -c -o $@ $<

$(OBJDIR)/lc_%.o: $(LIBCDIR)/rtmux/%.c | $(OBJDIR)
	$(msg) CC $@
	$(Q)$(CC) $(LIBC_CPPFLAGS) $(LIBC_CFLAGS) -c -o $@ $<

$(OBJDIR)/lc_%.o: $(LIBCDIR)/posix/mman/%.c | $(OBJDIR)
	$(msg) CC $@
	$(Q)$(CC) $(LIBC_CPPFLAGS) $(LIBC_CFLAGS) -c -o $@ $<

$(OBJDIR)/lc_%.o: $(LIBCDIR)/posix/%.c | $(OBJDIR)
	$(msg) CC $@
	$(Q)$(CC) $(LIBC_CPPFLAGS) $(LIBC_CFLAGS) -c -o $@ $<

$(OBJDIR)/lc_%.o: $(LIBCDIR)/support/%.c | $(OBJDIR)
	$(msg) CC $@
	$(Q)$(CC) $(LIBC_CPPFLAGS) $(LIBC_CFLAGS) -c -o $@ $<

$(OBJDIR)/lc_%.o: $(LIBCDIR)/$(ARCH)/%.c | $(OBJDIR)
	$(msg) CC $@
	$(Q)$(CC) $(LIBC_CPPFLAGS) $(LIBC_CFLAGS) -c -o $@ $<

$(OBJDIR):
	$(Q)mkdir -p $@

clean:
	$(Q)rm -rf $(BUILD_BASE)

.PHONY: all clean
