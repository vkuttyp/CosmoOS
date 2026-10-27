#!/usr/bin/env python3
"""Roadmap 1.2 A1's acceptance: the C compiler on the installed system
(docs/userland/testing.md, "test-selfhost"; ports/tcc/README.md).

    selfhost_test.py --image cosmoos.img --workdir DIR --tcc-src SRC [--timeout S]

This build installed on a disk (install_test.py's install) and booted
from it; the harness serves the source the installed tcc was built from
(SRC: the pinned tarball with ports/tcc's patches, unpacked by
ports/tcc/tcc.mk) as a tarball, and tests/tcc, over HTTP, the guest fetches them into /root/selfhost, and
tests/tcc/selfhost.sh runs each stage there:

  programs  `cc` builds a static program, a dynamic one, and a dynamic
            one that opens a library it built with dlopen; each runs.
  tests2    tcc's own tests/tests2 suite, each test as its Makefile runs
            it, less the exclusions listed in tests/tcc/tests2-skip*.txt.
  rebuild   tcc's configure, then tcc.c, built by the installed cc
            (stage 1); stage 1 builds itself (stage 2) and the two are
            the same bytes; the installed cc and stage 1 build the same
            hello, byte for byte, and it runs.

Exit 0 PASS, 1 FAIL.
"""
import argparse
import os
import shutil
import sys
import tarfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import install_test  # noqa: E402
import net_login_test  # noqa: E402
from install_test import Boot, Fail, need, EXIT_SUCCESS  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SERVED = ("selfhost.sh", "hello.c", "dl-main.c", "dl-greet.c",
          "tests2-skip.txt", "tests2-skip-x86_64.txt", "tests2-skip-aarch64.txt")
DIR = "/root/selfhost"
# Each stage is one long command on a slow CPU; what the deadline guards
# against, one that never answers, is a property of each (fill_test.py).
STAGE_BUDGET = 1800


def stage(b, results, name, want):
    t0 = time.monotonic()
    out = b.status(f"sh {DIR}/selfhost.sh {DIR} {name}", name)
    took = time.monotonic() - t0
    m = need(out, rf"^selfhost: {name}: PASS(.*)$", f"selfhost.sh {name}")
    need(out, rf"^{name}=0$", f"selfhost.sh {name}'s exit status")
    for w in want:
        need(out, w, f"selfhost.sh {name}")
    results.append(f"{name}: PASS{m.group(1)} ({took:.0f} s)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--image", required=True)
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--tcc-src", required=True)
    ap.add_argument("--timeout", type=float, default=300.0)
    ap.add_argument("--key", help="unused: install_test.stage_install's signature")
    args = ap.parse_args()
    shutil.rmtree(args.workdir, ignore_errors=True)
    os.makedirs(args.workdir)
    www = os.path.join(args.workdir, "www")
    os.makedirs(www)
    with tarfile.open(os.path.join(www, "tinycc.tar.gz"), "w:gz") as t:
        t.add(args.tcc_src, arcname="tinycc")
    for f in SERVED:
        shutil.copyfile(os.path.join(ROOT, "tests", "tcc", f), os.path.join(www, f))
    srv, port = net_login_test.start_http(www)
    results = []
    t0 = time.monotonic()
    try:
        inst = install_test.stage_install(args, results)
        b = Boot("selfhost", inst["target"], args.workdir, {"QEMU_DISKS": "boot"}, args.timeout)
        try:
            b.wait_prompt("the first prompt")
            url = f"http://10.0.2.2:{port}"
            files = " ".join(("tinycc.tar.gz",) + SERVED)
            out = b.status(f"mkdir -p {DIR} && cd {DIR} && for f in {files}; do wget -q {url}/$f || exit 1; done "
                           f"&& cd /", "fetch")
            need(out, r"^fetch=0$", "the files fetched from the harness")
            b.per_command(STAGE_BUDGET)
            stage(b, results, "programs", (r"^cc-hello: static, built on this system by cc$",
                                           r"^cc-hello: dynamic, built on this system by cc$",
                                           r"^cc-dlopen: loaded by dlopen, 42$"))
            stage(b, results, "tests2", ())
            stage(b, results, "rebuild", ())
            rc = b.finish("poweroff")
            if rc != EXIT_SUCCESS:
                raise Fail(f"selfhost: QEMU exit code {rc}")
        except Exception:
            b.kill()
            raise
    except Fail as e:
        for r in results:
            print(f"test-selfhost: {r}")
        print(f"test-selfhost: FAIL: {e} (logs in {args.workdir})")
        return 1
    finally:
        srv.shutdown()
    for r in results:
        print(f"test-selfhost: {r}")
    print(f"test-selfhost: PASS in {time.monotonic() - t0:.1f}s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
