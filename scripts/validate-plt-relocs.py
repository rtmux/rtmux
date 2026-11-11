#!/usr/bin/env python3
"""
Validate staged RTMux ELF files before they are packed into boot images.

The runtime linker expects RISC-V PLT relocations to be Elf64_Rela entries and
only handles JUMP_SLOT / IRELATIVE in the PLT pass. Catching malformed or stale
objects here prevents a bad initramfs.cpio / romfs image from failing late at boot
with "Unknown relocation type ... in PLT".

The same pass also checks that executables' dynamic undefined symbols are
provided by the staged DT_NEEDED shared objects. That catches stale image
composition such as a new toolbox importing unsetenv while the packaged libc.so
does not export it.

The closure pass only runs when the scan set contains shared libraries —
executable-only staging trees (e.g. $(FSIMG_ROOT)) get the PLT-only checks.
Closure resolution is one level deep against direct DT_NEEDED edges; RTMux's
userland currently links libc.so flat, so transitive scope is not modeled.
"""

import argparse
import os
import shlex
import subprocess
import sys


ELF_MAGIC = b"\x7fELF"
RELA_ENTRY_SIZE = 24
VALID_PLT_TYPES = {"R_RISCV_JUMP_SLOT", "R_RISCV_IRELATIVE"}

# Force C locale so readelf emits the English headings the parser keys on,
# and decode tolerantly in case object paths or comments aren't UTF-8.
_READELF_ENV = {**os.environ, "LC_ALL": "C", "LANG": "C"}


def is_elf(path):
    try:
        with open(path, "rb") as f:
            return f.read(4) == ELF_MAGIC
    except OSError:
        return False


def iter_files(paths):
    for path in paths:
        if os.path.isdir(path):
            for root, _, files in os.walk(path):
                for name in files:
                    full = os.path.join(root, name)
                    if os.path.isfile(full) and not os.path.islink(full):
                        yield full
        elif os.path.isfile(path) and not os.path.islink(path):
            yield path


def run_readelf(readelf, flags, path):
    # Allow $(READELF) to carry wrapper arguments (e.g. "ccache foo-readelf")
    # without breaking exec; fall back to a single-token argv when it names
    # an on-disk binary directly. `flags` may be a single flag or a list.
    flag_list = [flags] if isinstance(flags, str) else list(flags)
    prefix = [readelf] if os.path.isfile(readelf) else shlex.split(readelf)
    try:
        return subprocess.run(
            prefix + flag_list + [path],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            encoding="utf-8",
            errors="replace",
            env=_READELF_ENV,
        )
    except OSError as e:
        raise RuntimeError(f"cannot run {readelf}: {e}") from e


def readelf_lines(readelf, flags, path):
    p = run_readelf(readelf, flags, path)
    if p.returncode != 0:
        shown = flags if isinstance(flags, str) else " ".join(flags)
        raise RuntimeError(f"{path}: readelf {shown} failed: {p.stderr.strip()}")
    return p.stdout.splitlines()


def _bracketed(line):
    # readelf renders DT_NEEDED / DT_SONAME values as "Library: [name]".
    # Return None when the brackets aren't there so a malformed or localized
    # line skips the tag instead of crashing the validator.
    lb = line.find("[")
    rb = line.find("]", lb + 1) if lb != -1 else -1
    return line[lb + 1 : rb] if lb != -1 and rb != -1 else None


def _parse_pltrel(value):
    # The tag itself contains "REL", so naive substring matching on the whole
    # line would always classify as REL when the value isn't literally "RELA".
    if "RELA" in value:
        return "RELA"
    if "REL" in value:
        return "REL"
    tokens = value.split()
    return tokens[-1] if tokens else None


def dynamic_info(readelf, path):
    info = {
        "has_dynamic": False,
        "has_jmprel": False,
        "needed": [],
        "pltrel": None,
        "pltrelsz": 0,
        "soname": None,
    }
    for line in readelf_lines(readelf, "-d", path):
        if "Dynamic section" in line:
            info["has_dynamic"] = True
            continue
        if "(JMPREL)" in line:
            info["has_jmprel"] = True
        elif "(NEEDED)" in line:
            name = _bracketed(line)
            if name:
                info["needed"].append(name)
        elif "(SONAME)" in line:
            name = _bracketed(line)
            if name:
                info["soname"] = name
        elif "(PLTREL)" in line:
            info["pltrel"] = _parse_pltrel(line.split("(PLTREL)", 1)[1])
        elif "(PLTRELSZ)" in line:
            # The tag appears once per line; take the token immediately
            # after it as the value.
            tokens = line.split()
            try:
                idx = tokens.index("(PLTRELSZ)")
            except ValueError:
                continue
            if idx + 1 < len(tokens):
                raw = tokens[idx + 1]
                try:
                    info["pltrelsz"] = int(raw, 0)
                except ValueError as e:
                    raise RuntimeError(
                        f"{path}: cannot parse DT_PLTRELSZ value {raw!r}: {e}"
                    ) from e
    return info


def elf_kind(readelf, path):
    # PIE executables are ET_DYN with PT_INTERP; bare libraries are ET_DYN
    # without. Distinguishing the two keeps toolbox-style PIE in the
    # executable bucket so its undefined symbols still get closure-checked.
    et = None
    for line in readelf_lines(readelf, "-h", path):
        if line.strip().startswith("Type:"):
            if "EXEC" in line:
                return "exec"
            if "DYN" in line:
                et = "DYN"
            break
    if et != "DYN":
        return None
    # readelf prints "  INTERP         0x000... 0x... ..." for PT_INTERP;
    # match the column header form to avoid false positives on paths.
    for line in readelf_lines(readelf, "-l", path):
        if line.lstrip().startswith("INTERP"):
            return "exec"
    return "lib"


