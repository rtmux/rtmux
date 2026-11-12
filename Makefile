#
# Top-level Makefile
#

.DEFAULT_GOAL := all
.DELETE_ON_ERROR:

include mk/def.mk
include mk/kconfig.mk
include mk/recurse.mk

# Host-side Python interpreter for tree-wide generator scripts.  Overridable
# (e.g. PYTHON=python3.12) for environments where the default isn't on PATH.
PYTHON ?= python3

# Canonical release version.  Drives internal/include/rtmux-version.h and the boot
# banner.  Bump here when cutting a release; override with `make VERSION=x.y`
# for ad-hoc builds.  Decoupled from git tags so historical tags don't leak
# into the RTMux version line.
VERSION := 0.11

# Decompose VERSION into major/minor/patch using pure make text functions.
# Missing trailing fields default to 0 (e.g. "0.11" -> 0/11/0).
ver_parts  := $(subst ., ,$(VERSION)) 0 0 0
VER_MAJOR  := $(word 1,$(ver_parts))
VER_MINOR  := $(word 2,$(ver_parts))
VER_PATCH  := $(word 3,$(ver_parts))

VER_FILE = internal/include/rtmux-version.h
IMAGE_DIR = $(OUT)/images
NVME_IMG = $(IMAGE_DIR)/nvme.img
NVME_PRE_IMG = $(IMAGE_DIR)/nvme-pre.dat
NVME_P8_IMG = $(IMAGE_DIR)/nvme-p8.dat
LKL_EXT4_IMG = $(IMAGE_DIR)/lkl-ext4.img

.PHONY: all servers lkl-servers initramfs clean distclean qemu host-tests
.PHONY: check-rt qemu-regress check-rt-budgets check-cap-namespace
.PHONY: check-emu-headless check-formal check-formal-claims check-gate-cert check-porting-contract check-kernel-nofp check-rcu-litmus check-ipc-litmus
.PHONY: check-ipc-cbmc check-user-slice check-cap-smt check-srr-pi-smt check-temporal
.PHONY: check-lifecycle-smt
.PHONY: check-ipc-deadlock
.PHONY: check-capability-abi check-capability-law check-smp-invariants
.PHONY: check-memory-safety-inventory check-assumptions
.PHONY: check-boot-manifest check-loader-negative verify
.PHONY: check-evidence-claims check-fault-recovery check-platform-boundary-contracts
.PHONY: check-fu740-smoke-scripts check-make-target-names check-ci-coverage check-lkl-scripts
.PHONY: check-header-deps
.PHONY: footprint-report check-lkl check-lkl-containment check-lkl-iommu-preflight check-lkl-iommu-live check-lkl-qemu check-lkl-differential check-lkl-trace check-trace-export check-trace-overhead
.PHONY: check-qemu-deps check-lkl-differential-deps check-map-phys-inventory check-rcu-sched driver-gate indent check-indent check-style-deps
.PHONY: posix-matrix check-posix-matrix check-posix-matrix-strict-ignore check-posix-matrix-options check-pse51 check-opts-pse51 check-posix-profile-header check-posix-profile-config check-posix-profile-surface posix-profile-boot-probe check-lkl-floor check-lkl-profile lkl-profile-smoke check-syscall-abi check-libc-abi

# Ensure include/arch symlink matches CONFIG_ARCH before anything else
ifneq ($(ARCH),none)
ARCH_LINK_TARGET := ../arch/$(ARCH)/include
ARCH_LINK_ACTUAL := $(shell readlink include/arch 2>/dev/null)
ifneq ($(ARCH_LINK_ACTUAL),$(ARCH_LINK_TARGET))
  $(info Fixing include/arch -> $(ARCH_LINK_TARGET))
  $(shell rm -rf include/arch && ln -s $(ARCH_LINK_TARGET) include/arch)
endif
endif

all: $(AUTOCONF_H) $(VER_FILE) startup initramfs

# Verify QEMU is available. OpenSBI ships with QEMU 11+ (and macOS
# Homebrew packages it that way), so we only check for the QEMU binary
# itself; scripts/emu.sh auto-detects opensbi (default = bundled, Debian path
# when present, env BIOS=... override).
check-qemu-deps:
	@command -v qemu-system-riscv64 >/dev/null 2>&1 || \
		{ echo "Error: qemu-system-riscv64 not found in PATH."; \
		  echo "  Debian/Ubuntu: apt install qemu-system-misc opensbi"; \
		  echo "  macOS:         brew install qemu"; exit 1; }
ifeq ($(CONFIG_LKL_SERVERS),y)
	@if test -n "$$MKFS_EXT4"; then \
		command -v "$$MKFS_EXT4" >/dev/null 2>&1; \
	else \
		command -v mkfs.ext4 >/dev/null 2>&1 || \
		test -x /opt/homebrew/opt/e2fsprogs/sbin/mkfs.ext4 || \
		test -x /usr/local/opt/e2fsprogs/sbin/mkfs.ext4; \
	fi || { echo "Error: mkfs.ext4 not found in PATH."; \
		  echo "  Debian/Ubuntu: apt install e2fsprogs"; \
		  echo "  macOS:         brew install e2fsprogs"; exit 1; }
endif

check-lkl-differential-deps:
	@if test -n "$$MKFS_EXT4"; then \
		command -v "$$MKFS_EXT4" >/dev/null 2>&1; \
	else \
		command -v mkfs.ext4 >/dev/null 2>&1 || \
		test -x /opt/homebrew/opt/e2fsprogs/sbin/mkfs.ext4 || \
		test -x /usr/local/opt/e2fsprogs/sbin/mkfs.ext4; \
	fi || { echo "Error: mkfs.ext4 not found in PATH."; \
		  echo "  Debian/Ubuntu: apt install e2fsprogs"; \
		  echo "  macOS:         brew install e2fsprogs"; exit 1; }
	@if test -n "$$FSCK_EXT4"; then \
		command -v "$$FSCK_EXT4" >/dev/null 2>&1; \
	else \
		command -v fsck.ext4 >/dev/null 2>&1 || \
		command -v e2fsck >/dev/null 2>&1 || \
		test -x /opt/homebrew/opt/e2fsprogs/sbin/fsck.ext4 || \
		test -x /opt/homebrew/opt/e2fsprogs/sbin/e2fsck || \
		test -x /usr/local/opt/e2fsprogs/sbin/fsck.ext4 || \
		test -x /usr/local/opt/e2fsprogs/sbin/e2fsck; \
	fi || { echo "Error: e2fsck/fsck.ext4 not found in PATH."; \
		  echo "  Debian/Ubuntu: apt install e2fsprogs"; \
		  echo "  macOS:         brew install e2fsprogs"; exit 1; }
	@if test -n "$$DEBUGFS"; then \
		command -v "$$DEBUGFS" >/dev/null 2>&1; \
	else \
		command -v debugfs >/dev/null 2>&1 || \
		test -x /usr/sbin/debugfs || test -x /sbin/debugfs || \
		test -x /opt/homebrew/opt/e2fsprogs/sbin/debugfs || \
		test -x /usr/local/opt/e2fsprogs/sbin/debugfs; \
	fi || { echo "Error: debugfs not found in PATH."; \
		  echo "  Debian/Ubuntu: apt install e2fsprogs"; \
		  echo "  macOS:         brew install e2fsprogs"; exit 1; }

