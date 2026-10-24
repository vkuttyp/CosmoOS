#!/usr/bin/env python3
"""Roadmap 1.2 B3's acceptance test: mirror replacement and resilvering
(docs/roadmap-1.2.md; docs/kernel-services/filesystem/cosmofs/testing.md,
"test-mirror"; docs/guide/mirrors.md).

    mirror_test.py --image cosmoos.img --workdir DIR [--stages ...] [--timeout S]

Stage `install`: the live image with two blank disks; `cosmo-install A
--mirror B` puts the system on both, the root a two-way mirror of their
root partitions. Power off.

Stage `replace`: the installed disk A boots (QEMU's boot disk), B on a
PCIe root port, an empty root port beside it. `cosmo-pool status` shows
the mirror whole. A shell loop writes and syncs; the harness removes B
(QMP device_del); the system keeps running degraded and says so -- the
pool's MISSING line, `cosmo-pool status` DEGRADED, `/proc/mounts`
`rw,degraded` -- while the writer goes on. A new blank disk C is added
(device_add) and `cosmo-pool replace` resilvers it; the status reaches
ONLINE, the writer is stopped, `fsctl scrub` is clean. Power off.

Stage `reboot`: A and C alone: the root mounts from the new pair, whole,
and the writer's file reads back.

Stage `spare`: A and C with a fourth blank disk D on a root port: D is
added as a spare, the writer runs, C is removed, and the spare takes over
by itself (`spare D takes over`), is resilvered and is a member; the
scrub is clean; then A and D alone boot whole.

Stage `degraded`: A alone boots, init says the root pool is degraded,
the status has a copy MISSING; a file is written. Stage `lagging`: A and
D again: D is behind and is resilvered after the switch to the disk root,
and the file reads back; the scrub is clean.

Every boot is QEMU through scripts/qemu-run.sh driven at the shell's
prompt, as test-install drives it; QMP on a unix socket. Each stage's
times (removal to MISSING, resilver, scrub) are printed. Exit 0 PASS, 1
FAIL.
"""

import argparse
import os
import re
import shutil
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from install_test import Boot, Fail, need, EXIT_SUCCESS  # noqa: E402
from pci_hotplug_test import Qmp, wait_line  # noqa: E402

SS = 512
# Sizes tell the disks apart in `cosmo-install --list`; each one after A
# holds A's root partition.
MIB = {"a": 256, "b": 288, "c": 320, "d": 352, "e": 272, "f": 384}
WRITER = "(while true; do cat /bin/busybox > /mirror-w || break; sync; done) > /dev/null 2>&1 &"


def disk(work, name):
    path = os.path.join(work, f"disk-{name}.img")
    if not os.path.exists(path):
        with open(path, "wb") as f:
            f.truncate(MIB[name] << 20)
    return path


def find_disk(b, name):
    out = b.run("cosmo-install --list")
    m = need(out, rf"^(\S+)\s+-\s+{(MIB[name] << 20) // SS}\s+{SS}\s", f"disk {name} in cosmo-install --list")
    return m.group(1)


def root_ports(arch, n):
    s = " ".join(f"-device pcie-root-port,id=rp{i},chassis={30 + i},slot={i}" for i in range(1, n + 1))
    if arch == "x86_64":
        s += " -global ICH9-LPC.acpi-pci-hotplug-with-bridge-support=off"
    return s


def status(b):
    # 0 whole, 1 degraded; anything else (a crash) is a failure.
    out = b.status("cosmo-pool status /", "pool-status")
    need(out, r"^pool-status=[01]$", "cosmo-pool status's exit")
    return out


def wait_status(b, pattern, timeout, what):
    end = time.monotonic() + timeout
    out = ""
    while time.monotonic() < end:
        out = status(b)
        if re.search(pattern, out, re.M):
            return out
        time.sleep(2)
    raise Fail(f"{what}: `cosmo-pool status /` never matched {pattern!r}; last:\n{out}")


def mount_id(b, path="/"):
    out = b.run("fsctl list")
    m = need(out, rf"^\s*(\d+)\s+cosmofs\s+\S+\s+{re.escape(path)}\s*$", "fsctl list")
    return m.group(1)


