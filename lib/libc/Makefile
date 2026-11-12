#
# Makefile for RTMux libc
#
# Produces:
#   libc.a   - static archive
#   libc.so - shared library (for user-space programs)
#

include ../../mk/def.mk

INCDIR     = ../../include
ARCHINCDIR = ../../arch/$(ARCH)/include
LIBC_INTERNALDIR = internal

CPPFLAGS += -I$(INCDIR) -I$(ARCHINCDIR) -I. -I$(LIBC_INTERNALDIR) \
            -include rtmux-libc-local.h
# libc is C11-clean: the POSIX bodies and the RISC-V asm seam both build under
# strict -std=c11 (the seam uses the language-neutral __asm__ spelling). Keep
# c11 as the default so any reintroduced GNU-ism breaks the build; override with
# `make -C lib/libc CSTD=gnu17` if ever needed.
CSTD     ?= c11
CFLAGS   += -std=$(CSTD) -g -fPIC

# --- Source lists ---
STRING_SRCS = memcpy.c memmove.c memset.c memcmp.c memchr.c memccpy.c \
              strcmp.c strcpy.c strncpy.c strcat.c strncat.c \
              strchr.c strrchr.c strlen.c strncmp.c strdup.c \
              strstr.c \
              strspn.c strcspn.c strpbrk.c \
              strcasecmp.c strtok.c

STDLIB_SRCS = strtol.c strtoul.c strtoll.c strtoull.c \
              qsort.c bsearch.c abs.c labs.c div.c ldiv.c rand_r.c \
              atoi.c atol.c atoll.c getsubopt.c basename.c \
              getenv.c getopt.c fnmatch.c exit.c abort.c

RTMUX_SRCS = sigemptyset.c sigfillset.c sigaddset.c sigdelset.c sigismember.c \
           signal-state.c \
           tls.c \
           posix-id.c posix-stubs.c time.c \
           openfd.c atfd.c devctl.c \
           sleepon.c \
           spawnp.c spawn.c \
           syspage-ptr.c \
           getpwent.c getgrent.c getlogin.c ttyname.c \
           sha512.c crypt.c

POSIX1_SRCS = sleep.c write.c read.c close.c fstat.c fdatasync.c fsync.c sync.c \
              sigprocmask.c sigaction.c pthread-sigmask.c pthread-kill.c fpathconf.c \
              setpgid.c setids.c tcsetpgrp.c tcsetsid.c \
              open.c stat.c access.c chdir.c chroot.c issetugid.c \
              getgroups.c sysconf.c umask.c time-posix.c times.c \
              lseek.c dup.c kill.c pause.c \
              pipe.c waitpid.c getcwd.c isatty.c creat.c \
              tcgetattr.c tcsetattr.c tcgetpgrp.c tcflush.c tcdrain.c \
              tcflow.c tcsendbreak.c termios-speed.c \
              statvfs.c poll.c select.c resource.c fcntl.c signal.c iodir.c _exit.c exec.c raise.c unlink.c \
              gettimeofday.c localtime.c strftime.c setjmp.c uname.c

ifeq ($(ARCH),riscv)
POSIX1_ASM_SRCS = _setjmp.S
else
POSIX1_ASM_SRCS =
endif

POSIX1A_SRCS = readlink.c confstr.c

XOPEN_SRCS = waitid.c

POSIX1B_SRCS = mem-offset.c mmap.c mlock.c mprotect.c msync.c munmap.c \
               nanosleep.c pread.c pwrite.c sched.c sched-yield.c shm.c sem.c \
               mqueue.c \
               timer.c

ALLOC_SRCS = malloc-core.c malloc.c

SUPPORT_SRCS = _vopen.c _connect.c _connect-entry.c _connect-ctrl.c \
               _connect-combine.c _connect-object.c _connect-fd.c _devctl.c \
               readx.c \
               _conf-destroy.c _conf-get.c _conf-set.c \
               assert.c \
               dbgprintf.c

DISPATCH_SRCS = context.c \
                message.c \
                name.c \
                backend.c detach.c \
                thread-pool.c _thread-pool-thread.c

IOSERV_SRCS = open-bind.c handler.c io-handler.c \
              connect-handler.c binding-funcs.c ocb.c \
              unbind.c msgread.c msgreadv.c \
              msgwrite.c msgwritev.c again.c \
              link-alloc.c link-free.c \
              handle.c \
              close-handler.c dup-handler.c \
              io-access.c disconnect-handler.c \
              io-func.c link-query.c \
              mmap-handler.c status.c \
              notify-handler.c openfd-handler.c \
              unblock-handler.c devino.c data.c \
              detach-id.c

POSIX1D_SRCS = posix-spawnattr-getpgroup.c \
               posix-spawnattr-getsched.c posix-spawnattr-getsig.c \
               posix-spawnattr-getflags.c \
               posix-spawnattr-support.c posix-spawnattr-private.c \
               posix-spawnattr-init.c \
               posix-spawn-file-getactions.c \
               posix-spawn-file-actions-support.c \
               posix-spawn-file-actions-private.c \
               posix-spawn-file-actions-init.c \
               posix-spawn.c posix-spawnp.c posix-spawn-waitfd.c

