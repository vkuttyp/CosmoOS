#!/usr/bin/env python3
"""Shell completeness S7's acceptance test: an installed v1.0.3 system is
upgraded to this build without reinstalling, and rolled back
(docs/userland/testing.md, "test-upgrade").

    upgrade_test.py --base cosmoos-ARCH.img --set DIR --pub KEY.pub --workdir DIR

--base is the published v1.0.3 live image (tools/fetch-release.py), --set
the release set to upgrade to: this build's (make release-set), or a
published one (make test-upgrade UPGRADE_SET_TAG=v1.0.4): cosmoos-ARCH-
kernel.elf, -boot.tar, -cmdline, -manifest, -cosmo-upgrade.cpk, SHA256SUMS
and SHA256SUMS.sig; --pub the public key that set is signed with. The host
serves them over HTTP under the release's names; the installed system
fetches them with wget and takes cosmo-upgrade and cosmo-verify out of the
signed package after `pkg install -n` has checked it, as
docs/guide/upgrading.md tells an operator to, and cosmo-upgrade reads the
set under those names. The refused variants are signed by a throwaway key
the refuse boot trusts for the while, so a published set needs no private
key here.

Boots, each of the installed disk alone after the first:

  install   v1.0.3's live image installs itself on a blank 256 MiB disk.
  refuse    v1.0.3 boots verified with the release key. Before the set's
            key is in /etc/pkg/keys (a build's own key; a published set's
            is there already) v1.0.3's pkg refuses the package and the
            upgrade is refused (unknown signing key); with it, the package
            checks and a package with one byte changed is refused; a
            kernel.elf with one byte changed (cosmo-verify and
            cosmo-upgrade: not SHA256SUMS's), a SHA256SUMS with one digest
            changed (bad signature) and a set without SHA256SUMS.sig are
            refused; and, under SHA256SUMS signed by the throwaway key, a
            manifest with one byte changed (bad signature), one signed by
            another key (unknown signing key), an archive with one byte
            changed (SHA-512) and a set whose loader trusts the throwaway
            key but not v1.0.3's (the release key's entry in its table
            replaced), each exit 3. Power off: the ESP is byte for byte
            what it was.
  written   --stop-after=written, then the machine is killed. The ESP's
            loader and set are still v1.0.3's, byte for byte.
  loader    v1.0.3 boots verified; --stop-after=loader, killed. The
            loader is the new one, the set still v1.0.3's.
  upgrade   The new loader boots v1.0.3's set verified (release key).
            pkg install hello=1.0; the full upgrade; power off. The ESP holds the new set,
            \\cosmo\\prev v1.0.3's set and loader.
  hold      This build boots verified with its own key, \\EFI\\BOOT's
            loader swapped by the host for one that does not trust the
            release key. A second cosmo-upgrade while one holds the lock,
            and a --rollback neither loader allows, are refused (exit 3);
            the ESP is unchanged, and the host puts the loader back.
  new       This build boots verified with its own key; the base system
            is the new one (mkfs.cosmofs, the new /etc/rc), the
            installer's /etc/passwd and a file of the operator's are kept;
            pkg update && pkg upgrade takes hello to 1.1; then
            cosmo-upgrade --rollback.
  back      v1.0.3 boots verified with the release key; its base system
            is back (no mkfs.cosmofs or cosmo-upgrade, its /etc/rc), the
            operator's file is still there, and so is hello 1.1. The
            build's cosmo-upgrade, fetched into /tmp, rolls back again:
            for a build's set v1.0.3's loader does not trust the build's
            key, so the loader (\\cosmo\\prev's) switches first; for a
            published set (UPGRADE_SET_TAG) it trusts the release key, so
            the set switches first, and a loader identical to v1.0.3's
            (v1.0.4's) needs no switch.
  forward   This build boots verified with its own key again; the ESP
            holds the new set and loader, \\cosmo\\prev v1.0.3's.

--base-tag v1.0.4 starts from the published v1.0.4 instead (make
test-upgrade UPGRADE_BASE_TAG=v1.0.4), which has its own cosmo-upgrade and
takes the guide's "From v1.0.4 and later" path: install; upgrade (the
installed system's /sbin/cosmo-upgrade on the fetched set, no package);
new (as above); back (v1.0.4 boots again and rolls forward with its own
cosmo-upgrade, the loader first only when v1.0.4's loader lacks the set's
key); forward (as above). The refusals, the interruptions and the hold
are v1.0.3-based only: they test v1.0.3's tools and the switches, which
do not depend on the base.

Exit 0 PASS, 1 FAIL.
"""