# Version file — generated from the VERSION make variable above.
# The build identity is a git short hash ("-d" when dirty); override with
# RTMUX_BUILD_ID=.  FORCE re-runs the generator every time, but it only replaces
# the file when the content changed, so an unchanged identity keeps the mtime
# and does not retrigger compilation.
$(VER_FILE): FORCE
	@./scripts/gen-version.sh $@ '$(VERSION)' '$(VER_MAJOR)' '$(VER_MINOR)' \
	    '$(VER_PATCH)' '$(RTMUX_BUILD_ID)'

.PHONY: FORCE
FORCE:

version: $(VER_FILE)

$(call RTMUX_RECURSE_ALIAS,startup,arch/$(ARCH),$(VER_FILE) libc)
$(call RTMUX_RECURSE_ALIAS,libc,lib/libc,$(AUTOCONF_H) $(VER_FILE))
$(call RTMUX_RECURSE_ALIAS,ldso,lib/ldso,libc)
$(call RTMUX_RECURSE_ALIAS,userland,userland,libc)
$(call RTMUX_RECURSE_ALIAS,userland-tests,tests/userland,libc)

SERVER_DIRS = servers/roottask servers/logger servers/pci servers/virtio-mmio
LKL_SERVER_DIRS = servers/lkl-fsd servers/lkl-netd
KERNEL_AUX_DIRS = kernel/startup kernel
HOST_TOOL_DIRS = tools/host
$(call RTMUX_RECURSE_DIRS,$(SERVER_DIRS) $(LKL_SERVER_DIRS) $(KERNEL_AUX_DIRS) $(HOST_TOOL_DIRS))
$(SERVER_DIRS): libc
$(LKL_SERVER_DIRS): libc

# CONFIG_LKL_SERVERS gates the LKL Linux-domain servers into the default
# initramfs and check paths. They stay buildable via `make lkl-servers`
# regardless, but link a large external archive, so the default build leaves
# them out.
ifeq ($(CONFIG_LKL_SERVERS),y)
INITRAMFS_LKL_DEP = lkl-servers
else
INITRAMFS_LKL_DEP =
endif

# roottask embeds the bootstrap capability manifest for boot-image payloads.
# Build those payloads first so the linked manifest matches the archive created
# by the same `make initramfs` run.
servers/roottask: userland userland-tests servers/logger servers/pci servers/virtio-mmio $(INITRAMFS_LKL_DEP)

servers: $(SERVER_DIRS)
lkl-servers: $(LKL_SERVER_DIRS)
host-tools: $(HOST_TOOL_DIRS)
host-tests: $(AUTOCONF_H) $(VER_FILE)
	$(Q)$(MAKE) -C tools/host host-tests
host-lkl-fence-asm:
	$(Q)$(MAKE) -C tools/host host-lkl-fence-asm RISCV_CC='$(RISCV_CC)'
host-sanitize: $(AUTOCONF_H) $(VER_FILE)
	$(Q)$(MAKE) -C tools/host host-sanitize
host-fuzz: $(AUTOCONF_H) $(VER_FILE)
	$(Q)$(MAKE) -C tools/host host-fuzz
.PHONY: host-tools host-tests host-lkl-fence-asm host-sanitize host-fuzz

clean:
	$(Q)rm -rf $(OUT) $(AUTOCONF_H) $(VER_FILE)

# distclean: clean + the runtime disk images
# ($(NVME_IMG) and the two-piece NVMe split).  The images are
# auto-regenerated by scripts/emu.sh on first run and take a few seconds
# to lay; we don't want a routine `make clean` to wipe them, but
# `make distclean` gives a truly fresh tree.
distclean: clean
	$(Q)rm -f $(NVME_IMG) $(NVME_PRE_IMG) $(NVME_P8_IMG) $(LKL_EXT4_IMG)
	$(Q)rmdir $(IMAGE_DIR) 2>/dev/null || true

qemu: check-qemu-deps $(AUTOCONF_H) $(VER_FILE) startup initramfs
	./scripts/emu.sh

initramfs: $(INITRAMFS_CPIO)
.PHONY: initramfs

# Rebuild the CPIO whenever any of its component artifacts changes.
# The recipe wipes $(INITRAMFS_DIR) up front so renamed / removed
# binaries don't haunt the next archive, and writes the CPIO via a
# tempfile so an interrupted run never leaves a half-written archive
# at the canonical path.
$(INITRAMFS_CPIO): servers/roottask libc ldso userland userland-tests servers $(INITRAMFS_LKL_DEP) scripts/validate-plt-relocs.py
	$(msg) MKDIR $(INITRAMFS_DIR)/
	$(Q)rm -rf $(INITRAMFS_DIR)
	$(Q)mkdir -p $(INITRAMFS_DIR)/boot $(INITRAMFS_DIR)/lib $(INITRAMFS_DIR)/sbin
	$(Q)cp $(ROOTTASK_BIN) $(INITRAMFS_DIR)/boot/
	$(Q)cp $(ROOTTASK_BIN) $(INITRAMFS_DIR)/sbin/coresrv
	$(Q)cp $(ROOTTASK_BIN) $(INITRAMFS_DIR)/sbin/namesrv
	$(Q)cp $(LIBC_SO) $(INITRAMFS_DIR)/lib/
	$(Q)cp $(LDSO_SO) $(INITRAMFS_DIR)/lib/
	$(Q)ln -sf ld-rtmux.so $(INITRAMFS_DIR)/lib/ld.so.1
	$(Q)cp $(OUT)/servers/logger/logger $(INITRAMFS_DIR)/sbin/
	$(Q)cp $(OUT)/servers/pci/pci-server $(INITRAMFS_DIR)/sbin/
	$(Q)cp $(OUT)/servers/virtio-mmio/virtio-mmio $(INITRAMFS_DIR)/sbin/
ifeq ($(CONFIG_LKL_SERVERS),y)
	$(Q)mkdir -p $(INITRAMFS_DIR)/share
	$(Q)if test -f $(LKL_EXT4_IMG); then cp $(LKL_EXT4_IMG) $(INITRAMFS_DIR)/share/; fi
	$(Q)cp $(OUT)/servers/lkl-fsd/lkl-fsd $(INITRAMFS_DIR)/sbin/
	$(Q)cp $(OUT)/servers/lkl-netd/lkl-netd $(INITRAMFS_DIR)/sbin/
	$(Q)$(STRIP) --strip-unneeded $(INITRAMFS_DIR)/sbin/lkl-fsd
	$(Q)$(STRIP) --strip-unneeded $(INITRAMFS_DIR)/sbin/lkl-netd
