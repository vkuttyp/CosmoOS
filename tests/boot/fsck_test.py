#!/usr/bin/env python3
"""Roadmap 1.2 B2's acceptance test: fsck and scrub as commands that repair
(docs/roadmap-1.2.md; docs/kernel-services/filesystem/cosmofs/testing.md,
"test-fsck").

    fsck_test.py --image cosmoos.img --workdir DIR [--classes a,b,...] [--stages ...]

Stage `install`: test-install's, onto a blank 256 MiB disk.

Stage `prepare`: boot the installed disk once and give it what the damage
needs: a small tree under /fsck (files a to f, a directory with a file in
it, and /fsck/sparse, two runs of which the first is two blocks), a
snapshot taken after it (format 12), and a file removed after the
snapshot, so the snapshot has a deadlist. Power off: that disk is the base.

Stage `classes`: for each class tools/cosmofs-damage.py knows, a copy of
the base damaged in that one way, booted. A class a boot repairs (the
design's unattended and logged classes) is reported and repaired: the
boot's line "cosmofs: root checked, N findings, M repaired", the class's
`class` line with everything repaired, the disk root mounted, and `fsctl
check` of it clean. Any other refuses the mount with the class named: the
line "cosmofs: root refused: ...", the live shell reached with the pool
unmounted, then `fsck.cosmofs --repair` from that shell repairs what it can
(its exit status 1 when that is everything, 4 when findings are left) and
a second `fsck.cosmofs --check` reports exactly what an operator is left
with -- nothing, or the classes no repair has one right answer for.

Stage `readonly`: CF21 from the shell. The base disk is booted with a
blank disk beside it, which the shell formats and on which it writes and
removes a file, so the pool's root has a free record pending; on the next
boot `fsck.cosmofs --check` must report the replay done in memory and a
read-only mount reads the pool, and the host compares every block of that
disk before and after: the same.

Stage `mirror`: the base disk booted with two blank disks beside it,
which `mkfs.cosmofs --mirror` makes one pool; a file is written to it and
its checksum taken; power off. On the host one copy of 16 of the file's
blocks is flipped (cosmofs-damage.py --flip-mirror). Booted again: `fsctl
scrub` reports the bad copies and repairs nothing (exit 4), `fsctl scrub
--repair` repairs them (exit 1), a second scrub is clean (exit 0) and the
file reads back with its checksum. Then the scrub's bound: a writer timed
alone and while a scrub bounded at 512 blocks a second runs (both
reported), and the scrub's own time at least what its bound allows.

Each boot is QEMU through scripts/qemu-run.sh, driven at the shell's
prompt as test-install drives it. Exit 0 PASS, 1 FAIL.
"""

import argparse
import math
import os
import re
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import install_test  # noqa: E402
from install_test import Boot, Fail, need, EXIT_SUCCESS  # noqa: E402

DAMAGE = os.path.join(install_test.ROOT, "tools", "cosmofs-damage.py")

# What each damage leaves, by fsck.cosmofs's class names (fsreport.h):
#   boot:   the classes the boot's line reports (all repaired when the
#           boot repairs, else the ones the refusal names)
#   left:   what `fsck.cosmofs --check` still reports after the shell's
#           `fsck.cosmofs --repair` (refused classes only)
# A damage whose classes are all a boot's to repair has no `left`.
EXPECT = {
    "leaked": {"boot": ["leaked"]},
    "counter": {"boot": ["counter"]},
    "nlink": {"boot": ["nlink"]},
    "orphan": {"boot": ["orphan"]},
    "snap-identity": {"boot": ["snap-identity"]},
    "rollback-record": {"boot": ["rollback-record"]},
    "hold": {"boot": ["hold"]},
    "free-in-use": {"refused": ["free-in-use"], "left": []},
    "cross-linked": {"refused": ["cross-linked"], "left": ["cross-linked"]},
    "dangling": {"refused": ["dangling"], "left": ["dangling", "orphan"]},
    "bad-entry": {"refused": ["bad-entry"], "left": ["bad-entry", "orphan"]},
    "extent-order": {"refused": ["extent-order"], "left": ["extent-order"]},
    "extent-overlap": {"refused": ["extent-overlap"], "left": ["extent-overlap"]},
    "dup-name": {"refused": ["dup-name"], "left": ["dup-name"]},
    "unreadable": {"refused": ["unreadable"], "left": ["unreadable", "orphan"]},
    # A chain that names itself is claimed again at each revisit until the
    # walker's bound stops it, and a second live claim is a cross-link: both.
    "cycle-extent": {"refused": ["cycle"], "left": ["cross-linked", "cycle"]},
    "cycle-deadlist": {"refused": ["cycle"], "left": ["cycle"]},
    # The mount refuses a snapshot list that does not end while it loads the
    # snapshots: a device class, before the checker could run.
    "cycle-snaplist": {"refused": ["snapshots"], "left": ["snapshots"]},
    "cycle-directory": {"refused": ["bad-entry"], "left": ["bad-entry", "nlink"]},   # the parent gains a link
    "cycle-free-record": {"refused": ["free-record"], "left": ["free-record"]},
    "cycle-orphan-record": {"refused": ["orphan-record"], "left": ["orphan-record"]},
    "root-type": {"refused": ["root"], "left": ["root"]},
    "superblock": {"refused": ["superblock"], "left": ["superblock"]},
}