import argparse
import hashlib
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

RELEASE_KEY = "36ae7bb4947896ab"   # tools/keys/release-2026.pub: v1.0.3's and v1.0.4's signer
BASE = "v1.0.3"   # the published release the test starts from (--base-tag)
SET = ("kernel.elf", "boot.tar", "cmdline", "manifest")
# v1.0.3's cosmofs loses a large file being written now and then: a
# compressed record rewritten once its last page fills no longer
# compresses, and the plain fallback leaves it unreadable (fixed on this
# build: tools/cosmofs-record-fallback-probe.py). So v1.0.3 never writes
# the release to its disk here: each boot that needs it fetches it into
# /tmp, which is ramfs, as docs/guide/upgrading.md tells an operator to.
UP = "/tmp/up"
# Where the tools land when the signed package is unpacked in UP.
TOOL = f"{UP}/sbin/cosmo-upgrade"
VERIFY = f"{UP}/sbin/cosmo-verify"


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
        need(b.text(), rf"^\[ INFO\] boot: verified boot, key {RELEASE_KEY} ", f"the {BASE} live image")
        out = b.run("cosmo-install --list")
        m = need(out, rf"^(\S+)\s+-\s+{(install_test.TARGET_MIB << 20) // install_test.SS}\s+{install_test.SS}\s",
                 "cosmo-install --list")
        dev = m.group(1)
        out = sh(b, f"cosmo-install {dev}", "inst")
        need(out, rf"^cosmo-install: installed on {dev}: ", f"{BASE}'s cosmo-install")
        rc = b.finish()
        if rc != install_test.EXIT_SUCCESS:
            raise Fail(f"install boot: QEMU exit code {rc}")
    except Exception:
        b.kill()
        raise
    results.append(f"install: {BASE} (key {RELEASE_KEY}) installed itself on {dev}; PASS in {b.elapsed():.1f}s")


def fetch_all(b, port, arch, variants=False, check=True):
    """The set, its signed SHA256SUMS and the package into UP; the package
    checked by v1.0.3's pkg against /etc/pkg/keys (unless `check` is
    False: the refuse boot's first look, before the key is trusted) and
    unpacked, as the guide does it."""
    url = f"http://10.0.2.2:{port}"
    files = " ".join(f"cosmoos-{arch}-{n}" for n in SET) + f" {cpk_name(arch)} SHA256SUMS SHA256SUMS.sig trust.pub"
    sh(b, f"mkdir -p {UP} && cd {UP} && for f in {files}; do wget -q {url}/$f || exit 1; done && cd /", "fetch")
    if check:
        out = sh(b, f"cd {UP} && pkg install -n ./{cpk_name(arch)}; echo pkgn=$?; cd /", "pkgn")
        need(out, r"^pkgn=0$", "v1.0.3's pkg checking the package")
        # 3 files from v1.0.6 (cosmo-fetch too), 2 in a published v1.0.4 or v1.0.5 set
        need(out, r"^pkg: installing cosmo-upgrade-\S+ \([23] files\)$", "v1.0.3's pkg checking the package")
    unpack(b, arch)
    if not variants:
        return
    # The refused variants: links to the good files, the rest replaced.
    for d, bad in VARIANTS:
        links = " && ".join(f"ln -s {UP}/cosmoos-{arch}-{f} /tmp/{d}/{f}" for f in SET if f not in bad)
        gets = " && ".join(f"wget -q -O /tmp/{d}/{f} {url}/{d}/{f}" for f in bad + SUMS_FILES[d])
        sh(b, f"mkdir -p /tmp/{d} && {links} && {gets}", f"get{d}")
    sh(b, f"wget -q -O /tmp/untrusting.pub {url}/untrusting.pub && wget -q -O /tmp/variants.pub {url}/variants.pub "
          f"&& wget -q -O /tmp/bad.cpk {url}/bad.cpk", "getpubs")


def cpk_name(arch):
    return f"cosmoos-{arch}-cosmo-upgrade.cpk"


