# RTMux Syscall ABI

RTMux RV64 kcalls use `ecall` with the syscall number in `a7`, up to six
argument words in `a0-a5`, and the return value in `a0`.  The private
kernel/libc slot numbers live in `internal/include/sys/calls.h`; the kernel
dispatch table is emitted from `kernel/include/core/call-table.h`; libc stubs
live in `lib/libc/riscv/syscalls.c`.

The six argument words are represented on the kernel side by `KARGSLOT(...)`
fields in `kernel/include/core/args.h`.  Pointer-width fields are carried as one
RV64 word.  Wider payloads must cross as pointers to copy/iov objects, not extra
trap-register arguments.

`scripts/check-syscall-abi.py` is the pre-boot drift gate.  It checks enum/table
alignment, active-slot libc coverage, the `KARGSLOT` word budget, and the trap
handler's `a0-a5` restart snapshot.

## Trap-Boundary Arithmetic Edges

| Path | Edge | Guard | Failure |
| --- | --- | --- | --- |
| Private kcall ABI | More than six scalar/pointer argument words | `scripts/check-syscall-abi.py` counts `KARGSLOT` fields and the RV64 trap snapshot | Host check fails before boot |
| Private kcall ABI | Enum, dispatch table, and libc stub drift | `scripts/check-syscall-abi.py` compares `internal/include/sys/calls.h`, `kernel/include/core/call-table.h`, and `lib/libc/riscv/syscalls.c` | Host check fails before boot |
| Roottask executable loader | `e_phnum * sizeof(Elf64_Phdr)` wrap or oversized stack copy | `elf_range` phdr-size checks before `alloca()` / `proc_read()` | `ENOEXEC` / `ELIBBAD` |
| Roottask executable loader | `p_vaddr + p_memsz` wrap across reserved roottask window check | `elf_addr_range_overflows()` before `mmap()` | `ENOEXEC` |
| Roottask interpreter loader | `p_memsz + page_delta` or page round-up wrap | `elf_addr_range_overflows()` plus `elf_roundup_overflows()` before `mmap()` | `ELIBBAD` |
| Roottask interpreter loader | Relocated segment address wraps under ET_DYN bias | `elf_addr_reloc_overflows()` before page rounding | `ELIBBAD` |
| Kernel initramfs loader | Program-header table runs past module image | `elf_phdr_table_overflows()` before deriving `phdr` | Loader rejects module |
| Kernel initramfs loader | `PT_LOAD` file range `p_offset + p_filesz` wraps or exceeds CPIO member | `elf_file_range_overflows()` before pointer arithmetic / `memcpy()` | Loader rejects module |
| Kernel initramfs loader | `PT_LOAD` VA range, load bias, or page allocation size wraps | `elf_addr_range_overflows()` plus `elf_roundup_overflows()` before allocation / `cpu_map_pages()` | Loader rejects module |
| Memory manager mapping validation | User VA plus length wraps | `as_validate()` rejects `end < vaddr` before map isolation | `EFAULT` / `EINVAL` by operation |
