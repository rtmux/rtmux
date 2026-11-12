#!/usr/bin/env python3
"""Pack the fixed Pi3 architecture fixture as deterministic CPIO newc."""
import pathlib
import sys


def entry(name, data, inode):
    name = name.encode("ascii") + b"\0"
    fields = (inode, 0o100755, 0, 0, 1, 0, len(data), 0, 0, 0, 0, len(name), 0)
    result = b"070701" + b"".join(f"{n:08x}".encode() for n in fields) + name
    result += b"\0" * (-len(result) % 4)
    result += data
    return result + b"\0" * (-len(result) % 4)


if __name__ == "__main__":
    source, target = map(pathlib.Path, sys.argv[1:])
    target.write_bytes(entry("boot/pi3-user", source.read_bytes(), 1)
                       + entry("TRAILER!!!", b"", 2))
