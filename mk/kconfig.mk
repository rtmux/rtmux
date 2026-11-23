# mk/kconfig.mk — Kconfiglib integration (config, defconfig, oldconfig,
# savedefconfig, menuconfig, guiconfig) and C-header generation.
#
# Included only by the top-level Makefile; sub-Makefiles never need it.

ifndef _MK_KCONFIG_INCLUDED
_MK_KCONFIG_INCLUDED := 1

KCONFIG_DIR     := tools/kconfig
KCONFIG_URL     := https://github.com/sysprog21/Kconfiglib.git
KCONFIG_SCHEMA  := Kconfig
DEFCONFIG       ?= configs/defconfig
AUTOCONF_H      := internal/include/generated/autoconf.h

KCONFIG_ENV = KCONFIG_CONFIG=$(CURDIR)/.config \
              KCONFIG_AUTOHEADER=$(CURDIR)/$(AUTOCONF_H) \
              srctree=$(CURDIR)

.PHONY: config menuconfig guiconfig defconfig oldconfig savedefconfig

# Fetch sysprog21/Kconfiglib on first use. The existence guard lets
# `make -B` re-fire this recipe safely: a populated tools/kconfig/ from
# a prior run would otherwise cause `git clone` to fail.
$(KCONFIG_DIR)/menuconfig.py:
	$(Q)if [ -e "$@" ]; then exit 0; fi; \
	    $(if $(Q),,set -x;) \
	    printf '  %-8s %s\n' CLONE Kconfiglib; \
	    mkdir -p $(dir $(KCONFIG_DIR)); \
	    git clone --depth=1 $(KCONFIG_URL) $(KCONFIG_DIR)

config menuconfig: $(KCONFIG_DIR)/menuconfig.py
	$(KCONFIG_ENV) python3 $(KCONFIG_DIR)/menuconfig.py $(KCONFIG_SCHEMA)

guiconfig: $(KCONFIG_DIR)/menuconfig.py
	$(KCONFIG_ENV) python3 $(KCONFIG_DIR)/guiconfig.py $(KCONFIG_SCHEMA)

# Apply a checked-in defconfig as the new .config.
defconfig: $(KCONFIG_DIR)/menuconfig.py
	$(KCONFIG_ENV) python3 $(KCONFIG_DIR)/defconfig.py \
		--kconfig $(KCONFIG_SCHEMA) $(DEFCONFIG)
	$(Q)$(MAKE) --no-print-directory $(AUTOCONF_H)

# Update .config for new/changed Kconfig symbols (à la Linux).
oldconfig: $(KCONFIG_DIR)/menuconfig.py
	$(KCONFIG_ENV) python3 $(KCONFIG_DIR)/oldconfig.py \
		$(KCONFIG_SCHEMA)
	$(Q)$(MAKE) --no-print-directory $(AUTOCONF_H)

# Write a minimal defconfig back from the current .config — the inverse of
# `defconfig`, useful after tweaking with menuconfig.
savedefconfig: $(KCONFIG_DIR)/menuconfig.py
	$(KCONFIG_ENV) python3 $(KCONFIG_DIR)/savedefconfig.py \
		--kconfig $(KCONFIG_SCHEMA) --out $(DEFCONFIG)
	$(msg) SAVE $(DEFCONFIG)

# Every file the schema sources, so that a new symbol in a sub-Kconfig
# regenerates autoconf.h (genconfig.py fills in its default). Follow the
# `source` lines from the schema down, so a file at any depth is tracked.
kconfig_sources = $(foreach f,$(1),$(f) $(call kconfig_sources,$(wildcard \
    $(shell sed -n 's/^[[:space:]]*source[[:space:]]*"\([^"]*\)".*/\1/p' $(f)))))
KCONFIG_SOURCES := $(call kconfig_sources,$(KCONFIG_SCHEMA))

# Regenerate autoconf.h whenever .config or any Kconfig file changes. Beside
# it, record the .config words it was made from: a .config whose content differs
# from them gets autoconf.h regenerated even when it is older than autoconf.h
# (a copy restored with its time, a checkout), which the time rule misses.
# The mismatch makes the header out of date through FORCE (Makefile) rather
# than by deleting it while the makefiles are read, so make -n leaves it be.
AUTOCONF_SRC := $(AUTOCONF_H:.h=.config)
ifneq ($(strip $(file <$(AUTOCONF_SRC))),$(strip $(filter CONFIG_%,$(file <.config))))
$(AUTOCONF_H): FORCE
endif
$(AUTOCONF_H): .config $(KCONFIG_SOURCES) $(KCONFIG_DIR)/menuconfig.py
	$(Q)mkdir -p $(dir $@)
	$(msg) KCFG $@
	$(Q)$(KCONFIG_ENV) python3 $(KCONFIG_DIR)/genconfig.py \
		--header-path $@ $(KCONFIG_SCHEMA)
	$(Q)tr -s ' \t' '\n\n' <.config | grep '^CONFIG_' | tr '\n' ' ' >$(AUTOCONF_SRC)

endif # _MK_KCONFIG_INCLUDED
