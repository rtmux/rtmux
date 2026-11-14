# RTMux on Milk-V Duo 256M

The Milk-V Duo 256M carries a Sophgo SG2002: one T-Head C906 application core
at 1 GHz (RV64GCV), a second C906 running the vendor RTOS, and 256 MiB of DRAM.
It boots from an SD card through the vendor FSBL, OpenSBI and U-Boot. RTMux
runs on the application core and reaches `/sbin/init`; with no NVMe device
there is no `/usr`, so init ends in its rescue shell.

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
| `rtmux-kernel.bin`, `initramfs.cpio` | from the build directory; always copy them as a pair |
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

## Console and boot

UART0 is on pins 16 (TX, GP12) and 17 (RX, GP13), ground on pin 18, 115200 8N1.
Connect the adapter's TX only after the board is powered: driving the RX pin of
an unpowered board back-powers it and the console shows garbage.

On power-up U-Boot waits one second (press a key for the `soph#` prompt), loads
`uEnv.txt` and boots:

```
Boot command line: -Dsbi --limit-ram=160M
Board: Milk-V Duo256M
...
enable_mmu: Sv39 paging active
...
RT   bootimage: spawned /sbin/init (pid 2)
...
*** /usr unavailable — dropping to rescue shell ***
```

To change the command line, edit `rtmux_args` in `uEnv.txt`.

## Limitations

- `--limit-ram=160M` keeps RTMux below `0x8a000000`. The vendor DTB's
  `/reserved-memory`, which RTMux does not parse, holds a 75 MiB ION pool
  without a fixed address and the framebuffer at `0x8b13e000`; the RTOS
  core's image at `0x8fe00000` is already outside the `memory` node. The initramfs is loaded at `0x88000000` and must
  end below the limit, so it can be at most 32 MiB.
- No `/usr`: the board has no NVMe, so init falls back to the rescue shell.
- The rescue shell starts with `exec /bin/sh -i`, and exec does not start the
  new image yet, so the console stays silent after the rescue banner.
- The C906 memory-attribute path (MXSTATUS.MAEE=1) is only exercised on the
  board; QEMU reports MAEE=0.
