# SPDX-License-Identifier: Apache-2.0
# Standalone architecture bring-up, deliberately independent of the RISC-V
# .config, toolchain and include/arch symlink. Not the full RTMux kernel target.
.DEFAULT_GOAL := all
.DELETE_ON_ERROR:
TOP := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/..)
OUT ?= $(TOP)/build
PI3_OUT := $(abspath $(OUT))/arch/aarch64/raspi3-bringup
PI3_CLANG ?= clang
PI3_LD ?= ld.lld
PI3_OBJCOPY ?= llvm-objcopy
PYTHON ?= python3
QEMU_AARCH64 ?= qemu-system-aarch64
PLAT := $(TOP)/arch/aarch64/platform/raspi3
RESOURCE := $(shell $(PI3_CLANG) -print-resource-dir)
FLAGS := --target=aarch64-none-elf -mcpu=cortex-a53 -mgeneral-regs-only \
         -mno-outline-atomics -ffreestanding -fno-builtin -fno-pie \
         -fno-stack-protector -fno-strict-aliasing -nostdinc -g -O2 \
         -Wall -Wextra -Werror -MMD -MP -isystem $(RESOURCE)/include \
         -I$(PI3_OUT)/include -I$(TOP)/include -I$(TOP)/internal/include \
         -I$(TOP)/kernel/include -I$(TOP)/lib/common \
         -I$(TOP)/arch/aarch64/include -I$(PLAT)
ifneq ($(PI3_TEST_DISABLE_TIMER),)
FLAGS += -DPI3_TEST_DISABLE_TIMER=1
endif
SOURCES := arch/aarch64/startup/head.S arch/aarch64/startup/vectors.S \
           arch/aarch64/platform/raspi3/entry.c \
           arch/aarch64/platform/raspi3/uart.c \
           arch/aarch64/platform/raspi3/mmu.c \
           arch/aarch64/platform/raspi3/irq.c \
           arch/aarch64/platform/raspi3/exception.c \
           arch/aarch64/platform/raspi3/fixture.c arch/aarch64/cpu-elf.c \
           kernel/startup/lib/mem.c kernel/startup/lib/cpio.c \
           kernel/module/elf-validate.c
OBJECTS := $(addprefix $(PI3_OUT)/,$(addsuffix .o,$(SOURCES)))
ELF := $(PI3_OUT)/rtmux-pi3-bringup.elf
IMAGE := $(PI3_OUT)/kernel8.img
FIXTURE := $(PI3_OUT)/pi3-user.elf

.PHONY: all run check clean FORCE
all: $(IMAGE)

# Rebuild when flags/tools change, including the negative-test switch.
$(PI3_OUT)/flags: FORCE
	@mkdir -p $(@D)
	@printf '%s\n' '$(FLAGS)' '$(PI3_CLANG)' '$(PI3_LD)' '$(PI3_OBJCOPY)' > $@.tmp
	@cmp -s $@.tmp $@ && rm $@.tmp || mv $@.tmp $@

$(PI3_OUT)/include/arch:
	@mkdir -p $(@D)
	ln -s $(TOP)/arch/aarch64/include $@

$(PI3_OUT)/%.c.o: $(TOP)/%.c $(PI3_OUT)/flags | $(PI3_OUT)/include/arch
	@mkdir -p $(@D)
	$(PI3_CLANG) $(FLAGS) -std=gnu11 -c $< -o $@

$(PI3_OUT)/%.S.o: $(TOP)/%.S $(PI3_OUT)/flags | $(PI3_OUT)/include/arch
	@mkdir -p $(@D)
	$(PI3_CLANG) $(FLAGS) -D__ASSEMBLY__ -c $< -o $@

$(FIXTURE): $(PI3_OUT)/tests/arch/aarch64/pi3-user.S.o $(TOP)/tests/arch/aarch64/pi3-user.ld
	$(PI3_LD) -z max-page-size=4096 -T $(TOP)/tests/arch/aarch64/pi3-user.ld -o $@ $<

$(PI3_OUT)/fixture.cpio: $(FIXTURE) $(TOP)/scripts/mk-pi3-fixture.py
	$(PYTHON) $(TOP)/scripts/mk-pi3-fixture.py $< $@

$(PI3_OUT)/archive.S: $(PI3_OUT)/fixture.cpio
	@printf '%s\n' '.section .rodata.fixture, "a"' '.balign 8' \
	  '.global pi3_archive, pi3_archive_end' 'pi3_archive:' \
	  '.incbin "$(PI3_OUT)/fixture.cpio"' 'pi3_archive_end:' > $@

$(PI3_OUT)/archive.o: $(PI3_OUT)/archive.S $(PI3_OUT)/flags
	$(PI3_CLANG) $(FLAGS) -c $< -o $@

$(ELF): $(OBJECTS) $(PI3_OUT)/archive.o $(PLAT)/bringup.ld
	$(PI3_LD) -z max-page-size=4096 -T $(PLAT)/bringup.ld \
	  -Map=$(PI3_OUT)/rtmux-pi3-bringup.map -o $@ $(OBJECTS) $(PI3_OUT)/archive.o

$(IMAGE): $(ELF)
	$(PI3_OBJCOPY) -O binary $< $@

run: $(IMAGE)
	$(QEMU_AARCH64) -M raspi3b -accel tcg -display none -monitor none \
	  -serial stdio -no-reboot -kernel $(IMAGE)

check: $(IMAGE)
	$(PYTHON) $(TOP)/tests/arch/aarch64/test-runner.py
	$(PYTHON) $(TOP)/scripts/check-pi3-bringup.py --qemu $(QEMU_AARCH64) \
	  --image $(IMAGE) --log $(PI3_OUT)/qemu.log
	$(MAKE) -f $(TOP)/mk/pi3-bringup.mk OUT=$(abspath $(OUT))/pi3-negative \
	  PI3_TEST_DISABLE_TIMER=1 all
	$(PYTHON) $(TOP)/scripts/check-pi3-bringup.py --qemu $(QEMU_AARCH64) \
	  --image $(abspath $(OUT))/pi3-negative/arch/aarch64/raspi3-bringup/kernel8.img \
	  --log $(PI3_OUT)/qemu-no-timer.log --expect-failure timer-irq-timeout

clean:
	rm -rf $(PI3_OUT)

-include $(OBJECTS:.o=.d) $(PI3_OUT)/tests/arch/aarch64/pi3-user.S.d
