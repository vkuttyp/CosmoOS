#!/usr/bin/env python3
"""Roadmap 1.2 B4's package transactions (PKG-3; docs/pkg/design.md,
"Transactions"; docs/pkg/testing.md, "test-pkgtx"). Part of test-upgrade.

    pkgtx_test.py --image cosmoos.img --workdir DIR

Stage `install`: test-install's, onto a blank 256 MiB disk.

Stage `tx-install`: `pkg install hello fortunes` stopped after the first
package (`--stop-after=1`, which waits once that package is on disk) and
the machine killed there. The next boot's init finds the transaction's
snapshot, rolls the root back to it and reboots once; the boot after that
has neither package, a clean `fsctl check`, no pkg-* snapshot left once
the boot completed, and a `pkg install` that works (the transaction's lock
went with it).

Stage `tx-upgrade`: hello 1.0 and demolib 2.5 installed, `pkg upgrade`
stopped after the first of its two packages and the machine killed: after
the recovery both are at their old versions; a full `pkg upgrade` then
takes both.

Stage `rollback`: `pkg rollback hello` returns hello 1.1 to 1.0, its files
verified; the kept versions are the two last installed. `pkg install
hello=1.0` over 1.1 says it is a downgrade.

Stage `space`: the root filled, `pkg install fortune` is refused before
anything changes, naming how much it needs; with the room back it installs.

Exit 0 PASS, 1 FAIL.
"""

import argparse
import math
import os
import re
import shutil
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import install_test  # noqa: E402
from install_test import Boot, Fail, need, EXIT_SUCCESS  # noqa: E402
from snapshot_test import check_clean, root_id  # noqa: E402

UNDOING = r"^init: pkg: undoing the interrupted package transaction (pkg-tx-\d+): the root rolls back to it, " \
          r"and the tree it replaces is kept as (pkg-undone-\d+); rebooting once$"
UNDONE = r"^init: pkg: the interrupted package transaction {} is undone: the root is as it was before it$"
STOPPED = r"^pkg: test stop after 1 package\(s\): waiting to be killed$"


def boot(name, args, inst):
    return Boot(name, inst["target"], args.workdir, {"QEMU_DISKS": "boot"}, args.timeout)


def wait_text(b, start, pattern, what):
    while time.monotonic() < b.deadline:
        m = re.search(pattern, b.text()[start:], re.M)
        if m:
            return m
        if b.proc.poll() is not None:
            raise Fail(f"{b.name}: QEMU exited waiting for {what}")
        time.sleep(0.1)
    raise Fail(f"{b.name}: no {what} before the deadline")


def interrupted(args, inst, name, cmd):
    """Run `cmd` (a pkg transaction with --stop-after=1) and kill the
    machine once it has stopped; then boot, let init undo it -- its reboot
    ends that QEMU (no QEMU_REBOOT: a fresh QEMU for the next boot has the
    firmware-stall retry an in-place reset lacks, docs/testing/flakes.md)
    -- and return the boot after at its prompt and the transaction's name."""
    b = boot(name, args, inst)
    try:
        b.wait_prompt("the first prompt")
        before = len(b.text())
        b.proc.stdin.write(cmd.encode() + b"\n")
        b.proc.stdin.flush()
        wait_text(b, before, STOPPED, f"the test stop in {cmd!r}")
    finally:
        b.kill()   # the power cut, between two packages
    r = boot(name + "-recover", args, inst)
    try:
        m = wait_text(r, 0, UNDOING, "init's recovery of the interrupted transaction")
        try:
            r.proc.wait(timeout=60)   # init's restart: QEMU ends at the reset
        except Exception:
            raise Fail(f"{r.name}: QEMU did not end at init's restart")
    finally:
        r.kill()
    tx, undone = m.group(1), m.group(2)
    a = boot(name + "-after", args, inst)
    try:
        a.wait_prompt("the boot after the recovery")
        log = a.text()
        need(log, UNDONE.format(re.escape(tx)), "the transaction undone at the next boot")
        # The boot completes, the hold goes, and the tree the transaction
        # left (pkg-undone-*) with it.
        need(log, r"^init: pkg: deleted " + re.escape(undone), "the interrupted tree's removal")
        return a, tx
    except Exception:
        a.kill()
        raise


