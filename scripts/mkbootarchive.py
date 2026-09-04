#!/usr/bin/env python3
"""mkbootarchive.py OUTPUT.tar NAME=PATH [NAME=PATH ...]

Write a reproducible ustar archive for the kernel's boot archive parser
(kernel/core/bootarchive.c). Entries are written in the order given, so
the caller controls module load order. Every field that could vary
between builds is fixed: mode 0644, uid/gid 0, mtime 0, no user or group
names, no prefix. Names are limited to 100 bytes, regular files only.
"""

import os
import sys

BLOCK = 512


def octal(value, width):
    return ("%0*o" % (width - 1, value)).encode("ascii") + b"\0"


def header(name, size):
    name_b = name.encode("ascii")
    if len(name_b) > 100:
        raise SystemExit(f"mkbootarchive: name too long (>100 bytes): {name}")
    if name.startswith("/") or ".." in name.split("/"):
        raise SystemExit(f"mkbootarchive: name rejected: {name}")
    h = bytearray(BLOCK)
    h[0:len(name_b)] = name_b
    h[100:108] = octal(0o644, 8)
    h[108:116] = octal(0, 8)
    h[116:124] = octal(0, 8)
    h[124:136] = octal(size, 12)
    h[136:148] = octal(0, 12)
    h[148:156] = b"        "
    h[156] = ord("0")
    h[257:263] = b"ustar\0"
    h[263:265] = b"00"
    chksum = sum(h)
    h[148:156] = ("%06o" % chksum).encode("ascii") + b"\0 "
    return bytes(h)


def main(argv):
    if len(argv) < 2:
        sys.stderr.write(__doc__)
        return 2
    out = argv[1]
    seen = set()
    chunks = []
    for spec in argv[2:]:
        if "=" not in spec:
            raise SystemExit(f"mkbootarchive: expected NAME=PATH, got {spec}")
        name, path = spec.split("=", 1)
        if name in seen:
            raise SystemExit(f"mkbootarchive: duplicate entry {name}")
        seen.add(name)
        with open(path, "rb") as f:
            data = f.read()
        chunks.append(header(name, len(data)))
        chunks.append(data)
        pad = (-len(data)) % BLOCK
        chunks.append(b"\0" * pad)
    chunks.append(b"\0" * (2 * BLOCK))
    blob = b"".join(chunks)
    tmp = out + ".tmp"
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with open(tmp, "wb") as f:
        f.write(blob)
    os.replace(tmp, out)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
