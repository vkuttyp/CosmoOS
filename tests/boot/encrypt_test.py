#!/usr/bin/env python3
"""Roadmap 1.2 B5's acceptance test: an encrypted root.

    encrypt_test.py --image cosmoos.img --workdir DIR [--stages ...]

Stages, in order (each boot is a QEMU through scripts/qemu-run.sh, its
serial log kept in the work directory):

  install    the live image with two blank disks: `cosmo-install --encrypt`
             on disk A (the passphrase typed twice), `cosmo-install
             --encrypt --key-file` on disk B mirrored with B2. On the host,
             A's root superblock is format 14 with a key block.
  wrong      A alone, three wrong passphrases: init restarts the machine
             and nothing mounted.
  boot       A alone: one wrong passphrase, refused; the right one unlocks
             it (the Argon2id time is read off the line) and the system
             boots. A marker file is written, a second slot is added with
             `cosmo-key add`, and a spare disk is labelled for the pool.
  plaintext  On the host: neither disk A nor the spare holds a byte of the
             marker's text.
  forge      Each metadata domain of A forged as a CRC32C pool would accept
             it -- a byte changed, csum_algo CRC32C, the CRC recomputed --
             then booted, on a copy of A and the spare: the newest
             superblock slot, the inode map's root, the member table, the
             key block, and the spare's label. Each is refused.
  slot2      A with the second slot's passphrase: it boots. 1 GiB is written
             to a file, its SHA-256 taken, and the mounted check walks its
             two-level checksum index.
  readback   A with the first passphrase: the 1 GiB file reads back with the
             same SHA-256.
  keyfile    B and B2: no prompt, the key file on the ESP unlocks the
             mirror; then B2 alone, its own ESP's copy of the key file.

Exit 0 PASS, 1 FAIL.
"""

import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from install_test import Boot, Fail, need, EXIT_SUCCESS, SS  # noqa: E402

PASS1 = "correct horse battery staple"
PASS2 = "a second, different passphrase"
PASS_B = "the key-file disk's passphrase"
WRONG = "not the passphrase"
A_MIB, B_MIB, SPARE_MIB = 1536, 256, 64
BLOCK = 4096
BIG_MIB = 1024
MARKER = "COSMO-B5-PLAINTEXT-MARKER"

SB_GENERATION, SB_IMAP, SB_MEMBERS, SB_KEY_ROOT, SB_CRC = 24, 32, 88, 112, 160
HDR_CRC = 24
LABEL_CRC = 72


# --- CRC32C, as a CRC'd pool seals with it ----------------------------------------

def _crc32c_table():
    t = []
    for i in range(256):
        c = i
        for _ in range(8):
            c = (c >> 1) ^ 0x82F63B78 if c & 1 else c >> 1
        t.append(c)
    return t


_T = _crc32c_table()


def crc32c(data):
    c = 0xFFFFFFFF
    for b in data:
        c = _T[(c ^ b) & 0xFF] ^ (c >> 8)
    return c ^ 0xFFFFFFFF


# --- the disk, on the host ------------------------------------------------------------

def root_offset(path):
    """Byte offset of partition 2 (the root) of a GPT disk image."""
    with open(path, "rb") as f:
        f.seek(SS)
        h = f.read(92)
        if h[:8] != b"EFI PART":
            raise Fail(f"{path}: no GPT")
        entries, size = int.from_bytes(h[72:80], "little"), int.from_bytes(h[84:88], "little")
        f.seek(entries * SS + size)   # entry 2
        e = f.read(size)
        return int.from_bytes(e[32:40], "little") * SS


def read_block(path, off):
    with open(path, "rb") as f:
        f.seek(off)
        return bytearray(f.read(BLOCK))


def write_block(path, off, data):
    with open(path, "r+b") as f:
        f.seek(off)
        f.write(bytes(data))


