#!/usr/bin/env python3
"""A writer that outruns memory, and one that outruns the disk (MEM-9;
docs/backlog.md; vfs design.md, "Dirty pages, bounded").

    fill_test.py --image cosmoos.img --workdir DIR [--timeout S]

Dirty page-cache pages were bounded neither by memory nor by the pool's
free space: a `dd` onto a 256 MiB root wrote about 470 MB into memory
before a fault killed it, and every spawn after was `ENOMEM`
(test-pkgtx's first fill, 2026-10-20).

Stages, on a 256 MiB guest (QEMU_MEM) with this build installed on a
256 MiB root (install_test.py's install):

  writer   a 1 GiB data pool takes 512 MiB of incompressible bytes from
           one writer with no sync: twice the guest's memory, which the
           page cache must write back as it goes. it exits 0, nothing is
           killed, the file is its size and the pool's check is clean.
  enospc   the root takes an endless stream from one `dd` with no sync:
           dd ends `No space left on device`, nothing is killed, no
           write-back fails and no dirty page is lost, and spawns and a
           sync work after it; then the file is removed and the root's
           free space comes back.

Exit 0 PASS, 1 FAIL.
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

DATA_MIB = 1024
WRITE_MIB = 512
# The stream: 256 KiB of awk's noise, repeated (be_test.py's fill_root).
RND = ("awk \"BEGIN { srand(9); for (i = 0; i < 262144; i++) printf \\\"%c\\\", 1 + int(rand() * 255) }\" "
       "> /tmp/rnd")
# What a writer killed by a fault, or a spawn refused for memory, says
# (main at 75e1e15f, stage writer: `sh: cat: Out of memory`, the stream
# cut short and the file 229638144 bytes of the 512 MiB).
KILLED = re.compile(r"^.*(?:exited with status (?:1[3-9]\d|2\d\d)\b|killed|out of memory|ENOMEM|"
                    r"Cannot allocate memory).*$", re.M | re.I)


# What a write the pool could not hold, taken anyway, ends as: the
# commit refused and the pages dropped with their vnode (MEM-9's
# ENOSPC half, seen on the unfixed tree as `2010 dirty page(s) of inode
# 355 on cosmofs lost: write-back failed (-28)` while `ls` still gave
# the size).
LOST = re.compile(r"^.*(?:dirty page\(s\) of inode \d+ on \S+ lost|writeback commit failed).*$", re.M)


def killed_since(b, mark, what):
    text = b.text()[mark:]
    for pat in (KILLED, LOST):
        m = pat.search(text)
        if m:
            raise Fail(f"{what}: {m.group(0).strip()!r}")


def boot(name, args):
    extra = f"-drive if=none,id=dd,format=raw,file={args.data} -device virtio-blk-pci,drive=dd"
    env = {"QEMU_DISKS": "boot", "QEMU_EXTRA": (os.environ.get("QEMU_EXTRA", "") + " " + extra).strip()}
    b = Boot(name, args.target, args.workdir, env, args.timeout)
    b.wait_prompt("the first prompt")
    return b


def data_disk(b):
    out = b.run("cosmo-install --list")
    return need(out, rf"^(\S+)\s+-\s+{(DATA_MIB << 20) // install_test.SS}\s+{install_test.SS}\s",
                f"the {DATA_MIB} MiB disk in cosmo-install --list").group(1)


def stage_writer(b, results):
    dev = data_disk(b)
    out = b.status(f"mkfs.cosmofs {dev} && mkdir -p /mnt/d && mount -t cosmofs /dev/{dev} /mnt/d", "pool")
    need(out, r"^pool=0$", "the data pool")
    mark = len(b.text())
    t0 = time.monotonic()
    out = b.status(f"sh -c 'while cat /tmp/rnd; do :; done | head -c {WRITE_MIB << 20} > /mnt/d/big'", "wr")
    wrote = time.monotonic() - t0
    killed_since(b, mark, "writer")
    need(out, r"^wr=0$", f"{WRITE_MIB} MiB written with no sync")
    out = b.run("sync; ls -l /mnt/d/big; echo spawn-ok")
    need(out, rf"\s{WRITE_MIB << 20}\s+\S+\s+\S+\s+\S+\s+/mnt/d/big$", "the written file's size")
    need(out, r"^spawn-ok$", "a spawn after the writer")
    out = b.run("fsctl list")
    mid = need(out, r"^\s*(\d+)\s+cosmofs\s+\S+\s+/mnt/d$", "the data pool's mount id").group(1)
    need(b.run(f"fsctl check {mid}"), r"^clean$", "the data pool's check")
    b.run("umount /mnt/d")
    results.append(f"writer: {WRITE_MIB} MiB with no sync on a {os.environ.get('QEMU_MEM', '256M')} guest in "
                   f"{wrote:.0f} s; nothing killed; the pool clean")


def root_free(b):
    out = b.run("cosmo-snap list | grep 'KiB free'")
    return int(need(out, r"^cosmo-snap: /: (\d+) KiB free of \d+ KiB$", "the root's free space").group(1))


def stage_enospc(b, results):
    free0 = root_free(b)
    mark = len(b.text())
    t0 = time.monotonic()
    out = b.status("sh -c 'while cat /tmp/rnd; do :; done | dd of=/fill bs=262144'", "fill")
    filled = time.monotonic() - t0
    killed_since(b, mark, "enospc")
    need(out, r"No space left on device", "dd's error on the full root")
    need(out, r"^fill=1$", "dd's exit status on the full root")
    out = b.run("echo spawn-ok; ls / > /dev/null && echo ls-ok; sync && echo sync-ok")
    for w in ("spawn-ok", "ls-ok", "sync-ok"):
        need(out, rf"^{w}$", f"{w.split('-')[0]} after ENOSPC")
    killed_since(b, mark, "after enospc")
    size = int(need(b.run("ls -l /fill"), r"^\S+\s+\d+\s+\S+\s+\S+\s+(\d+)\s.*/fill$", "the fill's size").group(1))
    full = root_free(b)
    b.run("rm /fill && sync && sync")
    killed_since(b, mark, "after the fill's removal")
    free1 = root_free(b)
    if free1 < free0 - 1024:
        raise Fail(f"enospc: the root's free space did not come back ({free0} KiB before, {free1} after)")
    results.append(f"enospc: dd stopped by ENOSPC after {size >> 20} MiB in {filled:.0f} s, {full} KiB left; "
                   f"spawns and sync after it; {free1} KiB free again after rm (from {free0})")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--image", required=True)
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--timeout", type=float, default=240.0)
    ap.add_argument("--key", help="unused: install_test.stage_install's signature")
    args = ap.parse_args()
    shutil.rmtree(args.workdir, ignore_errors=True)
    os.makedirs(args.workdir)
    args.data = os.path.join(args.workdir, "data.img")
    with open(args.data, "wb") as f:
        f.truncate(DATA_MIB << 20)
    results = []
    t0 = time.monotonic()
    try:
        inst = install_test.stage_install(args, results)
        args.target = inst["target"]
        b = boot("fill", args)
        try:
            b.per_command(1200)   # each dd is long; a command that never answers still fails
            b.run(f"sh -c '{RND}'")
            stage_writer(b, results)
            stage_enospc(b, results)
            rc = b.finish("poweroff")
            if rc != EXIT_SUCCESS:
                raise Fail(f"fill: QEMU exit code {rc}")
        except Exception:
            b.kill()
            raise
    except Fail as e:
        for r in results:
            print(f"test-fill: {r}")
        print(f"test-fill: FAIL: {e} (logs in {args.workdir})")
        return 1
    for r in results:
        print(f"test-fill: {r}")
    print(f"test-fill: PASS in {time.monotonic() - t0:.1f}s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
