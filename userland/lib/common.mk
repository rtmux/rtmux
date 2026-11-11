#
# Shared userland build settings.
#

USERLAND_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/..)
RTMUX_TOPDIR   := $(abspath $(USERLAND_DIR)/..)

INCDIR     = $(RTMUX_TOPDIR)/include
ARCHINCDIR = $(RTMUX_TOPDIR)/arch/$(ARCH)/include
LIBCDIR    = $(RTMUX_TOPDIR)/lib/libc
LIBC_INTERNALDIR = $(LIBCDIR)/internal
UL_INCDIR  = $(USERLAND_DIR)/include

CPPFLAGS += -I$(UL_INCDIR) -I$(INCDIR) -I$(ARCHINCDIR) -I$(LIBCDIR) \
            -I$(LIBC_INTERNALDIR) -include $(LIBCDIR)/rtmux-libc-local.h
CFLAGS   += -std=gnu17 -g
