#!/usr/bin/env python3
"""scripts/publish-repo.py on a synthetic release (make host-test).

A key made for the run signs a release of two architectures whose
boot.tar files each carry repo/INDEX and one package. Checked: the first
publication (serial 1, a 30-day validity from --now, the signature, the
package, releases/TAG and LATEST); the weekly re-signing (serial 2, the
same stanzas, the releases kept); two more releases (serial 3 and 4) and
--keep 3 dropping the oldest; and refusals, each exit 1 naming why, of a
SHA256SUMS with a digest changed (its signature), a file changed, a file
missing, and a repo/INDEX signed by another key.
"""
import hashlib
import io
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import modsign  # noqa: E402
import pkgbuild  # noqa: E402

SCRIPT = os.path.join(ROOT, "scripts", "publish-repo.py")
NOW = 1792326896          # 2026-10-18T12:34:56Z
failures = 0


def check(cond, what):
    global failures
    if not cond:
        failures += 1
        print(f"test_publish_repo: FAIL: {what}")


def keypair(d, name):
    seed = os.urandom(32)
    k, p = os.path.join(d, name + ".key"), os.path.join(d, name + ".pub")
    open(k, "w").write(seed.hex() + "\n")
    open(p, "w").write(modsign.secret_to_public(seed).hex() + "\n")
    return k, p


def make_release(d, tag, key):
    os.makedirs(d)
    names = []
    for arch, loader in (("x86_64", "BOOTX64.EFI"), ("aarch64", "BOOTAA64.EFI")):
        cpk = f"{arch} hello {tag}".encode() * 50
        index = (f"name: hello\nversion: 1.1\nsummary: prints a greeting\nfile: hello-1.1.cpk\n"
                 f"size: {len(cpk)}\nsha512: {hashlib.sha512(cpk).hexdigest()}\n").encode()
        buf = io.BytesIO()
        with tarfile.open(fileobj=buf, mode="w") as t:
            for name, data in (("repo/INDEX", pkgbuild.sign(index, key)), ("repo/hello-1.1.cpk", cpk),
                               ("bin/sh", b"x")):
                ti = tarfile.TarInfo(name)
                ti.size = len(data)
                t.addfile(ti, io.BytesIO(data))
        files = {"boot.tar": buf.getvalue(), loader: b"loader", "kernel.elf": b"kernel", "cmdline": b"cmd",
                 "manifest": b"m", "cosmo-upgrade": b"u", "cosmo-upgrade.cpk": b"c", "img": b"image"}
        for f, data in files.items():
            n = f"cosmoos-{arch}.img" if f == "img" else f"cosmoos-{arch}-{f}"
            open(os.path.join(d, n), "wb").write(data)
            names.append(n)
    sums = "".join(f"{hashlib.sha256(open(os.path.join(d, n), 'rb').read()).hexdigest()}  {n}\n"
                   for n in sorted(names)).encode()
    open(os.path.join(d, "SHA256SUMS"), "wb").write(sums)
    open(os.path.join(d, "SHA256SUMS.sig"), "wb").write(modsign.detached(modsign.read_hex_key(key, "k"), sums))
    os.remove(os.path.join(d, "cosmoos-x86_64.img"))   # the images are not downloaded
    os.remove(os.path.join(d, "cosmoos-aarch64.img"))


def publish(site, out, key, pub, *extra, now=NOW):
    return subprocess.run([sys.executable, SCRIPT, "--site", site, "--out", out, "--key", key, "--pub", pub,
                           "--now", str(now), *extra], capture_output=True, text=True)


def header(path):
    payload, _ = modsign.split_trailer(open(path, "rb").read())
    return dict(line.split(": ", 1) for line in payload.decode().split("\n\n")[0].splitlines())


