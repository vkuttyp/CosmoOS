#!/usr/bin/env python3
"""Roadmap 1.2 B4's boot environments (docs/userland/testing.md,
"test-upgrade"; docs/userland/design.md, "Boot environments"). Run by
test-upgrade after its v1.0.3 stages.

    be_test.py --image cosmoos.img --set DIR --key MODSIGN.key --arch ARCH --workdir DIR
               [--older cosmoos-ARCH.img] [--only forward]

Set A is this build's release set (DIR); set B is made from it: the same
kernel, a boot.tar with one more file, usr/share/cosmo-set-b, and a
manifest and SHA256SUMS signed again with the build's key. Its base system
differs from A's by that file, which is how a root's files say which set's
base system they are (cosmo-upgrade tells it so too, outside /etc).

Stages, each on the one installed disk:

  install     test-install's, of this build (set A).
  upgrade     hello 1.0 installed, an /etc file written, and
              /var/lib/cosmo/rollback made a file (so that keeping the
              operator's files fails once); cosmo-upgrade to B takes the
              environment be-* of the root first and pairs it with A in
              \\cosmo\\be.
  new         B boots; pkg upgrade takes hello to 1.1 (a transaction: every
              kept set reads format 12); the /etc file changed; files under
              /root, /home and /var/log written, the file in the way removed
              from B's tree; --rollback returns both
              halves: the set now, the root at the next mount, and reboots.
  back-held   A boots on the environment's root; the operator's files
              cannot be copied (the file in the way): init keeps the
              rollback's hold and says so; the file removed.
  back        the next boot copies them and the hold goes: hello 1.0, the /etc file and
              the base system byte for byte the environment's (cosmo-snap
              diff of it and the live tree names nothing under them), no
              usr/share/cosmo-set-b; the operator's files copied to
              /var/lib/cosmo/rollback/<the replaced tree>/; --repair finds
              nothing to do.
  crash-record  --rollback (forward to B) stopped after the root's record
              and the machine killed: the next boot is A's kernel on B's
              root, and boots; --repair switches the set; then B, B, and
              the tree the record kept is a paired environment.
  mixed-old   --rollback (back to A) completed, then the root's rollback
              withdrawn: A's kernel boots B's root; --repair asks for the
              root's rollback; then A, A.
  crash-intent  --rollback stopped after the intent only, killed: the
              boot is A, A with \\cosmo\\be naming B; --repair rolls both;
              then B, B.
  crash-set   --rollback stopped after the set switch, killed: the next
              boot is A, A already (the record was before the switch).
  space       with a root rollback pending, cosmo-upgrade to B refused; then
              the root filled: refused before anything is written, naming
              the room it needs; the ESP byte for byte as it was.
  pending     cosmo-upgrade to B stopped after the set switch: --repair
              finishes its base pass; then B, B.

With --older (the published v1.0.7's live image, whose cosmo-upgrade
keeps no record of the root's base system), on a disk of its own:

  forward     v1.0.7 installs itself and upgrades to A with its own
              cosmo-upgrade (no environment); A's --rollback returns the set
              and the base system, its record naming v1.0.7's set; v1.0.7's
              own cosmo-upgrade --rollback rolls forward to A and leaves that
              record. On A, with the record as left: cosmo-upgrade to B is not
              refused (the root's files are A's: the record is corrected
              first); --list named both, and --repair corrected the record
              with no intent recorded. B boots on B's base system with the
              record B's and an environment paired with A.

--only forward runs that stage alone.

Exit 0 PASS, 1 FAIL.
"""

import argparse
import hashlib
import math
import os
import re
import shutil
import sys
import tarfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import install_test  # noqa: E402
from install_test import Boot, Fail, need, EXIT_SUCCESS  # noqa: E402
import mkbootarchive  # noqa: E402  (scripts/, on install_test's path)
import mkmanifest  # noqa: E402
import modsign  # noqa: E402
import net_login_test  # noqa: E402
from snapshot_test import check_clean, root_id  # noqa: E402
import upgrade_test  # noqa: E402

SET = ("kernel.elf", "boot.tar", "cmdline", "manifest")
MARK = "usr/share/cosmo-set-b"
UPB = "/tmp/b"
UPA = "/tmp/a"
# What --rollback must return byte for byte: packages, /etc, the base system.
RETURNED = ("/etc", "/bin", "/sbin", "/usr", "/lib", "/var/db/pkg")