CLASS_LINE = re.compile(r"^class (\S+) count (\d+) repaired (\d+) (\S+)$", re.M)


def classes_in(text):
    """{class: (count, repaired)} of the `class` lines in `text`, the last
    report of each: a boot that repairs prints the read-only check's
    report and then the repair's."""
    out = {}
    for m in CLASS_LINE.finditer(text):
        out[m.group(1)] = (int(m.group(2)), int(m.group(3)))
    return out


def boot_disk(name, image, args, extra=""):
    env = {"QEMU_DISKS": "boot"}
    if extra:
        env["QEMU_EXTRA"] = (os.environ.get("QEMU_EXTRA", "") + " " + extra).strip()
    b = Boot(name, image, args.workdir, env, args.timeout)
    b.wait_prompt("the first prompt")
    return b


def stage_prepare(args, results, inst):
    b = boot_disk("prepare", inst["target"], args)
    try:
        need(b.text(), r"^init: switched to the disk root \S+ ", "the installed boot")
        need(b.text(), r"^cosmofs: root checked, 0 findings, 0 repaired$", "the installed boot's check")
        out = b.status("mkdir -p /fsck/dir && for f in a b c d e f old; do echo $f > /fsck/$f; done && "
                       "echo x > /fsck/dir/x && gzip -c /bin/busybox > /tmp/rnd && "
                       "dd if=/tmp/rnd of=/fsck/sparse bs=4096 count=8 2>/dev/null && "
                       "dd if=/tmp/rnd of=/fsck/sparse bs=4096 count=1 skip=8 seek=20 conv=notrunc 2>/dev/null && "
                       "sync", "files")
        need(out, r"^files=0$", "the /fsck tree")
        out = b.status("cosmo-snap create fsckbase", "snap")
        need(out, r"^snap=0$", "cosmo-snap create fsckbase")
        out = b.status("rm /fsck/old && sync", "rm")
        need(out, r"^rm=0$", "removing a file the snapshot keeps")
        rc = b.finish("poweroff")
        if rc != EXIT_SUCCESS:
            raise Fail(f"prepare boot: QEMU exit code {rc}")
    except Exception:
        b.kill()
        raise
    base = os.path.join(args.workdir, "base.img")
    shutil.copyfile(inst["target"], base)
    d = subprocess.run([sys.executable, DAMAGE, base, "--describe"], capture_output=True, text=True)
    if d.returncode != 0 or not re.search(r"^format 12,", d.stdout, re.M):
        raise Fail(f"the base disk: {d.stdout.strip()} {d.stderr.strip()}")
    results.append(f"prepare: PASS ({d.stdout.strip()})")
    return base