def scrub_clean(b, what):
    t0 = time.monotonic()
    out = b.status(f"fsctl scrub {mount_id(b)}", "scrub-status")
    need(out, r"^scrub-status=0$", f"{what}: fsctl scrub")
    return time.monotonic() - t0


def boot(work, image, name, extra, timeout, qmp_path=None):
    if qmp_path:
        extra += f" -qmp unix:{qmp_path},server=on,wait=off"
    env = {"QEMU_DISKS": "boot", "QEMU_EXTRA": (os.environ.get("QEMU_EXTRA", "") + " " + extra).strip()}
    return Boot(name, image, work, env, timeout)


def root_checked(b, what):
    # The boot's check of a mirrored root runs: its detached mount holds
    # the pool's other copy itself, which is not "mounted elsewhere".
    need(b.text(), r"^cosmofs: root checked, 0 findings, 0 repaired$", f"{what}: the boot's check of the root")


def no_panic(text, what):
    for bad in ("KERNEL PANIC", "KASSERT", "page fault in kernel"):
        if bad in text:
            raise Fail(f"{what}: the log has '{bad}'")
    m = re.search(r"^.*process: pid \d+ '[^']*' fault: signal.*$", text, re.M)
    if m:
        raise Fail(f"{what}: a process faulted: {m.group(0)}")


def stage_install(args, results):
    work = args.workdir
    live = os.path.join(work, "live")
    os.makedirs(live, exist_ok=True)
    image = os.path.join(live, "cosmoos.img")
    shutil.copyfile(args.image, image)
    a, bimg = disk(work, "a"), disk(work, "b")
    extra = (f"-drive if=none,id=da,format=raw,file={a} -device virtio-blk-pci,drive=da "
             f"-drive if=none,id=db,format=raw,file={bimg} -device virtio-blk-pci,drive=db")
    b = Boot("install", image, work, {"QEMU_EXTRA": (os.environ.get("QEMU_EXTRA", "") + " " + extra).strip()},
             args.timeout)
    try:
        b.wait_prompt("the first prompt")
        da, db = find_disk(b, "a"), find_disk(b, "b")
        t0 = time.monotonic()
        out = b.status(f"cosmo-install {da} --mirror {db}", "install-status")
        need(out, r"^install-status=0$", "cosmo-install --mirror")
        need(out, rf"^cosmo-install: the root is mirrored on \S+ and \S+ \({db}'s root partition", "cosmo-install --mirror")
        t_inst = time.monotonic() - t0
        rc = b.finish("poweroff")
        if rc != EXIT_SUCCESS:
            raise Fail(f"install: QEMU exit code {rc}")
    except Exception:
        b.kill()
        raise
    results.append(f"install: {da} --mirror {db} in {t_inst:.1f}s")


