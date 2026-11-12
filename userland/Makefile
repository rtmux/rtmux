#
# Makefile for userland/ — user-space programs and toolbox applets.
#

.DEFAULT_GOAL := all

include ../mk/def.mk
include ../mk/recurse.mk
include lib/common.mk

ifeq ($(CONFIG_LKL_SERVERS),y)
CPPFLAGS += -DCONFIG_LKL_SERVERS=1
endif

CORE_DIRS = pipe waitfor
DRIVER_DIRS = dev-serial-8250 dev-serial-sifive dev-nvme romfs ramblk
# The syscall-conformance suite lives in tests/userland (built and installed by
# the top-level Makefile), not here.

SUBDIRS = $(CORE_DIRS) $(DRIVER_DIRS)
$(call RTMUX_RECURSE_DIRS,$(SUBDIRS))

USR_BUILD = $(OUT)/userland
TOOLBOX = $(USR_BUILD)/toolbox

TOOLBOX_SRCS = \
    utils/toolbox.c \
    utils/vi.c \
    utils/sh.c \
    utils/getty.c \
    utils/mount.c \
    utils/fstab.c \
    utils/vfslist.c \
    utils/dmesg.c
TOOLBOX_OBJS = $(TOOLBOX_SRCS:%.c=$(OBJDIR)/%.o)

# Each entry is the path under $(USR_BUILD) to a built binary; the install dst
# in $(INITRAMFS_DIR)/bin/ is just the basename, so no src:dst pairs needed.
INSTALL_BIN  = \
    toolbox \
    dev-serial-8250/dev-serial-8250 \
    dev-serial-sifive/dev-serial-sifive \
    dev-nvme/dev-nvme \
    romfs/romfs \
    ramblk/ramblk \
    pipe/pipe \
    waitfor/waitfor
TOOLBOX_BIN_APPLETS = \
    authtest \
    basename \
    cat \
    clear \
    cp \
    crash \
    cut \
    date \
    df \
    dirname \
    dmesg \
    echo \
    env \
    expr \
    false \
    find \
    grep \
    head \
    help \
    id \
    kill \
    less \
    login \
    ls \
    more \
    mount \
    mv \
    not \
    printenv \
    printf \
    ps \
    pwd \
    readlink \
    rm \
    seq \
    setconsole \
    sh \
    sleep \
    sort \
    stat \
    stty \
    sync \
    tail \
    tee \
    test \
    top \
    touch \
    tr \
    true \
    uniq \
    vi \
    wc \
    which \
    whoami \
    xargs \
    yes
# The network applets are always compiled into the toolbox binary (BusyBox
# style: one multi-call binary, all applets), so the binary is byte-identical
# regardless of CONFIG_LKL_SERVERS. Toggling the flag only decides whether they
# are exposed as /bin symlinks below — it does not change any compiled object,
# so no config-toggle rebuild stamp is needed and /sbin/getty (a toolbox copy)
# keeps one manifest hash across configs.
# inject is a separate copy of the toolbox (like login), because its boot-manifest
# grant of RTMUX_PRIV_CAP_INJECT is keyed by resolved path and a symlink would
# resolve to /bin/toolbox. Installed only on a CONFIG_DEBUG_INJECT kernel.
ifeq ($(CONFIG_DEBUG_INJECT),y)
TOOLBOX_BIN_APPLETS += inject
endif

ifeq ($(CONFIG_LKL_SERVERS),y)
TOOLBOX_BIN_APPLETS += \
    dns \
    hello \
    host \
    httpget \
    ifconfig \
    ip \
    nc \
    netstat \
    nslookup \
    ping \
    route \
    wget
endif
TOOLBOX_SBIN_APPLETS = \
    getty \
    shutdown

all: build

build: $(SUBDIRS) $(TOOLBOX)

toolbox: $(TOOLBOX)

$(OBJDIR)/%.o: %.c
	@mkdir -p $(@D)
	$(msg) CC $@
	$(Q)$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(TOOLBOX): $(TOOLBOX_OBJS) $(CRT0_O) $(LIBC_SO)
	@mkdir -p $(@D)
	$(msg) LD $@
	$(Q)$(LD) -nostdlib -o $@ $(CRT0_O) $(TOOLBOX_OBJS) $(LIBC_SO)

install: build
	$(Q)mkdir -p $(INITRAMFS_DIR)/bin $(INITRAMFS_DIR)/sbin
	$(Q)ln -snf usr/conf $(INITRAMFS_DIR)/etc
	$(Q)for src in $(INSTALL_BIN); do \
		dst=$(INITRAMFS_DIR)/bin/$${src##*/}; \
		cp $(USR_BUILD)/$$src $$dst || exit 1; \
		$(STRIP) --strip-unneeded $$dst || exit 1; \
	done
	$(Q)for applet in $(TOOLBOX_BIN_APPLETS); do \
		case "$$applet" in \
		login|inject) \
			cp $(USR_BUILD)/toolbox $(INITRAMFS_DIR)/bin/$$applet || exit 1; \
			$(STRIP) --strip-unneeded $(INITRAMFS_DIR)/bin/$$applet || exit 1 ;; \
		*) \
			ln -sf toolbox $(INITRAMFS_DIR)/bin/$$applet || exit 1 ;; \
		esac; \
	done
ifeq ($(CONFIG_LKL_SERVERS),y)
	$(Q)cp verify-lkl-net $(INITRAMFS_DIR)/bin/verify-lkl-net
	$(Q)chmod +x $(INITRAMFS_DIR)/bin/verify-lkl-net
endif
	$(Q)for applet in $(TOOLBOX_SBIN_APPLETS); do \
		cp $(USR_BUILD)/toolbox $(INITRAMFS_DIR)/sbin/$$applet || exit 1; \
		$(STRIP) --strip-unneeded $(INITRAMFS_DIR)/sbin/$$applet || exit 1; \
	done
	$(Q)cp init $(INITRAMFS_DIR)/sbin/init
	$(Q)chmod +x $(INITRAMFS_DIR)/sbin/init

clean: $(addprefix clean-,$(SUBDIRS))
	$(Q)rm -f $(TOOLBOX) $(TOOLBOX_OBJS) $(TOOLBOX_OBJS:.o=.d) \
		$(USR_BUILD)/.cfg-lkl-on $(USR_BUILD)/.cfg-lkl-off

.PHONY: all build toolbox install clean

# Auto-generated header dependencies (-MMD/-MP above). Pulls in the per-object
# .d files so a touched header — including the generated autoconf.h /
# rtmux-version.h — rebuilds the toolbox objects instead of leaving them stale.
-include $(TOOLBOX_OBJS:.o=.d)