def newest_super(path, base):
    """(slot, its bytes) of the superblock with the higher generation."""
    best = None
    for slot in (0, 1):
        b = read_block(path, base + slot * BLOCK)
        if b[:8] != b"COSMOFS1":
            continue
        gen = int.from_bytes(b[SB_GENERATION:SB_GENERATION + 8], "little")
        if best is None or gen > best[2]:
            best = (slot, b, gen)
    if best is None:
        raise Fail(f"{path}: no superblock")
    return best


def forge(path, off, crc_off, touch, header):
    """Change a byte, make it look CRC32C-sealed; the block as it was."""
    b = read_block(path, off)
    old = bytes(b)
    b[touch] ^= 0x01
    if header:
        b[HDR_CRC + 4:HDR_CRC + 8] = (1).to_bytes(4, "little")   # csum_algo CRC32C
    b[crc_off:crc_off + 4] = b"\0\0\0\0"
    b[crc_off:crc_off + 4] = crc32c(bytes(b)).to_bytes(4, "little")
    write_block(path, off, b)
    return old


def clone(src, dst):
    """A copy of a sparse disk image that stays sparse: a clone on APFS,
    holes kept elsewhere."""
    for cmd in (["cp", "-c", src, dst], ["cp", "--sparse=always", src, dst]):
        if subprocess.run(cmd, stderr=subprocess.DEVNULL).returncode == 0:
            return
    shutil.copyfile(src, dst)


def sparse_disk(path, mib):
    with open(path, "wb") as f:
        f.truncate(mib << 20)


def scan(path, needle):
    """Whether `needle` occurs anywhere in the file, read in pieces."""
    step, keep = 16 << 20, len(needle) - 1
    tail = b""
    with open(path, "rb") as f:
        while True:
            chunk = f.read(step)
            if not chunk:
                return False
            if needle in tail + chunk:
                return True
            tail = (tail + chunk)[-keep:]


# --- typing at prompts ------------------------------------------------------------------

def converse(b, cmd, steps, what):
    """Type `cmd`; for each (prompt, reply) wait for the prompt after what
    came before and type the reply; then wait for the shell's prompt. What
    it printed."""
    before = len(b.text())
    b.proc.stdin.write(cmd.encode() + b"\n")
    b.proc.stdin.flush()
    at = before
    for prompt, reply in steps:
        at = wait_text(b, prompt, at, what) + len(prompt)
        b.proc.stdin.write(reply.encode() + b"\n")
        b.proc.stdin.flush()
    b.wait_prompt(what)
    return b.text()[before:]


def wait_text(b, text, start, what):
    while time.monotonic() < b.deadline:
        i = b.text().find(text, start)
        if i >= 0:
            return i
        if b.proc.poll() is not None:
            raise Fail(f"{b.name}: QEMU exited (code {b.proc.returncode}) waiting for {text!r} ({what})")
        time.sleep(0.05)
    raise Fail(f"{b.name}: no {text!r} before the deadline ({what})")


def unlock_at_boot(b, replies, what):
    """Answer init's passphrase prompts in turn; where the last one was."""
    at = 0
    for r in replies:
        at = wait_text(b, "passphrase for cosmo-root: ", at, what) + 1
        b.proc.stdin.write(r.encode() + b"\n")
        b.proc.stdin.flush()
    return at


def dev_by_size(listing, mib, what):
    m = re.search(rf"^(\S+)\s+-\s+{(mib << 20) // SS}\s+{SS}\s", listing, re.M)
    if not m:
        raise Fail(f"{what}: no {mib} MiB disk in `cosmo-install --list`")
    return m.group(1)


def boot_disks(args, name, disks, timeout=None):
    """The first disk boots; the others are attached after it."""
    extra = " ".join(f"-drive if=none,id=x{i},format=raw,file={d} -device virtio-blk-pci,drive=x{i}"
                     for i, d in enumerate(disks[1:]))
    env = {"QEMU_DISKS": "boot", "QEMU_EXTRA": (os.environ.get("QEMU_EXTRA", "") + " " + extra).strip()}
    return Boot(name, disks[0], args.workdir, env, timeout or args.timeout)