endif
	$(Q)$(MAKE) -C userland install
	$(Q)$(MAKE) -C tests/userland install
	$(msg) PLTREL $(INITRAMFS_DIR)
	$(Q)$(PYTHON) scripts/validate-plt-relocs.py --readelf "$(READELF)" \
	    $(INITRAMFS_DIR)
	$(msg) BOOTHASH $(INITRAMFS_DIR)/boot-manifest.hashes
	$(Q)$(PYTHON) scripts/gen-boot-manifest-hashes.py \
	    --manifest servers/roottask/sys/bootstrap-manifest.c \
	    --root $(INITRAMFS_DIR) --out $(INITRAMFS_DIR)/boot-manifest.hashes
	$(msg) CPIO $@
	$(Q)tmp=$(abspath $@).tmp; trap 'rm -f "$$tmp"' EXIT; \
	    (cd $(INITRAMFS_DIR) && find . | sort | cpio -H newc -o) > $$tmp 2>/dev/null \
	    && mv $$tmp $@; \
	    status=$$?; \
	    trap - EXIT; \
	    rm -f "$$tmp"; \
	    exit $$status

MKROMFS = $(OUT)/tools/host/mkromfs

$(MKROMFS): tools/host/mkromfs.c userland/romfs/fs.h
	$(Q)$(MAKE) -C tools/host $@

# ---------------------------------------------------------------------
# Self-test: boot RTMux under qemu-system-riscv64, parse the conformance
# suite result, and reject crash/fault/rescue-shell signatures in the boot log.
# ---------------------------------------------------------------------
ifeq ($(CONFIG_LKL_SERVERS),y)
LKL_CHECK_DEP = check-lkl
else
LKL_CHECK_DEP =
endif
LKL_QEMU_MODES = \
    lkl-contain:60 \
    lkl-iommu-probe:60 \
    lkl-iommu-sys-probe:60 \
    lkl-iommu-metadata:60 \
    lkl-iommu-domain-preflight:60 \
    lkl-netd-iommu-fail-closed:60 \
    lkl-host-selftest:60 \
    lkl-fsd-crash:180 \
    lkl-fsd-tree:180 \
    lkl-netd-crash:180 \
    lkl-netd-revoke:180 \
    lkl-netd-stale:180 \
    lkl-netd-http:300

LKL_DIFF_MODES = \
    lkl-fsd-tree:180 \
    lkl-netd-http:300 \
    lkl-multi:180
LKL_HOST_AUTHORED_MODES = \
    lkl-fsd-tricky:180
LKL_TRACE_TIMEOUT ?= 360
QEMU_SYSCALL_TIMEOUT ?= 300
QEMU_SMP_TIMEOUT ?= 180
# Seconds a guest boot or mode may run; slower hosts (CI runners) raise it.
QEMU_BOOT_TIMEOUT ?= 180
LKL_TRACE_MODES = \
    tracelogger-lkl-fsd:$(LKL_TRACE_TIMEOUT) \
    tracelogger-lkl-netd:$(LKL_TRACE_TIMEOUT)