def unpack(b, arch):
    sh(b, f"cd {UP} && tar -xf {cpk_name(arch)} sbin/cosmo-upgrade sbin/cosmo-verify && cd /", "unpack")


# Each refused set: the files it replaces, and how it carries its sums.
VARIANTS = (("badasset", ("kernel.elf",)), ("badsums", ()), ("nosig", ()),
            ("badsig", ("manifest",)), ("otherkey", ("manifest",)), ("badtar", ("boot.tar",)),
            ("untrusting", ("boot.tar", "manifest")))
# badasset keeps the set's own signed SHA256SUMS; badsums changes one of
# its digests under the set's signature; nosig has no signature; the rest
# have SHA256SUMS over their own files, signed by the throwaway variant key.
SUMS_FILES = {d: (("SHA256SUMS",) if d == "nosig" else ("SHA256SUMS", "SHA256SUMS.sig")) for d, _ in VARIANTS}


def stage_refuse(args, results, port, keyid, otherid, vkeyid):
    b = boot_disk("refuse", args)
    arch = args.arch
    try:
        b.wait_prompt("the first prompt")
        # The ESP once the firmware is done with it: x86 firmware with no
        # variable store writes \\NvVars there at every boot, before the
        # kernel runs (install_test's note). QEMU writes the raw image
        # through, so the host reads what the guest has written.
        before = esp_bytes(args.target)
        k = verified_key(b, "the installed v1.0.3")
        if k != RELEASE_KEY:
            raise Fail(f"the installed v1.0.3 booted with key {k}, not {RELEASE_KEY}")
        fetch_all(b, port, arch, variants=True, check=False)
        trusted = keyid == RELEASE_KEY   # a published set: v1.0.3 shipped its key
        if not trusted:
            out = sh(b, f"cd {UP} && pkg install -n ./{cpk_name(arch)}; echo pkgn=$?; cd /", "pkguntrusted")
            need(out, r"^pkgn=3$", "v1.0.3's pkg before the set's key is trusted")
            need(out, rf"^pkg: \./{re.escape(cpk_name(arch))}: unknown signing key {keyid} \(not in /etc/pkg/keys\)$",
                 "v1.0.3's pkg before the set's key is trusted")
            out = sh(b, f"{TOOL} {UP}", "untrusted", 3)
            need(out, rf"^cosmo-upgrade: {UP}: SHA256SUMS: unknown signing key {keyid} \(not in /etc/pkg/keys\); "
                      r"nothing written$", "the upgrade before its key is trusted")
            sh(b, f"cp {UP}/trust.pub /etc/pkg/keys/{args.pub_name} && sync", "trust")
        out = sh(b, f"cd {UP} && pkg install -n ./{cpk_name(arch)}; echo pkgn=$?; cd /", "pkgtrusted")
        need(out, r"^pkgn=0$", "v1.0.3's pkg checking the package")
        out = sh(b, "pkg install -n /tmp/bad.cpk; echo pkgn=$?", "pkgbad")
        need(out, r"^pkgn=3$", "v1.0.3's pkg and a package with one byte changed")
        need(out, r"^pkg: /tmp/bad\.cpk: bad signature \(key \S+\)$", "v1.0.3's pkg and a package with one byte changed")
        out = sh(b, f"{VERIFY} {UP}/cosmoos-{arch}-kernel.elf {UP}/SHA256SUMS {UP}/SHA256SUMS.sig", "verifyok")
        need(out, rf"^cosmo-verify: {UP}/cosmoos-{arch}-kernel\.elf: OK \(SHA-256 [0-9a-f]{{16}}\.\.\., in SHA256SUMS "
                  rf"signed by key {keyid}, ", "cosmo-verify of a release file")
        out = sh(b, f"cp /tmp/badasset/kernel.elf /tmp/cosmoos-{arch}-kernel.elf && "
                    f"{VERIFY} /tmp/cosmoos-{arch}-kernel.elf {UP}/SHA256SUMS {UP}/SHA256SUMS.sig", "verifybad", 3)
        need(out, rf"^cosmo-verify: /tmp/cosmoos-{arch}-kernel\.elf does not match SHA256SUMS; do not use it$",
             "cosmo-verify of a kernel.elf with one byte changed")
        out = sh(b, f"{TOOL} /tmp/badasset", "badasset", 3)
        need(out, rf"^cosmo-upgrade: /tmp/badasset: cosmoos-{arch}-kernel\.elf does not match SHA256SUMS; nothing "
                  r"written$", "a kernel.elf with one byte changed")
        out = sh(b, f"{TOOL} /tmp/badsums", "badsums", 3)
        need(out, r"^cosmo-upgrade: /tmp/badsums: SHA256SUMS: bad signature \(key \S+\); nothing written$",
             "a SHA256SUMS with one digest changed")
        out = sh(b, f"{TOOL} /tmp/nosig", "nosig", 3)
        need(out, r"^cosmo-upgrade: /tmp/nosig: /tmp/nosig/SHA256SUMS\.sig: No such file or directory \(a release is "
                  r"checked against its signed SHA256SUMS\); nothing written$", "a set without SHA256SUMS.sig")
        # The rest carry SHA256SUMS signed by the throwaway variant key.
        sh(b, "cp /tmp/variants.pub /etc/pkg/keys/variants.pub && sync", "trustvariants")
        out = sh(b, f"{TOOL} /tmp/badsig", "badsig", 3)
        need(out, rf"^cosmo-upgrade: /tmp/badsig: manifest: bad signature \(key \S+\); nothing written$",
             "a manifest with one byte changed")
        out = sh(b, f"{TOOL} /tmp/otherkey", "otherkey", 3)
        need(out, rf"^cosmo-upgrade: /tmp/otherkey: manifest: unknown signing key {otherid} \(not in "
                  r"/etc/pkg/keys\); nothing written$", "a manifest signed by another key")
        out = sh(b, f"{TOOL} /tmp/badtar", "badtar", 3)
        need(out, r"^cosmo-upgrade: /tmp/badtar: boot\.tar does not match the manifest \(SHA-512\); nothing written$",
             "an archive with one byte changed")
        out = sh(b, "cp /tmp/untrusting.pub /etc/pkg/keys/untrusting.pub && sync && "
                    f"{TOOL} /tmp/untrusting", "untrusting", 3)
        need(out, rf"^cosmo-upgrade: /tmp/untrusting: its loader does not trust key {RELEASE_KEY}, which signed the "
                  r"current set: an interruption after the loader switch would leave nothing that boots; nothing "
                  r"written$", "a set whose loader does not trust the current set's key")
        sh(b, "rm /etc/pkg/keys/untrusting.pub /etc/pkg/keys/variants.pub && sync", "untrust")
        out = sh(b, f"{TOOL} --check {UP}", "check")
        need(out, rf"^cosmo-upgrade: verified SHA256SUMS, signed by key {keyid} in /etc/pkg/keys, and the set's four "
                  r"files against it$", "--check")
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
    first = "v1.0.3 already trusted the set's key" if keyid == RELEASE_KEY else \
        f"v1.0.3's pkg and cosmo-upgrade refused before the set's key {keyid} was trusted"
    results.append(f"refuse: v1.0.3 booted with {RELEASE_KEY}; {first}; pkg -n checked the package and refused one "
                   f"with one byte changed; a kernel.elf with one byte changed (cosmo-verify and cosmo-upgrade), a "
                   f"SHA256SUMS with one digest changed and a set without SHA256SUMS.sig refused; under SHA256SUMS "
                   f"signed by {vkeyid}, a manifest with one byte changed (bad signature), one signed by {otherid}, an "
                   f"archive with one byte changed and a set whose loader does not trust {RELEASE_KEY}, each exit 3; "
                   f"the ESP byte for byte unchanged; PASS in {b.elapsed():.1f}s")


