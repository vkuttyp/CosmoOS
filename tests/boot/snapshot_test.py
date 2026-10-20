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

Stage `rollback`: snapshot the root as `good`, then break the system --
/bin removed, /etc/rc made to fail, a file rewritten -- and `cosmo-snap
rollback good`, which reboots. The machine comes back in the same QEMU:
the kernel says it rolled back, init's /etc/rc runs (so /bin is back),
init releases the hold, the file has its old contents, the tree before is
the safety snapshot and `fsctl check` is clean.

Stage `crash`: the three points the rollback's crash safety is written
for (CF19), each a killed machine and `fsctl check` after the next boot:
after the record's commit (`rollback --no-reboot`, then the kill); during
the next mount's switch (the installed disk booted with a kernel built
COSMOFS_STOP=1, which stops the switch after its blocks and before its
root; the kill; then the ordinary kernel again, which redoes it); and
after the hold is released (the kill once the boot completed).

Each boot is QEMU through scripts/qemu-run.sh, driven at the shell's
prompt as test-install drives it (tests/boot/install_test.py). Exit 0 PASS,
1 FAIL.
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

sys.path.insert(0, os.path.join(install_test.ROOT, "scripts"))
import mkmanifest  # noqa: E402
import modsign  # noqa: E402

ROLLED = r"cosmofs: \S+: rolled back: the live tree is snapshot '{}'"
RELEASED = r"^init: boot complete: released the rollback hold on /$"


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


def wait_after(b, start, pattern, what):
    """Wait until `pattern` appears in the log past offset `start`, then
    for the prompt after it, and take that prompt as consumed -- a reboot
    inside one QEMU prints a prompt of the old boot before it goes."""
    while time.monotonic() < b.deadline:
        text = b.text()
        m = re.search(pattern, text[start:], re.M)
        if m:
            at = start + m.end()
            if "\ncosmo$ " in text[at:]:
                with b.lock:
                    b.prompts_seen = b.buf.count(install_test.PROMPT)
                return text[start:]
        if b.proc.poll() is not None:
            raise Fail(f"{b.name}: QEMU exited waiting for {what}")
        time.sleep(0.1)
    raise Fail(f"{b.name}: no {what} before the deadline")


def boot_disk(name, args, inst):
    b = Boot(name, inst["target"], args.workdir, {"QEMU_DISKS": "boot"}, args.timeout)
    b.wait_prompt("the first prompt")
    return b


