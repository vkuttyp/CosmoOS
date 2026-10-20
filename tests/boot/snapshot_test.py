#!/usr/bin/env python3
"""Roadmap 1.2 B1's acceptance test: snapshots and rollback from the shell
(docs/roadmap-1.2.md; docs/kernel-services/filesystem/cosmofs/testing.md,
"test-snapshot").

    snapshot_test.py --image cosmoos.img --workdir DIR

Stage `install`: test-install's, onto a blank 256 MiB disk.

Stage `identity`: boot the installed disk alone. Its root is format 11 (the
installer formats 11); `cosmo-snap create base` raises it to 12 and gives
the snapshot a uuid. The live tree then changes -- a file rewritten, one
removed, a directory added -- and `cosmo-snap diff base -` names exactly
those paths; the snapshot still reads the old contents; `fsctl check` of the
root is clean; `cosmo-snap list` shows base with a non-zero size; then
`delete` and `create` again, power off, boot again, and the second
snapshot's id is above the deleted one's (ids are never reused).

Each boot is QEMU through scripts/qemu-run.sh, driven at the shell's
prompt as test-install drives it (tests/boot/install_test.py). Exit 0 PASS,
1 FAIL.
"""

import argparse
import os
import re
import shutil
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import install_test  # noqa: E402
from install_test import Boot, Fail, need, EXIT_SUCCESS  # noqa: E402


def root_id(b):
    out = b.run("fsctl list")
    m = need(out, r"^\s*(\d+)\s+cosmofs\s+\S*n\S*\s+/$", "fsctl list (the root, with snapshots)")
    return m.group(1)


def check_clean(b, rid, what):
    out = b.status(f"fsctl check {rid}", "check")
    need(out, r"^check=0$", f"fsctl check after {what}")
    need(out, r"^clean$", f"fsctl check after {what}")
    return need(out, r"(\d+) snapshots \(", f"fsctl check after {what}").group(1)


def list_entry(out, name):
    """(id, refer KiB, unique KiB, uuid, flags) of one `cosmo-snap list` row."""
    m = need(out, rf"^{re.escape(name)}\s+(\d+)\s+\S+\s+(\d+)K\s+(\d+)K\s+([0-9a-f-]{{36}})\s+(.*)$",
             f"cosmo-snap list: {name}")
    return int(m.group(1)), int(m.group(2)), int(m.group(3)), m.group(4), m.group(5).strip()


