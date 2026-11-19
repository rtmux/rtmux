# RTMux on Milk-V Duo 256M

The Milk-V Duo 256M carries a Sophgo SG2002: one T-Head C906 application core
at 1 GHz (RV64GCV), a second C906 running the vendor RTOS, and 256 MiB of DRAM.
It boots from an SD card through the vendor FSBL, OpenSBI and U-Boot. RTMux
runs on the application core and boots to a login prompt, with `/usr` served
from memory instead of NVMe.

## Requirements

Besides the normal RTMux build requirements (`docs/building.md`):

- `dtc`, `clang` and `ld.lld` to build OpenSBI. Upstream OpenSBI needs a
  PIE-capable linker, which the bare-metal GNU toolchain does not provide.
- The `fip.bin` from the official image, for example
  `milkv-duo256m-musl-riscv64-sd_v2.0.1.img`. It sits in the first (FAT)
  partition, which starts at sector 1; with mtools:

  ```sh
  mcopy -i milkv-duo256m-musl-riscv64-sd_v2.0.1.img@@512 ::fip.bin vendor-fip.bin
  ```

  A card already written with the official image has the same file.
- A 3.3 V USB-UART adapter.

## Build and write the SD card

```sh
make defconfig && make
scripts/milkv-duo/mksd.sh --vendor-fip vendor-fip.bin /run/media/$USER/boot
```

The last argument is the mounted FAT partition of the card, or any directory
to copy from later. `mksd.sh` never touches block devices. It writes:

| File | Content |
|---|---|
| `fip.bin` | vendor FSBL, RTOS and U-Boot, with OpenSBI replaced and U-Boot's `sdboot` patched (below) |
| `rtmux-kernel.bin` | from the build directory |
| `initramfs.cpio` | the build's initramfs plus `usr.romfs` (below); always copy it together with the kernel |
| `uEnv.txt` | load addresses and the RTMux command line; kept if it already exists (`--force-uenv` replaces it) |

A leftover `boot.sd` (the vendor Linux image) is harmless; the patched U-Boot
no longer reads it. Intermediate files go to `build/milkv-duo/`.

## What `mksd.sh` changes in the firmware

- **OpenSBI 1.9 instead of the vendor 0.9.** RTMux prints through the SBI
  v2.0 debug console (DBCN, `-Dsbi`), which 0.9 lacks. The new OpenSBI embeds
  the DTB taken from the vendor one, with T-Head compatibles put first so that
  upstream drivers bind correctly: `sophgo,sg2002` on the root node,
  `thead,c900-clint` (no 64-bit MMIO) and `thead,c900-plic` (S-mode access
  delegation). The original strings stay after them for RTMux and U-Boot.
- **U-Boot reads `uEnv.txt`.** The vendor U-Boot is built without `saveenv`,
  so its environment cannot be changed persistently. `bootcmd` runs `sdboot`,
  which normally loads `boot.sd`; `mksd.sh` rewrites that default variable to

  ```
  fatload mmc 0 0x81000000 uEnv.txt && env import -t -r 0x81000000 ${filesize} && run uenvcmd
  ```

  so boot parameters live in a text file on the card.

`scripts/milkv-duo/fip.py parse <fip.bin>` checks every checksum in a fip.bin
and can extract its sections.

## `/usr` from memory

`/sbin/init` mounts `/usr` with `romfs "$mainfs" /usr`, where `mainfs=` comes
from the kernel command line. On QEMU it names an NVMe partition. The board has
no NVMe, so `mksd.sh` builds the same `/usr` tree with `scripts/mkusr.sh` into
a 1 MiB image, adds it to the initramfs as `/usr.romfs`, and `uEnv.txt` passes
`mainfs=/usr.romfs`; romfs reads the image like any other file.

## Console and boot

UART0 is on pins 16 (TX, GP12) and 17 (RX, GP13), ground on pin 18, 115200 8N1.
Connect the adapter's TX only after the board is powered: driving the RX pin of
an unpowered board back-powers it and the console shows garbage.

On power-up U-Boot waits one second (press a key for the `soph#` prompt), loads
`uEnv.txt` and boots:

```
Boot command line: -Dsbi --limit-ram=160M mainfs=/usr.romfs
Board: Milk-V Duo256M
...
enable_mmu: Sv39 paging active
...
RT   bootimage: spawned /sbin/init (pid 2)
...
romfs: mounted romfs at /usr (dev=/usr.romfs)
Sysinit: level1 running.
login:
```

Log in as `root` (password `RTMux`) or `rtmuxuser` (`rtmux`). To change the
command line, edit `rtmux_args` in `uEnv.txt`.

## Limitations

- `--limit-ram=160M` keeps RTMux below `0x8a000000`. The vendor DTB's
  `/reserved-memory`, which RTMux does not parse, holds a 75 MiB ION pool
  without a fixed address and the framebuffer at `0x8b13e000`; the RTOS
  core's image at `0x8fe00000` is already outside the `memory` node. The
  initramfs is loaded at `0x88000000` and must end below the limit, so it can
  be at most 32 MiB.
- `/usr` is a read-only copy in RAM; changing it means rerunning `mksd.sh`.
- Init still probes for PCI and NVMe and waits about 10 s for `/dev/pci`
  before mounting `/usr`.
- The C906 memory-attribute path (MXSTATUS.MAEE=1) is only exercised on the
  board; QEMU reports MAEE=0.
