# Host Fuzzing The Parser Surface

RTMux fuzzes its trust-boundary parsers on the host, out of the target build, so
a crafted input that walks off a buffer is caught by AddressSanitizer or
UndefinedBehaviorSanitizer rather than reaching a real boot. The harnesses and
seed corpora live in tests/host/fuzz/; the build wiring is in tools/host/Makefile.

## Running

    make host-sanitize            # ASan + UBSan, replays every corpus, clang-gated
    make host-fuzz FUZZ_SECONDS=N # libFuzzer, N seconds per target

host-sanitize is the sanitizer gate (a standalone target, not pulled in by
make host-tests): it builds each target with
-fsanitize=address,undefined -fno-sanitize-recover=all, replays the seed corpus,
and then asserts a planted out-of-bounds access still aborts (fuzz-oob-selftest.c).
Both targets skip cleanly when clang is absent, so invoking them stays portable
under a plain cc.

libFuzzer needs a clang that ships the fuzzer runtime. make host-fuzz probes for
it and prints SKIP when it is missing, which is what happens on Apple clang, so
in practice host-fuzz runs on the reference host (its Ubuntu clang is known good) while ASan
and UBSan via host-sanitize work in both places.

## Targets

Each entry in the FUZZ_TARGETS list of tools/host/Makefile is one parser. The
loop compiles fuzz-NAME.c against the tuple's flags and source, so a harness file
name equals its target name.

- cpio: cpio_iterate, the initramfs newc archive walk.
- path: path_normalize, the roottask path canonicalizer.
- gpt: gpt_parse, the NVMe GPT partition table.
- xfer: xfer_memchk, the kernel IPC transfer bounds check.
- xferlen: xferlen, the kernel iov length summer.
- elf: initramfs_elf_find_rtmux, the ELF .rtmux identity-section lookup.
- romfs: the romfs on-disk decoder (romfs_init, namei, iread, readi).
- buildpath: build_lkl_path, the LKL filesystem path-traversal join.
- connectpath: rtmux_lkl_connect_path_ok, the connect-path NUL-termination check.
- fnmatch, strtox, getsubopt: the libc fnmatch, integer, and suboption parsers.

Several of these found and fixed real defects: a heap overflow in fnmatch, a
section-header offset overflow in the ELF validator, an unbounded directory-scan
denial of service in romfs, and a cascade of integer-overflow undefined behavior
in the printf core behind vfprintf.

## Build patterns

The parsers reach the host in one of four ways, chosen by how coupled the source
is to the target build:

1. Plain host libc parsers (fnmatch, strtox, getsubopt): the harness links the
   RTMux implementation directly, so the host libc copy is never pulled in and no
   include flags are needed.
2. A stub header layer (xfer, xferlen, connectpath): a small stub directory
   shadows one kernel or message header with the same struct fields on host
   integer types, so the real .c compiles without the RTMux platform typedef
   chain. xfer and xferlen share tests/host/fuzz/xfer-stubs (core/globals.h;
   xferlen also builds with -DNDEBUG to drop its debug-only address-space
   assertion), and connectpath has connectpath-stubs (sys/iomsg.h).
3. A pure RTMux header world (vfprintf): the three stdio sources build with
   -nostdinc plus the RTMux include dirs and the libc prelude, then link to the
   host-world harness by a plain vsnprintf prototype (no FILE crosses the
   boundary). The elf target uses a lighter variant, an elf-stubs sys/platform.h
   for host integer types plus a sys/elf.h shim that forces the RTMux header over
   the host one.
4. Extraction to a leaf (elf, buildpath, connectpath): a function buried in a
   paging-coupled loader or a heavy server main.c is split into its own
   host-buildable file (kernel/module/elf-validate.c, servers/lkl-fsd/pathbuild.c,
   servers/lkl/connectpath.c) so it builds host-side with a minimal dependency
   surface. romfs needed no extraction; its one I/O seam is stubbed inline in the
   harness.

## Adding a target

1. Write tests/host/fuzz/fuzz-NAME.c defining
   int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size). For the sanitize
   path it links tests/host/fuzz/standalone-main.c, which replays each argv file.
2. Add NAME_SRC and NAME_INC variables and a "NAME $(NAME_INC) $(NAME_SRC)" tuple
   to FUZZ_TARGETS in tools/host/Makefile. If the source needs a kernel or message
   header that pulls the platform chain, add a NAME-stubs directory or use the
   -nostdinc pattern above.
3. Seed tests/host/fuzz/corpus/NAME/ with valid inputs and any crash reproducers.
4. Prove the harness catches a fault: temporarily break the parser (drop a bound,
   narrow an accumulator) and confirm the sanitizer trips on a seed, then restore.
   A memory-safe validator can instead assert a security property in the harness,
   as buildpath does for path-traversal escapes.

Run make host-sanitize locally and make host-fuzz on the reference host before committing.