def stage_stop(args, results, port, step, base, new_loader, expect_key):
    b = boot_disk(step, args)
    try:
        b.wait_prompt("the first prompt")
        k = verified_key(b, f"the boot before --stop-after={step}")
        if k != expect_key:
            raise Fail(f"the boot before --stop-after={step}: key {k}, not {expect_key}")
        fetch_all(b, port, args.arch)
        out = sh(b, f"{TOOL} --stop-after={step} {UP}", "stop", 4)
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
        out = sh(b, f"{TOOL} {UP}", "up")
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


def stage_hold(args, results, keyid, new_loader, uloader):
    """Two refusals of the upgraded system's own cosmo-upgrade, ESP
    untouched: a second run while one holds the lock, and a rollback when
    neither loader trusts the other set's key. For the second, the host
    puts in \\EFI\\BOOT the build's loader with the release key's entry
    replaced: it starts the current set and not \\cosmo\\prev (v1.0.3's,
    release-signed), and \\cosmo\\prev's loader (v1.0.3's) does not trust
    the build's key. A published set is signed by the release key too, so
    that state cannot arise and only the lock is checked."""
    lpath = f"::/EFI/BOOT/{loader_name(args.arch)}"
    swap = keyid != RELEASE_KEY
    if swap:
        mkmanifest.esp_put(args.target, lpath, uloader)
    b = boot_disk("hold", args)
    try:
        b.wait_prompt("the first prompt")
        before = esp_bytes(args.target)
        k = verified_key(b, "the upgraded system")
        if k != keyid:
            raise Fail(f"the upgraded system booted with key {k}, not {keyid}")
        # The first run takes the lock, then reads the key ring and the
        # set. A FIFO in the ring (lk.pub) tells the test it holds the lock:
        # writing to it returns once the run has opened it, which it does
        # only after binding the lock. It then blocks on a second FIFO in
        # place of its kernel.elf, lock held, while a second run is refused.
        # (cosmofs has no FIFOs: the ring's entry is a link to one in /tmp.)
        out = sh(b, "mkdir -p /tmp/lk && mkfifo /tmp/lk/kernel.elf /tmp/lk/key && ln -s /tmp/lk/key /etc/pkg/keys/lk.pub && "
                    "{ cosmo-upgrade /tmp/lk > /tmp/lk.out 2>&1 & } && echo x > /tmp/lk/key && "
                    # gone before the second run, which must not wait on it if it got the lock
                    "rm /etc/pkg/keys/lk.pub && cosmo-upgrade /tmp/none; echo second=$?", "lock")
        need(out, r"^cosmo-upgrade: another cosmo-upgrade is running; nothing written$", "a second run")
        need(out, r"^second=3$", "a second run")
        out = sh(b, "echo x > /tmp/lk/kernel.elf; sleep 1; cat /tmp/lk.out", "lockdone")
        need(out, r"^cosmo-upgrade: /tmp/lk/kernel\.elf: Invalid argument$", "the first run, its FIFO opened")
        what = "the lock"
        if swap:
            out = sh(b, "cosmo-upgrade --rollback", "norollback", 3)
            need(out, rf"^cosmo-upgrade: neither \\EFI\\BOOT\\{loader_name(args.arch)} trusts key {RELEASE_KEY}, which "
                      rf"signed \\cosmo\\prev, nor \\cosmo\\prev's loader key {keyid}, which signed the current set: "
                      r"an interruption between the switches would leave nothing that boots; nothing written$",
                 "a rollback neither loader allows")
            what += " and a rollback neither loader allows"
        rc = b.finish()
        if rc != install_test.EXIT_SUCCESS:
            raise Fail(f"hold boot: QEMU exit code {rc}")
    except Exception:
        b.kill()
        raise
    after = esp_bytes(args.target)
    diff = [s for s in range(0, len(before), 512) if before[s:s + 512] != after[s:s + 512]]
    if diff:
        raise Fail(f"the hold boot changed {len(diff)} ESP sectors (first {diff[0] // 512}), expected none")
    if swap:
        mkmanifest.esp_put(args.target, lpath, new_loader)
        if esp_file(args.target, lpath) != new_loader:
            raise Fail("the new loader could not be put back")
    results.append(f"hold: this build booted with {keyid}; {what} refused, exit 3; the ESP byte for byte unchanged; "
                   f"PASS in {b.elapsed():.1f}s")


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