# Build the LKL servers once into a private tree, then run every smoke mode
# against it (mode:timeout pairs from LKL_QEMU_MODES) without rebuilding.
define run-lkl-qemu-modes
	$(Q)rm -rf "$(OUT)/lkl-regress"
	+$(Q)$(MAKE) OUT="$(OUT)/lkl-regress" CONFIG_LKL_SERVERS=y startup initramfs
	+$(Q)for t in $(LKL_QEMU_MODES); do \
    mode=$${t%:*}; timeout=$${t#*:}; \
    LKL_REGRESS_BUILD_DIR="$(OUT)/lkl-regress" \
    LKL_REGRESS_SKIP_BUILD=1 \
    scripts/qemu-regress.sh --mode "$$mode" --timeout "$$timeout" || exit $$?; \
done
endef
check: check-posix-matrix check-posix-matrix-strict-ignore check-porting-contract
check: check-kernel-nofp check-formal-claims check-gate-cert
check: check-posix-matrix-options check-pse51 check-posix-profile-header
check: check-posix-profile-config check-posix-profile-surface check-lkl-profile
check: check-syscall-abi check-libc-abi check-make-target-names check-trace-export check-rt-budgets check-irq-contract
check: check-platform-boundary-contracts
check: check-lock-rank check-preempt-inventory check-loader-negative check-fu740-smoke-scripts
check: driver-gate $(LKL_CHECK_DEP) check-emu-headless check-rt check-qemu-deps
check: $(AUTOCONF_H) $(VER_FILE) startup initramfs
ifeq ($(CONFIG_LKL_SERVERS),y)
	@echo "  CHECK    LKL QEMU smoke modes"
	$(run-lkl-qemu-modes)
endif
	@echo "  CHECK    boot RTMux + syscall conformance suite"
	$(Q)./scripts/qemu-regress.sh --mode syscall-testing --timeout $(QEMU_SYSCALL_TIMEOUT)
	@echo "  CHECK    boot syscall conformance suite from /usr block filesystem"
	$(Q)./scripts/qemu-regress.sh --mode syscall-testing-disk --timeout $(QEMU_SYSCALL_TIMEOUT)
	@echo "  CHECK    PCI/NVMe bring-up smoke"
	$(Q)./scripts/qemu-regress.sh --mode pci-nvme-smoke --timeout $(QEMU_BOOT_TIMEOUT)
	@echo "  CHECK    boot RTMux + toolbox applet suite"
	$(Q)./scripts/qemu-regress.sh --mode toolbox --timeout $(QEMU_BOOT_TIMEOUT)
.PHONY: check

# Boot RTMux and run the /bin toolbox applet functional suite in isolation.
check-toolbox: check-qemu-deps $(AUTOCONF_H) $(VER_FILE) startup initramfs
	@echo "  CHECK    boot RTMux + toolbox applet suite"
	$(Q)./scripts/qemu-regress.sh --mode toolbox --timeout $(QEMU_BOOT_TIMEOUT)
.PHONY: check-toolbox

check-emu-headless:
	$(Q)scripts/emu-headless-test.sh
	$(Q)sh tests/hunt-hang/artifacts.sh

# Boot coverage by target, one owner per mode so no gate repeats another's boot:
#   check              syscall-testing once, syscall-testing-disk, pci-nvme, toolbox
#   check-cap-namespace host tests + syscall-testing once (the capgate evidence the
#                      capability docs cite); no interactive-smoke
#   qemu-regress       interactive-smoke once + syscall-testing x3 (flake gate); the
#                      only gate that boots interactive-smoke
qemu-regress: check-qemu-deps $(AUTOCONF_H) $(VER_FILE) startup initramfs
	@echo "  CHECK    repeated QEMU boot/syscall regression gate"
	$(Q)./scripts/qemu-regress.sh --mode interactive-smoke --timeout $(QEMU_BOOT_TIMEOUT)
	$(Q)./scripts/qemu-regress.sh --mode syscall-testing --timeout $(QEMU_SYSCALL_TIMEOUT) --repeat 3

# Interactive TTY smoke: expect-style serial I/O driving login, the shell, and
# the vi applet, then job control (^C/^Z/fg/tostop). Kept out of `check` because it is timing-sensitive; run it
# explicitly with `make tty-smoke`.
tty-smoke: check-qemu-deps $(AUTOCONF_H) $(VER_FILE) startup initramfs
	@echo "  CHECK    interactive TTY smoke (login + sh + vi over serial)"
	$(Q)python3 scripts/vi-test.py
	@echo "  CHECK    job control over serial (^C, ^Z, fg, TOSTOP)"
	$(Q)python3 scripts/tty-jobctl-test.py
.PHONY: tty-smoke

check-cap-namespace: host-tests check-posix-matrix check-qemu-deps $(AUTOCONF_H) $(VER_FILE) startup initramfs
	@echo "  CHECK    capability/namespace regression gate"
	$(Q)$(MAKE) -C tests/userland
	$(Q)./scripts/qemu-regress.sh --mode syscall-testing --timeout $(QEMU_SYSCALL_TIMEOUT)

# Gate provenance certificates (scripts/gate_cert.py): each gate script appends
# the facts it observed to build/certs/<gate>.assert, gate-write turns them into
# build/certs/<gate>.json.  $@ is the gate target name.
gate-begin = $(Q)mkdir -p build/certs && rm -f build/certs/$@.assert
gate-env = GATE_ASSERTIONS=build/certs/$@.assert
gate-write = $(Q)$(PYTHON) scripts/gate_cert.py write $@ --tool $(1) \
    --assertions-file build/certs/$@.assert

check-capability-abi:
	$(msg) CHECK "capability ABI contract"
	$(Q)$(PYTHON) scripts/check-capability-abi.py
	$(Q)$(MAKE) check-cap-namespace

check-capability-law: check-cap-smt
	$(msg) CHECK "capability law checklist"
	$(gate-begin)
	$(Q)$(gate-env) $(PYTHON) scripts/check-capability-law.py
	$(Q)$(gate-env) $(MAKE) check-capability-abi
	$(call gate-write,python3) --needs check-cap-smt

check-boot-manifest: check-qemu-deps $(AUTOCONF_H) $(VER_FILE) startup initramfs
	$(msg) CHECK "boot capability manifest"
	$(Q)$(PYTHON) scripts/check-boot-manifest.py
	$(Q)$(MAKE) -C tools/host host-tests
	$(Q)$(MAKE) check-cap-namespace
	$(Q)scripts/qemu-regress.sh --mode least-privilege-manifest --timeout $(QEMU_BOOT_TIMEOUT)
	$(Q)scripts/qemu-regress.sh --mode boot-manifest --timeout $(QEMU_BOOT_TIMEOUT)

check-loader-negative: check-qemu-deps $(AUTOCONF_H) $(VER_FILE) startup initramfs
	$(msg) CHECK "loader image policy negatives"
	$(Q)scripts/qemu-regress.sh --mode loader-negative --timeout $(QEMU_BOOT_TIMEOUT)

check-smp-invariants: check-qemu-deps $(AUTOCONF_H) $(VER_FILE) startup initramfs
	$(msg) CHECK "SMP invariant ledger"
	$(Q)$(PYTHON) scripts/check-smp-invariants.py
	$(Q)scripts/qemu-regress.sh --mode smp-invariants --timeout $(QEMU_SMP_TIMEOUT)

check-memory-safety-inventory:
	$(msg) CHECK "memory-safety inventory"
	$(Q)$(PYTHON) scripts/check-memory-safety-inventory.py
	$(Q)$(PYTHON) scripts/check-memory-safety-inventory.py \
	    --expect-fail --expect-error "empty test_command" \
	    docs/memory-safety-inventory-negative.md

check-assumptions:
	$(msg) CHECK "assumption ledger"
	$(gate-begin)
	$(Q)$(gate-env) $(PYTHON) scripts/check-assumptions.py
	$(call gate-write,python3)

check-evidence-claims:
	$(msg) CHECK "safety-runtime evidence claims"
	$(gate-begin)
	$(Q)$(gate-env) scripts/check-evidence-claims.sh
	$(Q)$(gate-env) scripts/check-evidence-claims.sh --expect-fail \
	    docs/safety-runtime-negative.md
	$(call gate-write,python3)

check-fault-recovery: check-qemu-deps $(AUTOCONF_H) $(VER_FILE) startup initramfs
	$(msg) CHECK "fault recovery model"
	$(Q)scripts/check-fault-recovery.sh
	$(Q)scripts/check-fault-recovery.sh --expect-fail \
	    docs/fault-recovery-negative.md
	$(Q)scripts/qemu-regress.sh --mode fault-recovery --timeout $(QEMU_BOOT_TIMEOUT)

check-fu740-smoke-scripts:
	$(msg) CHECK "FU740 smoke script parsers"
	$(Q)scripts/fu740-smoke-test.sh

check-formal-claims:
	$(msg) CHECK "formal claim schema"
	$(gate-begin)
	$(Q)$(gate-env) $(PYTHON) scripts/check-formal-claims.py
	$(Q)$(PYTHON) scripts/check-formal-claims.py tests/formal-claims/valid-*.md
	$(Q)$(PYTHON) scripts/check-formal-claims.py --expect-fail \
	    tests/formal-claims/bad-*.md
	$(Q)$(PYTHON) scripts/check-formal-claims.py --expect-fail --layers \
	    tests/formal-claims/coverage-incomplete.md
	$(call gate-write,python3)

check-platform-boundary-contracts:
	$(msg) CHECK "platform boundary contracts"
	$(Q)scripts/check-platform-boundary-contracts.sh
	$(Q)scripts/check-platform-boundary-contracts.sh \
	    --contracts tests/platform-boundary/contracts.md \
	    tests/platform-boundary/valid-claims.md
	$(Q)scripts/check-platform-boundary-contracts.sh \
	    --contracts tests/platform-boundary/contracts.md \
	    tests/platform-boundary/valid-claims.md \
	    tests/platform-boundary/pcol-a.md tests/platform-boundary/pcol-b.md
	$(Q)scripts/check-platform-boundary-trap-test.sh
	$(Q)scripts/check-platform-boundary-path-test.sh
	$(Q)for f in tests/platform-boundary/bad-*.md; do \
	    case $$f in \
	    */bad-contracts-*) c=$$f; t=$$f ;; \
	    *) c=tests/platform-boundary/contracts.md; t=$$f ;; \
	    esac; \
	    scripts/check-platform-boundary-contracts.sh --expect-fail \
	        --contracts $$c $$t >/dev/null 2>&1 || { echo "platform-boundary: $$f"; exit 1; }; \
	done
	$(Q)for f in tests/platform-boundary/meta-*.md; do \
	    if scripts/check-platform-boundary-contracts.sh --expect-fail \
	        --contracts tests/platform-boundary/contracts.md $$f >/dev/null 2>&1; then \
	        echo "platform-boundary: $$f was accepted as the expected failure"; exit 1; \
	    fi; \
	done

check-gate-cert:
	$(msg) CHECK "gate certificate verifier negatives"
	$(Q)$(PYTHON) tests/gate-cert/test_gate_cert.py

check-porting-contract: libc
	$(msg) CHECK "porting omissions"
	$(Q)CC="$(CC)" LD="$(LD)" NM="$(NM)" LIBC_A="$(LIBC_A)" LIBC_SO="$(LIBC_SO)" \
	    scripts/check-porting-contract.sh

