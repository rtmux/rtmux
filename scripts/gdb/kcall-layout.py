# SPDX-License-Identifier: Apache-2.0
#
# gdb script for scripts/check-kcall-layouts.py: print, as one JSON line, the
# layout of each struct named in $KCALL_STRUCTS from the debug info of the
# loaded ELF: its size, its top-level field names, and every scalar it holds
# as (member path, bit offset, bit size). Union members are all listed, and
# anonymous members add nothing to the path, so a kernel KARGSLOT union's
# member reads as the field it slots. An array contributes its whole extent,
# its element size and its first element, so the element's layout is compared
# too. Field names list an anonymous member's own fields in its place.
import json
import os

import gdb


def size_of(t):
    try:
        return t.sizeof
    except gdb.error:  # a flexible array member
        return 0


def names(t):
    out = []
    for f in t.strip_typedefs().fields():
        if f.name:
            out.append(f.name)
        elif f.type.strip_typedefs().code in (gdb.TYPE_CODE_STRUCT,
                                              gdb.TYPE_CODE_UNION):
            out += names(f.type)
        # else an unnamed bit field (padding): no name, and no fields to list
    return out


def leaves(t, path, bit, acc):
    t = t.strip_typedefs()
    if t.code in (gdb.TYPE_CODE_STRUCT, gdb.TYPE_CODE_UNION):
        for f in t.fields():
            if f.bitpos is None:
                continue
            sub = path + "." + f.name if f.name else path
            if f.bitsize:
                acc.append((sub, bit + f.bitpos, f.bitsize))
            else:
                leaves(f.type, sub, bit + f.bitpos, acc)
    elif t.code == gdb.TYPE_CODE_ARRAY:
        acc.append((path + "[]", bit, size_of(t) * 8))
        acc.append((path + "[elem]", bit, size_of(t.target()) * 8))
        leaves(t.target(), path + "[0]", bit, acc)
    else:
        acc.append((path, bit, size_of(t) * 8))


out = {}
for name in os.environ["KCALL_STRUCTS"].split():
    try:
        t = gdb.lookup_type("struct " + name)
    except gdb.error:
        out[name] = None
        continue
    acc = []
    leaves(t, "", 0, acc)
    out[name] = {
        "size": t.sizeof,
        "fields": names(t),
        "leaves": sorted(set(acc)),
    }
print("KCALLJSON" + json.dumps(out))