def stage_tx_install(args, results, inst):
    b = boot("tx-prep", args, inst)
    try:
        b.wait_prompt("the first prompt")
        out = b.status("pkg update", "upd")
        need(out, r"^upd=0$", "pkg update")
        rc = b.finish("poweroff")
        if rc != EXIT_SUCCESS:
            raise Fail(f"tx-prep: QEMU exit code {rc}")
    except Exception:
        b.kill()
        raise
    r, tx = interrupted(args, inst, "tx-install", "pkg install --stop-after=1 hello fortunes")
    try:
        out = r.status("pkg list", "list")
        need(out, r"^list=0$", "pkg list after the recovery")
        for pkgname in ("hello", "fortunes"):
            if re.search(rf"^{pkgname}\s", out, re.M):
                raise Fail(f"tx-install: {pkgname} is installed after the interrupted transaction was undone")
        out = r.run("ls /usr/bin/hello; ls /usr/share/fortunes 2>&1 | head -1")
        if re.search(r"^/usr/bin/hello$", out, re.M):
            raise Fail("tx-install: /usr/bin/hello is on disk after the transaction was undone")
        check_clean(r, root_id(r), "the undone install")
        out = r.status("cosmo-snap list --all", "snaps")
        if re.search(r"^pkg-", out, re.M):
            raise Fail(f"tx-install: a pkg-* snapshot is left after the recovery: {out[-400:]!r}")
        out = r.status("pkg install hello fortunes && hello", "again")
        need(out, r"^again=0$", "pkg install after the recovery (the lock went with the transaction)")
        need(out, r"^hello, world \(hello 1\.1\)$", "hello after the install")
        out = r.status("cosmo-snap list --all", "snaps2")
        if re.search(r"^pkg-tx-", out, re.M):
            raise Fail("tx-install: a finished transaction left its snapshot")
        # The prefix is pkg's: an operator's snapshot cannot take it, so init
        # never mistakes one for a transaction.
        out = r.status("cosmo-snap create pkg-tx-mine", "mine")
        need(out, r"^mine=1$", "cosmo-snap create of a pkg- name")
        rc = r.finish("poweroff")
        if rc != EXIT_SUCCESS:
            raise Fail(f"tx-install: QEMU exit code {rc}")
    except Exception:
        r.kill()
        raise
    results.append(f"tx-install: PASS (killed after the first of two packages; {tx} rolled back at the next "
                   "boot, one reboot; neither installed, check clean, no pkg-* left; installed again after)")


def stage_tx_upgrade(args, results, inst):
    b = boot("tx-up-prep", args, inst)
    try:
        b.wait_prompt("the first prompt")
        out = b.status("pkg remove hello fortunes && pkg install hello=1.0 demolib=2.5 && hello", "prep")
        need(out, r"^prep=0$", "hello 1.0 and demolib 2.5")
        need(out, r"^hello, world \(hello 1\.0\)$", "hello 1.0")
        rc = b.finish("poweroff")
        if rc != EXIT_SUCCESS:
            raise Fail(f"tx-up-prep: QEMU exit code {rc}")
    except Exception:
        b.kill()
        raise
    r, tx = interrupted(args, inst, "tx-upgrade", "pkg upgrade --stop-after=1")
    try:
        out = r.status("pkg list && hello", "list")
        need(out, r"^list=0$", "pkg list after the recovery")
        need(out, r"^hello\s+1\.0\b", "hello still 1.0")
        need(out, r"^demolib\s+2\.5\b", "demolib still 2.5")
        need(out, r"^hello, world \(hello 1\.0\)$", "hello 1.0 runs")
        check_clean(r, root_id(r), "the undone upgrade")
        out = r.status("pkg upgrade && pkg list", "up")
        need(out, r"^up=0$", "pkg upgrade after the recovery")
        need(out, r"^pkg: 2 packages upgraded$", "both upgraded")
        need(out, r"^hello\s+1\.1\b", "hello 1.1")
        need(out, r"^demolib\s+3\.5\b", "demolib 3.5")
        rc = r.finish("poweroff")
        if rc != EXIT_SUCCESS:
            raise Fail(f"tx-upgrade: QEMU exit code {rc}")
    except Exception:
        r.kill()
        raise
    results.append(f"tx-upgrade: PASS (killed after the first of two upgrades; {tx} undone; both at their old "
                   "versions; a full upgrade then took both)")