def stage_back(args, results, port, keyid, base, new, new_loader, nonce):
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
        # Forward again with the set's binary. A build's set: v1.0.3's
        # loader lacks the build's key, \cosmo\prev's loader trusts
        # v1.0.3's, so the loader goes first. A published set: v1.0.3's
        # loader trusts the release key, so the set goes first, and the
        # loader is switched after it only if the set's differs (v1.0.4's
        # is v1.0.3's, byte for byte).
        cpk = cpk_name(args.arch)
        out = sh(b, f"mkdir -p {UP} && cd {UP} && wget -q http://10.0.2.2:{port}/{cpk} && pkg install -n ./{cpk}; "
                    "echo pkgn=$?; cd /", "fetchtool")
        need(out, r"^pkgn=0$", "v1.0.3's pkg checking the package after the rollback")
        unpack(b, args.arch)
        out = sh(b, f"{TOOL} --rollback", "again")
        lname = loader_name(args.arch)
        prev_first = (rf"^cosmo-upgrade: loader switched: \\EFI\\BOOT\\{lname} is \\cosmo\\prev's, "
                      r"which starts both sets$")
        set_own = rf"^cosmo-upgrade: loader switched: \\EFI\\BOOT\\{lname} is the set's own$"
        if keyid != RELEASE_KEY:
            need(out, prev_first, "the second rollback's loader switch")
            order = "loader first"
        else:
            if re.search(prev_first, out, re.M):
                raise Fail("the second rollback switched the loader first, but v1.0.3's loader trusts the release key")
            if new_loader != base["loader"]:
                need(out, set_own, "the second rollback's loader switch, after the set")
                order = "set first, then the loader"
            elif re.search(set_own, out, re.M):
                raise Fail("the second rollback switched to a loader identical to v1.0.3's")
            else:
                order = "set first, the loader v1.0.3's already"
        need(out, r"^cosmo-upgrade: rolled back; reboot to run it$", "the second rollback")
        rc = b.finish()
        if rc != install_test.EXIT_SUCCESS:
            raise Fail(f"back boot: QEMU exit code {rc}")
    except Exception:
        b.kill()
        raise
    results.append(f"back: v1.0.3 booted again with {RELEASE_KEY}, its loader and base system back, /root/keep "
                   f"and hello 1.1 kept; the set's cosmo-upgrade rolled back again, {order}; PASS in "
                   f"{b.elapsed():.1f}s")


