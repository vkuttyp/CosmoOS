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


STALL_S = 60


def digest(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def fetch_once(url, tmp, want):
    """Download url into tmp; None on success, else the reason it failed."""
    try:
        # A name of our own: matt.ucc.asn.au (dropbear's upstream) answers
        # Python's default User-Agent with 403 Forbidden. The timeout is per
        # blocking read, not the whole download: a slow transfer that keeps
        # moving finishes, a stalled one gives up in a minute, so every
        # URL's attempts end well inside a CI job.
        req = urllib.request.Request(url, headers={"User-Agent": "cosmoos-fetch-pinned/1"})
        with urllib.request.urlopen(req, timeout=STALL_S) as r, open(tmp, "wb") as f:
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
    try:
        attempts = int(os.environ.get("COSMO_FETCH_ATTEMPTS", "3"))
        backoff = float(os.environ.get("COSMO_FETCH_BACKOFF", "2"))
    except ValueError as e:
        raise SystemExit(f"fetch-pinned: COSMO_FETCH_ATTEMPTS / COSMO_FETCH_BACKOFF: {e}")
    if not 1 <= attempts <= 20 or not 0 <= backoff <= 300:
        raise SystemExit("fetch-pinned: COSMO_FETCH_ATTEMPTS must be 1-20 and COSMO_FETCH_BACKOFF 0-300 seconds")
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