def main():
    w = tempfile.mkdtemp(prefix="publish-repo-")
    try:
        key, pub = keypair(w, "release")
        okey, _ = keypair(w, "other")
        pubbytes = modsign.read_hex_key(pub, "p")
        empty = os.path.join(w, "empty")
        os.makedirs(empty)
        make_release(os.path.join(w, "r106"), "v1.0.6", key)
        s1 = os.path.join(w, "s1")
        r = publish(empty, s1, key, pub, "--release", os.path.join(w, "r106"), "--tag", "v1.0.6",
                    "--published", "2026-10-18T12:00:00Z", "--latest")
        check(r.returncode == 0, f"first publication: {r.stderr.strip()}")
        idx = os.path.join(s1, "repo", "x86_64", "INDEX")
        h = header(idx)
        check(h == {"serial": "1", "published": "2026-10-18T12:34:56Z", "expires": "2026-11-17T12:34:56Z"},
              f"the first header {h}")
        blob = open(idx, "rb").read()
        check(modsign.verify_detached([pubbytes], blob[:-88], blob[-88:]) is None, "the INDEX signature")
        check(open(os.path.join(s1, "repo", "aarch64", "hello-1.1.cpk"), "rb").read().startswith(b"aarch64 hello"),
              "aarch64's package")
        check(open(os.path.join(s1, "releases", "LATEST")).read() == "tag: v1.0.6\npublished: 2026-10-18T12:00:00Z\n",
              "LATEST")
        rel = sorted(os.listdir(os.path.join(s1, "releases", "v1.0.6")))
        check(len(rel) == 16 and "SHA256SUMS.sig" in rel and "cosmoos-x86_64-boot.tar" in rel and
              not any(n.endswith(".img") for n in rel), f"releases/v1.0.6 holds {rel}")
        check(os.path.exists(os.path.join(s1, ".nojekyll")), ".nojekyll")

        s2 = os.path.join(w, "s2")
        r = publish(s1, s2, key, pub, now=NOW + 7 * 86400)
        check(r.returncode == 0, f"re-signing: {r.stderr.strip()}")
        h2 = header(os.path.join(s2, "repo", "x86_64", "INDEX"))
        check(h2["serial"] == "2" and h2["expires"] == "2026-11-24T12:34:56Z", f"the re-signed header {h2}")
        check(pkgbuild.index_stanzas(open(os.path.join(s2, "repo", "x86_64", "INDEX"), "rb").read()) ==
              pkgbuild.index_stanzas(blob), "the re-signed stanzas")
        check(os.path.exists(os.path.join(s2, "releases", "v1.0.6", "SHA256SUMS")) and
              os.path.exists(os.path.join(s2, "releases", "LATEST")), "the releases kept by re-signing")

        site = s2
        for n, tag in enumerate(("v1.0.7", "v1.0.8", "v1.0.9"), start=3):
            make_release(os.path.join(w, tag), tag, key)
            out = os.path.join(w, "s" + tag)
            r = publish(site, out, key, pub, "--release", os.path.join(w, tag), "--tag", tag,
                        "--published", "2026-10-19T00:00:00Z", "--latest")
            check(r.returncode == 0, f"{tag}: {r.stderr.strip()}")
            check(header(os.path.join(out, "repo", "aarch64", "INDEX"))["serial"] == str(n), f"{tag}'s serial")
            site = out
        check(sorted(os.listdir(os.path.join(site, "releases"))) == ["LATEST", "v1.0.7", "v1.0.8", "v1.0.9"],
              f"--keep 3 left {sorted(os.listdir(os.path.join(site, 'releases')))}")
        check(open(os.path.join(site, "releases", "LATEST")).read().startswith("tag: v1.0.9\n"), "LATEST moved")

        bad = os.path.join(w, "bad")
        for what, change in (("a changed digest", "sums"), ("a changed file", "file"), ("a missing file", "missing"),
                             ("an INDEX signed by another key", "index")):
            shutil.rmtree(bad, ignore_errors=True)
            make_release(bad, "v2.0.0", okey if change == "index" else key)
            if change == "sums":
                p = os.path.join(bad, "SHA256SUMS")
                t = open(p).read()
                open(p, "w").write(("1" if t[0] == "0" else "0") + t[1:])
            elif change == "file":
                open(os.path.join(bad, "cosmoos-x86_64-cmdline"), "ab").write(b" ")
            elif change == "missing":
                os.remove(os.path.join(bad, "cosmoos-aarch64-kernel.elf"))
            elif change == "index":
                # SHA256SUMS signed by the release key, the archives' INDEX by another
                sums = open(os.path.join(bad, "SHA256SUMS"), "rb").read()
                open(os.path.join(bad, "SHA256SUMS.sig"), "wb").write(
                    modsign.detached(modsign.read_hex_key(key, "k"), sums))
            r = publish(site, os.path.join(w, "sbad"), key, pub, "--release", bad, "--tag", "v2.0.0",
                        "--published", "2026-10-20T00:00:00Z", "--latest")
            reason = {"sums": "SHA256SUMS.sig: bad signature", "file": "cosmoos-x86_64-cmdline does not match SHA256SUMS",
                      "missing": "was not downloaded",
                      "index": "repo/INDEX: unknown signing key"}[change]
            check(r.returncode == 1 and reason in r.stderr, f"{what} refused ({reason}): {r.stderr.strip()}")
    finally:
        shutil.rmtree(w, ignore_errors=True)
    if failures:
        print(f"test_publish_repo: {failures} failure(s)")
        return 1
    print("test_publish_repo: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