def stage_forward(args, results, keyid, base, new, new_loader):
    check_set(args.target, new, "::/cosmo", "after the second rollback")
    check_set(args.target, base, "::/cosmo/prev", "after the second rollback")
    for p in (f"::/EFI/BOOT/{loader_name(args.arch)}", f"::/cosmo/{loader_name(args.arch)}"):
        if esp_file(args.target, p) != new_loader:
            raise Fail(f"after the second rollback: {p} is not the new loader")
    if esp_file(args.target, f"::/cosmo/prev/{loader_name(args.arch)}") != base["loader"]:
        raise Fail(f"after the second rollback: \\cosmo\\prev's loader is not {BASE}'s")
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
                   f"\\cosmo\\prev {BASE}'s; its base system and hello 1.1; PASS in {b.elapsed():.1f}s")


class Done(Exception):
    """The shorter flow from a base with its own cosmo-upgrade has ended."""


def fetch_set(b, port, arch):
    """The set and its signed SHA256SUMS into UP, as the guide's "From
    v1.0.4 and later" does (no package: the system has cosmo-upgrade)."""
    url = f"http://10.0.2.2:{port}"
    files = " ".join(f"cosmoos-{arch}-{n}" for n in SET) + " SHA256SUMS SHA256SUMS.sig"
    sh(b, f"mkdir -p {UP} && cd {UP} && for f in {files}; do wget -q {url}/$f || exit 1; done && cd /", "fetch")