def dynamic_symbols(readelf, path):
    # --dyn-syms only: ldso resolves against .dynsym, not .symtab. A symbol
    # present in .symtab but excluded from .dynsym (hidden visibility,
    # version script) must not be treated as exported, or stale image
    # composition slips through the closure check.
    defined = set()
    undefined = set()
    for line in readelf_lines(readelf, ["--dyn-syms", "-W"], path):
        fields = line.split()
        if len(fields) < 8 or not fields[0].endswith(":"):
            continue
        bind, ndx, name = fields[4], fields[6], fields[7].split("@", 1)[0]
        if not name:
            continue
        if ndx == "UND":
            if bind != "WEAK":
                undefined.add(name)
        elif bind in {"GLOBAL", "WEAK"}:
            defined.add(name)
    return defined, sorted(undefined)


def plt_reloc_types(readelf, path):
    in_plt = False
    types = []
    for line in readelf_lines(readelf, "-r", path):
        stripped = line.strip()
        if stripped.startswith("Relocation section "):
            in_plt = "'.rela.plt'" in stripped or "'.rel.plt'" in stripped
            continue
        if not in_plt or not stripped:
            continue
        fields = stripped.split()
        if len(fields) < 3 or fields[2] == "Type":
            continue
        types.append(fields[2])
    return types


def validate_elf(readelf, path, info):
    errors = []
    if not info["has_dynamic"]:
        return errors
    if info["pltrelsz"] == 0:
        # No PLT relocations to validate, but if either related tag is
        # present alone the metadata is internally inconsistent.
        if info["has_jmprel"] or info["pltrel"] is not None:
            errors.append("DT_JMPREL/DT_PLTREL present but DT_PLTRELSZ is zero")
        return errors

    if not info["has_jmprel"]:
        errors.append("DT_PLTRELSZ is non-zero but DT_JMPREL is missing")
    if info["pltrel"] != "RELA":
        errors.append(f"DT_PLTREL is {info['pltrel'] or 'missing'}, expected RELA")
    if info["pltrelsz"] % RELA_ENTRY_SIZE != 0:
        errors.append(
            f"DT_PLTRELSZ={info['pltrelsz']} is not a multiple of {RELA_ENTRY_SIZE}"
        )

    types = plt_reloc_types(readelf, path)
    expected = info["pltrelsz"] // RELA_ENTRY_SIZE
    if len(types) != expected:
        errors.append(
            f".rela.plt has {len(types)} decoded entries, DT_PLTRELSZ implies "
            f"{expected}"
        )
    for idx, reloc_type in enumerate(types):
        if reloc_type not in VALID_PLT_TYPES:
            errors.append(f".rela.plt[{idx}] has unsupported {reloc_type}")
    return errors


def validate_symbol_closure(readelf, elf_paths, infos, kinds):
    errors_by_path = {path: [] for path in elf_paths}

    exports_by_lib = {}
    for path in elf_paths:
        info = infos[path]
        if not info["has_dynamic"] or kinds[path] != "lib":
            continue
        defined, _ = dynamic_symbols(readelf, path)
        names = {os.path.basename(path)}
        if info["soname"] is not None:
            names.add(info["soname"])
        for name in names:
            exports_by_lib.setdefault(name, set()).update(defined)

    # The closure pass is only meaningful when the scan set contains the
    # libraries that the executables resolve against. For executable-only
    # roots (e.g. $(FSIMG_ROOT), which only stages pipe and syscall_testing),
    # skip the check rather than report every DT_NEEDED as missing.
    if not exports_by_lib:
        return errors_by_path

    for path in elf_paths:
        info = infos[path]
        if not info["has_dynamic"] or kinds[path] != "exec":
            continue

        available = set()
        missing_libs = []
        for needed in info["needed"]:
            exports = exports_by_lib.get(needed)
            if exports is None:
                missing_libs.append(needed)
            else:
                available.update(exports)

        if missing_libs:
            errors_by_path[path].append(
                "DT_NEEDED library not staged: " + ", ".join(sorted(missing_libs))
            )

        _, undefined = dynamic_symbols(readelf, path)
        missing_symbols = [name for name in undefined if name not in available]
        if missing_symbols:
            errors_by_path[path].append(
                "unresolved dynamic symbol(s): " + ", ".join(missing_symbols)
            )

    return errors_by_path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--readelf", required=True)
    parser.add_argument("paths", nargs="+")
    args = parser.parse_args()

    checked = 0
    failed = 0
    try:
        files = [
            path for path in sorted(set(iter_files(args.paths))) if is_elf(path)
        ]
        infos = {path: dynamic_info(args.readelf, path) for path in files}
        kinds = {path: elf_kind(args.readelf, path) for path in files}
        symbol_errors = validate_symbol_closure(args.readelf, files, infos, kinds)
        for path in files:
            checked += 1
            errors = validate_elf(args.readelf, path, infos[path])
            errors.extend(symbol_errors.get(path, []))
            if errors:
                failed += 1
                for error in errors:
                    print(f"{path}: {error}", file=sys.stderr)
    except RuntimeError as e:
        print(f"validate-plt-relocs: {e}", file=sys.stderr)
        return 2

    if failed:
        print(
            f"validate-plt-relocs: {failed} of {checked} ELF file(s) failed",
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
