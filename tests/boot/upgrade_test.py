#!/usr/bin/env python3
"""Shell completeness S7's acceptance test: an installed v1.0.3 system is
upgraded to this build without reinstalling, and rolled back
(docs/userland/testing.md, "test-upgrade").

    upgrade_test.py --base cosmoos-ARCH.img --set DIR --pub KEY.pub --workdir DIR

--base is the published v1.0.3 live image (tools/fetch-release.py), --set
this build's release set (make release-set: cosmoos-ARCH-kernel.elf,
-boot.tar, -cmdline, -manifest, -cosmo-upgrade), --pub the public key the
build signs with. The host serves them over HTTP under the release's names;
the installed system fetches them with wget, as an operator would, and
cosmo-upgrade reads them under those names.

Boots, each of the installed disk alone after the first:

  install   v1.0.3's live image installs itself on a blank 256 MiB disk.
  refuse    v1.0.3 boots verified with the release key. Before the build's
            key is in /etc/pkg/keys the upgrade is refused (unknown signing
            key); with it, a manifest with one byte changed (bad
            signature), one signed by another key (unknown signing key) and
            an archive with one byte changed (SHA-512) are refused, each
            exit 3; so is a set, signed by a key the ring holds, whose
            loader trusts that key but not v1.0.3's (the release key's
            entry in its table replaced). Power off: the ESP is byte for
            byte what it was.
  written   --stop-after=written, then the machine is killed. The ESP's
            loader and set are still v1.0.3's, byte for byte.
  loader    v1.0.3 boots verified; --stop-after=loader, killed. The
            loader is the new one, the set still v1.0.3's.
  upgrade   The new loader boots v1.0.3's set verified (release key).
            pkg install hello=1.0; the full upgrade; power off. The ESP holds the new set,
            \\cosmo\\prev v1.0.3's set and loader.
  new       This build boots verified with its own key; the base system
            is the new one (mkfs.cosmofs, the new /etc/rc), the
            installer's /etc/passwd and a file of the operator's are kept;
            pkg update && pkg upgrade takes hello to 1.1; then
            cosmo-upgrade --rollback.
  back      v1.0.3 boots verified with the release key; its base system
            is back (no mkfs.cosmofs or cosmo-upgrade, its /etc/rc), the
            operator's file is still there, and so is hello 1.1. The
            build's cosmo-upgrade, fetched into /tmp, rolls back again:
            v1.0.3's loader does not trust the build's key, so the loader
            (\\cosmo\\prev's) switches first.
  forward   This build boots verified with its own key again; the ESP
            holds the new set and loader, \\cosmo\\prev v1.0.3's.

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
from install_test import Boot, Fail, need  # noqa: E402
import mkmanifest  # noqa: E402  (scripts/, on install_test's path)
import modsign  # noqa: E402
import net_login_test  # noqa: E402

RELEASE_KEY = "36ae7bb4947896ab"   # tools/keys/release-2026.pub: v1.0.3's signer
SET = ("kernel.elf", "boot.tar", "cmdline", "manifest")
# v1.0.3's cosmofs loses a large file being written now and then: a
# compressed record rewritten once its last page fills no longer
# compresses, and the plain fallback leaves it unreadable (fixed on this
# build: tools/cosmofs-record-fallback-probe.py). So v1.0.3 never writes
# the release to its disk here: each boot that needs it fetches it into
# /tmp, which is ramfs, as docs/guide/upgrading.md tells an operator to.
UP = "/tmp/up"


def loader_name(arch):
    return "BOOTX64.EFI" if arch == "x86_64" else "BOOTAA64.EFI"


def esp_bytes(target):
    data = open(target, "rb").read()
    return install_test.esp_of(data)


def esp_file(target, path):
    return mkmanifest.esp_get(target, path)


def boot_disk(name, args, port=None):
    env = {"QEMU_DISKS": "boot"}
    return Boot(name, args.target, args.workdir, env, args.timeout)


def verified_key(b, what):
    m = need(b.text(), r"^\[ INFO\] boot: verified boot, key ([0-9a-f]{16}) ", what)
    need(b.text(), r"^init: switched to the disk root \S+ \(root=boot-disk\)$", what)
    return m.group(1)


def sh(b, cmd, tag, want=0):
    out = b.status(cmd, tag)
    m = need(out, rf"^{tag}=(\d+)$", cmd)
    if int(m.group(1)) != want:
        raise Fail(f"{cmd!r}: exit {m.group(1)}, want {want}; output {out[-800:]!r}")
    return out


def check_set(target, files, where, what):
    """The ESP's set at `where` (a directory, '::/cosmo') is `files`."""
    for name in SET:
        got = esp_file(target, f"{where}/{name}")
        if got != files[name]:
            raise Fail(f"{what}: {where}/{name} is {len(got)} bytes, not the expected {len(files[name])}")


