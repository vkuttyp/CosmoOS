#!/usr/bin/env python3
"""
Emit a compile_commands.json for editor tooling (clangd).

Reads lines of the form  <source-path-relative-to-root> TAB <cflags>
on stdin (produced by `make compile-commands`) and writes JSON on stdout
using the "arguments" array form, which sidesteps every shell-quoting
problem that the "command" string form has with -D"..." flags.
"""

import json
import shlex
import sys


def main():
    if len(sys.argv) != 3:
        print("usage: gen-compile-commands.py ROOT CC < entries", file=sys.stderr)
        return 2
    root, cc = sys.argv[1], sys.argv[2]

    entries = []
    for line in sys.stdin:
        line = line.rstrip("\n")
        if not line or "\t" not in line:
            continue
        src, flags = line.split("\t", 1)
        path = f"{root}/{src}"
        entries.append({
            "directory": root,
            "file": path,
            "arguments": [cc] + shlex.split(flags) + ["-c", path],
        })

    json.dump(entries, sys.stdout, indent=1)
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