def stage_rollback(args, results, inst):
    # QEMU_REBOOT=1: the rollback's reboot restarts the machine in the same
    # QEMU rather than ending it (scripts/qemu-run.sh).
    b = Boot("rollback", inst["target"], args.workdir, {"QEMU_DISKS": "boot", "QEMU_REBOOT": "1"}, args.timeout)
    b.wait_prompt("the first prompt")
    try:
        rid = root_id(b)
        b.run("mkdir -p /rbdemo && echo good > /rbdemo/v.txt && sync")
        out = b.status("cosmo-snap create good", "good")
        need(out, r"^good=0$", "cosmo-snap create good")
        # Break the system: no /bin, an /etc/rc that fails, a file changed.
        # The shell already running keeps working; /sbin/cosmo-snap is static.
        # (sync first: it is in /bin. The rollback's own commit is durable.)
        b.run("echo broken > /rbdemo/v.txt && echo 'exit 7' > /etc/rc && sync && rm -rf /bin")
        out = b.run("if [ -e /bin/sh ]; then echo bin-present; else echo bin-gone; fi")
        need(out, r"^bin-gone$", "removing /bin")
        before = len(b.text())
        b.proc.stdin.write(b"cosmo-snap rollback good\n")
        b.proc.stdin.flush()
        # The same QEMU restarts the machine: wait for the next boot's rc.
        log = wait_after(b, before, r"^init: rc exited with status \d+$", "/etc/rc of the boot after the rollback")
        need(log, r"^cosmo-snap: rollback to good pending; the tree it replaces is kept as snapshot "
                  r"(pre-rollback-\d+)", "cosmo-snap rollback good")
        need(log, r"^init: restart requested$", "the reboot cosmo-snap asks init for")
        m = need(log, ROLLED.format("good") + r" \(generation \d+\); the tree before it is snapshot "
                      r"'(pre-rollback-\d+)'", "the mount's switch")
        safety = m.group(1)
        need(log, r"^init: rc exited with status 0$", "/etc/rc after the rollback")
        need(log, RELEASED, "the hold's release when the boot completed")
        out = b.run("cat /rbdemo/v.txt && ls /bin/sh && cat /.snapshots/" + safety + "/rbdemo/v.txt")
        need(out, r"^good$", "/rbdemo/v.txt after the rollback")
        need(out, r"^/bin/sh$", "/bin after the rollback")
        need(out, r"^broken$", "the safety snapshot's copy of /rbdemo/v.txt")
        check_clean(b, rid, "the rollback")
        out = b.status("cosmo-snap list", "list")
        sid, _, _, _, flags = list_entry(out, safety)
        if flags != "-":
            raise Fail(f"cosmo-snap list: {safety} has flags {flags!r} after the boot completed")
        out = b.status(f"cosmo-snap delete {safety} && cosmo-snap delete good", "del")
        need(out, r"^del=0$", "deleting both snapshots after the rollback")
        check_clean(b, rid, "deleting both snapshots")
        rc = b.finish("poweroff")
        if rc != EXIT_SUCCESS:
            raise Fail(f"rollback boot: QEMU exit code {rc}")
    except Exception:
        b.kill()
        raise
    results.append(f"rollback: PASS (/bin removed and /etc/rc broken; rolled back to good on the reboot, the "
                   f"tree before kept as {safety} and released when the boot completed; check clean)")


def swap_kernel(image, kernel, boot_tar, key):
    """Put another kernel.elf and boot.tar on the installed disk's ESP and
    sign the set again with the build's key (verified boot)."""
    mkmanifest.esp_put(image, "::/cosmo/kernel.elf", open(kernel, "rb").read())
    mkmanifest.esp_put(image, "::/cosmo/boot.tar", open(boot_tar, "rb").read())
    mkmanifest.resign_image(image, modsign.read_hex_key(key, "signing key"))