def stage_install(args, results):
    work = args.workdir
    live = os.path.join(work, "live")
    os.makedirs(live, exist_ok=True)
    image = os.path.join(live, "base.img")
    shutil.copyfile(args.base, image)
    args.target = os.path.join(work, "target.img")
    with open(args.target, "wb") as f:
        f.truncate(install_test.TARGET_MIB << 20)
    env = {"QEMU_EXTRA": (os.environ.get("QEMU_EXTRA", "") +
                          f" -drive if=none,id=inst,format=raw,file={args.target} "
                          "-device virtio-blk-pci,drive=inst").strip()}
    b = Boot("install", image, work, env, args.timeout)
    try:
        b.wait_prompt("the first prompt")
        need(b.text(), rf"^\[ INFO\] boot: verified boot, key {RELEASE_KEY} ", "the v1.0.3 live image")
        out = b.run("cosmo-install --list")
        m = need(out, rf"^(\S+)\s+-\s+{(install_test.TARGET_MIB << 20) // install_test.SS}\s+{install_test.SS}\s",
                 "cosmo-install --list")
        dev = m.group(1)
        out = sh(b, f"cosmo-install {dev}", "inst")
        need(out, rf"^cosmo-install: installed on {dev}: ", "v1.0.3's cosmo-install")
        rc = b.finish()
        if rc != install_test.EXIT_SUCCESS:
            raise Fail(f"install boot: QEMU exit code {rc}")
    except Exception:
        b.kill()
        raise
    results.append(f"install: v1.0.3 (key {RELEASE_KEY}) installed itself on {dev}; PASS in {b.elapsed():.1f}s")


def fetch_all(b, port, arch, variants=False):
    url = f"http://10.0.2.2:{port}"
    files = " ".join(f"cosmoos-{arch}-{n}" for n in SET) + f" cosmoos-{arch}-cosmo-upgrade trust.pub"
    sh(b, f"mkdir -p {UP} && cd {UP} && for f in {files}; do wget -q {url}/$f || exit 1; done && "
          f"mv cosmoos-{arch}-cosmo-upgrade cosmo-upgrade && chmod 755 cosmo-upgrade && cd /", "fetch")
    if not variants:
        return
    # The refused variants: links to the good files, the rest replaced.
    for d, bad in VARIANTS:
        links = " && ".join(f"ln -s {UP}/cosmoos-{arch}-{f} /tmp/{d}/{f}" for f in SET if f not in bad)
        gets = " && ".join(f"wget -q -O /tmp/{d}/{f} {url}/{d}/{f}" for f in bad)
        sh(b, f"mkdir -p /tmp/{d} && {links} && {gets}", f"get{d}")
    sh(b, f"wget -q -O /tmp/untrusting.pub {url}/untrusting.pub", "getuntrustingpub")


VARIANTS = (("badsig", ("manifest",)), ("otherkey", ("manifest",)), ("badtar", ("boot.tar",)),
            ("untrusting", ("boot.tar", "manifest")))