def stage_identity(args, results, inst):
    b = Boot("identity", inst["target"], args.workdir, {"QEMU_DISKS": "boot"}, args.timeout)
    try:
        b.wait_prompt("the first prompt")
        need(b.text(), r"^init: switched to the disk root \S+ ", "the installed boot")
        rid = root_id(b)
        out = b.status("cosmo-snap list", "list0")
        need(out, r"^list0=0$", "cosmo-snap list on a fresh install")
        need(out, r"^cosmo-snap: /: format 11$", "a fresh install's format (11: nothing raised it)")
        # A tree to change.
        b.run("mkdir -p /snapdemo/keep && echo one > /snapdemo/edit.txt && echo gone > /snapdemo/drop.txt && "
              "echo kept > /snapdemo/keep/k.txt && sync")
        out = b.status("cosmo-snap create base", "create")
        need(out, r"^create=0$", "cosmo-snap create base")
        m = need(out, r"^cosmo-snap: created base \(id (\d+), uuid ([0-9a-f-]{36}), generation \d+\)$",
                 "cosmo-snap create base")
        base_id, base_uuid = int(m.group(1)), m.group(2)
        need(b.text(), r"cosmofs: format version 11 -> 12 \(snapshot identity", "the raise to format 12")
        out = b.status("cosmo-snap create base", "again")
        need(out, r"^again=1$", "a second snapshot named base")
        # The live tree moves on.
        b.run("echo two > /snapdemo/edit.txt && rm /snapdemo/drop.txt && mkdir /snapdemo/new && "
              "echo fresh > /snapdemo/new/n.txt && sync")
        out = b.status("cosmo-snap diff base -", "diff")
        need(out, r"^diff=0$", "cosmo-snap diff base -")
        got = sorted(re.findall(r"^([+\-M]) (/\S*)$", out, re.M))
        want = sorted([("M", "/snapdemo/edit.txt"), ("-", "/snapdemo/drop.txt"), ("+", "/snapdemo/new"),
                       ("+", "/snapdemo/new/n.txt")])
        # The live tree also differs wherever the system wrote since the
        # snapshot (a log, a lease); only /snapdemo is this test's to judge.
        mine = [g for g in got if g[1].startswith("/snapdemo")]
        if mine != want:
            raise Fail(f"cosmo-snap diff base -: /snapdemo changes {mine}, expected {want}")
        out = b.run("cat /.snapshots/base/snapdemo/edit.txt /.snapshots/base/snapdemo/drop.txt")
        need(out, r"^one$", "the snapshot's copy of edit.txt")
        need(out, r"^gone$", "the snapshot's copy of drop.txt")
        out = b.status("cosmo-snap list", "list1")
        need(out, r"^cosmo-snap: /: format 12$", "the format after the first snapshot")
        sid, refer, unique, uuid, flags = list_entry(out, "base")
        if sid != base_id or uuid != base_uuid or refer == 0 or unique == 0:
            raise Fail(f"cosmo-snap list: base is id {sid} uuid {uuid} refer {refer}K unique {unique}K")
        snaps = check_clean(b, rid, "the snapshot")
        if snaps != "1":
            raise Fail(f"fsctl check counted {snaps} snapshots, expected 1")
        out = b.status("cosmo-snap delete base", "delete")
        need(out, r"^delete=0$", "cosmo-snap delete base")
        out = b.status("cosmo-snap create second", "create2")
        need(out, r"^create2=0$", "cosmo-snap create second")
        rc = b.finish("poweroff")
        if rc != EXIT_SUCCESS:
            raise Fail(f"identity boot: QEMU exit code {rc}")
    except Exception:
        b.kill()
        raise
    b2 = Boot("identity-again", inst["target"], args.workdir, {"QEMU_DISKS": "boot"}, args.timeout)
    try:
        b2.wait_prompt("the first prompt")
        out = b2.status("cosmo-snap list", "list2")
        sid2, _, _, _, _ = list_entry(out, "second")
        if sid2 <= base_id:
            raise Fail(f"the snapshot taken after deleting base (id {base_id}) has id {sid2}: an id was reused")
        out = b2.status("cosmo-snap delete second && cosmo-snap create third && cosmo-snap list", "list3")
        sid3, _, _, _, _ = list_entry(out, "third")
        if sid3 <= sid2:
            raise Fail(f"after a remount and a delete, third has id {sid3}, second had {sid2}")
        check_clean(b2, root_id(b2), "the remount")
        rc = b2.finish("poweroff")
        if rc != EXIT_SUCCESS:
            raise Fail(f"identity-again boot: QEMU exit code {rc}")
    except Exception:
        b2.kill()
        raise
    results.append(f"identity: PASS (format 11 -> 12 at the first snapshot; base id {base_id} {base_uuid}; diff "
                   f"named the four changes; ids {base_id} < {sid2} < {sid3} across a delete and a remount)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--image", required=True)
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--timeout", type=float, default=240.0, help="per boot")
    args = ap.parse_args()
    shutil.rmtree(args.workdir, ignore_errors=True)
    os.makedirs(args.workdir)
    results = []
    t0 = time.monotonic()
    try:
        inst = install_test.stage_install(args, results)
        stage_identity(args, results, inst)
    except Fail as e:
        for r in results:
            print(f"test-snapshot: {r}")
        print(f"test-snapshot: FAIL: {e} (logs in {args.workdir})")
        return 1
    for r in results:
        print(f"test-snapshot: {r}")
    print(f"test-snapshot: PASS in {time.monotonic() - t0:.1f}s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