check-kernel-nofp:
	$(msg) CHECK "kernel executes no FP instructions"
	$(Q)OBJDUMP="$(OBJDUMP)" scripts/check-kernel-nofp.sh

check-lkl-scripts:
	$(msg) CHECK LKL source fetch and patch scripts
	$(Q)sh scripts/lkl/fetch-test.sh
	$(Q)sh scripts/lkl/apply-patches-test.sh

check-ci-coverage:
	$(msg) CHECK CI covers every test target
	$(Q)$(PYTHON) scripts/check-ci-coverage.py

# Lint every Makefile for $(DEPFLAGS)/-include, then touch representative headers
# on the built tree and require the dependent components to rebuild. Needs a
# built tree (make) for the behavioural half.
check-header-deps: all
	$(msg) CHECK "header changes rebuild dependents"
	$(Q)OUT="$(OUT)" scripts/check-header-deps.sh

check-make-target-names:
	$(msg) CHECK "make target naming"
	$(Q)$(PYTHON) scripts/check-make-target-names.py

check-formal: check-assumptions check-evidence-claims check-formal-claims check-capability-law
check-formal: check-gate-cert
check-formal: check-ipc-cbmc check-user-slice
check-formal: check-cap-smt check-srr-pi-smt check-temporal
check-formal: check-lifecycle-smt
check-formal: check-ipc-deadlock

# Frama-C over kernel/: front end accepts every source, and every discharged
# obligation under formal/framac/proofs/ still discharges.  Deliberately not a
# check-formal prerequisite -- that aggregate runs in CI, and this one needs an
# opam Frama-C switch.  See formal/framac/README.md; the script takes a stage
# argument (parse | proofs) when you want only one half.
verify: $(AUTOCONF_H) $(VER_FILE)
	$(msg) VERIFY "Frama-C kernel front end + proof obligations"
	$(Q)scripts/verify-framac.sh

footprint-report: $(AUTOCONF_H) $(VER_FILE) startup initramfs
	$(Q)OUT="$(OUT)" SIZE="$(SIZE)" KERNEL_BIN="$(KERNEL_BIN)" \
	    ROOTTASK_BIN="$(ROOTTASK_BIN)" LIBC_SO="$(LIBC_SO)" \
	    LDSO_SO="$(LDSO_SO)" INITRAMFS_DIR="$(INITRAMFS_DIR)" \
	    INITRAMFS_CPIO="$(INITRAMFS_CPIO)" \
	    $(PYTHON) scripts/footprint-report.py

check-lkl:
	$(msg) CHECK LKL domain gates
	$(Q)scripts/check-lkl-domain.sh
	$(Q)LKL_LIB=prebuilt/lkl-rv64/out-fsd/liblkl-rtmux.a \
	    LKL_RTMUX_OUT=prebuilt/lkl-rv64/out-fsd \
	    scripts/build-lkl-rv64.sh --check-only
	$(Q)LKL_LIB=prebuilt/lkl-rv64/out-netd/liblkl-rtmux.a \
	    LKL_RTMUX_OUT=prebuilt/lkl-rv64/out-netd \
	    scripts/build-lkl-rv64.sh --check-only

check-map-phys-inventory:
	$(msg) CHECK MAP_PHYS inventory
	$(Q)scripts/check-map-phys-inventory.sh

check-rcu-sched:
	$(msg) CHECK RCU-sched hooks
	$(Q)scripts/check-rcu-sched.sh
	$(msg) CHECK RCU-sched mutations
	$(Q)$(PYTHON) tests/rcu/sched-mutations.py

# Diagnostic reproduction of an open race, not a proof-of-correctness gate.
check-reply-timeout-race:
	$(msg) CHECK "reply timeout race reproduction"
	$(Q)sh scripts/check-reply-timeout-race.sh
.PHONY: check-reply-timeout-race

# Diagnostic reproduction of open teardown races, not a correctness gate.
check-channel-teardown-race:
	$(msg) CHECK "channel teardown race reproduction"
	$(Q)sh scripts/check-channel-teardown-race.sh
.PHONY: check-channel-teardown-race

# Diagnostic reproduction of an open debug-reader race (Q85), not a correctness gate.
check-debug-blocked-on:
	$(msg) CHECK "debug_thread blocked_on reader reproduction"
	$(Q)sh scripts/check-debug-blocked-on.sh
.PHONY: check-debug-blocked-on

check-rcu-litmus:
	$(msg) CHECK RCU RVWMO litmus
	$(Q)scripts/check-rcu-litmus.sh

check-ipc-litmus:
	$(msg) CHECK IPC WAIT_PENDING RVWMO litmus
	$(Q)scripts/check-ipc-litmus.sh

check-ipc-cbmc:
	$(msg) CHECK "IPC message-pool alloc/free pairing (CBMC)"
	$(gate-begin)
	$(Q)$(gate-env) scripts/check-ipc-cbmc.sh
	$(call gate-write,cbmc)

check-user-slice:
	$(msg) CHECK "typed user-pointer boundary (CBMC)"
	$(gate-begin)
	$(Q)$(gate-env) scripts/check-user-slice.sh
	$(call gate-write,cbmc)

check-cap-smt:
	$(msg) CHECK "capability rights/revocation algebra (Z3)"
	$(gate-begin)
	$(Q)$(gate-env) scripts/check-cap-smt.sh
	$(call gate-write,z3)

check-srr-pi-smt:
	$(msg) CHECK "SRR + bounded-PI safety (Z3)"
	$(gate-begin)
	$(Q)$(gate-env) scripts/check-srr-pi-smt.sh
	$(call gate-write,z3)

check-lifecycle-smt:
	$(msg) CHECK "object-lifecycle teardown invariants (Z3)"
	$(gate-begin)
	$(Q)$(gate-env) scripts/check-lifecycle-smt.sh
	$(call gate-write,z3)

check-temporal:
	$(msg) CHECK "IPC/PI temporal liveness (Spin)"
	$(gate-begin)
	$(Q)$(gate-env) scripts/check-temporal.sh
	$(call gate-write,spin)

check-ipc-deadlock:
	$(msg) CHECK "SRR deadlock freedom: send ranks + wait-for graph (Spin)"
	$(gate-begin)
	$(Q)sh tests/ipc-deadlock/tool-failures.sh
	$(Q)$(gate-env) scripts/check-ipc-deadlock.sh
	$(call gate-write,spin)

check-lkl-iommu-preflight: check-qemu-deps $(AUTOCONF_H) $(VER_FILE) startup initramfs
	@echo "  CHECK    LKL IOMMU preflight"
	$(Q)scripts/qemu-regress.sh --mode lkl-iommu-probe --timeout 60
	$(Q)scripts/qemu-regress.sh --mode lkl-iommu-sys-probe --timeout 60
	$(Q)scripts/qemu-regress.sh --mode lkl-iommu-metadata --timeout 60
	$(Q)scripts/qemu-regress.sh --mode lkl-iommu-domain-preflight --timeout 60