# --- stages -----------------------------------------------------------------------------

def stage_install(args, results, d):
    live = os.path.join(args.workdir, "live")
    os.makedirs(live, exist_ok=True)
    image = os.path.join(live, "cosmoos.img")
    shutil.copyfile(args.image, image)
    for k, mib in (("a", A_MIB), ("b", B_MIB), ("b2", B_MIB), ("spare", SPARE_MIB)):
        sparse_disk(d[k], mib)
    env = {"QEMU_EXTRA": (os.environ.get("QEMU_EXTRA", "") + " " + " ".join(
        f"-drive if=none,id={k},format=raw,file={d[k]} -device virtio-blk-pci,drive={k}" for k in ("a", "b", "b2")
    )).strip()}
    b = Boot("install", image, args.workdir, env, args.timeout)
    try:
        b.wait_prompt("the first prompt")
        out = b.run("cosmo-install --list")
        da = dev_by_size(out, A_MIB, "disk A")
        m = re.findall(rf"^(\S+)\s+-\s+{(B_MIB << 20) // SS}\s+{SS}\s", out, re.M)
        if len(m) != 2:
            raise Fail(f"install: {len(m)} disks of {B_MIB} MiB, expected 2")
        db, db2 = m
        # The live shell expands $? for a whole line before running any of
        # it, so each status is asked for on a line of its own.
        out = converse(b, f"cosmo-install --encrypt {da}",
                       [("new passphrase: ", PASS1), ("new passphrase again: ", PASS1)], "cosmo-install --encrypt")
        out += b.run("echo enc-a=$?")
        need(out, r"^enc-a=0$", "cosmo-install --encrypt")
        seal = need(out, r"^cosmo-install: slot 0 sealed with the passphrase \(argon2id 65536 KiB, 3 passes: "
                         r"([\d.]+) s\)$", "cosmo-install --encrypt")
        need(out, r"^cosmo-install: the root is encrypted \(format 14\): slot 0 the passphrase", "cosmo-install --encrypt")
        if PASS1 in out:
            raise Fail("cosmo-install echoed the passphrase")
        # A mistyped second passphrase writes nothing.
        out = converse(b, f"cosmo-install --encrypt --key-file {db} --mirror {db2}",
                       [("new passphrase: ", PASS_B), ("new passphrase again: ", PASS_B + "x")], "a mismatch")
        out += b.run("echo enc-x=$?")
        need(out, r"^cosmo-install: the two passphrases differ$", "a mismatched passphrase")
        need(out, r"^enc-x=3$", "a mismatched passphrase")
        out = converse(b, f"cosmo-install --encrypt --key-file {db} --mirror {db2}",
                       [("new passphrase: ", PASS_B), ("new passphrase again: ", PASS_B)], "--key-file --mirror")
        out += b.run("echo enc-b=$?")
        need(out, r"^enc-b=0$", "cosmo-install --encrypt --key-file --mirror")
        need(out, r"^cosmo-install: slot 1 sealed with the passphrase ", "cosmo-install --key-file")
        need(out, r"^cosmo-install: the root is encrypted \(format 14\): slot 0 the key file \\cosmo\.key on the "
                  r"ESPs, slot 1 the passphrase", "cosmo-install --key-file --mirror")
        root_a = need(b.text(), rf"^cosmo-install: installed on {re.escape(da)}: esp \S+, root (\S+), partuuid",
                      "disk A's root partition").group(1)
        out = b.run(f"blkid /dev/{root_a}")
        need(out, r'TYPE="cosmofs" ENCRYPTED="yes"', "blkid on the encrypted root")
        rc = b.finish()
        if rc != EXIT_SUCCESS:
            raise Fail(f"install boot: QEMU exit code {rc}")
    except Exception:
        b.kill()
        raise
    base = root_offset(d["a"])
    _, sb, gen = newest_super(d["a"], base)
    version = int.from_bytes(sb[8:12], "little")
    key_root = int.from_bytes(sb[SB_KEY_ROOT:SB_KEY_ROOT + 8], "little")
    if version != 14 or key_root == 0:
        raise Fail(f"disk A's root: format {version}, key block {key_root}; expected 14 with a key block")
    results.append(f"install: PASS in {b.elapsed():.1f}s (A {da}: format 14, slot 0 sealed in {seal.group(1)} s; "
                   f"B {db} + B2 {db2}: mirror, key file and a passphrase; a mismatched passphrase wrote nothing)")
    d["seal_s"] = seal.group(1)


