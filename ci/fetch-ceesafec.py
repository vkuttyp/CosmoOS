#!/usr/bin/env python3
"""
fetch-ceesafec.py VERSION SHA256 OUT [TRIES] - download the pinned ceesafec's
Linux x64 release archive (github.com/CeeSafe/CeeSafe) to OUT and check it
against SHA256, the digest in that release's SHA256SUMS.

Used by ci/Dockerfile (the toolchain image) and by .github/workflows/ci.yml
(the "CeeSafe sources" job). Each try goes through the GitHub API first (the
release by its tag, then the asset by its id) and then through the
github.com download URL. That URL has answered 504 for minutes at a time while
the API's asset links, which redirect to the same storage, kept working
(2026-11-09). A failed try waits 30 s, and the script gives up after TRIES
(default 9). A digest mismatch fails at once.

The API is rate-limited per address without a token: GH_TOKEN, or the file
/run/secrets/gh_token (the image build's BuildKit secret), is sent to
api.github.com only, never to the storage host the asset redirects to.
"""
import hashlib
import json
import os
import sys
import time
import urllib.request

REPO = "CeeSafe/CeeSafe"
ASSET = "ceesafec-linux-x64.tar.gz"
WAIT = 30
TIMEOUT = 120


def token():
    t = os.environ.get("GH_TOKEN", "").strip()
    if not t and os.path.exists("/run/secrets/gh_token"):
        with open("/run/secrets/gh_token") as f:
            t = f.read().strip()
    return t


def api_request(url, accept, tok):
    req = urllib.request.Request(url, headers={"Accept": accept, "User-Agent": "cosmoos-ci"})
    if tok:
        # Unredirected: a redirect to the storage host must not carry it.
        req.add_unredirected_header("Authorization", "Bearer " + tok)
    return req


def via_api(version, tok):
    with urllib.request.urlopen(api_request(f"https://api.github.com/repos/{REPO}/releases/tags/v{version}",
                                            "application/vnd.github+json", tok), timeout=TIMEOUT) as r:
        release = json.load(r)
    ids = [a["id"] for a in release["assets"] if a["name"] == ASSET]
    if not ids:
        raise RuntimeError(f"release v{version} has no {ASSET}")
    with urllib.request.urlopen(api_request(f"https://api.github.com/repos/{REPO}/releases/assets/{ids[0]}",
                                            "application/octet-stream", tok), timeout=TIMEOUT) as r:
        return r.read()


def via_download(version):
    req = urllib.request.Request(f"https://github.com/{REPO}/releases/download/v{version}/{ASSET}",
                                 headers={"User-Agent": "cosmoos-ci"})
    with urllib.request.urlopen(req, timeout=TIMEOUT) as r:
        return r.read()


def main():
    if len(sys.argv) not in (4, 5):
        sys.exit(__doc__.strip().splitlines()[0])
    version, want, out = sys.argv[1], sys.argv[2].lower(), sys.argv[3]
    tries = int(sys.argv[4]) if len(sys.argv) == 5 else 9
    tok = token()
    for attempt in range(1, tries + 1):
        for name, fetch in (("api", lambda: via_api(version, tok)), ("download", lambda: via_download(version))):
            try:
                data = fetch()
            except Exception as e:  # the network, HTTP errors, a malformed answer: the next way or try
                print(f"fetch-ceesafec: {name} (try {attempt}/{tries}): {e}", file=sys.stderr)
                continue
            got = hashlib.sha256(data).hexdigest()
            if got != want:
                sys.exit(f"fetch-ceesafec: {ASSET} of v{version} has SHA-256 {got}, not {want}")
            with open(out, "wb") as f:
                f.write(data)
            print(f"fetch-ceesafec: v{version} {ASSET} ({len(data)} bytes, {name}), SHA-256 ok", file=sys.stderr)
            return
        if attempt < tries:
            time.sleep(WAIT)
    sys.exit(f"fetch-ceesafec: no {ASSET} of v{version} after {tries} tries")


if __name__ == "__main__":
    main()