def stage_upgrade_own(args, results, port, keyid, base, new, new_loader, nonce):
    """The installed base upgrades itself with its own /sbin/cosmo-upgrade."""
    b = boot_disk("upgrade", args)
    try:
        b.wait_prompt("the first prompt")
        k = verified_key(b, f"the installed {BASE}")
        if k != RELEASE_KEY:
            raise Fail(f"the installed {BASE} booted with key {k}, not {RELEASE_KEY}")
        fetch_set(b, port, args.arch)
        trusted = keyid == RELEASE_KEY   # a published set: the base shipped its key
        if not trusted:
            # A build's set, signed by the build's key: refused until the
            # operator trusts that key, then accepted.
            out = sh(b, f"/sbin/cosmo-upgrade {UP}", "untrusted", 3)
            need(out, rf"^cosmo-upgrade: {UP}: SHA256SUMS: unknown signing key {keyid} \(not in /etc/pkg/keys\); "
                      r"nothing written$", "the base's cosmo-upgrade before the set's key is trusted")
            sh(b, f"wget -q -O /etc/pkg/keys/{args.pub_name} http://10.0.2.2:{port}/trust.pub && sync", "trust")
        sh(b, f"echo {nonce} > /root/keep && sync", "keep")
        out = sh(b, "pkg update && pkg install hello=1.0 && hello", "hello")
        need(out, r"^hello, world \(hello 1\.0\)$", f"pkg install hello=1.0 on {BASE}")
        t0 = time.monotonic()
        out = sh(b, f"/sbin/cosmo-upgrade {UP}", "up")
        up_s = time.monotonic() - t0
        need(out, r"^cosmo-upgrade: boot set switched: ", "the upgrade")
        if new_loader != base["loader"]:
            need(out, r"^cosmo-upgrade: loader switched: ", "the upgrade (the set's loader differs)")
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
    if esp_file(args.target, f"::/cosmo/prev/{loader_name(args.arch)}") != base["loader"]:
        raise Fail(f"after the upgrade: \\cosmo\\prev's loader is not {BASE}'s")
    first = "" if trusted else f"refused the set until its key {keyid} was trusted, then "
    results.append(f"upgrade: {BASE} booted with {k}; its own cosmo-upgrade {first}took {up_s:.1f}s (root: "
                   f"{m.group(1)} replaced, {m.group(2)} added, {m.group(3)} removed, {m.group(5)} kept); the ESP holds "
                   f"the new set and loader, \\cosmo\\prev {BASE}'s set and loader")


