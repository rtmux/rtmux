#!/usr/bin/env python3
"""Check that fixed bounds are documented in docs/assumptions.md."""

import argparse
import re
from pathlib import Path

import gate_cert

ROOT = Path(__file__).resolve().parents[1]
DOC = ROOT / "docs/assumptions.md"

SCAN_PATHS = (
    "kernel",
    "servers",
    "userland/dev-nvme",
    "userland/romfs",
    "userland/pipe",
    "lib/libc/rtmux/signal-state.c",
    "internal/include/sys/iomsg.h",
    "internal/include/sys/virtio-mmio-msg.h",
    "internal/include/sys/rtmux-core.h",
)

NAME_RE = re.compile(
    r"(MAX|QDEPTH|SLOTS?|RETRIES|TIMEOUT|BUFSZ|BUF_SIZE|QUEUE|QUEUES|DEPTH|LIMIT|COUNT|SIZE|PAGES|NUM|NENTRIES)"
)
DEFINE_RE = re.compile(r"^\s*#\s*define\s+([A-Za-z_][A-Za-z0-9_]*)\b")
ROW_RE = re.compile(r"^\|\s*`([A-Za-z_][A-Za-z0-9_]*)`\s*\|")

KINDS = {"architectural", "hardware-derived", "tuning", "temporary", "legacy"}

EXACT_SKIP = {
    "ADD_IN_NEW_QUEUE",
    "CPU_ELF_NUMBERS",
    "CPU_SYSTEM_HAVE_MULTIPLE_PAGESIZES",
    "DW_IATU_LIMIT_ADDR",
    "GET_QUEUE_IDX",
    "IATU_UR_LIMIT_ADDR",
    "KARGSLOT",
    "LBA_SIZE",
    "LIMITS_CHANNEL",
    "LIMITS_CONNECT",
    "LIMITS_INTERRUPT",
    "LIMITS_NOTIFICATION",
    "LIMITS_PROCESS",
    "LIMITS_SYNC",
    "LIMITS_SYNCEVENT",
    "LIMITS_THREAD",
    "LIMITS_TIMER",
    "NVME_CAP_MPSMAX",
    "NUM_ELTS",
    "PCI_BAD_REGISTER_NUMBER",
    "PCI_MSIX_TAB_SIZE",
    "QUEUE_MACRO_DEBUG_ASSERTIONS",
    "QUEUE_MACRO_DEBUG_TRASH",
    "QUEUE_TYPEOF",
    "RMSK_MAX",
    "ROOTTASK_L2_SLOT",
    "RTMUX_CAP_SLOT_MASK",
    "RTMUX_FLG_PROC_NO_LIMITS",
    "RTMUX_ITF_UNBLOCK_QUEUED",
    "RTMUX_PRIV_ENUM",
    "RTMUX_TCTL_PERFCOUNT",
    "SIGCODE_BDSLOT",
    "UNMAP_NORLIMIT",
    "VIRTIO_MMIO_GUEST_PAGE_SIZE",
    "VIRTIO_MMIO_QUEUE_ALIGN",
    "VIRTIO_MMIO_QUEUE_DESC_HIGH",
    "VIRTIO_MMIO_QUEUE_DESC_LOW",
    "VIRTIO_MMIO_QUEUE_DEVICE_HIGH",
    "VIRTIO_MMIO_QUEUE_DEVICE_LOW",
    "VIRTIO_MMIO_QUEUE_DRIVER_HIGH",
    "VIRTIO_MMIO_QUEUE_DRIVER_LOW",
    "VIRTIO_MMIO_QUEUE_NOTIFY",
    "VIRTIO_MMIO_QUEUE_NUM",
    "VIRTIO_MMIO_QUEUE_NUM_MAX",
    "VIRTIO_MMIO_QUEUE_PFN",
    "VIRTIO_MMIO_QUEUE_READY",
    "VIRTIO_MMIO_QUEUE_SEL",
    "_CONF_NUM_MAX",
    "_CONF_VALUE_MAX",
    "_MEMSIZE_T_",
    "_WCMAX",
}

PREFIX_SKIP = (
    "COF_",
    "FLAG_",
    "GETIOVLEN",
    "IOSERV_",
    "LEN_TO",
    "MMAPFD_",
    "NQUANTUM",
    "OBJECT_SIZE",
    "PATH_",
    "PROC_EVENT",
    "RTMUX_FLG_TIMEOUT",
    "RTMUX_TIMEOUT_",
    "_PATH",
    "_QUERY",
    "_SLOG_GETCOUNT",
)

