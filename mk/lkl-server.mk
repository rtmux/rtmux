# LKL_OUT_NAME selects the prebuilt/ subdir (and thus the archive) a server links.
# A server picks a lean per-personality archive (out-fsd / out-netd) by setting it
# before including this file; the default is the combined out-rtmux.  This holds
# whether the archive comes from --build-from-source (LKL_SRC set) or --extract:
# a server always links its own subdir, so a plain `make` after a from-source build
# reuses the lean archive instead of demanding a combined out-rtmux that a
# personality build never produces.
LKL_OUT_NAME ?= out-rtmux
LKL_OUT ?= ../../prebuilt/lkl-rv64/$(LKL_OUT_NAME)
LKL_RTMUX_LIB ?= $(LKL_OUT)/liblkl-rtmux.a
LKL_CPPFLAGS = -I../lkl/include -I../lkl -isystem $(LKL_OUT)/include
# The LKL headers come in through -isystem, which -MMD leaves out of the .d files,
# so a header-only change in a regenerated LKL tree (the archive is the only
# prerequisite that changes mtime otherwise) would not rebuild the servers. -MD
# records system headers too.
LKL_DEPFLAGS = -MD -MP
LKL_CONNECTPATH_SRC = ../lkl/connectpath.c
LKL_SERVER_VIRTIO ?= y
LKL_SERVER_OBJS = $(OBJDIR)/host.o $(OBJDIR)/server.o $(OBJDIR)/connectpath.o
ifeq ($(LKL_SERVER_VIRTIO),y)
LKL_SERVER_OBJS += $(OBJDIR)/virtio.o
endif

# Objects built from ../lkl against the LKL headers (virtio.o is linked only when
# LKL_SERVER_VIRTIO=y; listing it here is harmless otherwise).
LKL_HDR_OBJS = $(OBJDIR)/host.o $(OBJDIR)/server.o $(OBJDIR)/virtio.o
# Recipe for an object that includes the LKL headers (one place for the flags).
define lkl-cc
	@mkdir -p $(@D)
	$(msg) CC $@
	$(Q)$(CC) $(CPPFLAGS) $(LKL_CPPFLAGS) $(CFLAGS) $(LKL_DEPFLAGS) -c -o $@ $<
endef

$(LKL_HDR_OBJS): $(OBJDIR)/%.o: ../lkl/%.c $(LKL_RTMUX_LIB)
	$(lkl-cc)

# connectpath.c is pure libc (no LKL headers), also host-fuzzed via tools/host.
$(OBJDIR)/connectpath.o: $(LKL_CONNECTPATH_SRC)
	@mkdir -p $(@D)
	$(msg) CC $@
	$(Q)$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) -c -o $@ $<

# Obtain the LKL archive: build it from source when LKL_SRC is set (must be an
# absolute path to an LKL tree), else extract the prebuilt tarball.  Both land in
# LKL_OUT (a gitignored prebuilt/ path), keyed on LKL_OUT_NAME so a personality
# server resolves its own subdir either way -- extract lands the combined archive
# in out-fsd/out-netd as a fallback (no lean win, but a working link), while
# --build-from-source produces the lean archive there.
$(LKL_RTMUX_LIB):
	$(msg) LKL $@
	$(Q)cd ../.. && if test -n "$(LKL_SRC)"; then \
		case "$(LKL_SRC)" in /*) ;; \
			*) echo "LKL_SRC must be an absolute path" >&2; exit 2;; esac; \
		LKL_SRC="$(LKL_SRC)" LKL_PERSONALITY="$(LKL_PERSONALITY)" \
			LKL_RTMUX_OUT="prebuilt/lkl-rv64/$(LKL_OUT_NAME)" \
			scripts/build-lkl-rv64.sh --build-from-source; \
	else \
		LKL_RTMUX_OUT="prebuilt/lkl-rv64/$(LKL_OUT_NAME)" \
			scripts/build-lkl-rv64.sh --extract; \
	fi