def stage_back_own(args, results, keyid, base, new, new_loader, nonce):
    """The rolled-back base boots and rolls forward with its own cosmo-upgrade."""
    check_set(args.target, base, "::/cosmo", "after the rollback")
    for p in (f"::/EFI/BOOT/{loader_name(args.arch)}", f"::/cosmo/{loader_name(args.arch)}"):
        if esp_file(args.target, p) != base["loader"]:
            raise Fail(f"after the rollback: {p} is not {BASE}'s loader")
    check_set(args.target, new, "::/cosmo/prev", "after the rollback")
    b = boot_disk("back", args)
    try:
        b.wait_prompt("the first prompt")
        k = verified_key(b, "the rolled-back system")
        if k != RELEASE_KEY:
            raise Fail(f"the rolled-back system booted with key {k}, not {RELEASE_KEY}")
        out = sh(b, "cat /root/keep && hello", "kept")
        need(out, r"^hello, world \(hello 1\.1\)$", "the package pkg upgrade changed, after the rollback")
        need(out, rf"^{nonce}$", "the operator's /root/keep after the rollback")
        out = sh(b, "/sbin/cosmo-upgrade --rollback", "again")
        lname = loader_name(args.arch)
        prev_first = (rf"^cosmo-upgrade: loader switched: \\EFI\\BOOT\\{lname} is \\cosmo\\prev's, "
                      r"which starts both sets$")
        set_own = rf"^cosmo-upgrade: loader switched: \\EFI\\BOOT\\{lname} is the set's own$"
        if keyid != RELEASE_KEY:
            need(out, prev_first, "the second rollback's loader switch")   # the base's loader lacks the key
            order = "loader first"
        else:
            if re.search(prev_first, out, re.M):
                raise Fail(f"the second rollback switched the loader first, but {BASE}'s loader trusts the release key")
            need(out, set_own, "the second rollback's loader switch, after the set")
            order = "set first, then the loader"
        need(out, r"^cosmo-upgrade: rolled back; reboot to run it$", "the second rollback")
        rc = b.finish()
        if rc != install_test.EXIT_SUCCESS:
            raise Fail(f"back boot: QEMU exit code {rc}")
    except Exception:
        b.kill()
        raise
    results.append(f"back: {BASE} booted again with {RELEASE_KEY}, its loader back, /root/keep and hello 1.1 kept; "
                   f"its own cosmo-upgrade rolled forward again, {order}; PASS in {b.elapsed():.1f}s")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", required=True, help="the published base release's live image")
    ap.add_argument("--base-tag", choices=("v1.0.3", "v1.0.4"), default="v1.0.3")
    ap.add_argument("--set", required=True, help="this build's release set (make release-set)")
    ap.add_argument("--pub", required=True, help="the public key this build signs with")
    ap.add_argument("--arch", choices=("x86_64", "aarch64"), default=os.environ.get("QEMU_ARCH", "x86_64"))
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--timeout", type=float, default=240.0, help="per boot")
    args = ap.parse_args()
    global BASE
    BASE = args.base_tag
    shutil.rmtree(args.workdir, ignore_errors=True)
    os.makedirs(args.workdir)
    args.pub_name = os.path.basename(args.pub)
    results = []
    t0 = time.monotonic()
    try:
        rel = {n: os.path.join(args.set, f"cosmoos-{args.arch}-{n}") for n in SET + ("cosmo-upgrade.cpk",)}
        rel["SHA256SUMS"] = os.path.join(args.set, "SHA256SUMS")
        rel["SHA256SUMS.sig"] = os.path.join(args.set, "SHA256SUMS.sig")
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
        sums = open(rel["SHA256SUMS"], "rb").read()
        if modsign.verify_detached([pub], sums, open(rel["SHA256SUMS.sig"], "rb").read()):
            raise Fail(f"{rel['SHA256SUMS']} is not signed by {args.pub}")
        badk = bytearray(new["kernel.elf"])
        badk[len(badk) // 3] ^= 1
        open(os.path.join(www, "badasset", "kernel.elf"), "wb").write(badk)
        for d in ("badasset", "badsums", "nosig"):
            shutil.copyfile(rel["SHA256SUMS"], os.path.join(www, d, "SHA256SUMS"))
        shutil.copyfile(rel["SHA256SUMS.sig"], os.path.join(www, "badasset", "SHA256SUMS.sig"))
        line = f"  cosmoos-{args.arch}-kernel.elf\n".encode()
        at = sums.index(line) - 64
        open(os.path.join(www, "badsums", "SHA256SUMS"), "wb").write(
            sums[:at] + bytes(reversed(sums[at:at + 64])) + sums[at + 64:])
        shutil.copyfile(rel["SHA256SUMS.sig"], os.path.join(www, "badsums", "SHA256SUMS.sig"))
        cpk = open(rel["cosmo-upgrade.cpk"], "rb").read()
        open(os.path.join(www, "bad.cpk"), "wb").write(cpk[:600] + bytes([cpk[600] ^ 1]) + cpk[601:])
        vseed = os.urandom(32)
        vpub = modsign.secret_to_public(vseed)
        vkeyid = modsign.key_id(vpub).hex()
        open(os.path.join(www, "variants.pub"), "w").write(vpub.hex() + "\n")
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
        # Each variant signed by the throwaway key: SHA256SUMS over its own files.
        for d, bad_files in VARIANTS:
            if d in ("badasset", "badsums", "nosig"):
                continue
            lines = []
            for n in SET:
                data = open(os.path.join(www, d, n), "rb").read() if n in bad_files else new[n]
                lines.append(f"{hashlib.sha256(data).hexdigest()}  cosmoos-{args.arch}-{n}\n")
            text = "".join(lines).encode()
            open(os.path.join(www, d, "SHA256SUMS"), "wb").write(text)
            open(os.path.join(www, d, "SHA256SUMS.sig"), "wb").write(modsign.detached(vseed, text))
        srv, port = net_login_test.start_http(www)
        nonce = f"s7-kept-{os.getpid()}-{int(time.time())}"

        stage_install(args, results)
        if BASE != "v1.0.3":
            if new_loader == base["loader"]:
                raise Fail(f"the set's loader is {BASE}'s byte for byte: nothing for this test to switch")
            stage_upgrade_own(args, results, port, keyid, base, new, new_loader, nonce)
            stage_new(args, results, keyid, nonce)
            stage_back_own(args, results, keyid, base, new, new_loader, nonce)
            stage_forward(args, results, keyid, base, new, new_loader)
            raise Done()
        stage_refuse(args, results, port, keyid, otherid, vkeyid)
        stage_stop(args, results, port, "written", base, new_loader, RELEASE_KEY)
        stage_stop(args, results, port, "loader", base, new_loader, RELEASE_KEY)
        stage_upgrade(args, results, port, base, new, new_loader, nonce)
        stage_hold(args, results, keyid, new_loader, uloader)
        stage_new(args, results, keyid, nonce)
        stage_back(args, results, port, keyid, base, new, new_loader, nonce)
        stage_forward(args, results, keyid, base, new, new_loader)
    except Done:
        pass
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