class Done(Exception):
    """--only's stage has ended."""


def make_set_b(seta, setb, arch, seed, mark=MARK):
    """Set B from set A: boot.tar with `mark` (MARK) added, the manifest
    and SHA256SUMS signed again. test-mirror makes a set C with another."""
    os.makedirs(setb, exist_ok=True)
    f = {n: open(os.path.join(seta, f"cosmoos-{arch}-{n}"), "rb").read() for n in SET}
    with tarfile.open(os.path.join(seta, f"cosmoos-{arch}-boot.tar")) as t:
        last = t.getmembers()[-1]
        end = last.offset_data + (last.size + 511) // 512 * 512
    data = b"set b\n" if mark == MARK else f"{mark}\n".encode()
    member = mkbootarchive.header(mark, len(data)) + data + bytes(512 - len(data))
    tar = f["boot.tar"][:end] + member + f["boot.tar"][end:]
    f["boot.tar"] = tar
    f["manifest"] = mkmanifest.sign(seed, [f["kernel.elf"], tar, f["cmdline"]])
    lines = []
    for n in SET:
        open(os.path.join(setb, f"cosmoos-{arch}-{n}"), "wb").write(f[n])
        lines.append(f"{hashlib.sha256(f[n]).hexdigest()}  cosmoos-{arch}-{n}\n")
    text = "".join(lines).encode()
    open(os.path.join(setb, "SHA256SUMS"), "wb").write(text)
    open(os.path.join(setb, "SHA256SUMS.sig"), "wb").write(modsign.detached(seed, text))
    return f


def sh(b, cmd, tag, want=0):
    out = b.status(cmd, tag)
    m = need(out, rf"^{tag}=(\d+)$", cmd)
    if int(m.group(1)) != want:
        raise Fail(f"{cmd!r}: exit {m.group(1)}, want {want}; output {out[-900:]!r}")
    return out


def boot(name, args):
    b = Boot(name, args.target, args.workdir, {"QEMU_DISKS": "boot"}, args.timeout)
    b.wait_prompt("the first prompt")
    return b


def end(b, what):
    rc = b.finish("poweroff")
    if rc != EXIT_SUCCESS:
        raise Fail(f"{what}: QEMU exit code {rc}")


def fetch(b, port, arch, sub, to):
    url = f"http://10.0.2.2:{port}/{sub}"
    files = " ".join(f"cosmoos-{arch}-{n}" for n in SET) + " SHA256SUMS SHA256SUMS.sig"
    sh(b, f"mkdir -p {to} && cd {to} && for f in {files}; do wget -q {url}/$f || exit 1; done && cd /", "fetch")


def rebooting(b, cmd, what):
    """`cmd` arms the root's rollback and asks init to restart; init's
    restart ends this QEMU (no QEMU_REBOOT; pkgtx_test.py). Returns what
    the command printed."""
    start = len(b.text())
    b.proc.stdin.write(cmd.encode() + b"\n")
    b.proc.stdin.flush()
    try:
        b.proc.wait(timeout=120)
    except Exception:
        raise Fail(f"{what}: QEMU did not end at init's restart; tail {b.text()[-900:]!r}")
    out = b.text()[start:]
    need(out, r"^cosmo-upgrade: rebooting to apply it$", f"{what}: the reboot asked for")
    return out


def has_b(b):
    out = b.run(f"if [ -e /{MARK} ]; then echo base=b; else echo base=a; fi")
    return need(out, r"^base=(a|b)$", "which base system the root holds").group(1) == "b"


def expect_pair(b, which, what):
    """The root holds set `which`'s base system, and --repair has nothing
    to do: the pairing \\cosmo\\be intends is the one running."""
    if has_b(b) != (which == "b"):
        raise Fail(f"{what}: the root holds {'B' if has_b(b) else 'A'}'s base system, expected {which.upper()}'s")
    out = sh(b, "cosmo-upgrade --repair", "rep")
    need(out, r"^cosmo-upgrade: the root and the boot set are the pairing \\cosmo\\be intends: nothing to repair$",
         f"{what}: --repair")


