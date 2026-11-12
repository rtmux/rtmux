#!/usr/bin/env python3
#
# Check that the private kcall ABI stays aligned across the enum, kernel
# dispatch table, libc stubs, and RV64 argument-slot layout.
#
# SPDX-License-Identifier: Apache-2.0

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def fail(msg):
    print(f"syscall-abi: {msg}", file=sys.stderr)
    sys.exit(1)


def read(rel):
    return (ROOT / rel).read_text()


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//.*", "", text)


def parse_enum():
    text = strip_comments(read("internal/include/sys/calls.h"))
    m = re.search(r"enum\s*\{(?P<body>.*?)\};", text, re.S)
    if not m:
        fail("cannot find __KSYS enum")

    value = 0
    entries = []
    for raw in m.group("body").split(","):
        item = raw.strip()
        if not item:
            continue
        m_item = re.match(r"(?P<name>__KSYS_[A-Z0-9_]+)(?:\s*=\s*(?P<val>\d+))?$",
                          item)
        if not m_item:
            fail(f"cannot parse enum item {item!r}")
        if m_item.group("val") is not None:
            value = int(m_item.group("val"))
        entries.append((m_item.group("name"), value))
        value += 1
    return entries


def parse_call_table():
    text = read("kernel/include/core/call-table.h")
    body = []
    capture = False
    for line in text.splitlines():
        if line.startswith("#define MK_KSYSTABLE"):
            capture = True
        if capture:
            body.append(line)
            if line and not line.rstrip().endswith("\\"):
                break
    if not body:
        fail("cannot find MK_KSYSTABLE")
    return re.findall(r"_KT\(pref##_([a-z0-9_]+)\)", "\n".join(body))


def parse_libc_covers():
    return set(re.findall(r"__KSYS_[A-Z0-9_]+", read("lib/libc/riscv/syscalls.c")))


def parse_kcall_arg_slots():
    text = read("kernel/include/core/args.h")
    structs = re.findall(r"struct\s+(kcall_args_[a-z0-9_]+)\s*\{(.*?)\}\s*[a-z0-9_]+;",
                         text, re.S)
    if not structs:
        fail("cannot find kcall_args structs")

    counts = {}
    pointers = {}
    for name, body in structs:
        slots = re.findall(r"KARGSLOT\((.*?)\);", body, re.S)
        counts[name] = len(slots)
        pointers[name] = [
            " ".join(slot.split())
            for slot in slots
            if "*" in slot or re.search(r"\b(u?intptr_t|u?int64_t|paddr_t)\b", slot)
        ]
    return counts, pointers


def main():
    enum = parse_enum()
    values = {name: value for name, value in enum}
    bad = values.get("__KSYS_BAD")
    if bad is None:
        fail("__KSYS_BAD is missing")
    if bad != max(value for _, value in enum):
        fail("__KSYS_BAD must be the final enum value")

    table = parse_call_table()
    if len(table) != bad:
        fail(f"dispatch table has {len(table)} entries, expected {bad}")

    libc_covers = parse_libc_covers()
    value_to_name = {value: name for name, value in enum}
    missing = []
    for slot, handler in enumerate(table):
        name = value_to_name.get(slot)
        if name is None:
            if handler != "bad":
                fail(f"slot {slot} has handler {handler} but no enum name")
            continue
        if handler == "bad" or "SPARE" in name:
            continue
        if name not in libc_covers:
            missing.append(name)
    if missing:
        fail("libc stubs do not cover active slots: " + ", ".join(missing))

    arm_covers = set(re.findall(r"__KSYS_[A-Z0-9_]+", read("lib/libc/aarch64/syscalls.c")))
    if libc_covers != arm_covers:
        fail("AArch64/RISC-V libc syscall coverage differs: " +
             ", ".join(sorted(libc_covers ^ arm_covers)))
    arm = read("arch/aarch64/trap-handler.c")
    if not re.search(r"i < 6; i\+\+\).*?kap\[i\] = act->kcall_c_args\[i\] = r->gpr\[i\]", arm, re.S):
        fail("AArch64 must snapshot all six x0-x5 argument words")
    if "unsigned num = (unsigned) r->gpr[8]" not in arm:
        fail("AArch64 syscall number must be x8")

    counts, pointers = parse_kcall_arg_slots()
    oversized = [f"{name}={count}" for name, count in counts.items() if count > 6]
    if oversized:
        fail("kcall_args exceed RV64 a0-a5 budget: " + ", ".join(oversized))

    trap = read("arch/riscv/trap-handler.c")
    snapped = sorted({int(n) for n in re.findall(r"act->kcall_c_args\[(\d+)\]\s*=", trap)})
    if snapped != list(range(6)):
        fail(f"trap handler snapshots kcall_c_args {snapped}, expected 0..5")

    docs = read("docs/syscalls.md")
    if "six argument words" not in docs or "a0-a5" not in docs:
        fail("docs/syscalls.md must state the RV64 six-word/a0-a5 budget")

    pointer_total = sum(len(v) for v in pointers.values())
    print(f"syscall-abi: {bad} slots, {len(counts)} kcall_args structs, "
          f"{pointer_total} pointer-width fields checked")


if __name__ == "__main__":
    main()
