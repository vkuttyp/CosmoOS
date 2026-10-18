#!/usr/bin/env python3
"""cosmo-fetch's cases on the host (make host-test-fetch;
ports/mbedtls/README.md, "cosmo-fetch").

    test_fetch.py --fetch out/.../host/cosmo-fetch --workdir DIR

The host build of cosmo-fetch (the same source and mbedTLS configuration
as the target's, with ASan and UBSan) against local servers
(tests/boot/tlstest.py) with test certificates made for the run. Each
refusal of the server's certificate (an untrusted CA, an expired
certificate, another name, a wrong pin) must exit 3 with the server
having received no request; plain HTTP without -i exits 3 with no
connection made; redirects are followed on the same origin only; ranges
resume a partial file, a server that ignores them restarts it, a complete
one is left as it is; -m refuses a larger file; a body cut short exits 1
keeping what arrived. Exit 0 PASS, 1 FAIL.
"""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "boot"))
import tlstest  # noqa: E402


class Fail(Exception):
    pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--fetch", required=True)
    ap.add_argument("--workdir", required=True)
    a = ap.parse_args()
    fetch = os.path.abspath(a.fetch)
    work = os.path.abspath(a.workdir)
    shutil.rmtree(work, ignore_errors=True)
    tls, www, dl = (os.path.join(work, d) for d in ("tls", "www", "dl"))
    for d in (www, dl):
        os.makedirs(d)
    tlstest.make_test_certs(tls, ip="127.0.0.1")
    blob = os.urandom(300 * 1024 + 17)
    with open(os.path.join(www, "blob.bin"), "wb") as f:
        f.write(blob)
    want = hashlib.sha256(blob).hexdigest()
    env = dict(os.environ, SSL_CERT_FILE=os.path.join(tls, "ca.pem"))

    good = tlstest.Server(www, tls, "server")
    servers = {n: tlstest.Server(www, tls, n) for n in ("untrusted", "expired", "wrongname")}
    other = tlstest.Server(www, tls, "server")      # another origin (port) for redirects
    plain = tlstest.Server(www)
    results = []

    def run(*args, out="out.bin", keep=False):
        path = os.path.join(dl, out)
        if not keep and os.path.exists(path):
            os.unlink(path)
        r = subprocess.run([fetch, *args[:-1], args[-1], path], env=env, capture_output=True, text=True,
                           timeout=120)
        return r.returncode, r.stderr, path

    def expect(case, rc, err, code, text=None):
        if rc != code:
            raise Fail(f"{case}: exit {rc}, expected {code}:\n{err.strip()[-800:]}")
        if text and text not in err:
            raise Fail(f"{case}: stderr lacks '{text}':\n{err.strip()[-800:]}")
        if "runtime error" in err or "AddressSanitizer" in err:
            raise Fail(f"{case}: sanitizer report:\n{err.strip()[-1500:]}")
        results.append(case)

    def same(case, path):
        got = hashlib.sha256(open(path, "rb").read()).hexdigest()
        if got != want:
            raise Fail(f"{case}: SHA-256 {got}, the served file's {want}")

    url = f"https://127.0.0.1:{good.port}"

    rc, err, p = run(f"{url}/blob.bin")
    expect("https with the test CA", rc, err, 0)
    same("https with the test CA", p)
    if good.paths() != ["/blob.bin"]:
        raise Fail(f"requests {good.paths()}, expected one GET /blob.bin")

    for name, text in (("untrusted", "not accepted"), ("expired", "expired"), ("wrongname", "CN")):
        srv = servers[name]
        rc, err, p = run(f"https://127.0.0.1:{srv.port}/blob.bin")
        expect(f"{name} certificate refused", rc, err, 3, text)
        if srv.requests:
            raise Fail(f"{name}: the server received {srv.paths()} after a refused certificate")
        if os.path.exists(p):
            raise Fail(f"{name}: {p} left after a refusal")

    pin = tlstest.key_pin(tls, "server")
    rc, err, p = run("-p", pin, f"{url}/blob.bin")
    expect("the pinned key", rc, err, 0)
    same("the pinned key", p)
    before = len(good.requests)
    rc, err, p = run("-p", tlstest.key_pin(tls, "expired"), f"{url}/blob.bin")
    expect("another key than the pin refused", rc, err, 3, "not the pinned")
    if len(good.requests) != before:
        raise Fail("a GET was sent to a server whose key is not the pinned one")

    rc, err, p = run(f"http://127.0.0.1:{plain.port}/blob.bin")
    expect("plain HTTP refused without -i", rc, err, 3, "plain HTTP is refused")
    if plain.connections:
        raise Fail(f"plain HTTP without -i made {plain.connections} connection(s)")
    rc, err, p = run("-i", f"http://127.0.0.1:{plain.port}/blob.bin")
    expect("plain HTTP with -i", rc, err, 0)
    same("plain HTTP with -i", p)

    good.redirect["/r/abs"] = "/blob.bin"
    good.redirect["/r/rel"] = "../blob.bin"
    good.redirect["/r/full"] = f"{url}/blob.bin"
    good.redirect["/r/host"] = f"https://localhost:{other.port}/blob.bin"
    good.redirect["/r/http"] = f"http://127.0.0.1:{good.port}/blob.bin"
    good.redirect["/r/loop"] = "/r/loop"
    for path in ("/r/abs", "/r/rel", "/r/full"):
        rc, err, p = run(f"{url}{path}")
        expect(f"same-origin redirect {path}", rc, err, 0)
        same(f"same-origin redirect {path}", p)
    good.redirect["/r/q"] = "?v=2"
    good.alias["/r/q"] = "blob.bin"
    rc, err, p = run(f"{url}/r/q")
    expect("a query-only redirect keeps the path", rc, err, 0)
    same("a query-only redirect keeps the path", p)
    if ("/r/q", None) not in good.requests or good.paths()[-1] != "/r/q":
        raise Fail(f"the query-only redirect asked for {good.paths()[-2:]}")
    rc, err, p = run(f"{url}/r/host")
    expect("redirect to another host refused", rc, err, 3, "another host or scheme")
    if other.requests:
        raise Fail(f"the other host received {other.paths()}")
    rc, err, p = run(f"{url}/r/http")
    expect("redirect from https to http refused", rc, err, 3, "another host or scheme")
    rc, err, p = run(f"{url}/r/loop")
    expect("a redirect loop stops", rc, err, 1, "more than 5 redirects")

    rc, err, p = run(f"{url}/missing")
    expect("404", rc, err, 4, "HTTP 404")
    if os.path.exists(p):
        raise Fail("404 left the output file")

    def partial(n):
        path = os.path.join(dl, "part.bin")
        with open(path, "wb") as f:
            f.write(blob[:n])

    partial(100000)
    del good.requests[:]
    rc, err, p = run("-c", f"{url}/blob.bin", out="part.bin", keep=True)
    expect("resume by range", rc, err, 0)
    same("resume by range", p)
    if good.requests != [("/blob.bin", "bytes=100000-")]:
        raise Fail(f"resume sent {good.requests}, expected Range: bytes=100000-")
    good.ignore_range.add("/whole.bin")
    shutil.copy(os.path.join(www, "blob.bin"), os.path.join(www, "whole.bin"))
    partial(100000)
    rc, err, p = run("-c", f"{url}/whole.bin", out="part.bin", keep=True)
    expect("a server ignoring the range restarts the file", rc, err, 0)
    same("a server ignoring the range restarts the file", p)
    partial(len(blob))
    rc, err, p = run("-c", f"{url}/blob.bin", out="part.bin", keep=True)
    expect("resuming a complete file (416)", rc, err, 0)
    same("resuming a complete file (416)", p)

    rc, err, p = run("-m", "1000", f"{url}/blob.bin")
    expect("-m refuses by Content-Length", rc, err, 3, "larger than 1000 bytes")
    if os.path.exists(p):
        raise Fail("-m left the output file")
    good.chunked.add("/chunked.bin")
    shutil.copy(os.path.join(www, "blob.bin"), os.path.join(www, "chunked.bin"))
    rc, err, p = run(f"{url}/chunked.bin")
    expect("a chunked body", rc, err, 0)
    same("a chunked body", p)
    rc, err, p = run("-m", "20000", f"{url}/chunked.bin")
    expect("-m refuses a chunked body as it grows", rc, err, 3, "larger than 20000 bytes")

    good.truncate["/cut.bin"] = 50000
    shutil.copy(os.path.join(www, "blob.bin"), os.path.join(www, "cut.bin"))
    rc, err, p = run(f"{url}/cut.bin", out="cut.bin")
    expect("a body cut short", rc, err, 1, "closed after 50000 of")
    if os.path.getsize(p) != 50000:
        raise Fail(f"a cut body kept {os.path.getsize(p)} bytes, not the 50000 that arrived")
    del good.truncate["/cut.bin"]
    rc, err, p = run("-c", f"{url}/cut.bin", out="cut.bin", keep=True)
    expect("resuming the cut body", rc, err, 0)
    same("resuming the cut body", p)

    for path, mode, text in (("/two.bin", good.double_length, "second, different Content-Length"),
                             ("/cuthdr.bin", good.cut_header, "closed inside the headers")):
        mode.add(path)
        shutil.copy(os.path.join(www, "blob.bin"), os.path.join(www, path[1:]))
        rc, err, p = run(f"{url}{path}")
        expect(f"refused: {text}", rc, err, 1, text)
    good.bad_range.add("/shift.bin")
    shutil.copy(os.path.join(www, "blob.bin"), os.path.join(www, "shift.bin"))
    partial(100000)
    rc, err, p = run("-c", f"{url}/shift.bin", out="part.bin", keep=True)
    expect("a 206 from another offset", rc, err, 1, "the server sent another range")
    rc, err, p = run("-i", "-p", pin, f"http://127.0.0.1:{plain.port}/blob.bin")
    expect("-p with http:// (usage)", rc, err, 2, "-p needs an https:// URL")
    if plain.connections != 1:
        raise Fail(f"the plain server saw {plain.connections} connections; -p with http must make none")

    rc, err, p = run("ftp://127.0.0.1/x")
    expect("not an http(s) URL", rc, err, 2)
    rc, err, p = run("https://cosmo-fetch-test.invalid/x")
    expect("a name that does not resolve", rc, err, 1, "cannot resolve")

    print(f"test_fetch: {len(results)} cases: " + "; ".join(results))
    print("test_fetch: PASS")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (Fail, tlstest.CertError) as e:
        print(f"test_fetch: FAIL: {e}")
        sys.exit(1)