def stage_crash(args, results, inst):
    target = inst["target"]
    notes = []
    # 1. After the record's commit.
    b = boot_disk("crash-record", args, inst)
    try:
        b.run("echo r1 > /crash.txt && sync")
        need(b.status("cosmo-snap create c1", "c1"), r"^c1=0$", "cosmo-snap create c1")
        b.run("echo r1-later > /crash.txt && sync")
        out = b.status("cosmo-snap rollback --no-reboot c1", "rb1")
        need(out, r"^rb1=0$", "cosmo-snap rollback --no-reboot c1")
        out = b.status("cosmo-snap list", "l1")
        need(out, r"^cosmo-snap: /: format 12, a rollback is pending$", "the pending rollback in the listing")
    finally:
        b.kill()   # the machine stops: the record is committed, nothing else happens
    b = boot_disk("crash-record-after", args, inst)
    try:
        need(b.text(), ROLLED.format("c1"), "the switch after a stop just after the record's commit")
        need(b.run("cat /crash.txt"), r"^r1$", "/crash.txt after the switch")
        check_clean(b, root_id(b), "a stop after the record's commit")
        rc = b.finish("poweroff")
        if rc != EXIT_SUCCESS:
            raise Fail(f"crash-record-after boot: QEMU exit code {rc}")
    except Exception:
        b.kill()
        raise
    notes.append("after the record: switched at the next boot")

    # 2. During the switch: a kernel that stops it before its root.
    b = boot_disk("crash-switch-prepare", args, inst)
    try:
        b.run("echo s2 > /crash.txt && sync")
        need(b.status("cosmo-snap create c2", "c2"), r"^c2=0$", "cosmo-snap create c2")
        b.run("echo s2-later > /crash.txt && sync")
        need(b.status("cosmo-snap rollback --no-reboot c2", "rb2"), r"^rb2=0$", "cosmo-snap rollback c2")
        rc = b.finish("poweroff")
        if rc != EXIT_SUCCESS:
            raise Fail(f"crash-switch-prepare boot: QEMU exit code {rc}")
    except Exception:
        b.kill()
        raise
    saved_kernel = mkmanifest.esp_get(target, "::/cosmo/kernel.elf")
    saved_tar = mkmanifest.esp_get(target, "::/cosmo/boot.tar")
    swap_kernel(target, args.stop_kernel, args.stop_boot_tar, args.key)
    b = Boot("crash-switch", target, args.workdir, {"QEMU_DISKS": "boot"}, args.timeout)
    try:
        deadline = time.monotonic() + args.timeout
        while time.monotonic() < deadline:
            if re.search(r"cosmofs: test stop: the rollback switch's blocks are written and its root is not",
                         b.text()):
                break
            if b.proc.poll() is not None:
                raise Fail("crash-switch: QEMU exited before the switch stopped")
            time.sleep(0.1)
        else:
            raise Fail("crash-switch: the COSMOFS_STOP kernel never reached its stop")
    finally:
        b.kill()
    kfile = os.path.join(args.workdir, "kernel.elf.orig")
    tfile = os.path.join(args.workdir, "boot.tar.orig")
    open(kfile, "wb").write(saved_kernel)
    open(tfile, "wb").write(saved_tar)
    swap_kernel(target, kfile, tfile, args.key)
    b = boot_disk("crash-switch-after", args, inst)
    try:
        need(b.text(), ROLLED.format("c2"), "the switch redone after a stop inside it")
        need(b.run("cat /crash.txt"), r"^s2$", "/crash.txt after the redone switch")
        check_clean(b, root_id(b), "a stop inside the switch")
        # 3. After the hold's release: the machine stops with the boot
        # complete, and the next boot finds nothing to do.
        need(b.text(), RELEASED, "the hold's release")
        b.run("sync")
    finally:
        b.kill()
    notes.append("inside the switch: redone at the next boot")
    b = boot_disk("crash-clear-after", args, inst)
    try:
        if re.search(r"rolled back:", b.text()):
            raise Fail("crash-clear-after: a rollback was applied again after its hold was released")
        need(b.run("cat /crash.txt"), r"^s2$", "/crash.txt after a stop past the release")
        out = b.status("cosmo-snap list", "l3")
        need(out, r"^cosmo-snap: /: format 12$", "no pending or held rollback after the release")
        check_clean(b, root_id(b), "a stop after the release")
        rc = b.finish("poweroff")
        if rc != EXIT_SUCCESS:
            raise Fail(f"crash-clear-after boot: QEMU exit code {rc}")
    except Exception:
        b.kill()
        raise
    notes.append("after the release: nothing applied again")
    results.append("crash: PASS (" + "; ".join(notes) + "; fsctl check clean after each)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--image", required=True)
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--timeout", type=float, default=240.0, help="per boot")
    ap.add_argument("--key", required=True, help="the build's signing key (the crash stage signs its swapped kernel)")
    ap.add_argument("--stop-kernel", required=True, help="kernel.elf built COSMOFS_STOP=1")
    ap.add_argument("--stop-boot-tar", required=True, help="boot.tar of that build")
    ap.add_argument("--stages", default="identity,rollback,crash")
    args = ap.parse_args()
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        ap.error(f"--timeout must be a finite number of seconds above 0, not {args.timeout}")
    shutil.rmtree(args.workdir, ignore_errors=True)
    os.makedirs(args.workdir)
    results = []
    t0 = time.monotonic()
    try:
        inst = install_test.stage_install(args, results)
        stages = args.stages.split(",")
        if "identity" in stages:
            stage_identity(args, results, inst)
        if "rollback" in stages:
            stage_rollback(args, results, inst)
        if "crash" in stages:
            stage_crash(args, results, inst)
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
