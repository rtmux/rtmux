#
# Startup common (arch-independent) object list.
# Included by startup/Makefile (compilation) and arch/riscv/Makefile (linking).
#

STARTUP_COMMON_SRC = hardware-init common-options private ram addr-space hwi smp \
                     syspage-memory cpu-syspage-memory \
                     init-intrinfo init-raminfo

STARTUP_LIB_SRC = lib/mem lib/strtoul lib/getopt \
                  lib/typed-strings lib/util lib/cpio

STARTUP_ALL_SRC = $(STARTUP_COMMON_SRC) $(STARTUP_LIB_SRC)