def stage_wrong(args, results, d):
    b = boot_disks(args, "wrong", [d["a"]])
    try:
        unlock_at_boot(b, [WRONG, WRONG + "!", WRONG + "?"], "three wrong passphrases")
        wait_text(b, "init: three wrong passphrases for the root; restarting", 0, "three wrong passphrases")
        try:
            rc = b.proc.wait(timeout=60)
        except Exception:
            raise Fail("wrong: QEMU did not end after the restart")
        b.reader.join(timeout=5)
        log = b.text()
        if PASS1 in log or WRONG in log:
            raise Fail("wrong: a passphrase was echoed")
        if re.search(r"init: switched to the disk root|vfs: root switched to cosmofs|cosmofs: \S+: generation \d+",
                     log):
            raise Fail("wrong: something mounted after three wrong passphrases")
        n = len(re.findall(r"^cosmo-key: that passphrase opens no slot of cosmo-root$", log, re.M))
        if n != 3:
            raise Fail(f"wrong: {n} refusals, expected 3")
    except Exception:
        b.kill()
        raise
    results.append(f"wrong: PASS in {b.elapsed():.1f}s (three wrong passphrases refused, the machine restarted "
                   f"with nothing mounted; QEMU exit {rc})")


def stage_boot(args, results, d):
    b = boot_disks(args, "boot", [d["a"], d["spare"]])
    try:
        unlock_at_boot(b, [WRONG, PASS1], "a wrong then the right passphrase")
        b.wait_prompt("the first prompt")
        log = b.text()
        need(log, r"^cosmo-key: that passphrase opens no slot of cosmo-root$", "the wrong passphrase")
        t = need(log, r"^cosmo-key: cosmo-root unlocked with slot 0 \(argon2id 65536 KiB, 3 passes: ([\d.]+) s\)$",
                 "the unlock")
        need(log, r"^init: switched to the disk root \S+ \(root=boot-disk\)$", "the encrypted root's boot")
        need(log, r"^cosmofs: root checked, 0 findings, 0 repaired$", "the boot's check of the root")
        if PASS1 in log or WRONG in log:
            raise Fail("boot: a passphrase was echoed")
        d["unlock_s"] = t.group(1)
        out = b.status(f"i=0; while [ $i -lt 2000 ]; do echo {MARKER}-$i; i=$((i+1)); done > /root/marker && sync && "
                       f"grep -c {MARKER} /root/marker", "marker")
        need(out, r"^marker=0$", "writing the marker")
        need(out, r"^2000$", "writing the marker")
        out = converse(b, "cosmo-key add", [("a passphrase for cosmo-root: ", PASS1),
                                                          ("new passphrase: ", PASS2),
                                                          ("new passphrase again: ", PASS2)], "cosmo-key add")
        out += b.run("echo add=$?")
        need(out, r"^add=0$", "cosmo-key add")
        need(out, r"^cosmo-key: slot 1 of cosmo-root written \(argon2id 65536 KiB, 3 passes: ", "cosmo-key add")
        out = b.run("cosmo-key list")
        need(out, r"^slot 0: passphrase, argon2id 65536 KiB, 3 passes, 1 lane$", "cosmo-key list")
        need(out, r"^slot 1: passphrase, argon2id 65536 KiB, 3 passes, 1 lane$", "cosmo-key list")
        need(out, r"^slot 2: empty$", "cosmo-key list")
        # A spare: its label is the pool's, MAC'd (the forge stage's label).
        listing = b.run("cosmo-install --list")
        sp = dev_by_size(listing, SPARE_MIB, "the spare")
        out = b.status(f"cosmo-pool add-spare {sp} && cosmo-pool status", "spare")
        need(out, r"^spare=0$", "cosmo-pool add-spare")
        need(out, rf"^    {re.escape(sp)}\s+\d+ blocks$", "the spare in cosmo-pool status")
        d["spare_dev"] = sp
        rc = b.finish()
        if rc != EXIT_SUCCESS:
            raise Fail(f"boot: QEMU exit code {rc}")
    except Exception:
        b.kill()
        raise
    results.append(f"boot: PASS in {b.elapsed():.1f}s (a wrong passphrase refused, then unlocked with slot 0: "
                   f"Argon2id {d['unlock_s']} s; marker written, slot 1 added, spare {d['spare_dev']} labelled)")