def one_class(args, base, cls):
    img = os.path.join(args.workdir, f"{cls}.img")
    d = subprocess.run([sys.executable, DAMAGE, base, cls, "--out", img], capture_output=True, text=True)
    if d.returncode != 0:
        raise Fail(f"{cls}: cosmofs-damage.py: {d.stdout.strip()} {d.stderr.strip()}")
    exp = EXPECT[cls]
    b = boot_disk(f"class-{cls}", img, args)
    try:
        log = b.text()
        if "boot" in exp:
            m = need(log, r"^cosmofs: root checked, (\d+) findings, (\d+) repaired$", f"{cls}: the boot's line")
            got = classes_in(log[:m.end()])
            for c in exp["boot"]:
                n, r = got.get(c, (0, 0))
                if n == 0 or r != n:
                    raise Fail(f"{cls}: the boot reported {got}, expected {c} found and repaired")
            if int(m.group(1)) == 0 or m.group(1) != m.group(2):
                raise Fail(f"{cls}: {m.group(0)}")
            need(log, r"^init: switched to the disk root \S+ ", f"{cls}: the repaired root mounted")
            if cls in ("orphan", "nlink"):
                need(log, rf"^cosmofs: root: {cls} inode \d+ repaired$", f"{cls}: the logged repair")
            rid = need(b.run("fsctl list"), r"^\s*(\d+)\s+cosmofs\s+\S+\s+/$", f"{cls}: fsctl list").group(1)
            out = b.status(f"fsctl check {rid}", "check")
            need(out, r"^check=0$", f"{cls}: fsctl check after the boot's repair")
            need(out, r"^clean$", f"{cls}: fsctl check after the boot's repair")
            note = f"{cls}: repaired at boot ({m.group(0)})"
        else:
            m = need(log, r"^cosmofs: root refused: (.*); (\S+) is not mounted\. From this shell: "
                          r"fsck\.cosmofs --repair (\S+)$", f"{cls}: the refusal")
            named = [x.strip() for x in m.group(1).split(",")]
            for c in exp["refused"]:
                if c not in named:
                    raise Fail(f"{cls}: the refusal named {named}, expected {c}")
            dev = m.group(2)
            if re.search(r"^init: switched to the disk root", log, re.M):
                raise Fail(f"{cls}: refused, and the disk root was mounted all the same")
            need(log, rf"^init: the root {re.escape(dev)} is not mounted; the live root stays$",
                 f"{cls}: the live root")
            out = b.status(f"fsck.cosmofs --repair {dev}", "repair")
            rs = int(need(out, r"^repair=(\d+)$", f"{cls}: fsck.cosmofs --repair").group(1))
            out2 = b.status(f"fsck.cosmofs --check {dev}", "again")
            cs = int(need(out2, r"^again=(\d+)$", f"{cls}: fsck.cosmofs --check").group(1))
            left = sorted(classes_in(out2))
            want = sorted(set(exp["left"]))
            if left != want:
                raise Fail(f"{cls}: after --repair the check reports {left}, expected {want}")
            if want:
                if rs != 4 or cs != 4:
                    raise Fail(f"{cls}: --repair exited {rs}, --check {cs}; expected 4 and 4 (findings left)")
            else:
                if rs != 1 or cs != 0:
                    raise Fail(f"{cls}: --repair exited {rs}, --check {cs}; expected 1 (all repaired) and 0")
                need(out2, rf"^fsck\.cosmofs: {re.escape(dev)}: 0 finding\(s\), 0 repaired, clean$",
                     f"{cls}: the second check")
            note = f"{cls}: refused ({', '.join(named)}); --repair {rs}, then --check {cs}" + \
                (f" left {', '.join(want)}" if want else " clean")
        rc = b.finish("poweroff")
        if rc != EXIT_SUCCESS:
            raise Fail(f"{cls}: QEMU exit code {rc}")
    except Exception:
        b.kill()
        raise
    os.remove(img)
    return note


def blank(path, mib):
    with open(path, "wb") as f:
        f.truncate(mib << 20)


def find_disks(b, mib, n):
    out = b.run("cosmo-install --list")
    devs = re.findall(rf"^(\S+)\s+-\s+{(mib << 20) // 512}\s+512\s", out, re.M)
    if len(devs) != n:
        raise Fail(f"cosmo-install --list shows {devs} of {mib} MiB, expected {n}")
    return devs