def stage_refuse(args, results, port, keyid, otherid):
    b = boot_disk("refuse", args)
    try:
        b.wait_prompt("the first prompt")
        # The ESP once the firmware is done with it: x86 firmware with no
        # variable store writes \NvVars there at every boot, before the
        # kernel runs (install_test's note). QEMU writes the raw image
        # through, so the host reads what the guest has written.
        before = esp_bytes(args.target)
        k = verified_key(b, "the installed v1.0.3")
        if k != RELEASE_KEY:
            raise Fail(f"the installed v1.0.3 booted with key {k}, not {RELEASE_KEY}")
        fetch_all(b, port, args.arch, variants=True)
        out = sh(b, f"{UP}/cosmo-upgrade {UP}", "untrusted", 3)
        need(out, rf"^cosmo-upgrade: {UP}: manifest: unknown signing key {keyid} \(not in /etc/pkg/keys\); "
                  r"nothing written$", "the upgrade before its key is trusted")
        sh(b, f"cp {UP}/trust.pub /etc/pkg/keys/{args.pub_name} && sync", "trust")
        out = sh(b, f"{UP}/cosmo-upgrade /tmp/badsig", "badsig", 3)
        need(out, rf"^cosmo-upgrade: /tmp/badsig: manifest: bad signature \(key {re.escape(args.pub_name)}\); "
                  r"nothing written$", "a manifest with one byte changed")
        out = sh(b, f"{UP}/cosmo-upgrade /tmp/otherkey", "otherkey", 3)
        need(out, rf"^cosmo-upgrade: /tmp/otherkey: manifest: unknown signing key {otherid} \(not in "
                  r"/etc/pkg/keys\); nothing written$", "a manifest signed by another key")
        out = sh(b, f"{UP}/cosmo-upgrade /tmp/badtar", "badtar", 3)
        need(out, r"^cosmo-upgrade: /tmp/badtar: boot\.tar does not match the manifest \(SHA-512\); nothing written$",
             "an archive with one byte changed")
        out = sh(b, "cp /tmp/untrusting.pub /etc/pkg/keys/untrusting.pub && sync && "
                    f"{UP}/cosmo-upgrade /tmp/untrusting", "untrusting", 3)
        need(out, rf"^cosmo-upgrade: /tmp/untrusting: its loader does not trust key {RELEASE_KEY}, which signed the "
                  r"current set: an interruption after the loader switch would leave nothing that boots; nothing "
                  r"written$", "a set whose loader does not trust the current set's key")
        sh(b, "rm /etc/pkg/keys/untrusting.pub && sync", "untrust")
        out = sh(b, f"{UP}/cosmo-upgrade --check {UP}", "check")
        need(out, r"^cosmo-upgrade: check: it would replace the current set; ", "--check")
        rc = b.finish()
        if rc != install_test.EXIT_SUCCESS:
            raise Fail(f"refuse boot: QEMU exit code {rc}")
    except Exception:
        b.kill()
        raise
    after = esp_bytes(args.target)
    diff = [s for s in range(0, len(before), 512) if before[s:s + 512] != after[s:s + 512]]
    if diff:
        raise Fail(f"the refusals' boot changed {len(diff)} ESP sectors (first {diff[0] // 512}), expected none")
    results.append(f"refuse: v1.0.3 booted with {RELEASE_KEY}; refused before the build's key {keyid} was trusted, "
                   f"then a manifest with one byte changed (bad signature), one signed by {otherid}, an archive "
                   f"with one byte changed and a set whose loader does not trust {RELEASE_KEY}, each exit 3; the ESP byte for byte unchanged; PASS in {b.elapsed():.1f}s")


def stage_stop(args, results, port, step, base, new_loader, expect_key):
    b = boot_disk(step, args)
    try:
        b.wait_prompt("the first prompt")
        k = verified_key(b, f"the boot before --stop-after={step}")
        if k != expect_key:
            raise Fail(f"the boot before --stop-after={step}: key {k}, not {expect_key}")
        fetch_all(b, port, args.arch)
        out = sh(b, f"{UP}/cosmo-upgrade --stop-after={step} {UP}", "stop", 4)
        need(out, rf"^cosmo-upgrade: stopped after {step} \(--stop-after\); run it again to finish$",
             f"--stop-after={step}")
    finally:
        b.kill()   # the power cut: no shutdown, no sync beyond the tool's own flushes
    check_set(args.target, base, "::/cosmo", f"after --stop-after={step}")
    loader = esp_file(args.target, f"::/EFI/BOOT/{loader_name(args.arch)}")
    want = new_loader if step == "loader" else base["loader"]
    if loader != want:
        raise Fail(f"after --stop-after={step}: the loader is {'not ' if step == 'loader' else ''}the new one")
    which = "the new one" if step == "loader" else "v1.0.3's"
    results.append(f"{step}: booted with {k}; --stop-after={step} then killed: the ESP's set is v1.0.3's, the "
                   f"loader {which}, byte for byte")