SUBSTR_SKIP = (
    "_H_INCLUDED",
    "DT_RELACOUNT",
    "FNM_",
    "GCC_SIZE_T",
    "LOG2_SIZEOF",
    "MOUNT_",
    "RLIMIT_",
    "RLIM_",
    "SIZEOF_",
    "SYS_SIZETYPES",
)


def iter_files() -> list[Path]:
    files = []
    for item in SCAN_PATHS:
        path = ROOT / item
        if path.is_file():
            files.append(path)
            continue
        for child in path.rglob("*"):
            rel = child.relative_to(ROOT)
            if child.suffix not in {".c", ".h"}:
                continue
            if "tests" in rel.parts or "utils" in rel.parts:
                continue
            files.append(child)
    return sorted(files)


def skip_symbol(name: str) -> bool:
    return (
        name in EXACT_SKIP or
        any(name.startswith(prefix) for prefix in PREFIX_SKIP) or
        any(part in name for part in SUBSTR_SKIP)
    )


def scan_bounds() -> dict[str, str]:
    bounds = {}
    for path in iter_files():
        rel = path.relative_to(ROOT)
        for line_no, line in enumerate(path.read_text(errors="ignore").splitlines(), 1):
            match = DEFINE_RE.match(line)
            if not match:
                continue
            name = match.group(1)
            if NAME_RE.search(name) and not skip_symbol(name):
                bounds.setdefault(name, f"{rel}:{line_no}")
    return dict(sorted(bounds.items()))


def table_rows(path: Path) -> tuple[dict[str, tuple[int, list[str]]], list[str]]:
    rows = {}
    errors = []
    for line_no, line in enumerate(path.read_text().splitlines(), 1):
        if not line.startswith("|") or line.startswith("| ---"):
            continue
        cells = [cell.strip() for cell in line.strip("|").split("|")]
        if cells and cells[0] == "symbol":
            continue
        match = ROW_RE.match(line)
        if not match:
            continue
        symbol = match.group(1)
        rows[symbol] = (line_no, cells)
    return rows, errors


def formal_claim_rows() -> list[tuple[int, list[str]]]:
    path = ROOT / "docs/formal-claims.md"
    rows = []
    for line_no, line in enumerate(path.read_text().splitlines(), 1):
        if not line.startswith("|") or line.startswith("| ---"):
            continue
        cells = [cell.strip() for cell in line.strip("|").split("|")]
        if cells and cells[0] == "id":
            continue
        rows.append((line_no, cells))
    return rows


def check(path: Path) -> list[str]:
    bounds = scan_bounds()
    rows, errors = table_rows(path)

    for symbol, source in bounds.items():
        if symbol not in rows:
            errors.append(f"{path}: missing row for {symbol} ({source})")

    for symbol, (line_no, cells) in rows.items():
        if symbol not in bounds:
            errors.append(f"{path}: line {line_no}: stale row for {symbol}")
            continue
        if len(cells) != 5:
            errors.append(f"{path}: line {line_no}: expected 5 columns")
            continue
        _, kind, rationale, owner, trigger = cells
        if kind not in KINDS:
            errors.append(f"{path}: line {line_no}: bad kind for {symbol}: {kind}")
        for name, value in (
            ("rationale", rationale),
            ("owner", owner),
            ("revisit trigger", trigger),
        ):
            if not value or value.upper() in {"TODO", "TBD", "N/A"}:
                errors.append(f"{path}: line {line_no}: empty {name} for {symbol}")

    for line_no, cells in formal_claim_rows():
        if len(cells) < 7:
            continue
        bound = cells[4]
        for symbol in rows:
            citation = f"row `{symbol}` in `docs/assumptions.md`"
            if symbol in bound and citation not in bound:
                errors.append(
                    "docs/formal-claims.md: line "
                    f"{line_no}: {symbol} bound lacks matching assumption row citation"
                )
    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--list", action="store_true")
    parser.add_argument("path", nargs="?", default=str(DOC.relative_to(ROOT)))
    args = parser.parse_args()

    if args.list:
        for symbol, source in scan_bounds().items():
            print(f"{symbol} {source}")
        return 0

    errors = check(ROOT / args.path)
    if errors:
        for error in errors:
            print(error)
        return 1
    bounds = scan_bounds()
    gate_cert.emit(f"{len(bounds)} fixed bounds found under the scanned paths each have "
                   f"a {args.path} row")
    gate_cert.emit(f"{len(bounds)} ledger rows have a valid kind, rationale, owner and "
                   "revisit trigger")
    gate_cert.emit("ok", result=True)
    print("check-assumptions: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