def stage_replace(args, results, sock):
    work = args.workdir
    a, bimg, c = disk(work, "a"), disk(work, "b"), disk(work, "c")
    extra = (root_ports(args.arch, 2) + f" -drive if=none,id=db,format=raw,file={bimg} "
             f"-device virtio-blk-pci,id=dbdev,drive=db,bus=rp1 -drive if=none,id=dc,format=raw,file={c}")
    b = boot(work, a, "replace", extra, args.timeout, sock)
    try:
        b.wait_prompt("the first prompt")
        root_checked(b, "replace")
        q = Qmp(sock, time.monotonic() + 30)
        need(b.text(), r"^init: switched to the disk root \S+ \(root=boot-disk\)$", "the mirrored root")
        out = status(b)
        need(out, r"^pool / \(cosmo-root\): ONLINE", "the mirror before the removal")
        if len(re.findall(r"^\s+copy \d+\s+\S+\s+ONLINE", out, re.M)) != 2:
            raise Fail(f"the mirror before the removal: not two ONLINE copies:\n{out}")
        db = find_disk(b, "b")
        b.run(WRITER)
        time.sleep(3)
        start = len(b.text())
        t0 = time.monotonic()
        q.cmd("device_del", id="dbdev")
        q.wait_event("DEVICE_DELETED", "dbdev", 20)
        wait_line(b, r"pool: cosmo-root: member 0 copy \d+ \(\S+\) MISSING", start, 30, "the removal")
        t_missing = time.monotonic() - t0
        out = status(b)
        need(out, r"^pool / \(cosmo-root\): DEGRADED", "cosmo-pool status after the removal")
        m = need(out, r"^\s+copy (\d+)\s+\S+\s+MISSING", "cosmo-pool status after the removal")
        gone = f"0:{m.group(1)}"
        need(b.run("cat /proc/mounts"), r" / cosmofs rw,degraded ", "/proc/mounts after the removal")
        need(b.run("jobs"), r"Running", "the writer after the removal")
        # A new disk, and the replace.
        start = len(b.text())
        q.cmd("device_add", driver="virtio-blk-pci", id="dcdev", drive="dc", bus="rp2")
        wait_line(b, r"a device was added", start, 30, "the new disk")
        time.sleep(1)
        dc = find_disk(b, "c")
        t0 = time.monotonic()
        out = b.status(f"cosmo-pool replace {gone} {dc}", "replace-status")
        need(out, r"^replace-status=0$", "cosmo-pool replace")
        out = wait_status(b, r"^pool / \(cosmo-root\): ONLINE", args.timeout / 2, "the resilver")
        t_resilver = time.monotonic() - t0
        need(out, rf"^\s+copy \d+\s+{re.escape(dc)}\s+ONLINE", "the new disk in the mirror")
        b.run("kill %1; wait")
        t_scrub = scrub_clean(b, "after the replace")
        no_panic(b.text(), "replace")
        rc = b.finish("poweroff")
        if rc != EXIT_SUCCESS:
            raise Fail(f"replace: QEMU exit code {rc}")
    except Exception:
        b.kill()
        raise
    results.append(f"replace: {db} removed under a writer, MISSING {t_missing:.1f}s after device_del; {dc} "
                   f"replaced it, resilvered in {t_resilver:.1f}s; scrub clean in {t_scrub:.1f}s")


def stage_reboot(args, results, pair, what):
    work = args.workdir
    a, other = disk(work, "a"), disk(work, pair)
    extra = f"-drive if=none,id=dx,format=raw,file={other} -device virtio-blk-pci,drive=dx"
    b = boot(work, a, what, extra, args.timeout)
    try:
        b.wait_prompt("the first prompt")
        root_checked(b, what)
        need(b.text(), r"^init: switched to the disk root \S+ \(root=boot-disk\)$", f"{what}: the root")
        if "init: the root pool is degraded" in b.text():
            raise Fail(f"{what}: the root says it is degraded")
        out = status(b)
        need(out, r"^pool / \(cosmo-root\): ONLINE", f"{what}: cosmo-pool status")
        dx = find_disk(b, pair)
        need(out, rf"^\s+copy \d+\s+{re.escape(dx)}\s+ONLINE", f"{what}: the new pair")
        # The writer was stopped mid-copy at the latest: the file is the
        # binary whole or a prefix of it, compared as such (Codex on #442:
        # `cmp || ls` passed anything that existed).
        out = b.status("n=$(wc -c < /mirror-w) && head -c \"$n\" /bin/busybox | cmp - /mirror-w", "cmp-status")
        need(out, r"^cmp-status=0$", f"{what}: the writer's file")
        no_panic(b.text(), what)
        rc = b.finish("poweroff")
        if rc != EXIT_SUCCESS:
            raise Fail(f"{what}: QEMU exit code {rc}")
    except Exception:
        b.kill()
        raise
    results.append(f"{what}: the root mounted from disks a and {pair}, whole")


def stage_degraded(args, results):
    """Disk a alone: the root boots degraded and says so; a file is written
    that only a's copy has."""
    work = args.workdir
    b = boot(work, disk(work, "a"), "degraded", "", args.timeout)
    try:
        b.wait_prompt("the first prompt")
        root_checked(b, "degraded")
        need(b.text(), r"^init: switched to the disk root \S+ \(root=boot-disk\)$", "degraded: the root")
        need(b.text(), r"^init: the root pool is degraded: a copy is missing or being resilvered \(cosmo-pool status\)$",
             "degraded: init's line")
        out = status(b)
        need(out, r"^pool / \(cosmo-root\): DEGRADED", "degraded: cosmo-pool status")
        need(out, r"^\s+copy \d+\s+\S+\s+MISSING", "degraded: the missing copy")
        need(b.status("echo only-on-a > /lag-file && sync", "lag-status"), r"^lag-status=0$", "degraded: a write")
        no_panic(b.text(), "degraded")
        rc = b.finish("poweroff")
        if rc != EXIT_SUCCESS:
            raise Fail(f"degraded: QEMU exit code {rc}")
    except Exception:
        b.kill()
        raise
    results.append("degraded: disk a alone booted, said the root pool is degraded, copy MISSING")