def stage_upgrade(args, results, port, base, new, new_loader, nonce):
    b = boot_disk("upgrade", args)
    try:
        b.wait_prompt("the first prompt")
        k = verified_key(b, "the new loader with v1.0.3's set")
        if k != RELEASE_KEY:
            raise Fail(f"the new loader with v1.0.3's set booted with key {k}")
        fetch_all(b, port, args.arch)
        sh(b, f"echo {nonce} > /root/keep && sync", "keep")
        out = sh(b, "pkg update && pkg install hello=1.0 && hello", "hello")
        need(out, r"^hello, world \(hello 1\.0\)$", "pkg install hello=1.0 on v1.0.3")
        t0 = time.monotonic()
        out = sh(b, f"{UP}/cosmo-upgrade {UP}", "up")
        up_s = time.monotonic() - t0
        need(out, r"^cosmo-upgrade: loader switched: ", "the upgrade")
        need(out, r"^cosmo-upgrade: boot set switched: ", "the upgrade")
        m = need(out, r"^cosmo-upgrade: root: (\d+) files replaced, (\d+) added, (\d+) removed, (\d+) unchanged, "
                      r"(\d+) kept as changed here", "the upgrade's root pass")
        need(out, r"^cosmo-upgrade: upgraded; reboot to run it", "the upgrade")
        rc = b.finish()
        if rc != install_test.EXIT_SUCCESS:
            raise Fail(f"upgrade boot: QEMU exit code {rc}")
    except Exception:
        b.kill()
        raise
    check_set(args.target, new, "::/cosmo", "after the upgrade")
    check_set(args.target, base, "::/cosmo/prev", "after the upgrade")
    if esp_file(args.target, f"::/EFI/BOOT/{loader_name(args.arch)}") != new_loader:
        raise Fail("after the upgrade: the loader is not the new one")
    if esp_file(args.target, f"::/cosmo/{loader_name(args.arch)}") != new_loader:
        raise Fail("after the upgrade: \\cosmo's own loader is not the new one")
    if esp_file(args.target, f"::/cosmo/prev/{loader_name(args.arch)}") != base["loader"]:
        raise Fail("after the upgrade: \\cosmo\\prev's loader is not v1.0.3's")
    results.append(f"upgrade: the new loader booted v1.0.3's set ({k}); cosmo-upgrade took {up_s:.1f}s (root: "
                   f"{m.group(1)} replaced, {m.group(2)} added, {m.group(3)} removed, {m.group(5)} kept); the ESP holds "
                   f"the new set and loader, \\cosmo\\prev v1.0.3's set and loader")


def stage_new(args, results, keyid, nonce):
    b = boot_disk("new", args)
    try:
        b.wait_prompt("the first prompt")
        k = verified_key(b, "the upgraded system")
        if k != keyid:
            raise Fail(f"the upgraded system booted with key {k}, not the build's {keyid}")
        out = sh(b, "ls -1 /sbin/mkfs.cosmofs /sbin/cosmo-upgrade && grep -c 'mount -a' /etc/rc && cat /root/keep && "
                    "cat /etc/passwd /etc/pkg/repos.conf", "base")
        need(out, r"^/sbin/mkfs\.cosmofs$", "the new base system")
        need(out, r"^1$", "the new /etc/rc (unchanged since the install, so replaced)")
        need(out, rf"^{nonce}$", "the operator's /root/keep")
        need(out, r"^root:\*:0:0:root:/root:/bin/sh$", "the installer's /etc/passwd, kept")
        need(out, r"^/var/lib/pkg/repo$", "the installer's /etc/pkg/repos.conf, kept")
        out = sh(b, "cosmo-upgrade --status", "status")
        need(out, rf"^cosmo-upgrade: current: \S+, kernel\.elf \d+ bytes, boot\.tar \d+ bytes, key {keyid}$",
             "cosmo-upgrade --status")
        need(out, rf"^cosmo-upgrade: prev: kernel\.elf \d+ bytes, boot\.tar \d+ bytes, key {RELEASE_KEY}$",
             "cosmo-upgrade --status")
        out = sh(b, "pkg update && pkg upgrade && hello", "pkg")
        need(out, r"^pkg: 1 package upgraded$", "pkg upgrade from the new repository")
        need(out, r"^hello, world \(hello 1\.1\)$", "hello after pkg upgrade")
        out = sh(b, "cosmo-upgrade --rollback", "back")
        need(out, rf"^cosmo-upgrade: verified \\cosmo\\prev, signed by key {RELEASE_KEY} in /etc/pkg/keys$",
             "the rollback")
        need(out, r"^cosmo-upgrade: rolled back; reboot to run it$", "the rollback")
        rc = b.finish()
        if rc != install_test.EXIT_SUCCESS:
            raise Fail(f"new boot: QEMU exit code {rc}")
    except Exception:
        b.kill()
        raise
    results.append(f"new: this build booted with its key {keyid}; the new base system, /root/keep, the installer's "
                   f"/etc/passwd and repos.conf; pkg upgrade took hello 1.0 to 1.1; --rollback; PASS in {b.elapsed():.1f}s")


