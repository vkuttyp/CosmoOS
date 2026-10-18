#!/usr/bin/env python3
"""publish-repo.py: the site GitHub Pages serves from the `repo` branch
(roadmap 1.1 P6; docs/development.md, "Releases"; the workflow is
.github/workflows/repository.yml).

    publish-repo.py --site DIR --out DIR --key KEY --pub PUB
                    [--release DIR --tag TAG --published T [--latest]] [--keep N]

--site is the branch as it stands (empty the first time), --out the new
site, written from scratch. The site:

    repo/ARCH/INDEX, *.cpk      the package repository of each architecture:
                                INDEX made a network index (PK10:
                                serial, published, expires) and signed
                                with KEY
    releases/TAG/...            each release's SHA256SUMS, SHA256SUMS.sig
                                and the files cosmo-upgrade --fetch reads
                                (kernel.elf, boot.tar, cmdline, manifest),
                                with the loader, cosmo-upgrade and its
                                package; not the images
    releases/LATEST             "tag: TAG" and "published: T" of the
                                newest release, for cosmo-upgrade --check

With --release, DIR holds the GitHub release TAG's assets: SHA256SUMS is
checked against its signature and PUB, every file used against SHA256SUMS,
and each architecture's repository is taken from the repo/ of its
boot.tar (an INDEX signed by PUB's key); --latest also points LATEST at
it. Without --release, every INDEX on the site is signed again with a new
expiry (the weekly job). Either way each INDEX's serial is the site's one
plus 1, so it rises with every publication (PK10). Only the newest --keep
releases (default 3) stay under releases/.
"""
import argparse
import hashlib
import os
import re
import shutil
import sys
import tarfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import modsign  # noqa: E402
import pkgbuild  # noqa: E402

ARCHES = ("x86_64", "aarch64")
SET = ("kernel.elf", "boot.tar", "cmdline", "manifest", "cosmo-upgrade", "cosmo-upgrade.cpk")
LOADERS = {"x86_64": "BOOTX64.EFI", "aarch64": "BOOTAA64.EFI"}
TAG_RE = re.compile(r"^v(\d+)\.(\d+)\.(\d+)(?:-rc(\d+))?$")


class Fail(Exception):
    pass


def tag_key(tag):
    m = TAG_RE.match(tag)
    if not m:
        return None
    a, b, c, rc = m.groups()
    return (int(a), int(b), int(c), int(rc) if rc else 1 << 30)


def check_release(rel, pub):
    """SHA256SUMS, its signature, and the digests of the files present."""
    sums = open(os.path.join(rel, "SHA256SUMS"), "rb").read()
    why = modsign.verify_detached([modsign.read_hex_key(pub, "public key")], sums,
                                  open(os.path.join(rel, "SHA256SUMS.sig"), "rb").read())
    if why:
        raise Fail(f"SHA256SUMS.sig: {why}")
    want = {}
    for line in sums.decode().splitlines():
        digest, _, name = line.partition("  ")
        want[name] = digest
    for arch in ARCHES:
        for n in [f"cosmoos-{arch}-{f}" for f in SET + (LOADERS[arch],)]:
            p = os.path.join(rel, n)
            if n not in want:
                raise Fail(f"{n} is not in SHA256SUMS")
            if not os.path.exists(p):
                raise Fail(f"{n} was not downloaded")
            if hashlib.sha256(open(p, "rb").read()).hexdigest() != want[n]:
                raise Fail(f"{n} does not match SHA256SUMS")
    return want


def repo_from_archive(archive, pub):
    """{name: bytes} of the repo/ members of a boot.tar; its INDEX's
    signature must be PUB's."""
    files = {}
    with tarfile.open(archive) as t:
        for m in t.getmembers():
            if m.isfile() and m.name.startswith("repo/") and "/" not in m.name[5:]:
                files[m.name[5:]] = t.extractfile(m).read()
    if "INDEX" not in files:
        raise Fail(f"{archive} has no repo/INDEX")
    # The trailer is a detached signature's form at the end of the file.
    blob = files["INDEX"]
    why = modsign.verify_detached([modsign.read_hex_key(pub, "public key")], blob[:-modsign.TRAILER.size],
                                  blob[-modsign.TRAILER.size:])
    if why:
        raise Fail(f"{archive}: repo/INDEX: {why}")
    return files


