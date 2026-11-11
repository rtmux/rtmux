#
# Makefile for U-Boot boot script, and for deployment to RISC-V workstation
#

SCRIPT = boot.scr
REMOTE_HOST = freedom
REMOTE_DIR = /boot/rtmux
NVME_P8_IMG = $(OUT)/images/nvme-p8.dat

include ../mk/def.mk

all:
	mkimage -T script -A riscv -O linux -C none -n "Boot RTMux" -d rtmux.txt $(SCRIPT)

deploy: $(SCRIPT)
	ssh $(REMOTE_HOST) mkdir -p $(REMOTE_DIR)
	scp $(SCRIPT) $(REMOTE_HOST):$(REMOTE_DIR)/
	scp $(KERNEL_BIN).bin $(INITRAMFS_CPIO) $(REMOTE_HOST):$(REMOTE_DIR)/
	if test -f $(NVME_P8_IMG); then scp $(NVME_P8_IMG) $(REMOTE_HOST):$(REMOTE_DIR)/ ; fi

clean:
	rm -f $(SCRIPT)

.PHONY: all clean deploy