def stage_lagging(args, results):
    """Disks a and d again: d is behind; after the switch to the disk root
    it is resilvered, and what a alone wrote reads back from the pool."""
    work = args.workdir
    extra = f"-drive if=none,id=dx,format=raw,file={disk(work, 'd')} -device virtio-blk-pci,drive=dx"
    b = boot(work, disk(work, "a"), "lagging", extra, args.timeout)
    try:
        b.wait_prompt("the first prompt")
        root_checked(b, "lagging")
        t0 = time.monotonic()
        dx = find_disk(b, "d")
        out = wait_status(b, r"^pool / \(cosmo-root\): ONLINE", args.timeout / 2, "lagging: the resilver")
        t_res = time.monotonic() - t0
        need(out, rf"^\s+copy \d+\s+{re.escape(dx)}\S*\s+ONLINE", "lagging: d back in the pool")
        need(b.text(), rf"cosmofs: cosmo-root: {re.escape(dx)}\S* resilvered as member 0 copy \d+", "lagging: d resilvered")
        need(b.run("cat /lag-file"), r"^only-on-a$", "lagging: a's file")
        t_scrub = scrub_clean(b, "lagging")
        no_panic(b.text(), "lagging")
        rc = b.finish("poweroff")
        if rc != EXIT_SUCCESS:
            raise Fail(f"lagging: QEMU exit code {rc}")
    except Exception:
        b.kill()
        raise
    results.append(f"lagging: disk d, behind, resilvered after the switch to the disk root (ONLINE at "
                   f"{t_res:.1f}s after the prompt); scrub clean in {t_scrub:.1f}s")


def stage_spare(args, results, sock):
    work = args.workdir
    a, c, d = disk(work, "a"), disk(work, "c"), disk(work, "d")
    extra = (root_ports(args.arch, 2) + f" -drive if=none,id=dc,format=raw,file={c} "
             f"-device virtio-blk-pci,id=dcdev,drive=dc,bus=rp1 -drive if=none,id=dd,format=raw,file={d} "
             f"-device virtio-blk-pci,id=dddev,drive=dd,bus=rp2")
    b = boot(work, a, "spare", extra, args.timeout, sock)
    try:
        b.wait_prompt("the first prompt")
        root_checked(b, "spare")
        q = Qmp(sock, time.monotonic() + 30)
        dd = find_disk(b, "d")
        out = b.status(f"cosmo-pool add-spare {dd}", "spare-status")
        need(out, r"^spare-status=0$", "cosmo-pool add-spare")
        out = status(b)
        need(out, rf"^\s+{re.escape(dd)}\s+\d+ blocks", "the spare in cosmo-pool status")
        dc = find_disk(b, "c")
        b.run(WRITER)
        time.sleep(3)
        start = len(b.text())
        t0 = time.monotonic()
        q.cmd("device_del", id="dcdev")
        q.wait_event("DEVICE_DELETED", "dcdev", 20)
        wait_line(b, rf"spare {re.escape(dd)} takes over member 0 copy \d+", start, 30, "the spare's takeover")
        t_take = time.monotonic() - t0
        out = wait_status(b, r"^pool / \(cosmo-root\): ONLINE", args.timeout / 2, "the spare's resilver")
        t_res = time.monotonic() - t0
        need(out, rf"^\s+copy \d+\s+{re.escape(dd)}\s+ONLINE", "the spare as a member")
        if re.search(r"^\s+spares:", out, re.M):
            raise Fail(f"the spare is still listed as a spare:\n{out}")
        b.run("kill %1; wait")
        t_scrub = scrub_clean(b, "after the takeover")
        no_panic(b.text(), "spare")
        rc = b.finish("poweroff")
        if rc != EXIT_SUCCESS:
            raise Fail(f"spare: QEMU exit code {rc}")
    except Exception:
        b.kill()
        raise
    results.append(f"spare: {dc} removed under a writer, the spare {dd} took over {t_take:.1f}s after "
                   f"device_del and was ONLINE at {t_res:.1f}s; scrub clean in {t_scrub:.1f}s")


