# mk/def.mk — shim included by every sub-Makefile. The actual build
# framework lives in mk/toolchain.mk (toolchain, arch dispatch, flags)
# and mk/kconfig.mk (config targets, only used by the top-level Makefile).
#
# Kept as the single entry point so sub-Makefiles only need one include
# line: `include ../mk/def.mk`, `include ../../mk/def.mk`, etc.

_MK_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
include $(_MK_DIR)toolchain.mk
include $(_MK_DIR)build.mk
