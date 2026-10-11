#!/usr/bin/env python3
"""Host test of scripts/fetch-pinned.py (make host-test).

Uses file:// URLs and a URL that cannot resolve, so it needs no network:
- the first URL failing (unresolvable) falls through to the second;
- a URL serving the wrong bytes is refused and the next one tried;
- every URL failing fails, and the error names each URL;
- a cached OUTPUT with the pinned digest is kept without fetching;
- the attempts per URL are made (counted from stderr).
"""
import hashlib
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SCRIPT = os.path.join(ROOT, "scripts", "fetch-pinned.py")
BAD_HOST = "http://fetch-pinned-test.invalid/x.tar"  # RFC 2606: never resolves


def run(*args):
    env = dict(os.environ, COSMO_FETCH_ATTEMPTS="2", COSMO_FETCH_BACKOFF="0")
    return subprocess.run([sys.executable, SCRIPT, *args], capture_output=True, text=True, env=env)


def check(cond, what):
    if not cond:
        print(f"FAIL: {what}")
        sys.exit(1)
    print(f"ok: {what}")


def main():
    with tempfile.TemporaryDirectory() as d:
        good = os.path.join(d, "good.tar")
        bad = os.path.join(d, "bad.tar")
        with open(good, "wb") as f:
            f.write(b"pinned bytes\n" * 1000)
        with open(bad, "wb") as f:
            f.write(b"other bytes\n")
        want = hashlib.sha256(open(good, "rb").read()).hexdigest()
        out = os.path.join(d, "cache", "x.tar")

        r = run(out, want, BAD_HOST, "file://" + good)
        check(r.returncode == 0 and open(out, "rb").read() == open(good, "rb").read(),
              "an unresolvable first URL falls through to the second")
        check(r.stderr.count(f"{BAD_HOST} (attempt") == 2, "each URL is attempted COSMO_FETCH_ATTEMPTS times")

        os.unlink(out)
        r = run(out, want, "file://" + bad, "file://" + good)
        check(r.returncode == 0 and "SHA-256" in r.stderr, "a wrong digest is refused and the next URL tried")
        check(not os.path.exists(out + ".part"), "no partial file is left behind")

        r = run(out, want, BAD_HOST)
        check(r.returncode == 0 and "attempt" not in r.stderr, "a cached file with the pinned digest is kept")

        os.unlink(out)
        r = run(out, want, BAD_HOST, "file://" + bad)
        check(r.returncode != 0 and not os.path.exists(out), "every URL failing fails")
        tail = r.stderr[r.stderr.find("every URL failed"):]
        check(BAD_HOST in tail and "file://" + bad in tail and "SHA-256" in tail,
              "the final error names every URL with its reason")

        r = run(out, "not-a-digest", "file://" + good)
        check(r.returncode != 0, "a malformed SHA-256 argument is refused")

        env0 = dict(os.environ, COSMO_FETCH_ATTEMPTS="0", COSMO_FETCH_BACKOFF="0")
        r = subprocess.run([sys.executable, SCRIPT, out, want, "file://" + good], capture_output=True, text=True,
                           env=env0)
        check(r.returncode != 0 and "COSMO_FETCH_ATTEMPTS" in r.stderr, "zero attempts is refused, not a silent no-op")
    print("PASS: fetch-pinned")


if __name__ == "__main__":
    main()
