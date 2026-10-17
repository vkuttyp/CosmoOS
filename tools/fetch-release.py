#!/usr/bin/env python3
"""Fetch one architecture's published release set and check it.

    fetch-release.py --tag v1.0.3 --arch x86_64 --dest DIR

Downloads SHA256SUMS and every cosmoos-ARCH* asset of the GitHub release
TAG of this repository into DIR, and checks each against SHA256SUMS.
Files already in DIR that match are kept, so DIR is a cache. Shell
completeness S7's acceptance test (make test-upgrade) starts from the
release this way: the real, release-signed v1.0.3, not a rebuild.

Uses `gh release download` when gh is installed and authenticated, else the
REST API with GH_TOKEN or GITHUB_TOKEN (the repository is private). Exit 0
with every file checked, else 1.
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import urllib.parse
import urllib.request

REPO = os.environ.get("GITHUB_REPOSITORY", "vkuttyp/CosmoOS")


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


class SameHostAuth(urllib.request.HTTPRedirectHandler):
    """An asset download redirects to storage on another host, which
    refuses a request carrying the token: drop it when the host changes."""

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        new = super().redirect_request(req, fp, code, msg, headers, newurl)
        if new is not None and urllib.parse.urlsplit(newurl).netloc != urllib.parse.urlsplit(req.full_url).netloc:
            new.remove_header("Authorization")
        return new


def api(url, token, accept="application/vnd.github+json"):
    req = urllib.request.Request(url, headers={"Accept": accept, "Authorization": f"Bearer {token}",
                                               "X-GitHub-Api-Version": "2022-11-28"})
    return urllib.request.build_opener(SameHostAuth).open(req, timeout=120)


def fetch(tag, names, dest):
    if shutil.which("gh") and subprocess.run(["gh", "auth", "status"], capture_output=True).returncode == 0:
        args = ["gh", "release", "download", tag, "--repo", REPO, "--dir", dest, "--clobber"]
        for n in names:
            args += ["--pattern", n]
        subprocess.run(args, check=True)
        return
    token = os.environ.get("GH_TOKEN") or os.environ.get("GITHUB_TOKEN")
    if not token:
        sys.exit("fetch-release: neither an authenticated gh nor GH_TOKEN/GITHUB_TOKEN")
    with api(f"https://api.github.com/repos/{REPO}/releases/tags/{tag}", token) as r:
        assets = {a["name"]: a["url"] for a in json.load(r)["assets"]}
    for n in names:
        if n not in assets:
            sys.exit(f"fetch-release: {tag} has no asset {n}")
        tmp = os.path.join(dest, n + ".part")
        with api(assets[n], token, "application/octet-stream") as r, open(tmp, "wb") as f:
            shutil.copyfileobj(r, f)
        os.replace(tmp, os.path.join(dest, n))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tag", required=True)
    ap.add_argument("--arch", choices=("x86_64", "aarch64"), required=True)
    ap.add_argument("--dest", required=True)
    a = ap.parse_args()
    os.makedirs(a.dest, exist_ok=True)
    sums_path = os.path.join(a.dest, "SHA256SUMS")
    if not os.path.exists(sums_path):
        fetch(a.tag, ["SHA256SUMS"], a.dest)
    want = {}
    for line in open(sums_path):
        digest, name = line.split()
        if name.startswith(f"cosmoos-{a.arch}-") or name == f"cosmoos-{a.arch}.img":
            want[name] = digest
    if not want:
        sys.exit(f"fetch-release: {a.tag}'s SHA256SUMS names no {a.arch} file")
    missing = [n for n, d in want.items()
               if not os.path.exists(os.path.join(a.dest, n)) or sha256(os.path.join(a.dest, n)) != d]
    if missing:
        fetch(a.tag, missing, a.dest)
    bad = [n for n, d in want.items() if sha256(os.path.join(a.dest, n)) != d]
    if bad:
        print(f"fetch-release: FAIL: {', '.join(bad)} do not match {a.tag}'s SHA256SUMS")
        return 1
    print(f"fetch-release: {a.tag} {a.arch}: {len(want)} files match SHA256SUMS ({a.dest})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
