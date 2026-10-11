#!/usr/bin/env python3
"""fetch-pinned.py OUTPUT URL SHA256

Put the file at URL into OUTPUT, but only with the named SHA-256: a cached
OUTPUT whose digest matches is kept, and a download that does not match is
discarded with an error. The only network access the build makes
(ports/busybox/README.md); CI caches OUTPUT's directory.
"""
import hashlib
import os
import sys
import urllib.request


def digest(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main(argv):
    if len(argv) != 4:
        raise SystemExit(__doc__)
    out, url, want = argv[1], argv[2], argv[3].lower()
    if os.path.exists(out) and digest(out) == want:
        return 0
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    tmp = out + ".part"
    print(f"fetch-pinned: {url}", file=sys.stderr)
    with urllib.request.urlopen(url, timeout=300) as r, open(tmp, "wb") as f:
        while True:
            chunk = r.read(1 << 20)
            if not chunk:
                break
            f.write(chunk)
    got = digest(tmp)
    if got != want:
        os.unlink(tmp)
        raise SystemExit(f"fetch-pinned: {url}: SHA-256 {got}, pinned {want}")
    os.replace(tmp, out)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
