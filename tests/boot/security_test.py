#!/usr/bin/env python3
"""Roadmap M5's acceptance test, the security baseline
(docs/boot/testing.md, "test-security").

    security_test.py --image cosmoos.img --noverify-loader BOOTX64.EFI \\
        --key signing.key --workdir DIR [--stages verify]

Stage `verify` (verified boot, docs/boot/design.md):
  signed      the image as built: the loader checks the manifest's
              signature and the three files, the kernel logs "verified
              boot" with the key id and sysctl kernel.verified_boot says
              "yes <id>";
  kernel.elf, boot.tar, cmdline
              one byte of that file changed in a copy of the image: the
              loader refuses it with a line naming the file and no kernel
              output follows;
  other-key   the manifest signed again by a key the loader does not hold:
              refused, naming the key;
  forged-id   that manifest with the trusted key's id written over its own:
              the signature does not verify;
  no-manifest the manifest deleted: refused;
  unverified  the loader built with VERIFY_BOOT=0 put in the image with the
              tampered kernel: it boots, the banner says UNVERIFIED BOOT
              and the sysctl says "no".

Stage `aslr` (user-space ASLR and W^X, docs/kernel/process/design.md):
one boot of the signed image runs /boot/tests/security/aslr-static,
aslr-pie (static musl, ET_EXEC and static PIE) and aslr-native ten times
each. Every run's stack, stack pointer, mmap and (musl) break must differ
from every other run's, the bits that vary must lie inside the range the
design randomises and nearly all of them must vary; the PIE's load address
varies the same way and the ET_EXEC and native images stay at their link
address. Every run also reports code on its stack faulting with SIGSEGV
and a writable-and-executable mmap or mprotect refused with EINVAL.

The image's files are read and written through mtools at partition 1's
offset (scripts/mkmanifest.py). Every boot's serial log is kept in the
work directory. Exit 0 PASS, 1 FAIL.
"""

import argparse
import os
import re
import shutil
import struct
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import mkmanifest  # noqa: E402
import modsign  # noqa: E402
from install_test import EXIT_SUCCESS, Boot, Fail, need  # noqa: E402

KERNEL_OUTPUT = re.compile(r"^(CosmoOS kernel |\[ (INFO|WARN|ERROR|DEBUG)\] )", re.M)
# After the refusal: die() stalls 3 s and returns to the firmware; nothing
# the kernel would print may appear in this long after it.
QUIET_S = 8.0


def variant(args, name, src=None):
    """A copy of the image in a directory of its own (qemu-run.sh puts its
    scratch disks beside the image)."""
    d = os.path.join(args.workdir, name)
    os.makedirs(d, exist_ok=True)
    image = os.path.join(d, "cosmoos.img")
    shutil.copyfile(src or args.image, image)
    return image