def stage_readonly(args, results, base):
    disk = os.path.join(args.workdir, "ro.img")
    blank(disk, 16)
    img = os.path.join(args.workdir, "ro-base.img")
    shutil.copyfile(base, img)
    extra = f"-drive if=none,id=ro,format=raw,file={disk} -device virtio-blk-pci,drive=ro"
    b = boot_disk("readonly", img, args, extra)
    try:
        (dev,) = find_disks(b, 16, 1)
        # A file written and removed: the commit that removed it leaves a
        # free record naming its blocks, which the next mount replays --
        # and which a clean unmount does not retire (design.md, "Format
        # version 9").
        out = b.status(f"mkfs.cosmofs {dev} && mkdir -p /mnt/r && mount -t cosmofs /dev/{dev} /mnt/r && "
                       "gzip -c /bin/busybox > /mnt/r/gone && sync && rm /mnt/r/gone && sync && umount /mnt/r", "mk")
        need(out, r"^mk=0$", "the read-only stage's pool, with a free record pending")
        rc = b.finish("poweroff")
        if rc != EXIT_SUCCESS:
            raise Fail(f"readonly boot 1: QEMU exit code {rc}")
    except Exception:
        b.kill()
        raise
    before = open(disk, "rb").read()
    b = boot_disk("readonly-ro", img, args, extra)
    try:
        (dev,) = find_disks(b, 16, 1)
        out = b.status(f"fsck.cosmofs --check {dev}", "ck")
        need(out, r"^ck=0$", "fsck.cosmofs --check of the pool with its records pending")
        need(out, r"replay released [1-9]\d* block\(s\) and \d+ orphan inode\(s\) in memory only",
             "the read-only check's replay, in memory")
        out = b.status(f"mkdir -p /mnt/r && mount -r -t cosmofs /dev/{dev} /mnt/r && ls /mnt/r && umount /mnt/r", "ro")
        need(out, r"^ro=0$", "a read-only mount of it")
        rc = b.finish("poweroff")
        if rc != EXIT_SUCCESS:
            raise Fail(f"readonly boot 2: QEMU exit code {rc}")
    except Exception:
        b.kill()
        raise
    after = open(disk, "rb").read()
    changed = sum(1 for i in range(0, len(before), 4096) if before[i:i + 4096] != after[i:i + 4096])
    if changed:
        raise Fail(f"readonly: a read-only check and a read-only mount changed {changed} block(s) of the disk")
    results.append("readonly: PASS (a pool with a free record pending: fsck.cosmofs --check reported its replay "
                   "in memory, a read-only mount read it, and every block of the disk was as it was)")
    os.remove(img)