check-lkl-iommu-live: check-qemu-deps $(AUTOCONF_H) $(VER_FILE) startup initramfs
	@echo "  CHECK    LKL IOMMU live enforcement"
	$(Q)scripts/qemu-regress.sh --mode lkl-iommu-domain --timeout $(QEMU_BOOT_TIMEOUT) --require-supported
	$(Q)scripts/qemu-regress.sh --mode lkl-iommu-dma-deny --timeout $(QEMU_BOOT_TIMEOUT) --require-supported
	$(Q)scripts/qemu-regress.sh --mode lkl-iommu-revoke --timeout $(QEMU_BOOT_TIMEOUT) --require-supported

check-lkl-containment: check-lkl check-lkl-profile check-map-phys-inventory check-lkl-iommu-preflight check-qemu-deps $(AUTOCONF_H) $(VER_FILE) startup initramfs
	@echo "  CHECK    LKL containment regression"
	$(Q)scripts/qemu-regress.sh --mode lkl-contain --timeout 60
	$(Q)scripts/qemu-regress.sh --mode lkl-netd-direct-denied --timeout 60
	$(Q)scripts/qemu-regress.sh --mode lkl-netd-iommu-fail-closed --timeout 60

check-trace-export:
	$(msg) CHECK trace exporters
	$(Q)scripts/rt-trace-json-test.sh
	$(Q)scripts/rt-trace-ctf-test.sh
	$(Q)scripts/rt-trace-lkl-host-test.sh
	$(Q)scripts/rt-trace-irq-test.sh
	$(Q)scripts/rt-trace-sched-test.sh
	$(Q)scripts/rt-trace-budget-test.sh
	$(Q)scripts/rt-check-compare-test.sh

TRACE_OVERHEAD_REPEAT ?= 5
TRACE_OVERHEAD_TIMEOUT ?= 180
TRACE_OVERHEAD_DELTA_NS ?= 2000000

check-trace-overhead: check-qemu-deps $(AUTOCONF_H) $(VER_FILE) startup initramfs
	@echo "  CHECK    trace instrumentation overhead"
	$(Q)scripts/qemu-regress.sh --mode rt-check --repeat $(TRACE_OVERHEAD_REPEAT) \
	    --timeout $(TRACE_OVERHEAD_TIMEOUT) --keep-logs
	$(Q)scripts/qemu-regress.sh --mode rt-check-instr --repeat $(TRACE_OVERHEAD_REPEAT) \
	    --timeout $(TRACE_OVERHEAD_TIMEOUT) --keep-logs
	$(Q)python3 scripts/rt-check-compare.py \
	    --max-max-delta-ns $(TRACE_OVERHEAD_DELTA_NS) \
	    --max-p99-delta-ns $(TRACE_OVERHEAD_DELTA_NS) \
	    --base 'build/lkl-logs/rt-check-[0-9]*.log' \
	    --instr 'build/lkl-logs/rt-check-instr-[0-9]*.log'
	$(Q)python3 scripts/rt-check-compare.py --aggregate median \
	    --max-max-delta-ns $(TRACE_OVERHEAD_DELTA_NS) \
	    --max-p99-delta-ns $(TRACE_OVERHEAD_DELTA_NS) \
	    --base 'build/lkl-logs/rt-check-[0-9]*.log' \
	    --instr 'build/lkl-logs/rt-check-instr-[0-9]*.log'