def fill_root(b):
    """The root to under 256 KiB free, in synced steps of at most 8 MiB of
    incompressible bytes (docs/backlog.md MEM-9; pkgtx_test.py)."""
    b.run("sh -c 'awk \"BEGIN { srand(9); for (i = 0; i < 262144; i++) printf \\\"%c\\\", "
          "1 + int(rand() * 255) }\" > /tmp/rnd'")
    for k in range(96):
        out = b.run("cosmo-snap list | grep 'KiB free'")
        free = int(need(out, r"^cosmo-snap: /: (\d+) KiB free of \d+ KiB$", "the root's free space").group(1))
        if free < 256:
            return
        n = max(1, min(32, (free - 128) // 256))
        b.run(f"sh -c 'i=0; while [ $i -lt {n} ]; do cat /tmp/rnd; i=$((i+1)); done > /fill{k}; sync'")
    raise Fail("the root did not fill")


RELEASE_KEY = upgrade_test.RELEASE_KEY
OLDER = "v1.0.7"


def stage_forward(args, port, results):
    """The roll-forward an older cosmo-upgrade makes, on a disk of its own:
    v1.0.7 -> A -> (A's --rollback) v1.0.7 -> (v1.0.7's --rollback) A, then
    the upgrade to B."""
    sub = argparse.Namespace(base=args.older, workdir=os.path.join(args.workdir, "forward"), timeout=args.timeout,
                             arch=args.arch)
    os.makedirs(sub.workdir)
    upgrade_test.BASE = OLDER
    upgrade_test.stage_install(sub, results)   # sets sub.target
    t0 = time.monotonic()

    b = boot("fwd-upgrade", sub)
    try:
        need(b.text(), rf"^\[ INFO\] boot: verified boot, key {RELEASE_KEY} ", f"the installed {OLDER}")
        sh(b, f"wget -q -O /etc/pkg/keys/build.pub http://10.0.2.2:{port}/trust.pub && sync", "trust")
        fetch(b, port, args.arch, "a", UPA)
        out = sh(b, f"/sbin/cosmo-upgrade {UPA}", "up")
        need(out, r"^cosmo-upgrade: upgraded; reboot to run it", f"{OLDER}'s cosmo-upgrade to A")
        end(b, "fwd-upgrade")
    except Exception:
        b.kill()
        raise
    b = boot("fwd-new", sub)
    try:
        out = sh(b, "cosmo-upgrade --rollback", "back")
        need(out, r"^cosmo-upgrade: no boot environment pairs \\cosmo\\prev with a snapshot of the root", "A's --rollback")
        need(out, r"^cosmo-upgrade: rolled back; reboot to run it$", "A's --rollback")
        end(b, "fwd-new")
    except Exception:
        b.kill()
        raise
    b = boot("fwd-back", sub)
    try:
        need(b.text(), rf"^\[ INFO\] boot: verified boot, key {RELEASE_KEY} ", f"{OLDER} again")
        out = sh(b, "/sbin/cosmo-upgrade --rollback", "fwd")
        need(out, r"^cosmo-upgrade: rolled back; reboot to run it$", f"{OLDER}'s own --rollback (forward to A)")
        end(b, "fwd-back")
    except Exception:
        b.kill()
        raise

    # A, its root's record naming v1.0.7's set: the state the release notes
    # of 1.0.8 sent an operator into. The upgrade's gate first, on the record
    # as left; --list and --repair on it too, the record put back between.
    b = boot("fwd-again", sub)
    try:
        rec = "/var/lib/cosmo-upgrade/root-set"
        sh(b, f"cp {rec} /tmp/stale", "save")
        listed = sh(b, "cosmo-upgrade --list", "list")
        # A record that cannot be written is not corrected, and not said
        # to be: a directory where the new record is made fails its open.
        failed = sh(b, f"mkdir {rec}.cosmo-upgrade && cosmo-upgrade --repair 2>&1", "rfail", want=1)
        sh(b, f"rmdir {rec}.cosmo-upgrade && cmp {rec} /tmp/stale", "kept")
        need(failed, r"^cosmo-upgrade: the root's record names the previous set's \(v\S+, [0-9a-f]{12}\) base system, "
                     r"but its files are the current set's \(v\S+, [0-9a-f]{12}\): not corrected$",
             "--repair that cannot write the record")
        if re.search(r"record corrected|nothing more to repair", failed):
            raise Fail(f"fwd-again: --repair claimed a correction it could not write: {failed[-600:]!r}")
        repaired = sh(b, "cosmo-upgrade --repair", "rep")
        sh(b, f"cp /tmp/stale {rec} && sync", "restore")
        fetch(b, port, args.arch, "b", UPB)
        out = sh(b, f"cosmo-upgrade {UPB}", "up")
        need(out, r"^cosmo-upgrade: the root's record named the previous set's \(v\S+, [0-9a-f]{12}\) base system, "
                  r"but its files are the current set's \(v\S+, [0-9a-f]{12}\); it was written when the root moved "
                  r"from that set, and an older cosmo-upgrade \(v1\.0\.7 or before\) moved it back without updating "
                  r"it: record corrected$",
             "the upgrade correcting the record before its environment")
        env = need(out, r"^cosmo-upgrade: boot environment (be-\S+): the root as it is now, paired with the current set$",
                   "the upgrade's environment").group(1)
        need(out, r"^cosmo-upgrade: upgraded; reboot to run it", "the upgrade to B")
        m = need(listed, r"^cosmo-upgrade: the root's base system is the current set's \((v\S+), ([0-9a-f]{12})\)$",
                 "--list: what the root's files are")
        need(listed, r"^cosmo-upgrade: the root's record says the previous set's \(v\S+, [0-9a-f]{12}\); it was "
                     r"written when the root moved from that set, and an older cosmo-upgrade \(v1\.0\.7 or before\) "
                     r"moved it back without updating it: cosmo-upgrade --repair corrects it$",
             "--list: the record, and how it came to be wrong")
        need(repaired, r"^cosmo-upgrade: the root's record named the previous set's .* record corrected$",
             "--repair correcting the record")
        need(repaired, r"^cosmo-upgrade: no boot environments recorded: nothing more to repair$",
             "--repair with no intent")
        end(b, "fwd-again")
    except Exception:
        b.kill()
        raise
    b = boot("fwd-b", sub)
    try:
        if not has_b(b):
            raise Fail("fwd-b: the root does not hold B's base system after the upgrade")
        out = sh(b, f"cosmo-upgrade --list && sed -n 2p {rec}", "list")
        need(out, r"^cosmo-upgrade: the root's base system is the current set's \(", "--list on B")
        if re.search(r"^cosmo-upgrade: the root's record says", out, re.M):
            raise Fail(f"fwd-b: --list says the record is wrong: {out[-600:]!r}")
        need(out, rf"^{re.escape(env)}\s+previous\s+[0-9a-f]{{12}}\s+on the root$", "--list: A's environment")
        need(out, r"^cosmo-root-set 2$", "the record B's base pass wrote")
        end(b, "fwd-b")
    except Exception:
        b.kill()
        raise
    results.append(f"forward: {OLDER} -> A ({OLDER}'s cosmo-upgrade), A's --rollback, {OLDER}'s own --rollback "
                   f"forward to A (A is {m.group(1)}); the record left naming {OLDER}'s set: --list named both, "
                   f"--repair that could not write it failed and kept it, --repair corrected it with no intent, and with it put back the upgrade to B corrected it and "
                   f"took {env}; B booted on B's base system, its record B's; {time.monotonic() - t0:.1f}s")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--image", required=True)
    ap.add_argument("--set", required=True, help="this build's release set (set A)")
    ap.add_argument("--key", required=True, help="the build's signing key (scripts/modsign.py)")
    ap.add_argument("--arch", required=True)
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--timeout", type=float, default=240.0, help="per boot")
    ap.add_argument("--older", help=f"the published {OLDER}'s live image: the forward stage")
    ap.add_argument("--only", choices=("forward",), help="that stage alone")
    args = ap.parse_args()
    if args.only and not args.older:
        ap.error("--only forward needs --older")
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        ap.error(f"--timeout must be a finite number of seconds above 0, not {args.timeout}")
    shutil.rmtree(args.workdir, ignore_errors=True)
    os.makedirs(args.workdir)
    www = os.path.join(args.workdir, "www")
    os.makedirs(os.path.join(www, "a"))
    for n in list(SET) + ["SHA256SUMS", "SHA256SUMS.sig"]:
        src = n if n.startswith("SHA") else f"cosmoos-{args.arch}-{n}"
        shutil.copyfile(os.path.join(args.set, src), os.path.join(www, "a", src))
    seed = modsign.read_hex_key(args.key, "secret key")
    make_set_b(args.set, os.path.join(www, "b"), args.arch, seed)
    open(os.path.join(www, "trust.pub"), "w").write(modsign.secret_to_public(seed).hex() + "\n")
    srv, port = net_login_test.start_http(www)
    results = []
    t0 = time.monotonic()
    try:
        if args.older:
            stage_forward(args, port, results)
        if args.only:
            raise Done()
        inst = install_test.stage_install(args, results)
        args.target = inst["target"]

        # upgrade: A -> B, the environment taken first.
        b = boot("be-upgrade", args)
        try:
            out = sh(b, "pkg update && pkg install hello=1.0 && echo before > /etc/b4-note && mkdir -p /var/lib/cosmo && "
                        "echo in-the-way > /var/lib/cosmo/rollback && sync && hello", "prep")
            need(out, r"^hello, world \(hello 1\.0\)$", "hello 1.0 on A")
            fetch(b, port, args.arch, "b", UPB)
            out = sh(b, f"cosmo-upgrade {UPB}", "up")
            env_a = need(out, r"^cosmo-upgrade: boot environment (be-\S+): the root as it is now, paired with the "
                              r"current set$", "the environment the upgrade takes").group(1)
            need(out, rf"^cosmo-upgrade: upgraded; reboot to run it, then pkg update && pkg upgrade; --rollback "
                      rf"returns to {re.escape(env_a)}$", "the upgrade")
            end(b, "be-upgrade")
        except Exception:
            b.kill()
            raise
        results.append(f"upgrade: A -> B, environment {env_a} taken before anything was written")

        # new: B; changes; --rollback.
        b = boot("be-new", args)
        try:
            if not has_b(b):
                raise Fail("be-new: the root is not B's after the upgrade")
            out = sh(b, "cosmo-upgrade --list", "list")
            need(out, rf"^{re.escape(env_a)}\s+previous\s+[0-9a-f]{{12}}\s+on the root$", "--list: A's environment")
            out = sh(b, "pkg upgrade && hello", "pkgup")
            need(out, r"^hello, world \(hello 1\.1\)$", "pkg upgrade on B")
            if re.search(r"^pkg: no transaction", out, re.M):
                raise Fail("be-new: pkg took no transaction, though every kept set reads format 12")
            # The file in the way goes from B's tree, the one the rollback keeps:
            # it stays in A's environment, where back-held needs it, and the
            # crash-record stage's rollback to B's tree finds the way clear.
            sh(b, "rm /var/lib/cosmo/rollback && echo after > /etc/b4-note && echo x > /root/after.txt && mkdir -p /home/op && echo y > "
                  "/home/op/notes && echo z > /var/log/op.log && sync", "ops")
            out = rebooting(b, "cosmo-upgrade --rollback", "be-new")
            need(out, rf"^cosmo-upgrade: intent recorded: \\cosmo\\be names the previous set and {re.escape(env_a)}$",
                 "the rollback's intent")
            env_b = need(out, rf"^cosmo-upgrade: the root rolls back to {re.escape(env_a)} at the next boot; the tree "
                              r"it replaces is kept as (be-\S+)$", "the rollback's record").group(1)
            need(out, r"^cosmo-upgrade: rolled back: the boot set now, and the root -- packages, /etc and the base "
                      r"system as they were in ", "the rollback")
        except Exception:
            b.kill()
            raise
        results.append(f"new: B ran hello 1.1 (a transaction) and changed /etc; --rollback recorded the intent, "
                       f"the root's rollback to {env_a} (keeping {env_b}) and the set")

        # back-held: the operator's files cannot be kept; the hold stays.
        b = boot("be-back-held", args)
        try:
            log = b.text()
            held = need(log, r"^cosmo-upgrade: (\d+) of the files written under /root, /home and /var/log after "
                      rf"{re.escape(env_a)} was taken could not be copied to /var/lib/cosmo/rollback/{re.escape(env_b)} "
                      rf"\(0 were\); they are in snapshot {re.escape(env_b)}, kept for the next boot to try again$",
                 "the copy refused").group(1)
            need(log, r"^init: cosmo-upgrade --after-rollback did not keep every file; the rollback's hold on the "
                      r"tree it replaced stays, and the next boot tries again$", "init keeping the hold")
            if re.search(r"^init: boot complete: released the rollback hold", log, re.M):
                raise Fail("be-back-held: init released the hold though the files were not kept")
            sh(b, "rm /var/lib/cosmo/rollback && sync", "unblock")
            end(b, "be-back-held")
        except Exception:
            b.kill()
            raise
        results.append(f"back-held: {held} file(s) could not be copied (a file in the way of "
                       f"/var/lib/cosmo/rollback): the hold on {env_b} stayed, nothing marked done")

        # back: A on the environment's root.
        b = boot("be-back", args)
        try:
            log = b.text()
            m = need(log, rf"^cosmo-upgrade: (\d+) file\(s\) written under /root, /home and /var/log after "
                          rf"{re.escape(env_a)} was taken are kept in (/var/lib/cosmo/rollback/\S+)$",
                     "the operator's files kept, at the boot after")
            keep = m.group(2)
            need(log, r"^init: boot complete: released the rollback hold on /$", "the hold released once they are")
            if has_b(b):
                raise Fail("be-back: the root still holds B's base system")
            out = sh(b, "hello && cat /etc/b4-note && pkg list", "back")
            need(out, r"^hello, world \(hello 1\.0\)$", "hello 1.0 after the rollback")
            need(out, r"^before$", "the /etc file as it was before the upgrade")
            out = sh(b, f"cosmo-snap diff {env_a} -", "diff")
            moved = [p for p in re.findall(r"^[+\-M] (/\S*)$", out, re.M)
                     if any(p == r or p.startswith(r + "/") for r in RETURNED)]
            if moved:
                raise Fail(f"be-back: packages, /etc or the base system differ from {env_a}: {moved[:8]}")
            out = sh(b, f"cat {keep}/root/after.txt {keep}/home/op/notes {keep}/var/log/op.log", "kept")
            for want in ("x", "y", "z"):
                need(out, rf"^{want}$", f"the operator's file ({want}) in {keep}")
            check_clean(b, root_id(b), "the rollback")
            expect_pair(b, "a", "be-back")
            end(b, "be-back")
        except Exception:
            b.kill()
            raise
        results.append(f"back: A on {env_a}'s root: hello 1.0, /etc and the base system byte for byte {env_a}'s; "
                       f"{m.group(1)} operator file(s) kept in {keep}; --repair: nothing to do")

        # crash-record: forward to B, killed between the record and the set.
        b = boot("be-crash-record", args)
        try:
            out = sh(b, "cosmo-upgrade --rollback --stop-after=record", "stop", 4)
            need(out, r"^cosmo-upgrade: stopped after record \(--stop-after\); cosmo-upgrade --repair finishes it$",
                 "--stop-after=record")
            kept_a = need(out, r"^cosmo-upgrade: the root rolls back to be-\S+ at the next boot; the tree it replaces "
                               r"is kept as (be-\S+)$", "the tree the record keeps").group(1)
        finally:
            b.kill()
        b = boot("be-mixed-new", args)   # A's kernel, B's root: it boots
        try:
            if not has_b(b):
                raise Fail("be-mixed-new: the record's rollback did not apply at the mount")
            out = sh(b, "cosmo-upgrade --repair", "rep")
            need(out, r"^cosmo-upgrade: repaired; reboot to run the pairing \\cosmo\\be intends$", "--repair")
            end(b, "be-mixed-new")
        except Exception:
            b.kill()
            raise
        b = boot("be-repaired-b", args)
        try:
            expect_pair(b, "b", "be-repaired-b")
            out = sh(b, "cosmo-upgrade --list", "list")
            need(out, rf"^{re.escape(kept_a)}\s+previous\s+[0-9a-f]{{12}}\s+on the root$",
                 "--list: the tree the interrupted rollback kept, paired with A")
            end(b, "be-repaired-b")
        except Exception:
            b.kill()
            raise
        results.append(f"crash-record: killed after the root's record: A's kernel booted B's root; --repair "
                       f"switched the set; then B, B, with {kept_a} paired with A")

        # mixed-old: back to A completed, the root's rollback withdrawn.
        b = boot("be-mixed-old-prep", args)
        try:
            sh(b, "cosmo-upgrade --rollback --no-reboot", "rb")
            sh(b, "cosmo-snap rollback --cancel", "cancel")
            end(b, "be-mixed-old-prep")
        except Exception:
            b.kill()
            raise
        b = boot("be-mixed-old", args)   # A's kernel on B's root
        try:
            if not has_b(b):
                raise Fail("be-mixed-old: the root moved though its rollback was withdrawn")
            out = rebooting(b, "cosmo-upgrade --repair", "be-mixed-old")
            need(out, r"^cosmo-upgrade: the root rolls back to be-\S+ at the next boot", "--repair asking the root")
        except Exception:
            b.kill()
            raise
        b = boot("be-repaired-a", args)
        try:
            expect_pair(b, "a", "be-repaired-a")
            end(b, "be-repaired-a")
        except Exception:
            b.kill()
            raise
        results.append("mixed-old: A's kernel booted B's root (the root's rollback withdrawn); --repair asked for it "
                       "again; then A, A")

        # crash-intent: only the intent written.
        b = boot("be-crash-intent", args)
        try:
            sh(b, "cosmo-upgrade --rollback --stop-after=intent", "stop", 4)
        finally:
            b.kill()
        b = boot("be-intent", args)
        try:
            if has_b(b):
                raise Fail("be-intent: the root moved with only the intent written")
            rebooting(b, "cosmo-upgrade --repair", "be-intent")
        except Exception:
            b.kill()
            raise
        b = boot("be-intent-repaired", args)
        try:
            expect_pair(b, "b", "be-intent-repaired")
            end(b, "be-intent-repaired")
        except Exception:
            b.kill()
            raise
        results.append("crash-intent: killed after the intent: A, A booted; --repair rolled both; then B, B")

        # crash-set: killed after the set switch, before finish.
        b = boot("be-crash-set", args)
        try:
            sh(b, "cosmo-upgrade --rollback --stop-after=set", "stop", 4)
        finally:
            b.kill()
        b = boot("be-set", args)
        try:
            expect_pair(b, "a", "be-set")
            end(b, "be-set")
        except Exception:
            b.kill()
            raise
        results.append("crash-set: killed after the set switch: the next boot was A, A")

        # space: the upgrade refused on a full root, nothing written.
        b = boot("be-space", args)
        try:
            # The ESP once the firmware is done with it: x86 firmware with no
            # variable store writes \\NvVars there at every boot
            # (upgrade_test.py's refuse stage).
            before = install_test.esp_of(open(args.target, "rb").read())
            fetch(b, port, args.arch, "b", UPB)
            sh(b, "cosmo-snap create t && cosmo-snap rollback --no-reboot t", "arm")
            out = sh(b, f"cosmo-upgrade {UPB}", "pend", 3)
            need(out, r"^cosmo-upgrade: a rollback of the root is pending \(cosmo-snap list --all\): reboot to apply "
                      r"it, or cosmo-snap rollback --cancel; nothing written$", "the refusal with a rollback pending")
            sh(b, "cosmo-snap rollback --cancel && cosmo-snap delete t", "disarm")
            fill_root(b)
            out = sh(b, f"cosmo-upgrade {UPB}", "full", 3)
            m = need(out, r"^cosmo-upgrade: not enough space on the root for the boot environment: about (\d+) KiB "
                          r"needed .*, (\d+) KiB free; nothing written$", "the refusal naming the room")
            sh(b, "rm /fill* && sync", "unfill")
            end(b, "be-space")
        except Exception:
            b.kill()
            raise
        if install_test.esp_of(open(args.target, "rb").read()) != before:
            raise Fail("be-space: the refused upgrade changed the ESP")
        results.append(f"space: refused with a root rollback pending; a full root refused the upgrade ({m.group(1)} "
                       f"KiB needed, {m.group(2)} KiB free); the ESP byte for byte unchanged")

        # pending: an upgrade stopped after its set switch; --repair finishes it.
        b = boot("be-pending", args)
        try:
            fetch(b, port, args.arch, "b", UPB)   # /tmp is the boot's own
            sh(b, f"cosmo-upgrade --stop-after=set {UPB}", "stop", 4)
            out = sh(b, "cosmo-upgrade --repair", "rep")
            need(out, r"^cosmo-upgrade: an earlier run switched to the current set and was interrupted: finishing it$",
                 "--repair finishing the base pass")
            end(b, "be-pending")
        except Exception:
            b.kill()
            raise
        b = boot("be-pending-done", args)
        try:
            expect_pair(b, "b", "be-pending-done")
            end(b, "be-pending-done")
        except Exception:
            b.kill()
            raise
        results.append("pending: an upgrade stopped after its set switch; --repair finished its base pass; then B, B")
    except Done:
        pass
    except Fail as e:
        for r in results:
            print(f"test-be: {r}")
        print(f"test-be: FAIL: {e} (logs in {args.workdir})")
        return 1
    finally:
        srv.shutdown()
    for r in results:
        print(f"test-be: {r}")
    print(f"test-be: PASS in {time.monotonic() - t0:.1f}s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
