# mk/recurse.mk — helpers for recursive module builds.
#
# Use real phony prerequisites for subdirectories instead of shell loops.
# That keeps GNU make's jobserver visible, so `make -j` can schedule
# independent module builds and cleans in parallel.

ifndef _MK_RECURSE_INCLUDED
_MK_RECURSE_INCLUDED := 1

define RTMUX_RECURSE_DIR_template
.PHONY: $(1) clean-$(1)
$(1):
	$$(Q)$$(MAKE) -C $(1)
clean-$(1):
	$$(Q)$$(MAKE) -C $(1) clean
endef

define RTMUX_RECURSE_ALIAS_template
.PHONY: $(1) clean-$(1)
$(1): $(3)
	$$(Q)$$(MAKE) -C $(2)
clean-$(1):
	$$(Q)$$(MAKE) -C $(2) clean
endef

RTMUX_RECURSE_DIRS = $(foreach d,$(1),$(eval $(call RTMUX_RECURSE_DIR_template,$(d))))
RTMUX_RECURSE_ALIAS = $(eval $(call RTMUX_RECURSE_ALIAS_template,$(1),$(2),$(3)))

endif # _MK_RECURSE_INCLUDED
