#!/usr/bin/env python3
"""Roadmap 1.1 P6's acceptance test: pkg against network repositories
(docs/pkg/testing.md, "test-pkg-net").

    pkgnet_test.py --image cosmoos.img --repo PKG_REPO --sign-key KEY --workdir DIR

The live image installs itself on a blank disk (install_test.stage_install);
the installed system then boots on QEMU user networking, and this harness
serves package repositories over HTTPS from 127.0.0.1 (the guest's
10.0.2.2) with test certificates made for the run (tests/boot/tlstest.py).
The test CA is appended to the installed bundle, /etc/ssl/certs/
ca-certificates.crt, for this boot only. Every served INDEX is the build's
(--repo, signed with --sign-key) made into a network one by
`pkgbuild.py publish` (a serial, a publication time, an expiry).

  c. The repository behind an untrusted CA's certificate, an expired one
     and one for another name: `pkg update` exits 3, cosmo-fetch names the
     reason, and the server received no request at all (no GET of INDEX).
  a. Without sys_admin (priv -d) `pkg update`, `search` and `info` work and
     `pkg install` is refused. Then `pkg update && pkg install hello` from
     the index with serial 10 (hello 1.0 alone) installs and runs it,
     fetched from the server (GET INDEX, then the package).
  f. The served index changes to serial 11 (hello 1.0 and 1.1):
     `pkg update && pkg upgrade` moves hello from 1.0 to 1.1. With a
     second repository nothing answers for, `pkg update` reports it and
     exits 0.
  d. The serial-10 index served again (a replay) is refused, and so is a
     serial-12 index that expired; the stored index is still serial 11.
  b. A repository whose hello-1.1.cpk has one byte changed (its index
     intact) is refused at install, hello 1.1 still verified; one whose
     INDEX is signed by another key is refused at update.
  e. cosmo-upgrade --check against a served release index (LATEST)
     naming v9.9.9 reports it newer, and naming the running release
     reports this system up to date; `cosmo-upgrade --check --fetch URL`
     fetches this build's release set (--set) over HTTPS, verifies it and
     finds it already current; a set whose SHA256SUMS has one digest
     changed is refused before any of its files is fetched.
  g. The real published repository (GitHub Pages): with the installer's
     repos.conf and the default release index, `pkg update` accepts the
     network repository beside the disk's (three attempts: this needs the
     runner's internet), and `cosmo-upgrade --check` reads its LATEST.

Exit 0 PASS, 1 FAIL.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import install_test  # noqa: E402  (Boot, Fail, need, stage_install)
import tlstest  # noqa: E402
from install_test import Boot, Fail, need  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PKGBUILD = os.path.join(ROOT, "tools", "pkgbuild.py")
DAY = 86400
PUBLISHED = "https://vkuttyp.github.io/CosmoOS"   # the project's (docs/guide/packages.md)


def utc(t):
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(t))


class Repos:
    """The served directory: one subdirectory per repository, each with an
    INDEX made by `pkgbuild.py publish` and its packages."""

    def __init__(self, www, built, key):
        self.www, self.built, self.key = www, built, key

    def build_pub(self):
        sys.path.insert(0, os.path.join(ROOT, "scripts"))
        import modsign
        p = os.path.join(self.www, "..", "build.pub")
        with open(p, "w") as f:
            f.write(modsign.secret_to_public(modsign.read_hex_key(self.key, "secret key")).hex() + "\n")
        return p

    def publish(self, name, serial, only, key=None, published=None, days=30):
        """`key` signs it instead of the build's; the input is still the
        build's INDEX, checked against the build key's public half."""
        d = os.path.join(self.www, name)
        os.makedirs(d, exist_ok=True)
        for f in only:
            shutil.copy(os.path.join(self.built, f), os.path.join(d, f))
        argv = [sys.executable, PKGBUILD, "publish", "--index", os.path.join(self.built, "INDEX"),
                "--sign-key", key or self.key, "--serial", str(serial), "--days", str(days),
                "--out", os.path.join(d, "INDEX")]
        if published is not None:
            argv += ["--published", utc(published)]
        if key:
            argv += ["--input-pub", self.build_pub()]
        for f in only:
            argv += ["--only", f]
        r = subprocess.run(argv, capture_output=True, text=True)
        if r.returncode != 0:
            raise Fail(f"pkgbuild.py publish: {r.stderr.strip()[-400:]}")
        return d


def conf(b, line):
    out = b.status(f"echo '{line}' > /etc/pkg/repos.conf", "conf")
    need(out, r"^conf=0$", f"writing repos.conf ({line})")


def stage_pkgnet(args, results, inst):
    work = args.workdir
    tls, www = os.path.join(work, "tls"), os.path.join(work, "www")
    tlstest.make_test_certs(tls, ip="10.0.2.2")
    os.makedirs(www, exist_ok=True)
    shutil.copy(os.path.join(tls, "ca.pem"), os.path.join(www, "testca.pem"))
    repos = Repos(www, args.repo, args.sign_key)
    other_key = os.path.join(work, "other.key")
    with open(other_key, "w") as f:
        f.write(os.urandom(32).hex() + "\n")

    now = int(time.time())
    repos.publish("repo", 10, ["hello-1.0.cpk"], published=now - DAY)
    servers = {n: tlstest.Server(www, tls, n) for n in ("server", "untrusted", "expired", "wrongname")}
    plain = tlstest.Server(www)
    good = servers["server"]
    url = f"https://10.0.2.2:{good.port}"
    b = Boot("pkgnet", inst["target"], work, {"QEMU_DISKS": "boot"}, args.timeout)
    try:
        b.wait_prompt("the first prompt")
        need(b.text(), r"^init: switched to the disk root \S+ ", "the installed boot")
        # The shipped configuration, kept for step g, which puts these
        # very files back rather than writing what it expects.
        out = b.status("cat /etc/pkg/repos.conf /etc/cosmo-release.conf && "
                       "cp /etc/pkg/repos.conf /tmp/repos.conf.installed && "
                       "cp /etc/cosmo-release.conf /tmp/cosmo-release.conf.installed", "keep")
        need(out, r"^keep=0$", "keeping the installed repos.conf and cosmo-release.conf")
        need(out, rf"^cosmoos {re.escape(PUBLISHED)}/repo/{args.arch}$", "the installed repos.conf's network line")
        need(out, rf"^url {re.escape(PUBLISHED)}/releases$", "the installed cosmo-release.conf")
        out = b.status(f"wget -q -O /tmp/testca.pem http://10.0.2.2:{plain.port}/testca.pem && "
                       "cat /tmp/testca.pem >> /etc/ssl/certs/ca-certificates.crt", "ca")
        need(out, r"^ca=0$", "adding the test CA to the installed bundle")

        # c. Refused in the handshake: no request reaches the server.
        refused = []
        for name, why, reason in (("untrusted", "an untrusted CA", r"not correctly signed by the trusted CA"),
                                  ("expired", "an expired certificate", r"validity has expired"),
                                  ("wrongname", "another name", r"Common Name \(CN\) does not match")):
            srv = servers[name]
            conf(b, f"bad https://10.0.2.2:{srv.port}/repo")
            out = b.status("pkg update", "upd")
            need(out, r"^upd=3$", f"pkg update behind {why} (exit 3)")
            need(out, r"^cosmo-fetch: 10\.0\.2\.2: the server's certificate is not accepted:$",
                 f"cosmo-fetch's refusal of {why}")
            need(out, reason, f"the reason for refusing {why}")
            need(out, r"^pkg: bad: INDEX refused \(above\); nothing read from it$", f"pkg's refusal of {why}")
            if srv.requests:
                raise Fail(f"the server behind {why} received {srv.paths()}; nothing should reach it")
            refused.append(why)
        results.append(f"c. refused before any request (the servers logged none): {', '.join(refused)}")

        # a. Privileges, then install.
        conf(b, f"test {url}/repo")
        out = b.status("priv -d sys_admin pkg update", "upd")
        need(out, r"^upd=0$", "pkg update without sys_admin")
        need(out, r"^pkg: test: serial 10, published \S+, expires \S+$", "the accepted index's header")
        need(out, r"^pkg: index updated: 1 packages from 1 repository$", "pkg update's count")
        out = b.status("priv -d sys_admin pkg search hello && priv -d sys_admin pkg info hello", "q")
        need(out, r"^q=0$", "pkg search and info without sys_admin")
        need(out, r"^available: 1\.0 \(hello-1\.0\.cpk, \d+ bytes\)$", "pkg info hello")
        out = b.status("priv -d sys_admin pkg install hello", "inst")
        need(out, r"^inst=1$", "pkg install without sys_admin (exit 1)")
        need(out, r"^pkg: install needs the sys_admin privilege \(this process lacks it\)$",
             "the privilege refusal")
        del good.requests[:]
        out = b.status("pkg install -n hello", "dry")
        need(out, r"^dry=0$", "pkg install -n hello")
        need(out, r"^pkg: installing hello-1\.0 \(from test, not fetched: -n\)$", "the dry run's plan")
        if good.requests:
            raise Fail(f"pkg install -n fetched {good.paths()}; a dry run fetches nothing")
        out = b.status("pkg update && pkg install hello && hello", "inst")
        need(out, r"^inst=0$", "pkg update && pkg install hello && hello")
        need(out, r"^pkg: fetching hello-1\.0\.cpk from test$", "the package fetched")
        need(out, r"^pkg: installing hello-1\.0 \(\d+ files?\)$", "hello 1.0 installed")
        need(out, r"^hello, world \(hello 1\.0\)$", "hello 1.0 runs")
        if good.paths() != ["/repo/INDEX", "/repo/hello-1.0.cpk"]:
            raise Fail(f"the server saw {good.paths()}, expected INDEX then hello-1.0.cpk")
        results.append("a. update, search and info without sys_admin, install refused without it; install -n fetched nothing; "
                       "pkg update && pkg install hello fetched INDEX (serial 10) and hello-1.0.cpk, "
                       "'hello, world (hello 1.0)'")

        # f. A newer index: upgrade.
        repos.publish("repo", 11, ["hello-1.0.cpk", "hello-1.1.cpk"], published=now - DAY)
        out = b.status("pkg update && pkg upgrade && hello", "up")
        need(out, r"^up=0$", "pkg update && pkg upgrade && hello")
        need(out, r"^pkg: test: serial 11, ", "the serial-11 index accepted")
        need(out, r"^pkg: installing hello-1\.1 \(\d+ files?\)$", "hello 1.1 installed")
        need(out, r"^pkg: 1 package upgraded$", "pkg upgrade's count")
        need(out, r"^hello, world \(hello 1\.1\)$", "hello 1.1 runs")
        # A second repository that cannot be reached (nothing listens on
        # port 9): reported, and the update still succeeds from the first.
        out = b.status(f"printf 'test {url}/repo\\ngone https://10.0.2.2:9/repo\\n' > /etc/pkg/repos.conf && "
                       "pkg update", "upd")
        need(out, r"^upd=0$", "pkg update with one repository unreachable (exit 0)")
        need(out, r"^pkg: gone: not reached; its last index is kept$", "the unreachable repository reported")
        need(out, r"^pkg: index updated: 2 packages from 1 of 2 repositories$", "the reachable one updated")
        conf(b, f"test {url}/repo")
        results.append("f. serial 11 served: pkg upgrade moved hello 1.0 -> 1.1; a second, unreachable repository "
                       "was reported and the update still exited 0")

        # d. Freshness: a replay and an expired index.
        repos.publish("repo", 10, ["hello-1.0.cpk"], published=now - DAY)
        out = b.status("pkg update", "upd")
        need(out, r"^upd=3$", "a replayed serial-10 index (exit 3)")
        need(out, r"^pkg: test: INDEX: serial 10 is older than 11, the last accepted from this repository "
                  r"\(a replayed index\)$", "the replay's refusal")
        repos.publish("repo", 12, ["hello-1.0.cpk", "hello-1.1.cpk"], published=now - 40 * DAY, days=7)
        out = b.status("pkg update", "upd")
        need(out, r"^upd=3$", "an expired serial-12 index (exit 3)")
        m = need(out, r"^pkg: test: INDEX: expired at (\S+)$", "the expired index's refusal")
        out = b.status("pkg info hello", "info")
        need(out, r"^available: 1\.1 \(hello-1\.1\.cpk, \d+ bytes\)$", "the stored index after the refusals")
        results.append(f"d. refused: the serial-10 index replayed after 11, a serial-12 index expired at "
                       f"{m.group(1)}; the stored index stayed serial 11")

        # b. A changed package and an index signed by another key.
        tam = repos.publish("tam", 1, ["hello-1.1.cpk"], published=now - DAY)
        p = os.path.join(tam, "hello-1.1.cpk")
        data = bytearray(open(p, "rb").read())
        data[len(data) // 2] ^= 0x01
        open(p, "wb").write(bytes(data))
        conf(b, f"tam {url}/tam")
        out = b.status("pkg update && pkg install --reinstall hello", "tam")
        need(out, r"^tam=3$", "installing a changed package (exit 3)")
        need(out, r"^pkg: /var/cache/pkg/tam/hello-1\.1\.cpk: checksum does not match the index "
                  r"\(corrupt or replaced\)$", "the changed package's refusal")
        out = b.status("hello && pkg verify", "still")
        need(out, r"^still=0$", "hello after the refusal")
        need(out, r"^hello, world \(hello 1\.1\)$", "hello 1.1 untouched")
        need(out, r"^pkg: verify: 0 problems$", "pkg verify after the refusal")
        repos.publish("other", 13, ["hello-1.1.cpk"], key=other_key, published=now - DAY)
        conf(b, f"other {url}/other")
        out = b.status("pkg update", "other")
        need(out, r"^other=3$", "an index signed by another key (exit 3)")
        need(out, r"^pkg: other: INDEX: unknown signing key [0-9a-f]{16} \(not in /etc/pkg/keys\)$",
             "the other key's refusal")
        results.append("b. refused: hello-1.1.cpk with one byte changed (checksum does not match the index; "
                       "hello 1.1 still verified), an INDEX signed by another key (unknown signing key)")

        stage_upgrade_check(args, b, results, www, url, good)
        stage_published(args, b, results)

        rc = b.finish()
        if rc != install_test.EXIT_SUCCESS:
            raise Fail(f"pkgnet boot: QEMU exit code {rc}")
    except Exception:
        b.kill()
        raise
    finally:
        for s in list(servers.values()) + [plain]:
            s.shutdown()
    results.append(f"pkgnet boot: PASS in {b.elapsed():.1f}s")


def stage_upgrade_check(args, b, results, www, url, good):
    """e. cosmo-upgrade --check and --fetch against what this harness serves."""
    rel = os.path.join(www, "releases")
    os.makedirs(rel, exist_ok=True)
    out = b.status(f"echo 'url {url}/releases' > /etc/cosmo-release.conf", "rc")
    need(out, r"^rc=0$", "writing /etc/cosmo-release.conf")
    out = b.run("cat /proc/version")
    running = need(out, r"^Linux version .* (\S+)$", "/proc/version").group(1)
    with open(os.path.join(rel, "LATEST"), "w") as f:
        f.write(f"tag: v9.9.9\npublished: {utc(time.time())}\n")
    out = b.status("cosmo-upgrade --check", "chk")
    need(out, r"^chk=0$", "cosmo-upgrade --check")
    need(out, rf"^cosmo-upgrade: v9\.9\.9 is newer than this system's {re.escape(running)} \(published \S+\): "
              r"cosmo-upgrade --fetch v9\.9\.9$", "a newer release reported")
    # No privilege is needed (the inventory records why not a uid other than 0:
    # an installed system's /tmp is mounted 0755).
    out = b.status("priv -d all cosmo-upgrade --check", "chk")
    need(out, r"^chk=0$", "cosmo-upgrade --check with every privilege dropped")
    need(out, r"^cosmo-upgrade: v9\.9\.9 is newer than ", "a newer release reported without privileges")
    base = re.match(r"^(v\d+\.\d+\.\d+(?:-rc\d+)?)", running)
    if base:
        with open(os.path.join(rel, "LATEST"), "w") as f:
            f.write(f"tag: {base.group(1)}\npublished: {utc(time.time())}\n")
        out = b.status("cosmo-upgrade --check", "chk")
        need(out, r"^chk=0$", "cosmo-upgrade --check (current)")
        need(out, rf"^cosmo-upgrade: this system's {re.escape(running)} is up to date: the newest release at "
                  rf"\S+ is {re.escape(base.group(1))}$", "the running release reported up to date")
    else:
        raise Fail(f"the running build id '{running}' is not git describe's vX.Y.Z form")

    names = [f"cosmoos-{args.arch}-{n}" for n in ("kernel.elf", "boot.tar", "cmdline", "manifest")]
    for d, sums_change in (("set", False), ("setbad", True)):
        os.makedirs(os.path.join(www, d), exist_ok=True)
        for n in names + ["SHA256SUMS", "SHA256SUMS.sig"]:
            shutil.copy(os.path.join(args.set, n), os.path.join(www, d, n))
        if sums_change:
            p = os.path.join(www, d, "SHA256SUMS")
            lines = open(p).read().splitlines()
            lines[0] = ("0" if lines[0][0] != "0" else "1") + lines[0][1:]
            open(p, "w").write("\n".join(lines) + "\n")
    del good.requests[:]
    out = b.status(f"cosmo-upgrade --check --fetch {url}/set", "fetch")
    need(out, r"^fetch=0$", "cosmo-upgrade --check --fetch")
    need(out, r"^cosmo-upgrade: verified SHA256SUMS, signed by key [0-9a-f]{16} in /etc/pkg/keys$",
         "the fetched SHA256SUMS verified")
    need(out, r"^cosmo-upgrade: fetched the set's four files, each matching SHA256SUMS$", "the set fetched")
    need(out, r"^cosmo-upgrade: check: this set is already current; the ESP is \S+$", "the fetched set checked")
    got = good.paths()
    want = [f"/set/{n}" for n in ["SHA256SUMS", "SHA256SUMS.sig"] + names]
    if got != want:
        raise Fail(f"cosmo-upgrade --fetch asked for {got}, expected {want}")
    del good.requests[:]
    out = b.status(f"cosmo-upgrade --check --fetch {url}/setbad", "fetch")
    need(out, r"^fetch=3$", "a set with a changed SHA256SUMS (exit 3)")
    need(out, r"^cosmo-upgrade: \S+/setbad: SHA256SUMS: bad signature \(key \S+\); nothing more fetched, "
              r"nothing written$",
         "the changed SHA256SUMS refused")
    if any(not p.endswith(("/SHA256SUMS", "/SHA256SUMS.sig")) for p in good.paths()):
        raise Fail(f"files were fetched after a refused SHA256SUMS: {good.paths()}")
    results.append(f"e. cosmo-upgrade --check: v9.9.9 newer than {running} (also with every privilege dropped), "
                   f"{base.group(1)} up to date; "
                   f"--check --fetch fetched and verified this build's set (already current); a changed "
                   f"SHA256SUMS refused with nothing more fetched")


def stage_published(args, b, results):
    """g. The real published repository and release index (GitHub Pages),
    through QEMU's user networking and the runner's internet: the
    installed repos.conf and cosmo-release.conf, as kept at the start, put back,
    `pkg update` must accept both repositories (three attempts), and
    `cosmo-upgrade --check` must name the published LATEST."""
    out = b.status("cp /tmp/repos.conf.installed /etc/pkg/repos.conf && "
                   "cp /tmp/cosmo-release.conf.installed /etc/cosmo-release.conf", "rc")
    need(out, r"^rc=0$", "the installed repos.conf and cosmo-release.conf put back")
    last = ""
    for attempt in range(1, 4):
        out = b.status("pkg update", "upd")
        if re.search(r"^upd=0$", out, re.M) and re.search(r"^pkg: index updated: \d+ packages from 2 repositories$",
                                                        out, re.M):
            break
        last = out.strip()[-800:]
        time.sleep(5)
    else:
        raise Fail(f"pkg update against {PUBLISHED}: three attempts failed; the last said:\n{last}")
    m = need(out, r"^pkg: cosmoos: serial (\d+), published (\S+), expires (\S+)$", "the published index's header")
    for tries in range(1, 4):   # the runner's internet again
        chk = b.status("cosmo-upgrade --check", "chk")
        if re.search(r"^chk=0$", chk, re.M):
            break
        time.sleep(5)
    need(chk, r"^chk=0$", "cosmo-upgrade --check against the published release index (three attempts)")
    rel = need(chk, rf"^cosmo-upgrade: (?:(v\S+) is newer than this system's \S+|this system's \S+ is up to date: "
                    rf"the newest release at {re.escape(PUBLISHED)}/releases is (v\S+))", "the published LATEST")
    results.append(f"g. {PUBLISHED}: pkg update accepted repo/{args.arch} (serial {m.group(1)}, published "
                   f"{m.group(2)}, expires {m.group(3)}) beside the disk's (attempt {attempt}); cosmo-upgrade --check "
                   f"read LATEST {rel.group(1) or rel.group(2)}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--image", required=True)
    ap.add_argument("--repo", required=True, help="the build's package repository (PKG_REPO)")
    ap.add_argument("--sign-key", required=True, help="the key the build signed it with (PKGSIGN_KEY)")
    ap.add_argument("--set", required=True, help="this build's release set (make release-set)")
    ap.add_argument("--arch", required=True)
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--timeout", type=float, default=240.0, help="per boot")
    args = ap.parse_args()
    shutil.rmtree(args.workdir, ignore_errors=True)
    os.makedirs(args.workdir)
    results = []
    try:
        inst = install_test.stage_install(args, results)
        stage_pkgnet(args, results, inst)
    except (Fail, tlstest.CertError) as e:
        for r in results:
            print(f"test-pkg-net: {r}")
        print(f"test-pkg-net: FAIL: {e} (logs in {args.workdir})")
        return 1
    for r in results:
        print(f"test-pkg-net: {r}")
    print("test-pkg-net: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
