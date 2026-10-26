#!/usr/bin/env python3
"""A compressible file past the old run bound (CFS-32; docs/backlog.md;
cosmofs design.md, "The run bound, charged at the write"), and read by
the release before it.

    large_test.py --base cosmoos-ARCH.img --set DIR --key MODSIGN.key --arch ARCH --workdir DIR

A compressed record is a run of its own, and an inode may have 4096 runs:
a file LZ4 compresses stopped at about 128 MiB, and until v1.0.14 what
was written past that was lost at write-back. Now records past three
quarters of the bound are written plain, as runs that merge, and a write
the bound would still refuse is refused at the write.

Stages, on a disk the published release (--base, v1.0.13) installs on,
with two data disks the build makes pools on:

  install   the published release installs itself (upgrade_test.py's).
  upgrade   it trusts this build's key and upgrades to this build with
            its own cosmo-upgrade, which takes the environment be-v... of
            its root first.
  write     this build: a pool on the 1 GiB disk filled with one file of
            compressible chunks, each written with an fsync, until a
            write fails: the largest compressible file a 1 GiB pool holds
            (printed; docs/guide/limitations.md). A pool on the 1.5 GiB
            disk: a 1 GiB compressible file, every chunk written, its
            size and SHA-256, the pool's check clean.
  remount   this build again: both files the same after a reboot, the
            largest one every chunk of it; then cosmo-upgrade --rollback.
  old       the published release's kernel, booted from that environment:
            both pools mount, both files read back with the same SHA-256,
            both pools' checks clean.

--before installs the published release and runs only the write
stage's fill, on that release's own kernel: the number this build's is
compared with.

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
import be_test  # noqa: E402
from be_test import sh, fetch, end, UPA  # noqa: E402
import modsign  # noqa: E402
import net_login_test  # noqa: E402
import upgrade_test  # noqa: E402

class Done(Exception):
    """--before has measured what it measures."""


M_MIB, L_MIB = 1024, 1536   # the measured pool, and the 1 GiB file's
BIG_MIB = 1024
CHUNK = 4 << 20             # 4 MiB of the system's own programs: LZ4 shrinks them, one run per record
OLD_TAG = "v1.0.13"


def disks_env(args):
    extra = " ".join(f"-drive if=none,id={n},format=raw,file={p} -device virtio-blk-pci,drive={n}"
                     for n, p in (("dm", args.m_disk), ("dl", args.l_disk)))
    return {"QEMU_DISKS": "boot", "QEMU_EXTRA": (os.environ.get("QEMU_EXTRA", "") + " " + extra).strip()}


def boot(name, args, timeout=None):
    b = Boot(name, args.target, args.workdir, disks_env(args), timeout or args.timeout)
    b.wait_prompt("the first prompt")
    return b


def disk_named(b, mib):
    out = b.run("cosmo-install --list")
    return need(out, rf"^(\S+)\s+-\s+{(mib << 20) // install_test.SS}\s+{install_test.SS}\s",
                f"the {mib} MiB disk in cosmo-install --list").group(1)


def chunk(b):
    sh(b, f"cat /sbin/* /bin/* 2>/dev/null | head -c {CHUNK} > /tmp/chunk && test $(wc -c < /tmp/chunk) -eq {CHUNK}",
       "chunk")


def check_pool(b, mnt, what):
    out = b.run("fsctl list")
    mid = need(out, rf"^\s*(\d+)\s+cosmofs\s+\S+\s+{re.escape(mnt)}$", f"{what}: the pool's mount id").group(1)
    out = b.run(f"fsctl check {mid}")
    need(out, r"^clean$", f"{what}: the pool's check")


def fill(b):
    """The 1 GiB pool at /mnt/m, filled: a chunk at a time, each made
    durable before the next (dd's fsync reports what write-back met),
    until one fails. What the pool holds is the chunks before it.
    Returns (chunks, seconds, dd's last error)."""
    t0 = time.monotonic()
    out = b.status(f"i=0; while dd if=/tmp/chunk of=/mnt/m/fill bs={CHUNK} count=1 seek=$i conv=notrunc,fsync "
                   "2>/tmp/dd.err; do i=$((i+1)); done; echo chunks=$i; tail -n 1 /tmp/dd.err", "fill")
    chunks = int(need(out, r"^chunks=(\d+)$", "the chunks the 1 GiB pool took").group(1))
    err = out.split(f"chunks={chunks}", 1)[1].strip().splitlines()
    return chunks, time.monotonic() - t0, err[0] if err else ""


def stage_before(args, results):
    """--before: the same fill on the published release's own kernel, the
    measurement the change is compared with."""
    b = boot("before", args, timeout=max(args.timeout, 5400))
    try:
        need(b.text(), rf"^\[ INFO\] boot: verified boot, key {upgrade_test.RELEASE_KEY} ", f"the installed {OLD_TAG}")
        dm = disk_named(b, M_MIB)
        sh(b, f"mkfs.cosmofs {dm} && mkdir -p /mnt/m && mount -t cosmofs /dev/{dm} /mnt/m", "pool")
        chunk(b)
        chunks, filled, err = fill(b)
        lost = re.findall(r"^\[ *\w+\] vfs: (\d+) dirty page\(s\) of inode \d+ on cosmofs lost: (.*)$", b.text(), re.M)
        out = b.run("ls -l /mnt/m/fill; fsctl list")
        size = need(out, r"^\S+\s+\d+\s+\S+\s+\S+\s+(\d+)\s.*/mnt/m/fill$", "the filled file's size").group(1)
        end(b, "before")
    except Exception:
        b.kill()
        raise
    largest = chunks * CHUNK >> 20
    results.append(f"before: {OLD_TAG}'s kernel: the largest compressible file on a {M_MIB} MiB pool: {largest} MiB "
                   f"({chunks} chunks of 4 MiB, {filled:.0f} s); the next chunk's dd: {err!r}; the file's size "
                   f"{int(size) >> 20} MiB; pages lost: {lost or 'none logged'}")
    print(f"large: LARGEST {largest} MiB compressible on a {M_MIB} MiB pool ({OLD_TAG})", flush=True)


def stage_upgrade(args, port, results):
    b = boot("upgrade", args)
    try:
        need(b.text(), rf"^\[ INFO\] boot: verified boot, key {upgrade_test.RELEASE_KEY} ", f"the installed {OLD_TAG}")
        sh(b, f"wget -q -O /etc/pkg/keys/build.pub http://10.0.2.2:{port}/trust.pub && sync", "trust")
        fetch(b, port, args.arch, "a", UPA)
        out = sh(b, f"/sbin/cosmo-upgrade {UPA}", "up")
        env = need(out, r"^cosmo-upgrade: boot environment (be-\S+): the root as it is now, paired with the current set$",
                   f"{OLD_TAG}'s environment").group(1)
        need(out, r"^cosmo-upgrade: upgraded; reboot to run it", f"{OLD_TAG}'s cosmo-upgrade to this build")
        end(b, "upgrade")
    except Exception:
        b.kill()
        raise
    results.append(f"upgrade: {OLD_TAG} upgraded itself to this build, environment {env} taken first")
    return env


def stage_write(args, results, d):
    b = boot("write", args, timeout=max(args.timeout, 5400))
    try:
        if re.search(rf"^\[ INFO\] boot: verified boot, key {upgrade_test.RELEASE_KEY} ", b.text(), re.M):
            raise Fail("write: the upgraded root booted the release's kernel, not this build's")
        dm, dl = disk_named(b, M_MIB), disk_named(b, L_MIB)
        sh(b, f"mkfs.cosmofs {dm} && mkfs.cosmofs {dl} && mkdir -p /mnt/m /mnt/l && "
              f"mount -t cosmofs /dev/{dm} /mnt/m && mount -t cosmofs /dev/{dl} /mnt/l", "pools")
        chunk(b)
        d["chunks"], filled, _ = fill(b)
        sh(b, f"cp /tmp/chunk /mnt/l/c && sync", "keep")   # the chunk, for the old kernel to rebuild the expectation
        # The 1 GiB file on the larger pool: every chunk taken.
        t0 = time.monotonic()
        out = b.status(f"i=0; while [ $i -lt {BIG_MIB * (1 << 20) // CHUNK} ]; do "
                       f"dd if=/tmp/chunk of=/mnt/l/big bs={CHUNK} count=1 seek=$i conv=notrunc,fsync 2>/dev/null "
                       "|| break; i=$((i+1)); done; echo wrote=$i; ls -l /mnt/l/big; sha256sum /mnt/l/big", "big")
        wrote = time.monotonic() - t0
        need(out, rf"^wrote={BIG_MIB * (1 << 20) // CHUNK}$", "every chunk of the 1 GiB file")
        need(out, rf"\s{BIG_MIB << 20}\s+\S+\s+\S+\s+\S+\s+/mnt/l/big$", "the 1 GiB file's size")
        d["big"] = need(out, r"^([0-9a-f]{64})\s+/mnt/l/big$", "the 1 GiB file's SHA-256").group(1)
        want = b.run(f"i=0; while [ $i -lt {BIG_MIB * (1 << 20) // CHUNK} ]; do cat /tmp/chunk; i=$((i+1)); done "
                     "| sha256sum")
        if not re.search(rf"^{d['big']}\s", want, re.M):
            raise Fail("write: /mnt/l/big is not the chunk again and again")
        check_pool(b, "/mnt/l", "write")
        check_pool(b, "/mnt/m", "write")
        sh(b, "umount /mnt/m && umount /mnt/l", "umount")
        # No write taken and then dropped: past the run bound (CFS-32) or
        # past the pool's space, the fill's last chunk (MEM-9; on the
        # CFS-32 tree 164 pages lost -ENOSPC, x86-64, 2026-10-25).
        lost = re.search(r"^.*dirty page\(s\) of inode \d+ on \S+ lost.*$", b.text(), re.M)
        if lost:
            raise Fail(f"write: {lost.group(0).strip()!r}")
        end(b, "write")
    except Exception:
        b.kill()
        raise
    largest = d["chunks"] * CHUNK >> 20
    results.append(f"write: the largest compressible file on a {M_MIB} MiB pool: {largest} MiB ({d['chunks']} "
                   f"chunks of 4 MiB, {filled:.0f} s); the {BIG_MIB} MiB file written whole in {wrote:.0f} s; both "
                   "pools clean")
    print(f"large: LARGEST {largest} MiB compressible on a {M_MIB} MiB pool", flush=True)


def expect_pools(b, d, what):
    """Both pools mounted, both files what was written."""
    dm, dl = disk_named(b, M_MIB), disk_named(b, L_MIB)
    sh(b, f"mkdir -p /mnt/m /mnt/l && mount -t cosmofs /dev/{dm} /mnt/m && mount -t cosmofs /dev/{dl} /mnt/l",
       f"{what}-mount")
    out = b.run(f"ls -l /mnt/m/fill /mnt/l/big; sha256sum /mnt/l/big; "
                f"i=0; while [ $i -lt {d['chunks']} ]; do cat /mnt/l/c; i=$((i+1)); done | sha256sum; "
                f"head -c {d['chunks'] * CHUNK} /mnt/m/fill | sha256sum")
    need(out, rf"\s{BIG_MIB << 20}\s+\S+\s+\S+\s+\S+\s+/mnt/l/big$", f"{what}: the 1 GiB file's size")
    if not re.search(rf"^{d['big']}\s+/mnt/l/big$", out, re.M):
        raise Fail(f"{what}: the 1 GiB file does not read back the same")
    sums = re.findall(r"^([0-9a-f]{64})\s+-$", out, re.M)
    if len(sums) != 2 or sums[0] != sums[1]:
        raise Fail(f"{what}: the filled pool's file is not its {d['chunks']} chunks ({sums})")
    check_pool(b, "/mnt/m", what)
    check_pool(b, "/mnt/l", what)


def stage_remount(args, results, d):
    b = boot("remount", args, timeout=max(args.timeout, 3600))
    try:
        expect_pools(b, d, "remount")
        sh(b, "umount /mnt/m && umount /mnt/l", "umount")
        out = sh(b, "cosmo-upgrade --rollback --no-reboot", "back")
        need(out, r"^cosmo-upgrade: rolled back: ", "this build's --rollback")
        end(b, "remount")
    except Exception:
        b.kill()
        raise
    results.append("remount: both files the same after a reboot, both pools clean; rolled back")


def stage_old(args, results, d, env):
    b = boot("old", args, timeout=max(args.timeout, 3600))
    try:
        need(b.text(), rf"^\[ INFO\] boot: verified boot, key {upgrade_test.RELEASE_KEY} ", f"{OLD_TAG} again")
        expect_pools(b, d, "old")
        end(b, "old")
    except Exception:
        b.kill()
        raise
    results.append(f"old: {OLD_TAG}'s kernel, booted from {env}: both pools mount, both files read back the same "
                   "(the 1 GiB file by SHA-256, the filled one chunk by chunk), both checks clean")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", required=True, help=f"the published {OLD_TAG}'s live image")
    ap.add_argument("--set", required=True, help="this build's release set")
    ap.add_argument("--key", required=True, help="the build's signing key (scripts/modsign.py)")
    ap.add_argument("--arch", required=True)
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--timeout", type=float, default=240.0, help="per boot (the writing boots get longer)")
    ap.add_argument("--before", action="store_true",
                    help=f"install {OLD_TAG} and measure the largest compressible file on its own kernel, only")
    args = ap.parse_args()
    shutil.rmtree(args.workdir, ignore_errors=True)
    os.makedirs(args.workdir)
    www = os.path.join(args.workdir, "www")
    os.makedirs(os.path.join(www, "a"))
    for n in list(be_test.SET) + ["SHA256SUMS", "SHA256SUMS.sig"]:
        src = n if n.startswith("SHA") else f"cosmoos-{args.arch}-{n}"
        shutil.copyfile(os.path.join(args.set, src), os.path.join(www, "a", src))
    seed = modsign.read_hex_key(args.key, "secret key")
    open(os.path.join(www, "trust.pub"), "w").write(modsign.secret_to_public(seed).hex() + "\n")
    args.m_disk = os.path.join(args.workdir, "m.img")
    args.l_disk = os.path.join(args.workdir, "l.img")
    for p, mib in ((args.m_disk, M_MIB), (args.l_disk, L_MIB)):
        with open(p, "wb") as f:
            f.truncate(mib << 20)
    srv, port = net_login_test.start_http(www)
    results, d = [], {}
    t0 = time.monotonic()
    try:
        upgrade_test.BASE = OLD_TAG
        upgrade_test.stage_install(args, results)   # sets args.target
        if args.before:
            stage_before(args, results)
            raise Done()
        env = stage_upgrade(args, port, results)
        stage_write(args, results, d)
        stage_remount(args, results, d)
        stage_old(args, results, d, env)
    except Done:
        pass
    except Fail as e:
        for r in results:
            print(f"test-large: {r}")
        print(f"test-large: FAIL: {e} (logs in {args.workdir})")
        return 1
    finally:
        srv.shutdown()
    for r in results:
        print(f"test-large: {r}")
    print(f"test-large: PASS in {time.monotonic() - t0:.1f}s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