def stage_plaintext(results, d):
    needle = MARKER.encode()
    for k in ("a", "spare"):
        if scan(d[k], needle):
            raise Fail(f"plaintext: disk {k} holds the marker's text")
    results.append(f"plaintext: PASS (neither disk A ({A_MIB} MiB) nor the spare holds '{MARKER}')")


def stage_slot2(args, results, d):
    b = boot_disks(args, "slot2", [d["a"]], timeout=max(args.timeout, 5400))
    try:
        unlock_at_boot(b, [PASS2], "the second slot's passphrase")
        b.wait_prompt("the first prompt")
        need(b.text(), r"^cosmo-key: cosmo-root unlocked with slot 1 ", "the second slot")
        # 4 MiB of gzip output, written 256 times: 1 GiB, every block of it
        # encrypted. Already compressed, so its blocks stay plain: a file
        # LZ4 does compress is one run per 32 KiB record, and cosmofs loads
        # at most 4096 runs per inode (CFS_MAX_EXTENTS), so such a file
        # stops at 128 MiB (backlog CFS-32). head -c reads until it has
        # them (dd's count of a pipe's blocks counts its short reads).
        # A sync after each 32 MiB: dirty pages are not bounded (backlog
        # MEM-9), and 1 GiB written with one sync at the end took 3.3x as
        # long, written back under memory pressure (x86-64, 1275 s against
        # 386 s).
        t0 = time.monotonic()
        out = b.status("cat /sbin/* /bin/* 2>/dev/null | head -c 16777216 | gzip -c | head -c 4194304 > /tmp/chunk; "
                       "test $(wc -c < /tmp/chunk) -eq 4194304 && : > /big && "
                       f"for i in $(seq {BIG_MIB // 32}); do for j in 1 2 3 4 5 6 7 8; do cat /tmp/chunk; done >> /big "
                       "&& sync || break; done && "
                       "ls -l /big && sha256sum /big", "big")
        wrote = time.monotonic() - t0
        need(out, r"^big=0$", "writing 1 GiB")
        need(out, rf"\s{BIG_MIB << 20}\s+\S+\s+\S+\s+\S+\s+/big$", "the 1 GiB file's size")
        d["big_sha"] = need(out, r"^([0-9a-f]{64})\s+/big$", "the 1 GiB file's SHA-256").group(1)
        want = b.run(f"for i in $(seq {BIG_MIB // 4}); do cat /tmp/chunk; done | sha256sum")
        if not re.search(rf"^{d['big_sha']}\s", want, re.M):
            raise Fail("slot2: /big is not the chunk 256 times")
        out = b.run("fsctl list")
        mid = need(out, r"^\s*(\d+)\s+cosmofs\s+\S+\s+/$", "the root's mount id").group(1)
        out = b.run(f"fsctl check {mid}")
        need(out, r"^clean$", "the mounted check after 1 GiB")
        need(b.text(), r"cosmofs: check: \d+ two-level checksum tree\(s\) walked", "the check's two-level walk")
        rc = b.finish()
        if rc != EXIT_SUCCESS:
            raise Fail(f"slot2: QEMU exit code {rc}")
    except Exception:
        b.kill()
        raise
    results.append(f"slot2: PASS in {b.elapsed():.1f}s (unlocked with slot 1; 1 GiB written in {wrote:.0f} s, "
                   f"sha256 {d['big_sha'][:16]}...; the check clean, a two-level checksum tree walked)")


