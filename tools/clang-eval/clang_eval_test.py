#!/usr/bin/env python3
"""Roadmap 1.2 A1's clang evaluation, the boot (tools/clang-eval/README.md).

    clang_eval_test.py --image cosmoos.img --workdir DIR --clang TAR --tcc-src SRC [--timeout S]

This build installed on a disk (install_test.py's install) and booted from
it with a 2 GiB data disk, which takes the compiler (the installed root
is 256 MiB). The harness serves the clang package (TAR, build.sh), the
source the installed tcc was built from (SRC, as test-selfhost does) and
tests/tcc's programs over HTTP; tools/clang-eval/eval.sh runs on the
system:

  unpack    clang and lld run: their versions, resource directory and
            builtins.
  programs  clang builds a static, a dynamic and a dlopen program; each
            runs.
  bigger    tcc.c, one 30,000-line translation unit, built by tcc and by
            clang at -O0 and -O2; the clang-built tcc builds hello.

Each compile is timed and its peak resident memory sampled. QEMU_MEM
sets the guest's memory (the default 256M, as test-selfhost). Not a
pass/fail gate in CI: the evaluation's measurements, recorded in
docs/audit/2026-10-27-a1-clang-eval-report.md. Exit 0 when every stage
ran, 1 otherwise.
"""
import argparse
import os
import shutil
import sys
import tarfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tests", "boot"))
import install_test  # noqa: E402
import net_login_test  # noqa: E402
from install_test import Boot, Fail, need, EXIT_SUCCESS  # noqa: E402

DATA_MIB = 2048
DIR = "/mnt/d"
SERVED = (("tests/tcc", "hello.c"), ("tests/tcc", "dl-main.c"), ("tests/tcc", "dl-greet.c"),
          ("tools/clang-eval", "eval.sh"))
STAGE_BUDGET = 3600


def stage(b, results, name):
    t0 = time.monotonic()
    out = b.status(f"sh {DIR}/eval.sh {DIR} {name}", name)
    took = time.monotonic() - t0
    for line in out.splitlines():
        if line.startswith("clang-eval: ") or line.startswith("clang version") or line.startswith("LLD "):
            results.append(line.strip())
    need(out, rf"^clang-eval: {name}: PASS$", f"eval.sh {name}")
    need(out, rf"^{name}=0$", f"eval.sh {name}'s exit status")
    results.append(f"{name}: PASS ({took:.0f} s)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--image", required=True)
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--clang", required=True)
    ap.add_argument("--tcc-src", required=True)
    ap.add_argument("--timeout", type=float, default=300.0)
    ap.add_argument("--key", help="unused: install_test.stage_install's signature")
    args = ap.parse_args()
    shutil.rmtree(args.workdir, ignore_errors=True)
    os.makedirs(args.workdir)
    www = os.path.join(args.workdir, "www")
    os.makedirs(www)
    shutil.copyfile(args.clang, os.path.join(www, "clang.tar.gz"))
    with tarfile.open(os.path.join(www, "tinycc.tar.gz"), "w:gz") as t:
        t.add(args.tcc_src, arcname="tinycc")
    for d, f in SERVED:
        shutil.copyfile(os.path.join(ROOT, d, f), os.path.join(www, f))
    data = os.path.join(args.workdir, "data.img")
    with open(data, "wb") as f:
        f.truncate(DATA_MIB << 20)
    srv, port = net_login_test.start_http(www)
    results = []
    t0 = time.monotonic()
    try:
        inst = install_test.stage_install(args, results)
        extra = f"-drive if=none,id=dd,format=raw,file={data} -device virtio-blk-pci,drive=dd"
        env = {"QEMU_DISKS": "boot", "QEMU_EXTRA": (os.environ.get("QEMU_EXTRA", "") + " " + extra).strip()}
        b = Boot("clang-eval", inst["target"], args.workdir, env, args.timeout)
        try:
            b.wait_prompt("the first prompt")
            out = b.run("cosmo-install --list")
            dev = need(out, rf"^(\S+)\s+-\s+{(DATA_MIB << 20) // install_test.SS}\s", "the data disk").group(1)
            out = b.status(f"mkfs.cosmofs {dev} && mkdir -p {DIR} && mount -t cosmofs /dev/{dev} {DIR}", "pool")
            need(out, r"^pool=0$", "the data pool")
            url = f"http://10.0.2.2:{port}"
            files = " ".join(("clang.tar.gz", "tinycc.tar.gz") + tuple(f for _, f in SERVED))
            out = b.status(f"cd {DIR} && for f in {files}; do wget -q {url}/$f || exit 1; done && cd /", "fetch")
            need(out, r"^fetch=0$", "the files fetched from the harness")
            b.per_command(STAGE_BUDGET)
            for name in ("unpack", "programs", "bigger"):
                stage(b, results, name)
            rc = b.finish("poweroff")
            if rc != EXIT_SUCCESS:
                raise Fail(f"clang-eval: QEMU exit code {rc}")
        except Exception:
            b.kill()
            raise
    except Fail as e:
        for r in results:
            print(f"clang-eval: {r}" if not r.startswith("clang-eval: ") else r)
        print(f"clang-eval: FAIL: {e} (logs in {args.workdir})")
        return 1
    finally:
        srv.shutdown()
    for r in results:
        print(f"clang-eval: {r}" if not r.startswith("clang-eval: ") else r)
    print(f"clang-eval: done in {time.monotonic() - t0:.1f}s (QEMU_MEM {os.environ.get('QEMU_MEM', '256M')})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
