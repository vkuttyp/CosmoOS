#!/usr/bin/env python3
"""tools/pkgbuild.py's shared-library manifest lines (roadmap 1.1 P4;
docs/pkg/testing.md): lld links a library with a soname and a second one
that needs it; elf_libs reads DT_SONAME and DT_NEEDED, and manifest_text
lists what a package provides and the libraries it requires and does not
provide itself. CC is the project's clang (any target; lld links).
"""
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import pkgbuild  # noqa: E402

CC = os.environ.get("CC", "clang")


def check(cond, what):
    if not cond:
        raise SystemExit(f"test_pkgbuild_libs: FAIL {what}")
    print(f"ok   {what}")


def link(work, name, src, extra):
    c = os.path.join(work, name + ".c")
    with open(c, "w") as f:
        f.write(src)
    out = os.path.join(work, name)
    subprocess.run([CC, "--target=x86_64-unknown-linux-musl", "-fPIC", "-shared", "-nostdlib", "-fuse-ld=lld",
                    "-o", out, c] + extra, check=True)
    return open(out, "rb").read()


def main():
    with tempfile.TemporaryDirectory() as work:
        a = link(work, "liba.so", "int a(void) { return 1; }\n", ["-Wl,-soname,liba.so.1"])
        b = link(work, "libb.so", "int a(void); int b(void) { return a() + 1; }\n",
                 ["-Wl,-soname,libb.so", "-Wl,--no-as-needed", os.path.join(work, "liba.so")])
        check(pkgbuild.elf_libs(a) == ("liba.so.1", []), "a library's DT_SONAME, no DT_NEEDED")
        check(pkgbuild.elf_libs(b) == ("libb.so", ["liba.so.1"]), "a library needing another: DT_NEEDED liba.so.1")
        check(pkgbuild.elf_libs(b"#!/bin/sh\n") == (None, []), "a script: nothing")
        rec = {"name": "t", "version": "1.0", "summary": "s", "depends": []}
        m = pkgbuild.manifest_text(rec, [("usr/lib/libb.so", 0o755, b)]).decode()
        check("provides: libb.so\n" in m and "requires: liba.so.1\n" in m, "libb alone provides libb.so, requires liba.so.1")
        m = pkgbuild.manifest_text(rec, [("usr/lib/liba.so.1", 0o755, a), ("usr/lib/libb.so", 0o755, b)]).decode()
        check("provides: liba.so.1\n" in m and "provides: libb.so\n" in m and "requires:" not in m,
              "both: provides each, requires nothing it provides itself")
        rec2 = dict(rec, provides=["libx.so"], requires=["liby.so"])
        m = pkgbuild.manifest_text(rec2, []).decode()
        check("provides: libx.so\n" in m and "requires: liby.so\n" in m, "a recipe's own provides and requires")
    print("test_pkgbuild_libs: ok")


if __name__ == "__main__":
    main()