def stage_mirror(args, results, base):
    d0, d1 = os.path.join(args.workdir, "m0.img"), os.path.join(args.workdir, "m1.img")
    blank(d0, 32)
    blank(d1, 32)
    img = os.path.join(args.workdir, "mirror-base.img")
    shutil.copyfile(base, img)
    extra = (f"-drive if=none,id=m0,format=raw,file={d0} -device virtio-blk-pci,drive=m0 "
             f"-drive if=none,id=m1,format=raw,file={d1} -device virtio-blk-pci,drive=m1")
    b = boot_disk("mirror", img, args, extra)
    try:
        a, c = find_disks(b, 32, 2)
        out = b.status(f"mkfs.cosmofs --mirror {a} {c}", "mk")
        need(out, r"^mk=0$", "mkfs.cosmofs --mirror")
        need(out, rf"^mkfs\.cosmofs: made cosmofs mirrored on {a} and {c}$", "mkfs.cosmofs --mirror")
        out = b.status(f"mkdir -p /mnt/m && mount -t cosmofs /dev/{a} /mnt/m && "
                       "gzip -c /bin/busybox | head -c 262144 > /mnt/m/data && sync && "
                       "sha256sum /mnt/m/data && umount /mnt/m", "fill")
        need(out, r"^fill=0$", "the mirrored pool's file")
        digest = need(out, r"^([0-9a-f]{64})\s+/mnt/m/data$", "the file's checksum").group(1)
        rc = b.finish("poweroff")
        if rc != EXIT_SUCCESS:
            raise Fail(f"mirror boot 1: QEMU exit code {rc}")
    except Exception:
        b.kill()
        raise
    f = subprocess.run([sys.executable, DAMAGE, "--flip-mirror", d0, d1, "--path", "/data", "--count", "16"],
                       capture_output=True, text=True)
    if f.returncode != 0:
        raise Fail(f"cosmofs-damage.py --flip-mirror: {f.stdout} {f.stderr}")
    b = boot_disk("mirror-scrub", img, args, extra)
    notes = []
    try:
        a, c = find_disks(b, 32, 2)
        b.run(f"mkdir -p /mnt/m && mount -t cosmofs /dev/{a} /mnt/m")
        mid = need(b.run("fsctl list"), r"^\s*(\d+)\s+cosmofs\s+\S+\s+/mnt/m$", "fsctl list: the mirror").group(1)
        scrub = r"^mount \d+: (\d+) blocks read, \d+ inodes, (\d+) bad copies, (\d+) repaired, (\d+) unrecoverable, " \
                r"\d+ skipped \((\d+) ms, bound (\d+) blocks/s\)$"
        out = b.status(f"fsctl scrub {mid}", "s1")
        need(out, r"^s1=4$", "a scrub without --repair (findings left)")
        m1 = need(out, scrub, "the first scrub")
        if int(m1.group(2)) != 16 or int(m1.group(3)) != 0 or int(m1.group(4)) != 0:
            raise Fail(f"the first scrub: {m1.group(0)}; expected 16 bad copies, none repaired or unrecoverable")
        out = b.status(f"fsctl scrub {mid} --repair", "s2")
        need(out, r"^s2=1$", "fsctl scrub --repair (all repaired)")
        m2 = need(out, scrub, "the repairing scrub")
        if int(m2.group(2)) != 16 or int(m2.group(3)) != 16:
            raise Fail(f"the repairing scrub: {m2.group(0)}; expected 16 bad copies, 16 repaired")
        out = b.status(f"fsctl scrub {mid}", "s3")
        need(out, r"^s3=0$", "the second scrub (clean)")
        need(out, r"^clean$", "the second scrub")
        out = b.run("sha256sum /mnt/m/data")
        need(out, rf"^{digest}\s+/mnt/m/data$", "the file read back after the repair")
        notes.append(f"scrub: 16 bad copies reported (exit 4), repaired (exit 1), then clean (exit 0); "
                     f"the file's sha256 unchanged")

        # The bound, and what it leaves a writer. A pool large enough that
        # a scrub at 512 blocks a second takes a few seconds.
        out = b.status("for i in 1 2 3 4 5 6 7 8 9 10 11 12; do cat /bin/busybox; done > /mnt/m/bulk && sync", "bulk")
        need(out, r"^bulk=0$", "filling the mirror")
        t0 = time.monotonic()
        out = b.status("dd if=/dev/zero of=/mnt/m/w1 bs=4096 count=1024 2>/dev/null && sync", "w1")
        alone = time.monotonic() - t0
        need(out, r"^w1=0$", "the writer alone")
        b.run("sysctl fs.scrub_rate=512")
        b.run(f"fsctl scrub {mid} > /tmp/scrub.out 2>&1 &")
        t0 = time.monotonic()
        out = b.status("dd if=/dev/zero of=/mnt/m/w2 bs=4096 count=1024 2>/dev/null && sync", "w2")
        during = time.monotonic() - t0
        need(out, r"^w2=0$", "the writer during the scrub")
        deadline = time.monotonic() + args.timeout / 2
        so = ""
        while time.monotonic() < deadline:
            so = b.run("cat /tmp/scrub.out")
            if re.search(scrub, so, re.M):
                break
            time.sleep(1)
        ms = need(so, scrub, "the bounded scrub")
        blocks, ms_, rate = int(ms.group(1)), int(ms.group(5)), int(ms.group(6))
        if rate != 512:
            raise Fail(f"the bounded scrub ran at {rate}, sysctl fs.scrub_rate said 512")
        floor = (blocks // 51 - 1) * 100
        if ms_ < floor:
            raise Fail(f"the bounded scrub read {blocks} blocks in {ms_} ms, under its bound's {floor} ms")
        b.run("sysctl fs.scrub_rate=4096")
        notes.append(f"bound: {blocks} blocks at 512/s took {ms_} ms (at least {floor}); a 4 MiB writer took "
                     f"{alone:.2f}s alone and {during:.2f}s during the scrub")
        rc = b.finish("poweroff")
        if rc != EXIT_SUCCESS:
            raise Fail(f"mirror boot 2: QEMU exit code {rc}")
    except Exception:
        b.kill()
        raise
    results.append("mirror: PASS (" + "; ".join(notes) + ")")
    for p in (img, d0, d1):
        os.remove(p)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--image", required=True)
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--timeout", type=float, default=240.0, help="per boot")
    ap.add_argument("--key", help="unused; accepted as test-install's")
    ap.add_argument("--classes", default=",".join(EXPECT))
    ap.add_argument("--stages", default="classes,readonly,mirror")
    args = ap.parse_args()
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        ap.error(f"--timeout must be a finite number of seconds above 0, not {args.timeout}")
    shutil.rmtree(args.workdir, ignore_errors=True)
    os.makedirs(args.workdir)
    results = []
    t0 = time.monotonic()
    try:
        inst = install_test.stage_install(args, results)
        base = stage_prepare(args, results, inst)
        stages = args.stages.split(",")
        if "classes" in stages:
            for cls in args.classes.split(","):
                if cls not in EXPECT:
                    raise Fail(f"no class {cls}")
                t1 = time.monotonic()
                note = one_class(args, base, cls)
                results.append(f"class {note} ({time.monotonic() - t1:.0f}s)")
        if "readonly" in stages:
            stage_readonly(args, results, base)
        if "mirror" in stages:
            stage_mirror(args, results, base)
    except Fail as e:
        for r in results:
            print(f"test-fsck: {r}")
        print(f"test-fsck: FAIL: {e} (logs in {args.workdir})")
        return 1
    for r in results:
        print(f"test-fsck: {r}")
    print(f"test-fsck: PASS in {time.monotonic() - t0:.1f}s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