check-lkl-trace: check-qemu-deps $(AUTOCONF_H) $(VER_FILE) startup
	@echo "  CHECK    LKL traced latency windows"
	$(Q)rm -rf "$(OUT)/lkl-regress"
	+$(Q)skip=0; for t in $(LKL_TRACE_MODES); do \
    mode=$${t%:*}; timeout=$${t#*:}; log="build/lkl-logs/$${mode}-1.log"; \
    if [ $$skip -eq 1 ]; then \
        LKL_REGRESS_BUILD_DIR="$(OUT)/lkl-regress" \
        LKL_REGRESS_SKIP_BUILD=1 \
        scripts/qemu-regress.sh --mode "$$mode" --timeout "$$timeout" --keep-logs || exit $$?; \
    else \
        LKL_REGRESS_BUILD_DIR="$(OUT)/lkl-regress" \
        scripts/qemu-regress.sh --mode "$$mode" --timeout "$$timeout" --keep-logs || exit $$?; \
        skip=1; \
    fi; \
    grep -q '^measure_summary,' "$$log.sched.csv" || { echo "$$log.sched.csv: missing measure_summary" >&2; exit 1; }; \
    grep -q '^measure_summary,kcall_' "$$log.sched.csv" || { echo "$$log.sched.csv: missing per-kernel-call rows" >&2; exit 1; }; \
    grep -q '^measure_gap,' "$$log.sched.csv" || { echo "$$log.sched.csv: missing measure_gap" >&2; exit 1; }; \
    grep -q '^measure_client_state,' "$$log.sched.csv" || { echo "$$log.sched.csv: missing measure_client_state" >&2; exit 1; }; \
    grep -q '^measure_client_state_summary,' "$$log.sched.csv" || { echo "$$log.sched.csv: missing measure_client_state_summary" >&2; exit 1; }; \
    grep -q '^measure_critical,' "$$log.sched.csv" || { echo "$$log.sched.csv: missing measure_critical" >&2; exit 1; }; \
    grep -q '^measure_critical_state_summary,' "$$log.sched.csv" || { echo "$$log.sched.csv: missing measure_critical_state_summary" >&2; exit 1; }; \
    grep -q '^measure_critical_transition_summary,' "$$log.sched.csv" || { echo "$$log.sched.csv: missing measure_critical_transition_summary" >&2; exit 1; }; \
    grep -q '^measure_irq,' "$$log.sched.csv" || { echo "$$log.sched.csv: missing measure_irq section" >&2; exit 1; }; \
    grep -q '^measure_irq_summary,' "$$log.sched.csv" || { echo "$$log.sched.csv: missing measure_irq_summary section" >&2; exit 1; }; \
    python3 scripts/rt-budgets-check.py --schema-only "$$log.rt-budget.md" || exit $$?; \
    grep -q '^op,count,min_ns,p50_ns,p99_ns,max_ns$$' "$$log.lkl-host.csv" || { echo "$$log.lkl-host.csv: missing host-op header" >&2; exit 1; }; \
    test "$$(wc -l < "$$log.lkl-host.csv" | tr -d ' ')" -gt 1 || { echo "$$log.lkl-host.csv: missing host-op rows" >&2; exit 1; }; \
    grep -q '^irq,pid,count,min_ns,p50_ns,p99_ns,max_ns$$' "$$log.irq.csv" || { echo "$$log.irq.csv: missing irq header" >&2; exit 1; }; \
    test -s "$$log.ctf/metadata" || { echo "$$log.ctf: missing metadata" >&2; exit 1; }; \
    test -s "$$log.ctf/stream" || { echo "$$log.ctf: missing stream" >&2; exit 1; }; \
    if command -v babeltrace2 >/dev/null 2>&1; then \
        test -s "$$log.ctf.bt.log" || { echo "$$log.ctf: babeltrace2 did not parse the captured CTF" >&2; exit 1; }; \
    fi; \
done

check-lkl-qemu: check-rt check-qemu-deps $(AUTOCONF_H) $(VER_FILE) startup
	@echo "  CHECK    LKL QEMU smoke modes"
	$(run-lkl-qemu-modes)

check-lkl-differential: check-qemu-deps check-lkl-differential-deps $(AUTOCONF_H) $(VER_FILE) startup
	@echo "  CHECK    LKL differential oracles"
	$(Q)rm -rf "$(OUT)/lkl-regress"
	+$(Q)$(MAKE) OUT="$(OUT)/lkl-regress" CONFIG_LKL_SERVERS=y startup initramfs
	+$(Q)for t in $(LKL_DIFF_MODES); do \
    mode=$${t%:*}; timeout=$${t#*:}; \
    LKL_REGRESS_BUILD_DIR="$(OUT)/lkl-regress" \
    LKL_REGRESS_SKIP_BUILD=1 \
    scripts/qemu-regress.sh --mode "$$mode" --timeout "$$timeout" --differential || exit $$?; \
done
	+$(Q)for t in $(LKL_HOST_AUTHORED_MODES); do \
    mode=$${t%:*}; timeout=$${t#*:}; \
    LKL_REGRESS_BUILD_DIR="$(OUT)/lkl-regress" \
    LKL_REGRESS_SKIP_BUILD=1 \
    scripts/qemu-regress.sh --mode "$$mode" --timeout "$$timeout" || exit $$?; \
done

check-rt: check-rcu-sched check-rt-budgets check-qemu-deps $(AUTOCONF_H) $(VER_FILE) startup initramfs
	@echo "  CHECK    boot RTMux + realtime probes"
	$(Q)scripts/qemu-regress.sh --mode rt-check --timeout $(QEMU_BOOT_TIMEOUT)
	$(Q)scripts/qemu-regress.sh --mode pi-inversion-xfail --timeout 140

POSIX_MATRIX_SCRIPT = scripts/gen-posix-matrix.py

posix-matrix:
	$(msg) GEN docs/posix-compliance.md
	$(Q)$(PYTHON) $(POSIX_MATRIX_SCRIPT)

check-posix-matrix:
	$(msg) CHECK docs/posix-compliance.md
	$(Q)$(PYTHON) $(POSIX_MATRIX_SCRIPT) --check
	$(Q)$(PYTHON) tests/posix-matrix/test_porting_omissions.py

check-posix-matrix-strict-ignore:
	$(msg) CHECK POSIX drift has no ignore bypass
	$(Q)$(PYTHON) $(POSIX_MATRIX_SCRIPT) --check

check-posix-matrix-options:
	$(msg) CHECK POSIX advertised-option coverage
	$(Q)$(PYTHON) $(POSIX_MATRIX_SCRIPT) --check --strict-options

check-pse51:
	$(msg) CHECK PSE51 profile
	$(Q)$(PYTHON) scripts/check-pse51.py

check-opts-pse51: check-qemu-deps $(AUTOCONF_H) $(VER_FILE) startup initramfs
	$(msg) CHECK OPTS PSE51 subset
	$(Q)CC="$(CC)" LD="$(LD)" STRIP="$(STRIP)" \
	    CPPFLAGS="$(CPPFLAGS) -I$(TOPDIR)/userland/include -I$(TOPDIR)/include -I$(TOPDIR)/arch/$(ARCH)/include -I$(TOPDIR)/lib/libc -I$(TOPDIR)/lib/libc/internal -include $(TOPDIR)/lib/libc/rtmux-libc-local.h" \
	    CFLAGS="$(CFLAGS) -std=gnu17 -g" \
	    CRT0_O="$(OUT)/lib/libc/crt0.o" LIBC_SO="$(OUT)/lib/libc/libc.so" \
	    INITRAMFS_DIR="$(INITRAMFS_DIR)" INITRAMFS_CPIO="$(INITRAMFS_CPIO)" \
	    scripts/opts/run-pse51.sh

# Verify the checked-in profile-trim header still matches the profile registry.
check-posix-profile-header:
	$(msg) CHECK POSIX profile-trim header
	$(Q)$(PYTHON) scripts/gen-posix-profile-header.py --check

# Verify the configs/profile-*.config fragments still match the profile registry.
check-posix-profile-config:
	$(msg) CHECK POSIX profile-config fragments
	$(Q)$(PYTHON) scripts/check-profile-configs.py

# Preprocess <unistd.h> per profile and check the advertised option surface
# matches the registry -- the one gate that runs the real compiler, not the
# static model.  Skips gracefully when the cross compiler is absent.
check-posix-profile-surface:
	$(msg) CHECK POSIX advertised surface per profile
	$(Q)CC='$(CC)' \
	    PP_FLAGS='-nostdinc $(if $(GCC_INC),-isystem $(GCC_INC)) -Iinclude -Iinclude/arch -Iinternal/include -Ilib/libc -Ilib/libc/internal -Iuserland/include -Ilib/common' \
	    $(PYTHON) scripts/check-profile-surface.py

# Verify the recorded LKL libc floor still matches what the LKL servers pull.
# Standalone (not in `check`): it needs the LKL artifacts built, and skips
# gracefully otherwise.  Run it after touching the LKL servers.
check-lkl-floor:
	$(msg) CHECK LKL libc floor
	$(Q)$(PYTHON) scripts/lkl-floor.py --check

# Enforce the LKL-capable profile floor: the lkl profile covers every option the
# LKL servers pull, and CONFIG_LKL_SERVERS=y pairs only with the lkl/full
# profile.  Pure text (registry + active .config), so it runs in `check`.
check-lkl-profile:
	$(msg) CHECK LKL profile floor
	$(Q)$(PYTHON) scripts/check-lkl-profile.py

# Runtime leg of the LKL floor: build + boot the LKL servers under the lkl
# profile and run the LKL smoke modes.  Standalone (a full LKL build + boot;
# needs the LKL artifacts and mkfs.ext4).  Restores the full profile on exit.
# Override the mode list with ARGS, e.g. `make lkl-profile-smoke ARGS=lkl-fsd-tree`.
lkl-profile-smoke:
	$(msg) PROBE LKL smoke under the lkl profile
	$(Q)scripts/probe-lkl-profile.sh $(ARGS)

# Per-profile boot probe: build each smaller POSIX profile, boot it, and run the
# syscall_testing suite.  Standalone (not in `check`): each profile is a full
# -Bj rebuild plus a boot.  Always restores the full profile on exit.  Override
# the profile list with args, e.g. `make posix-profile-boot-probe ARGS=minimal`.
posix-profile-boot-probe:
	$(msg) PROBE "POSIX profiles (build + boot)"
	$(Q)scripts/probe-posix-profiles.sh $(ARGS)

check-syscall-abi:
	$(msg) CHECK syscall ABI shape
	$(Q)$(PYTHON) scripts/check-syscall-abi.py

check-libc-abi: libc
	$(msg) CHECK "libc.so export surface"
	$(Q)$(PYTHON) scripts/check-libc-abi.py --self-test
	$(Q)$(PYTHON) scripts/check-libc-abi.py --readelf "$(READELF)" --so "$(LIBC_SO)"

check-rt-budgets:
	$(msg) CHECK docs/rt-budgets.md
	$(Q)$(PYTHON) scripts/rt-budgets-check.py docs/rt-budgets.md
	$(Q)$(PYTHON) scripts/rt-budgets-check.py --expect-fail \
	    docs/rt-budgets-negative.md
	$(Q)$(PYTHON) scripts/rt-budgets-check.py --expect-fail \
	    docs/rt-budgets-claim-negative.md
	$(Q)$(PYTHON) scripts/rt-budgets-check.py --expect-fail --expect-error \
	    'only an `LKL network` class may cite PB-IOMMU-UNDESCRIBED' \
	    tests/rt-budgets/bad-iommu-nonnet.md
	$(Q)$(PYTHON) scripts/rt-budgets-check.py --expect-fail --expect-error \
	    'must cite exactly one of PB-IOMMU-ABSENT, PB-IOMMU-UNDESCRIBED' \
	    tests/rt-budgets/bad-net-no-iommu.md
	$(Q)$(PYTHON) scripts/rt-budgets-check.py --expect-fail --expect-error \
	    'must cite exactly one of PB-IOMMU-ABSENT, PB-IOMMU-UNDESCRIBED' \
	    tests/rt-budgets/bad-net-both-iommu.md
	$(Q)$(PYTHON) scripts/rt-budgets-check.py --expect-fail --expect-error \
	    'must cite exactly one of PB-IOMMU-ABSENT, PB-IOMMU-UNDESCRIBED' \
	    tests/rt-budgets/bad-net-riscv-only.md

check-irq-contract:
	$(msg) CHECK IRQ contract review coverage
	$(Q)scripts/check-irq-contract.sh
.PHONY: check-irq-contract

check-lock-rank:
	$(msg) CHECK static lock-rank acquisition order
	$(Q)$(PYTHON) scripts/check-lock-rank.py
.PHONY: check-lock-rank

# Preemption-checkpoint inventory: every long or unbounded loop in the
# scheduling-critical kernel subsystems must be classified in
# scripts/preempt-inventory.txt. Pure static text analysis, no build needed, so
# it joins the default make check next to check-lock-rank.
check-preempt-inventory:
	$(msg) CHECK preemption-checkpoint inventory
	$(Q)scripts/preempt-inventory.sh
	$(Q)$(PYTHON) tests/preempt-inventory/mutations.py
.PHONY: check-preempt-inventory

# Regenerate scripts/preempt-inventory.txt, preserving existing classifications
# and marking any newly appeared loop UNCLASSIFIED for review.
preempt-inventory-update:
	$(msg) UPDATE preemption-checkpoint inventory
	$(Q)scripts/preempt-inventory.sh update > scripts/preempt-inventory.txt.new
	$(Q)mv scripts/preempt-inventory.txt.new scripts/preempt-inventory.txt
.PHONY: preempt-inventory-update

# Spectre-V1 branch-inventory gate over the IPC capability-use window. Needs the
# built kernel objects, so it depends on startup and passes the toolchain
# objdump. Kept out of the default make check because that gate stays
# solver/tool-agnostic; run it after a build touching the cap/lookup fast path.
check-cap-v1: $(AUTOCONF_H) $(VER_FILE) startup
	$(msg) CHECK IPC cap-use window branch inventory, Spectre-V1 regression
	$(Q)OUT="$(OUT)" OBJDUMP="$(OBJDUMP)" $(PYTHON) scripts/check-cap-v1-branches.py
.PHONY: check-cap-v1

driver-gate:
	$(msg) CHECK driver import hygiene
	$(Q)scripts/check-driver-import-hygiene.sh

# ---------------------------------------------------------------------
# Style enforcement: the two-pass pipeline — commentflow first
# (banner shape, /* ... */ form, multi-line continuation), then
# clang-format (layout from .clang-format).  `make indent` rewrites in
# place; `make check-indent` reports drift and exits non-zero so CI can
# gate on it.  Both walk C/C++ sources under include/, kernel/, lib/,
# servers/, tools/, and userland/, skipping the cloned
# tools/kconfig/ checkout.
# ---------------------------------------------------------------------
INDENT_DIRS := include kernel lib servers tools userland
INDENT_FIND := find $(INDENT_DIRS) \
    \( -path tools/kconfig -o -path '*/build' \) -prune -o \
    -type f \( -name '*.c' -o -name '*.h' -o -name '*.cc' \
              -o -name '*.cpp' -o -name '*.cxx' \
              -o -name '*.hh' -o -name '*.hpp' -o -name '*.hxx' \) \
    -print

# commentflow (the comment-style pass) is not yet publicly released, so it is
# optional: environments without it still get the mandatory clang-format pass.
# clang-format is required.
HAVE_COMMENTFLOW := $(shell command -v commentflow >/dev/null 2>&1 && echo 1)

check-style-deps:
	@command -v clang-format >/dev/null 2>&1 || \
	    { echo "Error: clang-format not found in PATH."; \
	      echo "  Debian/Ubuntu: apt install clang-format"; \
	      echo "  macOS:         brew install clang-format"; exit 1; }
ifneq ($(HAVE_COMMENTFLOW),1)
	@echo "  Note: commentflow not found; skipping the comment-style pass."
	@echo "        Install from https://github.com/sysprog21/commentflow."
endif

indent: check-style-deps
ifeq ($(HAVE_COMMENTFLOW),1)
	$(msg) INDENT  commentflow pass 1/2
	$(Q)$(INDENT_FIND) | xargs -P 8 -n 50 commentflow
endif
	$(msg) INDENT  clang-format pass 1/2
	$(Q)$(INDENT_FIND) | xargs -P 8 -n 1 clang-format -i
ifeq ($(HAVE_COMMENTFLOW),1)
	$(msg) INDENT  commentflow pass 2/2
	$(Q)$(INDENT_FIND) | xargs -P 8 -n 50 commentflow
	$(msg) INDENT  clang-format pass 2/2
	$(Q)$(INDENT_FIND) | xargs -P 8 -n 1 clang-format -i
endif

check-indent: check-style-deps
	@echo "  CHECK    style compliance"
	$(Q)set -e; \
	    drift=$$(mktemp); trap 'rm -f $$drift' EXIT; \
	    if [ -n "$(HAVE_COMMENTFLOW)" ]; then \
	        $(INDENT_FIND) | xargs -P 8 -n 1 sh -c \
	            'commentflow --check "$$1" >/dev/null 2>&1 || \
	                 echo "$$1: commentflow drift"' _ >> $$drift; \
	    fi; \
	    $(INDENT_FIND) | xargs -P 8 -n 1 sh -c \
	        'clang-format --dry-run --Werror "$$1" >/dev/null 2>&1 || \
	             echo "$$1: clang-format drift"' _ >> $$drift; \
	    if [ -s $$drift ]; then \
	        echo "  FAIL    files out of style:"; \
	        sort -u $$drift | sed 's/^/    /' | head -50; \
	        n=$$(sort -u $$drift | wc -l | tr -d ' '); \
	        echo "  ($$n total — run 'make indent' to rewrite)"; \
	        exit 1; \
	    else \
	        echo "  PASS    all style checks clean"; \
	    fi