def stage_back(args, results, port, base, new, nonce):
    check_set(args.target, base, "::/cosmo", "after the rollback")
    for p in (f"::/EFI/BOOT/{loader_name(args.arch)}", f"::/cosmo/{loader_name(args.arch)}"):
        if esp_file(args.target, p) != base["loader"]:
            raise Fail(f"after the rollback: {p} is not v1.0.3's loader")
    check_set(args.target, new, "::/cosmo/prev", "after the rollback")
    b = boot_disk("back", args)
    try:
        b.wait_prompt("the first prompt")
        k = verified_key(b, "the rolled-back system")
        if k != RELEASE_KEY:
            raise Fail(f"the rolled-back system booted with key {k}, not {RELEASE_KEY}")
        out = b.run("ls /sbin/mkfs.cosmofs; ls /sbin/cosmo-upgrade; grep -c 'mount -a' /etc/rc; cat /root/keep; hello")
        need(out, r"mkfs\.cosmofs: No such file or directory|can't access '/sbin/mkfs\.cosmofs'",
             "v1.0.3's base system (no mkfs.cosmofs)")
        need(out, r"cosmo-upgrade: No such file or directory|can't access '/sbin/cosmo-upgrade'",
             "v1.0.3's base system (no cosmo-upgrade)")
        need(out, r"^hello, world \(hello 1\.1\)$", "the package pkg upgrade changed, after the rollback")
        need(out, r"^0$", "v1.0.3's /etc/rc")
        need(out, rf"^{nonce}$", "the operator's /root/keep after the rollback")
        # Forward again with the build's binary: v1.0.3's loader lacks the
        # build's key, \cosmo\prev's loader trusts v1.0.3's, so the loader
        # goes first.
        sh(b, f"mkdir -p {UP} && wget -q -O {UP}/cosmo-upgrade http://10.0.2.2:{port}/cosmoos-{args.arch}-cosmo-upgrade "
              f"&& chmod 755 {UP}/cosmo-upgrade", "fetchtool")
        out = sh(b, f"{UP}/cosmo-upgrade --rollback", "again")
        need(out, rf"^cosmo-upgrade: loader switched: \\EFI\\BOOT\\{loader_name(args.arch)} is \\cosmo\\prev's, "
                  r"which starts both sets$", "the second rollback's loader switch")
        need(out, r"^cosmo-upgrade: rolled back; reboot to run it$", "the second rollback")
        rc = b.finish()
        if rc != install_test.EXIT_SUCCESS:
            raise Fail(f"back boot: QEMU exit code {rc}")
    except Exception:
        b.kill()
        raise
    results.append(f"back: v1.0.3 booted again with {RELEASE_KEY}, its loader and base system back, /root/keep "
                   f"and hello 1.1 kept; the build's cosmo-upgrade rolled back again, loader first; PASS in "
                   f"{b.elapsed():.1f}s")