def strtab_byte(elf):
    """Offset of a byte of the kernel's .strtab: a section no PT_LOAD
    covers, so changing it changes the file the manifest hashes and
    nothing the loader loads -- the VERIFY_BOOT=0 loader must still boot
    the result."""
    shoff, = struct.unpack_from("<Q", elf, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", elf, 0x3A)

    def sh(i):
        return struct.unpack_from("<IIQQQQ", elf, shoff + i * shentsize)

    names = sh(shstrndx)[4]
    for i in range(shnum):
        name, _, flags, _, off, size = sh(i)
        if elf[names + name:elf.index(b"\0", names + name)] == b".strtab" and not flags & 2 and size > 2:
            at = off + size // 2
            while elf[at] == 0:
                at += 1
            return at
    raise Fail("kernel.elf has no .strtab to change")


def flip(data, at):
    data = bytearray(data)
    data[at] ^= 0x20 if 0x40 < data[at] < 0x7B else 0x01
    return bytes(data)


def tampered(args, which):
    """The image with one byte of \\cosmo\\<which> changed."""
    image = variant(args, f"tampered-{which}")
    path = f"::/cosmo/{which}"
    data = mkmanifest.esp_get(image, path)
    if which == "kernel.elf":
        at = strtab_byte(data)
    elif which == "boot.tar":
        at = len(data) - 1      # inside the archive's end-of-archive blocks
    else:
        at = len(data) - 1      # the command line's final newline
    mkmanifest.esp_put(image, path, flip(data, at))
    return image


def boot_signed(args, results):
    image = variant(args, "signed")
    b = Boot("signed", image, args.workdir, {}, args.timeout)
    try:
        b.wait_prompt("the first prompt")
        log = b.text()
        m = need(log, r"^verified boot: \\cosmo\\manifest signed by key ([0-9a-f]{16}) \((\S+)\)$", "the signed boot")
        kid = m.group(1)
        for f in ("kernel.elf", "boot.tar", "cmdline"):
            need(log, rf"^verified boot: \\cosmo\\{re.escape(f)} matches the manifest$", "the signed boot")
        need(log, rf"^Verified boot, key {kid} \(", "the signed boot's banner")
        need(log, rf"^\[ INFO\] boot: verified boot, key {kid} \(", "the signed boot")
        out = b.run("sysctl kernel.verified_boot")
        need(out, rf"^kernel\.verified_boot = yes {kid}$", "sysctl kernel.verified_boot")
        rc = b.finish()
        if rc != EXIT_SUCCESS:
            raise Fail(f"signed boot: QEMU exit code {rc}")
    except Exception:
        b.kill()
        raise
    if "UNVERIFIED" in b.text():
        raise Fail("the signed boot says UNVERIFIED")
    results.append(f"verify/signed: PASS in {b.elapsed():.1f}s (key {kid} {m.group(2)}, sysctl yes {kid})")
    return kid


FATAL = re.compile(r"^cosmoboot: FATAL: verified boot: refusing to boot \(status 0x800000000000001a\)\n", re.M)


def boot_refused(args, results, name, image, pattern):
    """The loader must print `pattern`, then its FATAL line, and the kernel nothing."""
    b = Boot(name, image, args.workdir, {}, args.timeout)
    try:
        # The whole FATAL line, newline included: the serial line delivers
        # it in pieces, and a check on a half-arrived line fails a refusal
        # that happened (CI run 38210009760).
        while time.monotonic() < b.deadline and not FATAL.search(b.text()):
            if KERNEL_OUTPUT.search(b.text()):
                raise Fail(f"{name}: the kernel ran")
            time.sleep(0.2)
        t = time.monotonic()
        while time.monotonic() - t < QUIET_S:
            time.sleep(0.2)
        log = b.text()
        need(log, pattern, f"the {name} boot")
        need(log, FATAL.pattern, f"the {name} boot")
        if KERNEL_OUTPUT.search(log):
            raise Fail(f"{name}: kernel output after the refusal")
        if re.search(r"^jumping to kernel entry", log, re.M):
            raise Fail(f"{name}: the loader jumped to the kernel")
    finally:
        b.kill()
    results.append(f"verify/{name}: PASS (refused: {need(log, pattern, name).group(0).strip()})")


def boot_unverified(args, results, image):
    efi = mkmanifest.esp_get(image, "::/EFI/BOOT/" + os.path.basename(args.noverify_loader))
    if not efi:
        raise Fail("unverified: the image has no loader to replace")
    with open(args.noverify_loader, "rb") as f:
        mkmanifest.esp_put(image, "::/EFI/BOOT/" + os.path.basename(args.noverify_loader), f.read())
    b = Boot("unverified", image, args.workdir, {}, args.timeout)
    try:
        b.wait_prompt("the first prompt")
        log = b.text()
        need(log, r"^verified boot: OFF \(this loader was built with VERIFY_BOOT=0\); nothing is checked$",
             "the unverified boot")
        need(log, r"^UNVERIFIED BOOT: the loader was built with VERIFY_BOOT=0 and checked nothing$",
             "the unverified boot's banner")
        need(log, r"^\[ WARN\] boot: UNVERIFIED BOOT: ", "the unverified boot")
        if re.search(r"^verified boot: \\cosmo", log, re.M):
            raise Fail("unverified: the VERIFY_BOOT=0 loader checked a file")
        out = b.run("sysctl kernel.verified_boot")
        need(out, r"^kernel\.verified_boot = no$", "sysctl kernel.verified_boot")
        rc = b.finish()
        if rc != EXIT_SUCCESS:
            raise Fail(f"unverified boot: QEMU exit code {rc}")
    except Exception:
        b.kill()
        raise
    results.append(f"verify/unverified: PASS in {b.elapsed():.1f}s (tampered kernel booted, banner UNVERIFIED BOOT, "
                   "sysctl no)")


def stage_verify(args, results):
    boot_signed(args, results)
    reasons = {
        "kernel.elf": "its SHA-512 does not match the signed manifest",
        "boot.tar": "its SHA-512 does not match the signed manifest",
        "cmdline": "its SHA-512 does not match the signed manifest",
    }
    for which, why in reasons.items():
        boot_refused(args, results, which, tampered(args, which),
                     rf"^verified boot: rejected \\cosmo\\{re.escape(which)}: {why}$")

    # A key the loader does not hold.
    with tempfile.TemporaryDirectory() as kd:
        key, pub = os.path.join(kd, "other.key"), os.path.join(kd, "other.pub")
        modsign.cmd_keygen(argparse.Namespace(out_key=key, out_pub=pub))
        other = modsign.read_hex_key(key, "key")
    image = variant(args, "other-key")
    mkmanifest.resign_image(image, other)
    other_id = modsign.key_id(modsign.secret_to_public(other)).hex()
    boot_refused(args, results, "other-key", image,
                 rf"^verified boot: \\cosmo\\manifest is signed by key {other_id}, which this loader does not trust$")

    # The same signature claiming the trusted key's id.
    trusted = modsign.key_id(modsign.secret_to_public(modsign.read_hex_key(args.key, "key")))
    image = variant(args, "forged-id", image)
    man = bytearray(mkmanifest.esp_get(image, mkmanifest.MANIFEST_PATH))
    man[mkmanifest.PAYLOAD_SIZE + 64:mkmanifest.PAYLOAD_SIZE + 72] = trusted
    mkmanifest.esp_put(image, mkmanifest.MANIFEST_PATH, bytes(man))
    boot_refused(args, results, "forged-id", image,
                 r"^verified boot: rejected \\cosmo\\manifest: the signature does not verify$")

    # No manifest at all.
    image = variant(args, "no-manifest")
    mkmanifest.esp_delete(image, mkmanifest.MANIFEST_PATH)
    boot_refused(args, results, "no-manifest", image,
                 r"^verified boot: cannot read \\cosmo\\manifest: a verifying loader boots nothing without the "
                 r"signed manifest$")

    # The developer override boots what the verifying loader refused.
    boot_unverified(args, results, variant(args, "unverified", os.path.join(args.workdir, "tampered-kernel.elf",
                                                                            "cosmoos.img")))


RUNS = 10
LINE = re.compile(r"^aslr: kind=(\w+) stack=0x([0-9a-f]+) sp=0x([0-9a-f]+) mmap=0x([0-9a-f]+)"
                  r"(?: brk=0x([0-9a-f]+))? load=0x([0-9a-f]+)$", re.M)
# The bits each value may vary in (process.h), with room for the carry or
# borrow the random term can cause: the stack top moves down by up to 2^22
# pages and the initial frame by a 16-byte multiple within a page (bits
# 4..33, a borrow into 34); the mmap base and a PIE's bias up by up to 2^28
# pages (bits 12..39, carries into 40-41); the break up to 2^13 pages above
# the image (bits 12..24), and with the image when it is a PIE. The second
# number is how many bits are random.
def bits(lo, hi):
    return ((1 << hi) - 1) & ~((1 << lo) - 1)


EXPECT = {
    "stack": (bits(4, 35), 30),
    "sp": (bits(4, 35), 30),
    "mmap": (bits(12, 42), 28),
    "load": (bits(12, 42), 28),
    "brk-static": (bits(12, 27), 13),
    "brk-pie": (bits(12, 43), 28),
}


def check_field(kind, name, values, key):
    # Ten draws from 2^13 pages (a static image's break) collide about
    # once in 180 boots (45 pairs / 8192): one repeated pair there is
    # chance, two is not (PR #369 review). Every wider field must be
    # distinct.
    allowed_repeats = 1 if key == "brk-static" else 0
    if len(values) - len(set(values)) > allowed_repeats:
        raise Fail(f"aslr {kind}: {name} repeats across {len(values)} runs: {[hex(v) for v in values]}")
    varying = 0
    for v in values:
        varying |= v ^ values[0]
    mask, want = EXPECT[key]
    if varying & ~mask:
        raise Fail(f"aslr {kind}: {name} varies outside its range: bits {hex(varying & ~mask)}")
    n = bin(varying).count("1")
    # Ten draws leave a given random bit unchanged with probability 2^-9;
    # four of them is beyond chance.
    if n < want - 4:
        raise Fail(f"aslr {kind}: {name} varies in {n} bits, expected about {want} ({hex(varying)})")
    return n


def stage_aslr(args, results):
    image = variant(args, "aslr")
    b = Boot("aslr", image, args.workdir, {}, args.timeout)
    summary = []
    try:
        b.wait_prompt("the first prompt")
        for prog in ("aslr-static", "aslr-pie", "aslr-native"):
            runs = []
            for i in range(RUNS):
                out = b.run(f"/boot/tests/security/{prog}")
                m = LINE.search(out)
                if not m:
                    raise Fail(f"{prog} run {i + 1}: no aslr line in {out!r}")
                runs.append(m)
                if prog == "aslr-native":
                    need(out, r"^aslr: wx mmap-wx=EINVAL mprotect-wx=EINVAL$", f"{prog} run {i + 1}")
                else:
                    need(out, r"^aslr: wx stack-exec=SIGSEGV mmap-wx=EINVAL mprotect-wx=EINVAL$",
                         f"{prog} run {i + 1}")
            kind = runs[0].group(1)
            vals = {f: [int(r.group(k), 16) for r in runs] for f, k in (("stack", 2), ("sp", 3), ("mmap", 4),
                                                                       ("load", 6))}
            line = [f"{kind}:"]
            for f in ("stack", "sp", "mmap"):
                line.append(f"{f} {check_field(kind, f, vals[f], f)} bits")
            if kind == "pie":
                line.append(f"load {check_field(kind, 'load', vals['load'], 'load')} bits")
            elif len(set(vals["load"])) != 1:
                raise Fail(f"aslr {kind}: the load address moved: {[hex(v) for v in vals['load']]}")
            else:
                line.append(f"load fixed {hex(vals['load'][0])}")
            if kind != "native":
                brk = [int(r.group(5), 16) for r in runs]
                line.append(f"brk {check_field(kind, 'brk', brk, 'brk-' + kind)} bits")
            summary.append(" ".join(line))
        rc = b.finish()
        if rc != EXIT_SUCCESS:
            raise Fail(f"aslr boot: QEMU exit code {rc}")
    except Exception:
        b.kill()
        raise
    results.append(f"aslr: PASS in {b.elapsed():.1f}s, {RUNS} runs each: " + "; ".join(summary) +
                   "; W^X: stack code SIGSEGV, W+X mmap/mprotect EINVAL")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--image", required=True)
    ap.add_argument("--noverify-loader", required=True)
    ap.add_argument("--key", help="the build's signing key (MODSIGN_KEY); the verify stage alone needs it")
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--stages", default="verify,aslr")
    ap.add_argument("--timeout", type=float, default=240.0, help="per boot")
    args = ap.parse_args()
    if "verify" in args.stages.split(",") and not args.key:
        ap.error("--key is needed for the verify stage (it signs manifests of its own)")
    shutil.rmtree(args.workdir, ignore_errors=True)
    os.makedirs(args.workdir)
    results = []
    t0 = time.monotonic()
    try:
        stages = args.stages.split(",")
        if "verify" in stages:
            stage_verify(args, results)
        if "aslr" in stages:
            stage_aslr(args, results)
    except Fail as e:
        for r in results:
            print(f"test-security: {r}")
        print(f"test-security: FAIL: {e} (logs in {args.workdir})")
        return 1
    for r in results:
        print(f"test-security: {r}")
    print(f"test-security: PASS in {time.monotonic() - t0:.1f}s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
