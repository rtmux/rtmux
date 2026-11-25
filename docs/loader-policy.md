# Loader Policy

The roottask loader admits only bounded ELF images. Every executable reaches
U-mode through `servers/roottask/sys/loader-elf.c`, so the policy lives there
instead of in a second dynamic-loading layer.

Current executable-image rules:

- ELF must be little-endian RISC-V `ET_EXEC` or `ET_DYN` with a non-empty,
  correctly-sized program-header table.
- Each `PT_LOAD` must have `p_filesz <= p_memsz`.
- No `PT_LOAD` may be both writable and executable.
- Main executable segments must not overlap the roottask reserved user mapping.
- Bootstrap privilege is never inferred from a name. It is granted only when the
  opened executable is on the read-only boot cpio, its resolved path is listed in
  `servers/roottask/sys/bootstrap-manifest.c`, and its image hash matches.

Unlisted valid images may still execute, but start without private capability
grants. Script fallback runs the interpreter as the executable image and receives
no grant for the script path.

Gate: `make check-loader-negative` boots QEMU and verifies malformed program
headers, oversized load data, writable-executable load segments, and `execvp()`
shell fallback fail before the child reaches U-mode.

## Process image replacement

The loader prepares the new image in a temporary process. Once preparation
succeeds, roottask quiesces the old threads, closes descriptors marked
close-on-exec and installs the new address space under the caller's PID.
The new program inherits the caller's signal mask and ignored dispositions,
and its pending signals: those queued for the old image and those its libc
held, with any that arrive before the new image registers, are delivered to
the new one. SIGKILL is the exception: one kept across the replacement ends
the process once the loader has started it, and is never delivered to the
new image. Caught dispositions return to their defaults. A failed image
lookup leaves the caller running and its process group eligible for a
parent's setpgid.