SERVICES_SRCS = rtmux-detach.c rtmux-phys-contain.c rtmux-hwres.c \
	rtmux-delegate.c \
	namespace-link.c namespace-symlink.c roottask-console-set.c ioserv.c

ifeq ($(ARCH),riscv)
ATOMIC_SRCS = atomic-rv64.S
USER_SRCS = syscalls.c syschan-init.c
else
ATOMIC_SRCS =
USER_SRCS =
endif

PTHREAD_SRCS = pthread-self.c pthread-equal.c \
               pthread-mutex-init.c pthread-mutex-destroy.c \
               pthread-mutex-lock.c pthread-mutex-unlock.c pthread-mutex-trylock.c \
               pthread-mutex-timedlock.c \
               pthread-ulock-debug.c \
               pthread-mutexattr-init.c pthread-mutexattr-destroy.c \
               pthread-mutexattr-protocol.c \
               pthread-cond-init.c pthread-cond-destroy.c \
               pthread-cond-signal.c pthread-cond-broadcast.c \
               pthread-cond-wait.c pthread-cond-timedwait.c \
               pthread-condattr-init.c pthread-condattr-destroy.c \
               pthread-condattr-getclock.c pthread-condattr-setclock.c \
               pthread-condattr-pshared.c \
               pthread-concurrency.c \
               pthread-attr-init.c pthread-attr-destroy.c \
               pthread-attr-getdetachstate.c pthread-attr-getstack.c \
               pthread-attr-getstackaddr.c pthread-attr-getstacksize.c \
               pthread-attr-sched.c \
               pthread-attr-setdetachstate.c pthread-attr-setstack.c \
               pthread-attr-setstackaddr.c pthread-attr-setstacksize.c \
               pthread-attr-default.c \
               pthread-create.c pthread-detach.c pthread-exit.c \
               pthread-join.c \
               pthread-cancel.c pthread-cancelstate.c \
               pthread-sched.c \
               pthread-testcancel.c \
               pthread-getspecific.c pthread-key-create.c pthread-key-delete.c \
               pthread-once.c pthread-setspecific.c pthread-key-data.c \
               pthread-spin.c pthread-barrier.c pthread-rwlock.c

ifeq ($(ARCH),riscv)
PTHREAD_ASM_SRCS = __my-thread-exit.S
else
PTHREAD_ASM_SRCS =
endif

STDIO_SRCS = findfp.c stdio.c flags.c makebuf.c wsetup.c refill.c \
             fvwrite.c wbuf.c rget.c \
             fopen.c fclose.c fread.c fwrite.c fgets.c fflush.c \
             fgetc.c fputc.c fputs.c puts.c getc.c getchar.c putc.c putchar.c \
             flockfile.c \
             ungetc.c fileno.c feof.c ferror.c clearerr.c \
             fdopen.c ftell.c fseek.c setvbuf.c perror.c \
             vfprintf.c fprintf.c printf.c vprintf.c \
             snprintf.c sprintf.c vsnprintf.c strerror.c

# Object file lists.
STRING_OBJS   = $(addprefix $(OBJDIR)/, $(STRING_SRCS:.c=.o))
STDLIB_OBJS   = $(addprefix $(OBJDIR)/, $(STDLIB_SRCS:.c=.o))
RTMUX_OBJS      = $(addprefix $(OBJDIR)/, $(RTMUX_SRCS:.c=.o))
POSIX1_OBJS   = $(addprefix $(OBJDIR)/, $(POSIX1_SRCS:.c=.o))
POSIX1_ASM_OBJS = $(addprefix $(OBJDIR)/, $(POSIX1_ASM_SRCS:.S=.o))
POSIX1A_OBJS  = $(addprefix $(OBJDIR)/, $(POSIX1A_SRCS:.c=.o))
POSIX1B_OBJS  = $(addprefix $(OBJDIR)/, $(POSIX1B_SRCS:.c=.o))
XOPEN_OBJS    = $(addprefix $(OBJDIR)/, $(XOPEN_SRCS:.c=.o))
ALLOC_OBJS    = $(addprefix $(OBJDIR)/, $(ALLOC_SRCS:.c=.o))
SUPPORT_OBJS  = $(addprefix $(OBJDIR)/, $(SUPPORT_SRCS:.c=.o))
DISPATCH_OBJS = $(addprefix $(OBJDIR)/, $(DISPATCH_SRCS:.c=.o))
IOSERV_OBJS   = $(addprefix $(OBJDIR)/, $(IOSERV_SRCS:.c=.o))
POSIX1D_OBJS  = $(addprefix $(OBJDIR)/, $(POSIX1D_SRCS:.c=.o))
SERVICES_OBJS = $(addprefix $(OBJDIR)/, $(SERVICES_SRCS:.c=.o))
ATOMIC_OBJS   = $(addprefix $(OBJDIR)/, $(ATOMIC_SRCS:.S=.o))
PTHREAD_OBJS  = $(addprefix $(OBJDIR)/, $(PTHREAD_SRCS:.c=.o))
PTHREAD_ASM_OBJS = $(addprefix $(OBJDIR)/, $(PTHREAD_ASM_SRCS:.S=.o))
STDIO_OBJS    = $(addprefix $(OBJDIR)/, $(STDIO_SRCS:.c=.o))
USER_OBJS     = $(addprefix $(OBJDIR)/, $(USER_SRCS:.c=.o))