def stage_forward(args, results, keyid, base, new, new_loader):
    check_set(args.target, new, "::/cosmo", "after the second rollback")
    check_set(args.target, base, "::/cosmo/prev", "after the second rollback")
    for p in (f"::/EFI/BOOT/{loader_name(args.arch)}", f"::/cosmo/{loader_name(args.arch)}"):
        if esp_file(args.target, p) != new_loader:
            raise Fail(f"after the second rollback: {p} is not the new loader")
    if esp_file(args.target, f"::/cosmo/prev/{loader_name(args.arch)}") != base["loader"]:
        raise Fail("after the second rollback: \\cosmo\\prev's loader is not v1.0.3's")
    b = boot_disk("forward", args)
    try:
        b.wait_prompt("the first prompt")
        k = verified_key(b, "the system rolled forward")
        if k != keyid:
            raise Fail(f"the system rolled forward booted with key {k}, not the build's {keyid}")
        out = sh(b, "ls -1 /sbin/mkfs.cosmofs /sbin/cosmo-upgrade && hello", "fwd")
        need(out, r"^/sbin/cosmo-upgrade$", "the build's base system again")
        need(out, r"^hello, world \(hello 1\.1\)$", "hello 1.1 after rolling forward")
        rc = b.finish()
        if rc != install_test.EXIT_SUCCESS:
            raise Fail(f"forward boot: QEMU exit code {rc}")
    except Exception:
        b.kill()
        raise
    results.append(f"forward: this build booted again with {keyid}; the ESP holds the new set and loader, "
                   f"\\cosmo\\prev v1.0.3's; its base system and hello 1.1; PASS in {b.elapsed():.1f}s")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", required=True, help="the v1.0.3 live image")
    ap.add_argument("--set", required=True, help="this build's release set (make release-set)")
    ap.add_argument("--pub", required=True, help="the public key this build signs with")
    ap.add_argument("--arch", choices=("x86_64", "aarch64"), default=os.environ.get("QEMU_ARCH", "x86_64"))
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--timeout", type=float, default=240.0, help="per boot")
    args = ap.parse_args()
    shutil.rmtree(args.workdir, ignore_errors=True)
    os.makedirs(args.workdir)
    args.pub_name = os.path.basename(args.pub)
    results = []
    t0 = time.monotonic()
    try:
        rel = {n: os.path.join(args.set, f"cosmoos-{args.arch}-{n}") for n in SET + ("cosmo-upgrade",)}
        new = {n: open(rel[n], "rb").read() for n in SET}
        pub = bytes.fromhex(open(args.pub).read().strip()[:64])
        keyid = modsign.key_id(pub).hex()
        lname = loader_name(args.arch)
        import tarfile
        with tarfile.open(rel["boot.tar"]) as t:
            new_loader = t.extractfile(f"efi/{lname}").read()
        base = {n: mkmanifest.esp_get(args.base, f"::/cosmo/{n}") for n in SET}
        base["loader"] = mkmanifest.esp_get(args.base, f"::/EFI/BOOT/{lname}")
        # What the host serves: the set, the tool, the key, the refused variants.
        www = os.path.join(args.workdir, "www")
        os.makedirs(www)
        for n, path in rel.items():
            shutil.copyfile(path, os.path.join(www, os.path.basename(path)))
        shutil.copyfile(args.pub, os.path.join(www, "trust.pub"))
        for d, _ in VARIANTS:
            os.makedirs(os.path.join(www, d))
        bad = bytearray(new["manifest"])
        bad[100] ^= 1   # inside the payload: the signature no longer matches
        open(os.path.join(www, "badsig", "manifest"), "wb").write(bad)
        seed = os.urandom(32)
        otherid = modsign.key_id(modsign.secret_to_public(seed)).hex()
        open(os.path.join(www, "otherkey", "manifest"), "wb").write(
            mkmanifest.sign(seed, [new["kernel.elf"], new["boot.tar"], new["cmdline"]]))
        badtar = bytearray(new["boot.tar"])
        badtar[len(badtar) // 2] ^= 1
        open(os.path.join(www, "badtar", "boot.tar"), "wb").write(badtar)
        # A loader that trusts a throwaway key in place of the release key:
        # its ring's entry { id, pub } (kernel/keyring.h) overwritten, its
        # set signed by that key.
        useed = os.urandom(32)
        upub = modsign.secret_to_public(useed)
        rid = bytes.fromhex(RELEASE_KEY)
        if new_loader.count(rid) != 1:
            raise Fail(f"the new loader holds the release key id {new_loader.count(rid)} times, expected once")
        at = new_loader.index(rid)
        uloader = new_loader[:at] + modsign.key_id(upub) + upub + new_loader[at + 40:]
        with tarfile.open(rel["boot.tar"]) as t:
            off = t.getmember(f"efi/{lname}").offset_data
        utar = new["boot.tar"][:off] + uloader + new["boot.tar"][off + len(uloader):]
        open(os.path.join(www, "untrusting", "boot.tar"), "wb").write(utar)
        open(os.path.join(www, "untrusting", "manifest"), "wb").write(
            mkmanifest.sign(useed, [new["kernel.elf"], utar, new["cmdline"]]))
        open(os.path.join(www, "untrusting.pub"), "w").write(upub.hex() + "\n")
        srv, port = net_login_test.start_http(www)
        nonce = f"s7-kept-{os.getpid()}-{int(time.time())}"

        stage_install(args, results)
        stage_refuse(args, results, port, keyid, otherid)
        stage_stop(args, results, port, "written", base, new_loader, RELEASE_KEY)
        stage_stop(args, results, port, "loader", base, new_loader, RELEASE_KEY)
        stage_upgrade(args, results, port, base, new, new_loader, nonce)
        stage_new(args, results, keyid, nonce)
        stage_back(args, results, port, base, new, nonce)
        stage_forward(args, results, keyid, base, new, new_loader)
    except Fail as e:
        for r in results:
            print(f"test-upgrade: {r}")
        print(f"test-upgrade: FAIL: {e} (logs in {args.workdir})")
        return 1
    for r in results:
        print(f"test-upgrade: {r}")
    print(f"test-upgrade: PASS in {time.monotonic() - t0:.1f}s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
