#!/usr/bin/env python3
"""check-docs.py [ROOT] -- every subsystem directory docs/README.md lists has
the five files of the per-subsystem convention (constitution section 64;
docs/README.md, "Per-subsystem convention"; roadmap M6).

The subsystem directories are the rows of docs/README.md's "Layout" table
whose first cell is a backticked directory (`kernel/memory/`), except the
record directories that hold reports rather than a subsystem (NOT_SUBSYSTEMS
below). Each must exist under docs/ and hold architecture.md, design.md,
api.md, invariants.md and testing.md, none of them empty. Exit 1 naming
every miss.
"""
import os
import re
import sys

FILES = ("architecture.md", "design.md", "api.md", "invariants.md", "testing.md")
# Rows of the Layout table that are records, not subsystems.
NOT_SUBSYSTEMS = {"history/", "testing/", "audit/"}


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
    docs = os.path.join(root, "docs")
    text = open(os.path.join(docs, "README.md"), encoding="utf-8").read()
    try:
        layout = text[text.index("## Layout"):text.index("## Per-subsystem convention")]
    except ValueError:
        print("check-docs: docs/README.md has no Layout table before the Per-subsystem convention")
        return 1
    dirs = [m.group(1) for m in re.finditer(r"^\| `([^`]+/)` \|", layout, re.M)]
    if not dirs:
        print("check-docs: docs/README.md's Layout table lists no directories")
        return 1
    bad = 0
    checked = 0
    for d in dirs:
        if d in NOT_SUBSYSTEMS:
            continue
        checked += 1
        path = os.path.join(docs, d)
        if not os.path.isdir(path):
            print(f"check-docs: docs/README.md lists {d}, but docs/{d} does not exist")
            bad = 1
            continue
        for f in FILES:
            p = os.path.join(path, f)
            if not os.path.isfile(p):
                print(f"check-docs: docs/{d}{f} is missing")
                bad = 1
            elif os.path.getsize(p) == 0:
                print(f"check-docs: docs/{d}{f} is empty")
                bad = 1
    print(f"check-docs: {checked} subsystem directories, five files each: {'NO' if bad else 'yes'}")
    return bad


if __name__ == "__main__":
    sys.exit(main())