def stage_readback(args, results, d):
    b = boot_disks(args, "readback", [d["a"]], timeout=max(args.timeout, 5400))
    try:
        unlock_at_boot(b, [PASS1], "the first passphrase")
        b.wait_prompt("the first prompt")
        t0 = time.monotonic()
        out = b.run("sha256sum /big")
        read = time.monotonic() - t0
        if not re.search(rf"^{d['big_sha']}\s+/big$", out, re.M):
            raise Fail("readback: the 1 GiB file did not read back the same")
        rc = b.finish()
        if rc != EXIT_SUCCESS:
            raise Fail(f"readback: QEMU exit code {rc}")
    except Exception:
        b.kill()
        raise
    results.append(f"readback: PASS in {b.elapsed():.1f}s (1 GiB read back after a reboot, same sha256, "
                   f"{read:.0f} s)")


def stage_keyfile(args, results, d):
    for name, disks in (("keyfile", [d["b"], d["b2"]]), ("keyfile-b2", [d["b2"]])):
        b = boot_disks(args, name, disks)
        try:
            b.wait_prompt("the first prompt")
            log = b.text()
            need(log, r"^cosmo-key: cosmo-root unlocked with the key file on the ESP$", f"{name}: the key file")
            if "passphrase for cosmo-root" in log:
                raise Fail(f"{name}: a passphrase was asked for")
            need(log, r"^init: switched to the disk root \S+ \(root=boot-disk\)$", f"{name}: the boot")
            if name == "keyfile":
                # Slot 1 is B's only passphrase, and only a passphrase opens
                # cosmo-key's slot commands: refused, though the passphrase
                # given opens it.
                out = b.run(f'echo "{PASS_B}" | cosmo-key remove 1; echo rm=$?')
                need(out, r"^cosmo-key: slot 1 of cosmo-root not removed: it is the last passphrase$",
                     "keyfile: the last passphrase kept")
                need(out, r"^rm=1$", "keyfile: cosmo-key remove 1's exit status")
                need(b.run("cosmo-key list"), r"^slot 1: passphrase, ", "keyfile: slot 1 after the refusal")
            rc = b.finish()
            if rc != EXIT_SUCCESS:
                raise Fail(f"{name}: QEMU exit code {rc}")
        except Exception:
            b.kill()
            raise
    results.append("keyfile: PASS (B and B2 unlocked with no prompt by the key file on the ESP; B2 alone by its "
                   "own ESP's copy: the mirror's disks share one master key; B's last passphrase not removed)")


