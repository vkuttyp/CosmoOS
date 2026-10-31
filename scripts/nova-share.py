#!/usr/bin/env python3
"""nova-share.py -- how much of CosmoOS is Nova (docs/roadmap-2.0.md, SA1 and the Nova rule).

Counts the lines of every tracked file in ring 0 (kernel/, kernel-services/, drivers/, compat/, boot/) and in CosmoOS's
own userland (libc/, userland/, pkg/), per tree and per subsystem (a tree's next directory), in four classes:

  nova      a .nova source;
  floor     what stays assembly and C by the Nova rule's point 3: assembly, and the C files FLOOR names (Nova's
            runtime shim, and the wrappers of instructions Nova has no syntax for, as they are named);
  convert   C still to convert: every other .c and .h. A port's `*_floor.c` (the C under a Nova port that Nova cannot
            express yet) is C to convert too, and is also shown on its own (`port floors`);
  generated C made from a .nova source by `make nova` (its "GENERATED: do not edit" banner): not counted again.

Lines are physical lines, as `wc -l` counts them, from a git tree (HEAD or --ref), so the count is a function of the
commit alone.

    python3 scripts/nova-share.py                     # the table for HEAD
    python3 scripts/nova-share.py --ref origin/main   # for another commit
    python3 scripts/nova-share.py --json              # the same, as JSON
    python3 scripts/nova-share.py --gate origin/main  # exit 1 if HEAD has more C to convert than that commit
                                                      # in any tree (CI's nova-share job; the Nova rule's point 1)
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

RING0 = ("kernel", "kernel-services", "drivers", "compat", "boot")
USERLAND = ("libc", "userland", "pkg")

# The named C floor (the Nova rule's point 3). A file is added here in the PR that makes it floor, where its review
# can ask why it cannot be Nova.
FLOOR = (
    "kernel/nova/nova_panic.c",     # Nova's runtime shim; the rest of it (nova_rt_core.c, rt.h) is generated
)

GENERATED = re.compile(rb"GENERATED: do not edit")
C_FILE = re.compile(r"\.(c|h)$")
ASM_FILE = re.compile(r"\.(S|s)$")
PORT_FLOOR = re.compile(r"_floor\.c$")


def files(ref: str) -> dict[str, bytes]:
    """Every tracked file under the counted trees at `ref`, by path."""
    listing = subprocess.run(["git", "ls-tree", "-r", "-z", "--format=%(objectname) %(path)", ref, "--",
                              *RING0, *USERLAND], cwd=ROOT, check=True, capture_output=True).stdout
    entries = [e.split(" ", 1) for e in listing.decode().split("\0") if e]
    wanted = [(sha, path) for sha, path in entries if C_FILE.search(path) or ASM_FILE.search(path)
              or path.endswith(".nova")]
    batch = subprocess.run(["git", "cat-file", "--batch"], cwd=ROOT, check=True, capture_output=True,
                           input="".join(f"{sha}\n" for sha, _ in wanted).encode()).stdout
    out, at = {}, 0
    for _, path in wanted:
        header_end = batch.index(b"\n", at)
        size = int(batch[at:header_end].split()[2])
        out[path] = batch[header_end + 1:header_end + 1 + size]
        at = header_end + 1 + size + 1
    return out


def classify(path: str, text: bytes) -> str:
    if path.endswith(".nova"):
        return "nova"
    if path in FLOOR or ASM_FILE.search(path):
        return "floor"
    if GENERATED.search(text[:2048]):
        return "generated"
    return "convert"


def lines(text: bytes) -> int:
    return text.count(b"\n") + (1 if text and not text.endswith(b"\n") else 0)


def count(ref: str) -> dict:
    """{tree: {subsystem: {class: lines}}}, with the port floors as their own key."""
    table: dict = {}
    for path, text in files(ref).items():
        parts = path.split("/")
        tree = parts[0]
        subsystem = parts[1] if len(parts) > 2 else "."
        kind = classify(path, text)
        row = table.setdefault(tree, {}).setdefault(subsystem, {"nova": 0, "floor": 0, "convert": 0, "generated": 0,
                                                                 "port floors": 0})
        n = lines(text)
        row[kind] += n
        if kind == "convert" and PORT_FLOOR.search(path):
            row["port floors"] += n
    return table


def totals(rows) -> dict:
    out = {"nova": 0, "floor": 0, "convert": 0, "generated": 0, "port floors": 0}
    for row in rows:
        for k in out:
            out[k] += row[k]
    return out


def share(t: dict) -> str:
    whole = t["nova"] + t["floor"] + t["convert"]
    return f"{100 * t['nova'] / whole:5.1f}%" if whole else "    -"


def print_table(table: dict, ref: str):
    print(f"nova-share: {ref} ({subprocess.run(['git', 'rev-parse', '--short', ref], cwd=ROOT, capture_output=True, text=True).stdout.strip()})")
    print(f"{'':34} {'nova':>8} {'floor':>8} {'convert':>9} {'(port floors)':>14} {'nova %':>7}")
    for group, trees in (("ring 0", RING0), ("userland", USERLAND)):
        group_rows = []
        for tree in trees:
            subsystems = table.get(tree, {})
            for name in sorted(subsystems):
                row = subsystems[name]
                label = f"  {tree}/{name}/" if name != "." else f"  {tree}/ (top)"
                print(f"{label:34} {row['nova']:8} {row['floor']:8} {row['convert']:9} {row['port floors']:14} "
                      f"{share(row):>7}")
            t = totals(subsystems.values())
            group_rows.append(t)
            print(f"{tree + '/':34} {t['nova']:8} {t['floor']:8} {t['convert']:9} {t['port floors']:14} {share(t):>7}")
        g = totals(group_rows)
        print(f"{'= ' + group:34} {g['nova']:8} {g['floor']:8} {g['convert']:9} {g['port floors']:14} {share(g):>7}")
    gen = sum(r["generated"] for subs in table.values() for r in subs.values())
    print(f"(generated C, not counted again: {gen} lines)")


def main() -> int:
    parser = argparse.ArgumentParser(description="How much of CosmoOS is Nova (roadmap 2.0 SA1).")
    parser.add_argument("--ref", default="HEAD", help="the commit to count (default HEAD)")
    parser.add_argument("--json", action="store_true", help="print the counts as JSON")
    parser.add_argument("--gate", metavar="BASE", help="fail if --ref has more C to convert than BASE in any tree")
    args = parser.parse_args()
    table = count(args.ref)
    if args.json:
        print(json.dumps(table, indent=1, sort_keys=True))
    else:
        print_table(table, args.ref)
    if not args.gate:
        return 0
    base = count(args.gate)
    grown = []
    for tree in RING0 + USERLAND:
        now = totals(table.get(tree, {}).values())["convert"]
        was = totals(base.get(tree, {}).values())["convert"]
        if now > was:
            grown.append(f"{tree}/: {was} -> {now} lines of C to convert (+{now - was})")
    for line in grown:
        print(f"nova-share: FAIL: {line}")
    if grown:
        print("nova-share: new kernel and userland code is Nova, and no new C is written (docs/roadmap-2.0.md, "
              "\"The Nova rule\", point 1); a file that must stay C is named in FLOOR in scripts/nova-share.py, "
              "where its review can ask why")
        return 1
    print(f"nova-share: no tree has more C to convert than {args.gate}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