ALL_OBJS = $(STRING_OBJS) $(STDLIB_OBJS) $(RTMUX_OBJS) $(POSIX1_OBJS) \
           $(POSIX1_ASM_OBJS) \
           $(POSIX1A_OBJS) $(POSIX1B_OBJS) $(XOPEN_OBJS) \
           $(ALLOC_OBJS) $(SUPPORT_OBJS) \
           $(DISPATCH_OBJS) $(IOSERV_OBJS) \
           $(POSIX1D_OBJS) $(SERVICES_OBJS) $(ATOMIC_OBJS) \
           $(PTHREAD_OBJS) $(PTHREAD_ASM_OBJS) \
           $(STDIO_OBJS) $(USER_OBJS)

ifeq ($(ARCH),riscv)
all: $(LIBC_A) $(LIBC_SO) $(CRT0_O)
else
all: $(LIBC_A) $(LIBC_SO)
endif

# Static archive (for roottask / kernel-mode linking — includes everything)
$(LIBC_A): $(ALL_OBJS)
	@mkdir -p $(@D)
	$(msg) AR $@
	$(Q)rm -f $@
	$(Q)$(AR) rcs $@ $^

# Shared library (for user-space)
# libc.map is the export list: a prerequisite, but not a link input ($^).
$(LIBC_SO): $(ALL_OBJS) libc.map
	@mkdir -p $(@D)
	$(msg) LD $@
	$(Q)$(LDSHARED) -nostdlib -Wl,-soname,libc.so -Wl,--version-script=libc.map -o $@ \
	    $(ALL_OBJS)

# Pattern rules by source directory
$(OBJDIR)/%.o: string/%.c | $(OBJDIR)
	$(msg) CC $@
	$(Q)$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/%.o: stdlib/%.c | $(OBJDIR)
	$(msg) CC $@
	$(Q)$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/%.o: rtmux/%.c | $(OBJDIR)
	$(msg) CC $@
	$(Q)$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/%.o: posix/%.c | $(OBJDIR)
	$(msg) CC $@
	$(Q)$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/%.o: posix/mman/%.c | $(OBJDIR)
	$(msg) CC $@
	$(Q)$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/%.o: xopen/%.c | $(OBJDIR)
	$(msg) CC $@
	$(Q)$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/%.o: alloc/%.c | $(OBJDIR)
	$(msg) CC $@
	$(Q)$(CC) $(CPPFLAGS) $(CFLAGS) -Ialloc -c -o $@ $<

$(OBJDIR)/%.o: support/%.c | $(OBJDIR)
	$(msg) CC $@
	$(Q)$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/%.o: services/%.c | $(OBJDIR)
	$(msg) CC $@
	$(Q)$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/%.o: dispatch/%.c | $(OBJDIR)
	$(msg) CC $@
	$(Q)$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/%.o: ioserv/%.c | $(OBJDIR)
	$(msg) CC $@
	$(Q)$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/%.o: pthread/%.c | $(OBJDIR)
	$(msg) CC $@
	$(Q)$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/%.o: spawn/%.c | $(OBJDIR)
	$(msg) CC $@
	$(Q)$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/%.o: stdio/%.c | $(OBJDIR)
	$(msg) CC $@
	$(Q)$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/%.o: posix/$(ARCH)/%.S | $(OBJDIR)
	$(msg) AS $@
	$(Q)$(CC) -I$(INCDIR) -I$(ARCHINCDIR) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/%.o: pthread/$(ARCH)/%.S | $(OBJDIR)
	$(msg) AS $@
	$(Q)$(CC) -I$(INCDIR) -I$(ARCHINCDIR) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/%.o: atomic/$(ARCH)/%.S | $(OBJDIR)
	$(msg) AS $@
	$(Q)$(CC) -I$(INCDIR) -I$(ARCHINCDIR) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/%.o: $(ARCH)/%.c | $(OBJDIR)
	$(msg) CC $@
	$(Q)$(CC) $(CPPFLAGS) $(CFLAGS) -fPIC -c -o $@ $<

# CRT0 — standalone object (not in libc.a), linked into every user-space program
$(CRT0_O): $(ARCH)/crt0.S
	@mkdir -p $(@D)
	$(msg) AS $@
	$(Q)$(CC) -I$(INCDIR) -I$(ARCHINCDIR) $(CFLAGS) -c -o $@ $<

$(OBJDIR):
	$(Q)mkdir -p $@

clean:
	$(Q)rm -rf $(BUILD_BASE)

.PHONY: all clean

# The system-channel implementation is shared by the arch startup wrappers.
$(OBJDIR)/syschan-init.o: rtmux/syschan-init.c