def site_serial(site, arch):
    p = os.path.join(site, "repo", arch, "INDEX")
    return pkgbuild.index_serial(open(p, "rb").read()) if os.path.exists(p) else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--site", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--key", required=True)
    ap.add_argument("--pub", required=True)
    ap.add_argument("--release")
    ap.add_argument("--tag")
    ap.add_argument("--published")
    ap.add_argument("--latest", action="store_true")
    ap.add_argument("--keep", type=int, default=3)
    ap.add_argument("--now", type=int, help="the time to publish at (tests)")
    a = ap.parse_args()
    now = a.now if a.now is not None else int(time.time())
    if a.release and (not a.tag or not tag_key(a.tag) or not a.published):
        raise Fail("--release needs --tag vX.Y.Z[-rcN] and --published")
    if a.release:
        check_release(a.release, a.pub)   # before anything is written
    if os.path.exists(a.out):
        shutil.rmtree(a.out)
    os.makedirs(a.out)
    rel_dir = os.path.join(a.site, "releases")
    if os.path.isdir(rel_dir):
        shutil.copytree(rel_dir, os.path.join(a.out, "releases"))
    os.makedirs(os.path.join(a.out, "releases"), exist_ok=True)

    for arch in ARCHES:
        serial = site_serial(a.site, arch) + 1
        dest = os.path.join(a.out, "repo", arch)
        os.makedirs(dest)
        if a.release:
            files = repo_from_archive(os.path.join(a.release, f"cosmoos-{arch}-boot.tar"), a.pub)
        else:
            src = os.path.join(a.site, "repo", arch)
            if not os.path.isdir(src):
                raise Fail(f"the site has no repo/{arch} to sign again")
            files = {f: open(os.path.join(src, f), "rb").read() for f in os.listdir(src)}
        for name, data in files.items():
            if name != "INDEX":
                with open(os.path.join(dest, name), "wb") as f:
                    f.write(data)
        blob, n, expires = pkgbuild.published_index(files["INDEX"], serial, now, pkgbuild.MAX_DAYS, a.key)
        with open(os.path.join(dest, "INDEX"), "wb") as f:
            f.write(blob)
        listed = {pkgbuild.stanza_field(st, "file") for st in pkgbuild.index_stanzas(blob)}
        for name in os.listdir(dest):
            if name != "INDEX" and name not in listed:
                os.remove(os.path.join(dest, name))
        missing = listed - set(os.listdir(dest))
        if missing:
            raise Fail(f"repo/{arch}/INDEX names files the repository lacks: {', '.join(sorted(missing))}")
        print(f"publish-repo: repo/{arch}: {n} packages, serial {serial}, expires {expires}")

    if a.release:
        d = os.path.join(a.out, "releases", a.tag)
        if os.path.exists(d):
            shutil.rmtree(d)
        os.makedirs(d)
        for n in ["SHA256SUMS", "SHA256SUMS.sig"] + [f"cosmoos-{arch}-{f}" for arch in ARCHES
                                                     for f in SET + (LOADERS[arch],)]:
            shutil.copy(os.path.join(a.release, n), os.path.join(d, n))
        print(f"publish-repo: releases/{a.tag}: SHA256SUMS checked against {os.path.basename(a.pub)}")
        if a.latest:
            with open(os.path.join(a.out, "releases", "LATEST"), "w") as f:
                f.write(f"tag: {a.tag}\npublished: {a.published}\n")
            print(f"publish-repo: releases/LATEST: {a.tag}")
    tags = sorted((t for t in os.listdir(os.path.join(a.out, "releases")) if tag_key(t)), key=tag_key)
    for t in tags[:-a.keep] if a.keep > 0 else []:
        shutil.rmtree(os.path.join(a.out, "releases", t))
        print(f"publish-repo: releases/{t}: dropped (keeping the newest {a.keep})")
    with open(os.path.join(a.out, ".nojekyll"), "w"):
        pass
    with open(os.path.join(a.out, "README.md"), "w") as f:
        f.write("# CosmoOS package repository and releases\n\n"
                "Made by `.github/workflows/repository.yml` (`scripts/publish-repo.py`); every commit replaces\n"
                "the last. Served by GitHub Pages at https://vkuttyp.github.io/CosmoOS/ (docs/guide/packages.md).\n")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (Fail, OSError, tarfile.TarError) as e:
        print(f"publish-repo: {e}", file=sys.stderr)
        sys.exit(1)
