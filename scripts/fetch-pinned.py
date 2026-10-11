#!/usr/bin/env python3
"""fetch-pinned.py OUTPUT SHA256 URL [URL...]

Put the file named by the URLs into OUTPUT, but only with the named
SHA-256: a cached OUTPUT whose digest matches is kept, and a download that
does not match is discarded. The URLs are tried in order -- the upstream
one first, then the copy the repository's owner controls (the
"pinned-sources" release of this repository, holding the same bytes) --
each with retries and backoff; the fetch fails only when every URL has
failed, and the error names each URL with its last failure. The only
network access the build makes (ports/busybox/README.md); CI caches
OUTPUT's directory.

A DNS failure of the single upstream URL once turned main red (run
38105614715, first attempt): one host's outage is not the build's.

COSMO_FETCH_ATTEMPTS (default 3) and COSMO_FETCH_BACKOFF (seconds, default
2, doubled after each failed attempt) tune the retries; the host test sets
them small.
"""
import hashlib
import os
import sys
import time
import urllib.request


def digest(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def fetch_once(url, tmp, want):
    """Download url into tmp; None on success, else the reason it failed."""
    try:
        with urllib.request.urlopen(url, timeout=300) as r, open(tmp, "wb") as f:
            while True:
                chunk = r.read(1 << 20)
                if not chunk:
                    break
                f.write(chunk)
    except Exception as e:  # URLError, HTTPError, timeouts, resets
        return f"{type(e).__name__}: {e}"
    got = digest(tmp)
    if got != want:
        return f"SHA-256 {got}, pinned {want}"
    return None


def main(argv):
    if len(argv) < 4:
        raise SystemExit(__doc__)
    out, want, urls = argv[1], argv[2].lower(), argv[3:]
    if len(want) != 64 or any(c not in "0123456789abcdef" for c in want):
        raise SystemExit(f"fetch-pinned: {want!r} is not a SHA-256 (usage: OUTPUT SHA256 URL...)")
    if os.path.exists(out) and digest(out) == want:
        return 0
    attempts = int(os.environ.get("COSMO_FETCH_ATTEMPTS", "3"))
    backoff = float(os.environ.get("COSMO_FETCH_BACKOFF", "2"))
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    tmp = out + ".part"
    failures = []
    for url in urls:
        why = None
        for attempt in range(1, attempts + 1):
            print(f"fetch-pinned: {url} (attempt {attempt}/{attempts})", file=sys.stderr)
            why = fetch_once(url, tmp, want)
            if why is None:
                os.replace(tmp, out)
                return 0
            print(f"fetch-pinned: {url}: {why}", file=sys.stderr)
            if os.path.exists(tmp):
                os.unlink(tmp)
            if attempt < attempts:
                time.sleep(backoff * (1 << (attempt - 1)))
        failures.append(f"  {url}: {why}")
    raise SystemExit(f"fetch-pinned: {os.path.basename(out)}: every URL failed after {attempts} attempts each:\n"
                     + "\n".join(failures))


if __name__ == "__main__":
    sys.exit(main(sys.argv))