# --- PKG-11: an upgrade reaches every disk's ESP ------------------------------------
#
# Disks e and f, installed as a mirror (each with an ESP of its own: the
# disks the replace and spare stages add are whole-disk copies, which have
# none). Set A is this build's, B and C are made from it (be_test.py's
# make_set_b), each boot.tar 1 KiB longer than the last, so the loader's
# `archive: N bytes read` line says which set a disk's ESP booted.


def booted_set(b, sizes, what):
    n = int(need(b.text(), r"^archive: (\d+) bytes read$", f"{what}: the loader's archive line").group(1))
    for name, size in sizes.items():
        if size == n:
            return name
    raise Fail(f"{what}: the loader read a {n}-byte archive, none of the sets' ({sizes})")


def esp_pair(work):
    e, f = disk(work, "e"), disk(work, "f")
    return e, f, f"-drive if=none,id=dx,format=raw,file={f} -device virtio-blk-pci,drive=dx"


def esp_boot(args, image, what, extra=""):
    b = boot(args.workdir, image, what, extra, args.timeout)
    b.wait_prompt("the first prompt")
    root_checked(b, what)
    return b


def esp_sh(b, cmd, tag, want=0):
    out = b.status(cmd, tag)
    got = int(need(out, rf"^{tag}=(\d+)$", cmd).group(1))
    if got != want:
        raise Fail(f"{cmd!r}: exit {got}, want {want}; output {out[-900:]!r}")
    return out


def esp_get(b, port, sub):
    import be_test  # noqa: E402  (its sets' file names)
    be_test.fetch(b, port, os.environ.get("QEMU_ARCH", "x86_64"), sub, f"/tmp/{sub}")


def esp_end(b, what):
    no_panic(b.text(), what)
    rc = b.finish("poweroff")
    if rc != EXIT_SUCCESS:
        raise Fail(f"{what}: QEMU exit code {rc}")


def esp_sets(args):
    """Sets A, B and C served over HTTP; their boot.tar sizes by name."""
    import be_test  # noqa: E402
    import modsign  # noqa: E402
    import net_login_test  # noqa: E402
    www = os.path.join(args.workdir, "www")
    shutil.rmtree(www, ignore_errors=True)
    os.makedirs(os.path.join(www, "a"))
    for n in list(be_test.SET) + ["SHA256SUMS", "SHA256SUMS.sig"]:
        src = n if n.startswith("SHA") else f"cosmoos-{args.arch}-{n}"
        shutil.copyfile(os.path.join(args.set, src), os.path.join(www, "a", src))
    seed = modsign.read_hex_key(args.key, "secret key")
    be_test.make_set_b(os.path.join(www, "a"), os.path.join(www, "b"), args.arch, seed)
    be_test.make_set_b(os.path.join(www, "b"), os.path.join(www, "c"), args.arch, seed, "usr/share/cosmo-set-c")
    sizes = {k: os.path.getsize(os.path.join(www, k, f"cosmoos-{args.arch}-boot.tar")) for k in "abc"}
    srv, port = net_login_test.start_http(www)
    return srv, port, sizes