def stage_forge(args, results, d):
    a = d["a"]
    base = root_offset(a)
    slot, sb, gen = newest_super(a, base)
    imap = int.from_bytes(sb[SB_IMAP:SB_IMAP + 8], "little") & ((1 << 56) - 1)
    members = int.from_bytes(sb[SB_MEMBERS:SB_MEMBERS + 8], "little") & ((1 << 56) - 1)
    key_root = int.from_bytes(sb[SB_KEY_ROOT:SB_KEY_ROOT + 8], "little") & ((1 << 56) - 1)
    cases = [
        ("superblock", "a", base + slot * BLOCK, SB_CRC, 64, False,
         rf"cosmofs: superblock slot {slot} \(generation {gen}\) does not authenticate", True),
        ("inode map root", "a", base + imap * BLOCK, HDR_CRC, 40, True, r"metadata \w+ fault: .* it does not authenticate",
         None),
        ("member table", "a", base + members * BLOCK, HDR_CRC, 80, True, r"cosmofs: the member table is unreadable",
         None),
        # A reserved byte of the key block: only its tag covers it, so it is
        # the MAC that refuses it, not KEY_INFO's look at the block's shape.
        ("key block", "a", base + key_root * BLOCK, HDR_CRC, 32 + 24, True,
         r"cosmofs: \S+: the key block does not authenticate; the pool is not unlocked", False),
        ("spare label", "spare", 0, LABEL_CRC, 39, False, None, True),
    ]
    seen = []
    for what, which, off, crc_off, touch, header, line, boots in cases:
        # On copies: a forged boot that falls back to the older root commits
        # over the slot it refused, so putting the block back afterwards
        # would put back a superblock whose blocks have been reused.
        copies = {k: os.path.join(args.workdir, f"forge-{k}.img") for k in ("a", "spare")}
        for k, path in copies.items():
            clone(d[k], path)
        forge(copies[which], off, crc_off, touch, header)
        try:
            b = boot_disks(args, f"forge-{what.replace(' ', '-')}", [copies["a"], copies["spare"]])
            try:
                unlock_at_boot(b, [PASS1], f"forged {what}")
                b.wait_prompt("the first prompt")
                log = b.text()
                if line:
                    need(log, line, f"the forged {what}")
                switched = re.search(r"^init: switched to the disk root", log, re.M) is not None
                if boots is True and not switched:
                    raise Fail(f"forge {what}: the root did not mount from what still verifies")
                if boots is False and switched:
                    raise Fail(f"forge {what}: the root mounted")
                if what == "spare label":
                    out = b.run("cosmo-pool status")
                    if d["spare_dev"] in out:
                        raise Fail("forge spare label: the forged label was taken for the pool's spare")
                    if not switched:
                        raise Fail("forge spare label: the root did not mount")
                    detail = "the spare not taken"
                else:
                    detail = "the root mounted from the older root" if switched else "nothing mounted"
                rc = b.finish()
                if rc != EXIT_SUCCESS:
                    raise Fail(f"forge {what}: QEMU exit code {rc}")
            except Exception:
                b.kill()
                raise
        finally:
            for path in copies.values():
                os.unlink(path)
        seen.append(f"{what}: refused, {detail}")
    results.append("forge: PASS (" + "; ".join(seen) + ")")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--image", required=True)
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--stages", default="install,wrong,boot,plaintext,forge,slot2,readback,keyfile")
    ap.add_argument("--timeout", type=float, default=300.0, help="per boot")
    ap.add_argument("--arch", default=os.environ.get("QEMU_ARCH", "x86_64"))
    args = ap.parse_args()
    shutil.rmtree(args.workdir, ignore_errors=True)
    os.makedirs(args.workdir)
    d = {k: os.path.join(args.workdir, f"disk-{k}.img") for k in ("a", "b", "b2", "spare")}
    results = []
    t0 = time.monotonic()
    stages = args.stages.split(",")
    try:
        stage_install(args, results, d)
        if "wrong" in stages:
            stage_wrong(args, results, d)
        if "boot" in stages:
            stage_boot(args, results, d)
        if "plaintext" in stages:
            stage_plaintext(results, d)
        if "forge" in stages:
            stage_forge(args, results, d)
        if "slot2" in stages:
            stage_slot2(args, results, d)
            if "readback" in stages:
                stage_readback(args, results, d)
        if "keyfile" in stages:
            stage_keyfile(args, results, d)
    except Fail as e:
        for r in results:
            print(f"test-encrypt: {r}")
        print(f"test-encrypt: FAIL ({args.arch}): {e} (logs in {args.workdir})")
        return 1
    for r in results:
        print(f"test-encrypt: {r}")
    print(f"test-encrypt: argon2id 65536 KiB, 3 passes ({args.arch}): sealing {d['seal_s']} s, unlocking at boot "
          f"{d.get('unlock_s', '-')} s")
    print(f"test-encrypt: PASS ({args.arch}) in {time.monotonic() - t0:.1f}s; logs in {args.workdir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