def stage_rollback(args, results, inst):
    b = boot("pkg-rollback", args, inst)
    try:
        b.wait_prompt("the first prompt")
        out = b.status("pkg rollback hello && hello && pkg verify hello && pkg list", "rb")
        need(out, r"^rb=0$", "pkg rollback hello")
        need(out, r"^pkg: rolling hello back from 1\.1 to 1\.0$", "the rollback's report")
        need(out, r"^hello, world \(hello 1\.0\)$", "hello 1.0 after the rollback")
        need(out, r"^hello\s+1\.0\b", "pkg list: hello 1.0")
        out = b.run("ls -1 /var/cache/pkg/kept/hello")
        kept = sorted(re.findall(r"^(hello-[\d.]+\.cpk)$", out, re.M))
        if kept != ["hello-1.0.cpk", "hello-1.1.cpk"]:
            raise Fail(f"pkg-rollback: kept versions {kept}, expected hello-1.0.cpk and hello-1.1.cpk")
        out = b.status("pkg rollback demolib-nope", "none")
        need(out, r"^none=1$", "pkg rollback of a package not installed")
        # No copy kept of the version before (a package installed before pkg
        # kept them): the repository's, checked as an install's.
        out = b.status("pkg upgrade hello && rm /var/cache/pkg/kept/hello/hello-1.0.cpk && pkg rollback hello && hello",
                       "repo")
        need(out, r"^repo=0$", "pkg rollback from the repository")
        need(out, r"^pkg: rolling hello back from 1\.1 to 1\.0 \(from \S+: no copy was kept\)$",
             "pkg rollback from the repository")
        need(out, r"^hello, world \(hello 1\.0\)$", "hello 1.0 from the repository")
        # An install of an older version than the installed one is a
        # downgrade, and says so (backlog PKG-9).
        out = b.status("pkg upgrade hello && pkg install hello=1.0 && hello", "down")
        need(out, r"^down=0$", "pkg install hello=1.0 over 1.1")
        need(out, r"^pkg: hello will be downgraded from 1\.1 to 1\.0$", "the downgrade's report")
        need(out, r"^hello, world \(hello 1\.0\)$", "hello 1.0 after the downgrade")

        check_clean(b, root_id(b), "pkg rollback")
        rc = b.finish("poweroff")
        if rc != EXIT_SUCCESS:
            raise Fail(f"pkg-rollback: QEMU exit code {rc}")
    except Exception:
        b.kill()
        raise
    results.append("rollback: PASS (hello 1.1 back to 1.0, verified; the two last versions kept; an install of 1.0 "
                   "over 1.1 said downgraded)")


def stage_space(args, results, inst):
    b = boot("pkg-space", args, inst)
    try:
        b.wait_prompt("the first prompt")
        # Fill the root to under 256 KiB free, in synced steps of at most
        # 8 MiB measured against what the pool reports: a single dd to
        # ENOSPC outruns memory first (docs/backlog.md MEM-9). The bytes
        # must not compress (zeros shrink to nothing) and there is no
        # /dev/urandom: 256 KiB from awk's rand(), repeated -- a 32 KiB
        # compression record never sees the same bytes twice.
        b.run("sh -c 'awk \"BEGIN { srand(7); for (i = 0; i < 262144; i++) printf \\\"%c\\\", "
              "1 + int(rand() * 255) }\" > /tmp/rnd'; ls -l /tmp/rnd")
        for k in range(96):
            out = b.run("cosmo-snap list | grep 'KiB free'")
            free = int(need(out, r"^cosmo-snap: /: (\d+) KiB free of \d+ KiB$", "the root's free space").group(1))
            if free < 256:
                break
            n = max(1, min(32, (free - 128) // 256))
            b.run(f"sh -c 'i=0; while [ $i -lt {n} ]; do cat /tmp/rnd; i=$((i+1)); done > /fill{k}; sync'")
        else:
            raise Fail("pkg-space: the root did not fill")
        out = b.status("pkg install fortune", "full")
        need(out, r"^full=1$", "pkg install on a full root")
        m = need(out, r"^pkg: not enough space for this install: about (\d+) KiB needed .*, (\d+) KiB free; "
                      r"nothing changed$", "the refusal naming the room needed")
        out = b.status("pkg list; cosmo-snap list --all", "after")
        if re.search(r"^fortune\s", out, re.M) or re.search(r"^pkg-tx-", out, re.M):
            raise Fail("pkg-space: the refused install changed something")
        out = b.status("rm /fill* && sync && pkg install fortune", "room")
        need(out, r"^room=0$", "pkg install with the room back")
        check_clean(b, root_id(b), "the refusal and the install")
        rc = b.finish("poweroff")
        if rc != EXIT_SUCCESS:
            raise Fail(f"pkg-space: QEMU exit code {rc}")
    except Exception:
        b.kill()
        raise
    results.append(f"space: PASS (a full root: refused before any change, {m.group(1)} KiB needed, "
                   f"{m.group(2)} KiB free; installed once the room was back)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--image", required=True)
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--timeout", type=float, default=240.0, help="per boot")
    args = ap.parse_args()
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        ap.error(f"--timeout must be a finite number of seconds above 0, not {args.timeout}")
    shutil.rmtree(args.workdir, ignore_errors=True)
    os.makedirs(args.workdir)
    results = []
    t0 = time.monotonic()
    try:
        inst = install_test.stage_install(args, results)
        stage_tx_install(args, results, inst)
        stage_tx_upgrade(args, results, inst)
        stage_rollback(args, results, inst)
        stage_space(args, results, inst)
    except Fail as e:
        for r in results:
            print(f"test-pkgtx: {r}")
        print(f"test-pkgtx: FAIL: {e} (logs in {args.workdir})")
        return 1
    for r in results:
        print(f"test-pkgtx: {r}")
    print(f"test-pkgtx: PASS in {time.monotonic() - t0:.1f}s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