def stage_esp(args, results, port, sizes):
    work = args.workdir
    # esp-install: e --mirror f from the live image (set A).
    live = os.path.join(work, "live")
    os.makedirs(live, exist_ok=True)
    image = os.path.join(live, "cosmoos.img")
    shutil.copyfile(args.image, image)
    e, f, with_f = esp_pair(work)
    extra = (f"-drive if=none,id=de,format=raw,file={e} -device virtio-blk-pci,drive=de "
             f"-drive if=none,id=df,format=raw,file={f} -device virtio-blk-pci,drive=df")
    b = Boot("esp-install", image, work, {"QEMU_EXTRA": (os.environ.get("QEMU_EXTRA", "") + " " + extra).strip()},
             args.timeout)
    try:
        b.wait_prompt("the first prompt")
        de, df = find_disk(b, "e"), find_disk(b, "f")
        esp_sh(b, f"cosmo-install {de} --mirror {df}", "inst")
        esp_end(b, "esp-install")
    except Exception:
        b.kill()
        raise

    # esp-upgrade: e and f, A -> B; f's ESP made e's.
    b = esp_boot(args, e, "esp-upgrade", with_f)
    try:
        if booted_set(b, sizes, "esp-upgrade") != "a":
            raise Fail("esp-upgrade: the installed system is not set A")
        if re.search(r"^cosmo-upgrade: the ESP on ", b.text(), re.M):
            raise Fail("esp-upgrade: the boot's check found the ESPs of a fresh install different")
        esp_get(b, port, "b")
        out = esp_sh(b, "cosmo-upgrade /tmp/b", "up")
        m = need(out, r"^cosmo-upgrade: the ESP on (\S+) is (\S+)'s \((\d+) sectors written\)$",
                 "esp-upgrade: the other disk's ESP")
        if int(m.group(3)) == 0:
            raise Fail("esp-upgrade: the upgrade wrote no sector of the other disk's ESP")
        peer, booted, n_up = m.group(1), m.group(2), int(m.group(3))
        esp_sh(b, "cosmo-upgrade --esp-check", "chk")
        esp_end(b, "esp-upgrade")
    except Exception:
        b.kill()
        raise

    # esp-pull: f alone, its ESP: set B.
    b = esp_boot(args, f, "esp-pull")
    try:
        got = booted_set(b, sizes, "esp-pull")
        if got != "b":
            raise Fail(f"esp-pull: disk f alone booted set {got.upper()}, not the upgrade's B")
        esp_end(b, "esp-pull")
    except Exception:
        b.kill()
        raise
    results.append(f"esp-upgrade: A -> B on e and f wrote {n_up} sectors of {peer} ({booted}'s copy); "
                   "f alone then booted B")

    # esp-rejoin: e and f again, e resilvered (f ran alone).
    b = esp_boot(args, e, "esp-rejoin", with_f)
    try:
        wait_status(b, r"^pool / \(cosmo-root\): ONLINE", args.timeout / 2, "esp-rejoin: the resilver")
        esp_end(b, "esp-rejoin")
    except Exception:
        b.kill()
        raise

    # esp-absent: e alone, B -> C: f's ESP cannot be written.
    b = esp_boot(args, e, "esp-absent")
    try:
        esp_get(b, port, "c")
        out = esp_sh(b, "cosmo-upgrade /tmp/c", "up")
        need(out, r"^cosmo-upgrade: 1 copy\(ies\) of the root are missing: their disks' ESPs are not updated; once "
                  r"they are back, cosmo-upgrade --repair brings them up to this one$", "esp-absent: the missing copy")
        esp_end(b, "esp-absent")
    except Exception:
        b.kill()
        raise

    # esp-lagging: f rejoins; the boot says its ESP lags; --repair.
    b = esp_boot(args, e, "esp-lagging", with_f)
    try:
        need(b.text(), rf"^cosmo-upgrade: the ESP on {re.escape(peer)} is not {re.escape(booted)}'s \(its set "
                       r"[0-9a-f]{12}, the booted [0-9a-f]{12}\): run cosmo-upgrade --repair to bring it up to date$",
             "esp-lagging: the boot's check")
        esp_sh(b, "cosmo-upgrade --esp-check", "chk", 1)
        wait_status(b, r"^pool / \(cosmo-root\): ONLINE", args.timeout / 2, "esp-lagging: the resilver")
        out = esp_sh(b, "cosmo-upgrade --repair", "rep")
        m = need(out, rf"^cosmo-upgrade: the ESP on {re.escape(peer)} is {re.escape(booted)}'s \((\d+) sectors "
                      r"written\)$", "esp-lagging: --repair")
        n_rep = int(m.group(1))
        if n_rep == 0:
            raise Fail("esp-lagging: --repair wrote no sector of the lagging ESP")
        esp_sh(b, "cosmo-upgrade --esp-check", "chk")
        esp_end(b, "esp-lagging")
    except Exception:
        b.kill()
        raise

    # esp-repaired: f alone boots C.
    b = esp_boot(args, f, "esp-repaired")
    try:
        got = booted_set(b, sizes, "esp-repaired")
        if got != "c":
            raise Fail(f"esp-repaired: disk f alone booted set {got.upper()}, not C")
        esp_end(b, "esp-repaired")
    except Exception:
        b.kill()
        raise
    results.append(f"esp-absent: B -> C with f absent named the missing copy; f back: the boot said its ESP "
                   f"lags, --repair wrote {n_rep} sectors, f alone then booted C")

    # esp-rollback: e and f, --rollback (C -> B) reaches both; f alone boots B.
    b = esp_boot(args, e, "esp-rollback", with_f)
    try:
        wait_status(b, r"^pool / \(cosmo-root\): ONLINE", args.timeout / 2, "esp-rollback: the resilver")
        start = len(b.text())
        b.proc.stdin.write(b"cosmo-upgrade --rollback\n")
        b.proc.stdin.flush()
        try:
            b.proc.wait(timeout=180)
        except Exception:
            raise Fail(f"esp-rollback: QEMU did not end at init's restart; tail {b.text()[-900:]!r}")
        out = b.text()[start:]
        need(out, rf"^cosmo-upgrade: the ESP on {re.escape(peer)} is {re.escape(booted)}'s \(\d+ sectors written\)$",
             "esp-rollback: the other disk's ESP")
        need(out, r"^cosmo-upgrade: rebooting to apply it$", "esp-rollback: the reboot")
        no_panic(b.text(), "esp-rollback")
    except Exception:
        b.kill()
        raise
    b = esp_boot(args, f, "esp-rolled-back")
    try:
        got = booted_set(b, sizes, "esp-rolled-back")
        if got != "b":
            raise Fail(f"esp-rolled-back: disk f alone booted set {got.upper()}, not the rollback's B")
        esp_end(b, "esp-rolled-back")
    except Exception:
        b.kill()
        raise
    results.append("esp-rollback: --rollback C -> B on e and f wrote f's ESP before the reboot; f alone then "
                   "booted B")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--image", required=True)
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--stages", default="install,replace,reboot,spare,reboot-spare,degraded,lagging,esp")
    ap.add_argument("--timeout", type=float, default=300.0)
    ap.add_argument("--set", help="this build's release set (set A): the esp stage")
    ap.add_argument("--key", help="the build's signing key (scripts/modsign.py): the esp stage")
    args = ap.parse_args()
    args.arch = os.environ.get("QEMU_ARCH", "x86_64")
    if "esp" in args.stages.split(",") and not (args.set and args.key):
        ap.error("the esp stage needs --set and --key")
    stages = args.stages.split(",")
    if "install" in stages and os.path.isdir(args.workdir):
        shutil.rmtree(args.workdir)
    os.makedirs(args.workdir, exist_ok=True)
    sockdir = tempfile.mkdtemp(prefix="cosmo-mr-")
    sock = os.path.join(sockdir, "qmp.sock")
    srv = None
    results = []
    t0 = time.monotonic()
    try:
        for st in stages:
            t = time.monotonic()
            if st == "install":
                stage_install(args, results)
            elif st == "replace":
                stage_replace(args, results, sock)
            elif st == "reboot":
                stage_reboot(args, results, "c", "reboot")
            elif st == "spare":
                stage_spare(args, results, sock)
            elif st == "reboot-spare":
                stage_reboot(args, results, "d", "reboot-spare")
            elif st == "degraded":
                stage_degraded(args, results)
            elif st == "lagging":
                stage_lagging(args, results)
            elif st == "esp":
                srv, port, sizes = esp_sets(args)
                stage_esp(args, results, port, sizes)
            else:
                raise Fail(f"unknown stage {st}")
            results[-1] += f" [{time.monotonic() - t:.1f}s]"
    except Fail as e:
        for r in results:
            print(f"test-mirror: {r}")
        print(f"test-mirror: FAIL: {e}\ntest-mirror: logs in {args.workdir}")
        return 1
    finally:
        shutil.rmtree(sockdir, ignore_errors=True)
        if srv is not None:
            srv.shutdown()
    for r in results:
        print(f"test-mirror: {r}")
    print(f"test-mirror: PASS ({args.arch}) in {time.monotonic() - t0:.1f}s; logs in {args.workdir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
