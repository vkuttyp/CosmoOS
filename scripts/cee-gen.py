#!/usr/bin/env python3
"""
cee-gen.py - the C that CosmoOS's CeeSafe sources generate (docs/build/ceesafe.md).

Kernel code is written in CeeSafe, a memory-safe language compiled to C
(https://github.com/vkuttyp/CeeSafe; roadmap 2.0's CeeSafe rule). The tree
holds the .cee files only: the build runs this script with the pinned
ceesafec (build/cee.mk) and compiles the generated C from out/cee. A
`make dist` tarball carries the generated C beside each source instead, so
it builds with no ceesafec. Each port lists its source, the C it generates,
and, for a port whose structs and API are defined in CeeSafe, the C header
ceesafec generates for it (--exports-header); every port shares CeeSafe's
runtime, generated too:

  kernel/block/gpt.cee              -> kernel/block/gpt.c
  kernel/object/handle.cee          -> kernel/object/handle.c, kernel/include/kernel/handle_types.h
  kernel/ceesafe/kcore_test.cee     -> kernel/ceesafe/kcore_test.c   (with CeeSafe's kernel library)
  (every port)                      -> kernel/include/ceesafe/rt.h, kernel/ceesafe/ceesafe_rt_core.c

The generated C needs a ceesafe_panic for the environment it is linked into
(kernel/ceesafe/ceesafe_panic.c in the kernel, tests/host/ceesafe_panic.c in
host tests and fuzz targets).

A driver module written in CeeSafe (MODULES; roadmap 2.0 SA4,
drivers/ceesafe/README.md) is its driver source compiled with the device
API's sources (drivers/ceesafe/*.cee) and CeeSafe's kernel library, into
one C file that ends with the module's COSMO_MODULE entry. Every module
links its own copy of the runtime, built with the module flags, and the
driver floor (drivers/ceesafe/driver_floor.c, which has its ceesafe_panic):

  drivers/virtio/virtio_rng.cee     -> drivers/virtio/virtio_rng.c   (module virtio_rng)
  (every module)                    -> drivers/ceesafe/ceesafe_rt_core.c

The compiler is pinned (CEESAFEC_VERSION): generated C is a pure function of
the source and the compiler, so two builds agree (`make reproducible`).
Moving to another ceesafec is a change of its own: bump the pin here and in
ci/Dockerfile and .github/workflows/ci.yml (with the release's SHA-256).

Every run but the build's own (--no-negative) also checks the sources the
kernel must refuse, tests/ceesafe/negative/*.cee (NEGATIVE): ceesafec must
give exactly the errors their `// error[N...]` lines name.

Usage:
    python3 scripts/cee-gen.py --out out/cee           # generate (make cee; the build runs it)
    python3 scripts/cee-gen.py --out out/cee --check   # compare only (make cee-check)
    python3 scripts/cee-gen.py --list                  # the generated files' paths (no ceesafec needed)
    python3 scripts/cee-gen.py --pinned                # print the pinned version
The ceesafec used is $CEESAFEC, else `ceesafec` on PATH. To install the pinned one:
    curl -fsSL https://vkuttyp.github.io/CeeSafe/install.sh | CEESAFE_VERSION=v<pinned> sh
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

CEESAFEC_VERSION = "0.2.0"

# Each port: its CeeSafe source, the C it generates, what that C is, the header ceesafec generates for it (None
# when the port's header is hand-written C, imported by the source), and whether it uses CeeSafe's kernel library
# (lib/kernel: locks, per-CPU values, kernel objects; ceesafec --kernel cosmoos).
PORTS = [
    ("kernel/block/gpt.cee", "kernel/block/gpt.c", "The GUID Partition Table (gpt.h)", None, False),
    ("kernel/process/cputime.cee", "kernel/process/cputime.c",
     "CPU time and the CPU-time interval timers (cputime.h)", None, False),
    ("kernel/process/nice.cee", "kernel/process/nice.c", "The rules of nice values (nice.h)", None, False),
    ("kernel/scheduler/fair.cee", "kernel/scheduler/fair.c",
     "The weighted fair share at the default priority level (fair.h)", None, False),
    ("kernel/object/object.cee", "kernel/object/object.c", "The reference-counted kernel object header (object.h)", None,
     False),
    ("kernel/object/handle.cee", "kernel/object/handle.c", "The per-process handle table (handle.h)",
     "kernel/include/kernel/handle_types.h", False),
    ("kernel/scheduler/wait.cee", "kernel/scheduler/wait.c", "Wait queues (wait.h)",
     "kernel/include/kernel/wait_types.h", False),
    ("kernel/scheduler/completion.cee", "kernel/scheduler/completion.c", "One-shot completions (completion.h)",
     "kernel/include/kernel/completion_types.h", False),
    ("kernel/scheduler/semaphore.cee", "kernel/scheduler/semaphore.c", "Counting semaphores (semaphore.h)",
     "kernel/include/kernel/semaphore_types.h", False),
    ("kernel/scheduler/mutex.cee", "kernel/scheduler/mutex.c", "Sleeping mutexes with priority inheritance (mutex.h)",
     "kernel/include/kernel/mutex_types.h", False),
    ("kernel/core/quiesce.cee", "kernel/core/quiesce.c", "Quiescence: epoch-based reclamation (quiesce.h)",
     "kernel/include/kernel/quiesce_types.h", False),
    ("kernel/ipc/pipe.cee", "kernel/ipc/pipe.c", "The pipe's ring, and anonymous pipes over it (pipe.h)",
     "kernel/include/kernel/pipe_types.h", False),
    ("kernel/ipc/fifo.cee", "kernel/ipc/fifo.c", "Named pipes: the pipe's ring behind a filesystem node (fifo.h)",
     "kernel/include/kernel/fifo_types.h", False),
    ("kernel/core/crc32c.cee", "kernel/core/crc32c.c", "CRC-32C (crc32c.h)", None, False),
    ("compat/linux/lxconv.cee", "compat/linux/lxconv.c",
     "The Linux personality's decoders, and the conversions fuzz_linux drives (convert.h)", None, False),
    ("kernel-services/network/netparse.cee", "kernel-services/network/netparse.c",
     "The network receive path's decoders: frames, TCP segments, DHCP and DNS (netparse.h)",
     "kernel/include/kernel/net/netparse_types.h", False),
    ("kernel/core/fbvalid.cee", "kernel/core/fbvalid.c", "The framebuffer validator (bootinfo.h)", None, False),
    ("kernel/ceesafe/crash_test.cee", "kernel/ceesafe/crash_test.c",
     "The crash test crash-cee: a CeeSafe bounds check that fails on purpose (CRASH_TEST=5)",
     "kernel/include/ceesafe/crash_test.h", False),
    ("kernel/ceesafe/kcore_test.cee", "kernel/ceesafe/kcore_test.c",
     "The boot self-test of CeeSafe's kernel library (selftest cee-kcore)", None, True),
]
RUNTIME = [("ceesafe/rt.h", "kernel/include/ceesafe/rt.h"), ("ceesafe_rt_core.c", "kernel/ceesafe/ceesafe_rt_core.c")]

# The device API every CeeSafe driver module is compiled with (drivers/ceesafe/README.md).
DEVICE_API = ["drivers/ceesafe/driver.cee", "drivers/ceesafe/virtio.cee"]

# Each CeeSafe driver module: its name, its driver source, the C it generates, what it is, and its COSMO_MODULE
# entry (kernel/include/kernel/module.h): version, dependencies (comma separated) and capabilities. The source exports
# <name>_module_init and <name>_module_shutdown, which the entry names; build/module.mk links the module.
MODULES = [
    ("virtio_rng", "drivers/virtio/virtio_rng.cee", "drivers/virtio/virtio_rng.c",
     "The virtio entropy device (module virtio_rng)", "1.0", "virtio", "MODULE_CAP_DRIVER"),
]
MODULE_RUNTIME = ("ceesafe_rt_core.c", "drivers/ceesafe/ceesafe_rt_core.c")
# The driver template (drivers/ceesafe/README.md): never built, but checked with the device API on every run but the
# build's own, so it keeps compiling as the API changes.
TEMPLATE = "drivers/ceesafe-template/template.cee"

MODULE_BANNER = """/*
 * {name} - {title}, generated from {source}
 * and the device API (drivers/ceesafe/driver.cee, virtio.cee).
 *
 * GENERATED: do not edit. Change {source} and run `make cee`
 * (scripts/cee-gen.py), which compiles it with the pinned ceesafec. The
 * module links CeeSafe's runtime (drivers/ceesafe/ceesafe_rt_core.c, also
 * generated) and the driver floor (drivers/ceesafe/driver_floor.c).
 */
"""

MODULE_ENTRY = """
/* The module's entry (kernel/include/kernel/module.h), from scripts/cee-gen.py's MODULES. */
#include <kernel/module.h>

COSMO_MODULE("{name}", "{version}", {name}_module_init, {name}_module_shutdown, "{deps}", {caps});
"""

BANNER = """/*
 * {name} - {title}, generated from {source}.
 *
 * GENERATED: do not edit. Change {source} and run `make cee`
 * (scripts/cee-gen.py), which compiles it with the pinned ceesafec. It needs
 * CeeSafe's runtime (kernel/include/ceesafe/rt.h, kernel/ceesafe/ceesafe_rt_core.c, also
 * generated) and a ceesafe_panic for the environment it is linked into
 * (kernel/ceesafe/ceesafe_panic.c in the kernel).
 */
"""


def ceesafec() -> str:
    exe = os.environ.get("CEESAFEC") or shutil.which("ceesafec")
    if exe is None:
        sys.exit(f"cee-gen: no ceesafec: set CEESAFEC or put ceesafec {CEESAFEC_VERSION} on PATH (see the usage in {__file__})")
    out = subprocess.run([exe, "--version"], capture_output=True, text=True).stdout.strip()
    if out != f"ceesafec {CEESAFEC_VERSION}":
        sys.exit(f"cee-gen: {exe} is '{out}', not the pinned ceesafec {CEESAFEC_VERSION}")
    return exe


def generated(exe: str, work: Path) -> dict[str, str]:
    """Every generated file, by its path in the tree, as the pinned ceesafec makes it now."""
    files: dict[str, str] = {}
    for i, (source, target, title, header, kernel) in enumerate(PORTS):
        out = work / str(i)
        # The exports header is included by its name under kernel/include (kernel/handle_types.h): the generated C
        # includes it by that name rather than defining its structs again, and its guard comes from it (CeeSafe ADR 0030).
        include = None if header is None else str(Path(header).relative_to("kernel/include"))
        subprocess.run([exe, "build", "--profile", "freestanding", "--no-float", *(["--kernel", "cosmoos"] if kernel else []),
                        *(["--exports-header", include] if include else []), "--emit", "c", "-o", out, source],
                       cwd=ROOT, check=True)
        main = (out / "main.c").read_text()
        files[target] = BANNER.format(name=Path(target).name, title=title, source=source) + main
        if include is not None:
            exports = (out / include).read_text()
            files[header] = BANNER.format(name=Path(header).name, title=title + ": its C interface",
                                          source=source) + exports
        for made, path in RUNTIME:
            text = (out / made).read_text()
            if files.setdefault(path, text) != text:
                sys.exit(f"cee-gen: {source}'s {path} differs from the other ports'")
    for i, (name, source, target, title, version, deps, caps) in enumerate(MODULES):
        out = work / f"module-{i}"
        subprocess.run([exe, "build", "--profile", "freestanding", "--no-float", "--kernel", "cosmoos", "--emit", "c",
                        "-o", out, *DEVICE_API, source], cwd=ROOT, check=True)
        files[target] = (MODULE_BANNER.format(name=Path(target).name, title=title, source=source)
                         + (out / "main.c").read_text()
                         + MODULE_ENTRY.format(name=name, version=version, deps=deps, caps=caps))
        made, path = MODULE_RUNTIME
        text = (out / made).read_text()
        if files.setdefault(path, text) != text:
            sys.exit(f"cee-gen: module {name}'s {path} differs from the other modules'")
    return files


# CeeSafe sources the kernel must refuse (roadmap 2.0 SA3: a sleep under a spin lock's guard does not compile). Each line
# marked `// error[N…]` must be refused with that code, and nothing else may be: the rest are the cases that compile.
NEGATIVE = sorted(p.relative_to(ROOT).as_posix() for p in (ROOT / "tests" / "ceesafe" / "negative").glob("*.cee"))


def refused(exe: str) -> list[str]:
    """What each negative source's diagnostics got wrong, against its annotations; empty when every one is right."""
    wrong = []
    for source in NEGATIVE:
        expected = set()
        for number, line in enumerate((ROOT / source).read_text().splitlines(), 1):
            expected.update((number, code) for code in re.findall(r"//\s*error\[(N\d{4})\]", line))
        run = subprocess.run([exe, "check", "--profile", "freestanding", "--no-float", "--kernel", "cosmoos",
                              "--diagnostics-format=json", source], cwd=ROOT, capture_output=True, text=True)
        try:
            found = {(d["start"]["line"], d["code"]) for d in json.loads(run.stdout)["diagnostics"]
                     if d["severity"] == "error"}
        except (ValueError, KeyError):
            wrong.append(f"{source}: ceesafec gave no diagnostics: {(run.stdout + run.stderr).strip()[:200]}")
            continue
        wrong += [f"{source}:{n}: expected error[{c}], which ceesafec did not give" for n, c in sorted(expected - found)]
        wrong += [f"{source}:{n}: ceesafec gave error[{c}], which is not expected" for n, c in sorted(found - expected)]
        if not expected:
            wrong.append(f"{source}: no `// error[N…]` line: a negative test must expect a refusal")
    run = subprocess.run([exe, "check", "--profile", "freestanding", "--no-float", "--kernel", "cosmoos", *DEVICE_API,
                          TEMPLATE], cwd=ROOT, capture_output=True, text=True)
    if run.returncode != 0:
        wrong.append(f"{TEMPLATE}: does not compile with the device API: {(run.stdout + run.stderr).strip()[:300]}")
    return wrong


def outputs() -> list[str]:
    """Every generated file's path, relative to the tree, without running ceesafec (make's list)."""
    paths = [target for _, target, _, _, _ in PORTS] + [h for _, _, _, h, _ in PORTS if h] + [p for _, p in RUNTIME]
    paths += [target for _, _, target, _, _, _, _ in MODULES] + ([MODULE_RUNTIME[1]] if MODULES else [])
    return sorted(set(paths))


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate the C of CosmoOS's CeeSafe sources.")
    parser.add_argument("--out", default="out/cee",
                        help="the directory the generated files go under, at their paths in the tree (default out/cee; "
                             "`make dist` writes them into the tree it packs)")
    parser.add_argument("--check", action="store_true", help="compare with the files under --out instead of writing")
    parser.add_argument("--list", action="store_true", help="print each generated file's path and exit (no ceesafec)")
    parser.add_argument("--no-negative", action="store_true", help="skip the negative sources (the build's own run)")
    parser.add_argument("--pinned", action="store_true", help="print the pinned ceesafec version and exit")
    args = parser.parse_args()
    if args.pinned:
        print(CEESAFEC_VERSION)
        return 0
    if args.list:
        print("\n".join(outputs()))
        return 0
    out = (ROOT / args.out).resolve()
    exe = ceesafec()
    with tempfile.TemporaryDirectory() as temp:
        files = generated(exe, Path(temp))
    stale = []
    for path, text in files.items():
        target = out / path
        if target.exists() and target.read_text() == text:
            continue
        stale.append(path)
        if not args.check:
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(text)
    where = out.relative_to(ROOT) if out.is_relative_to(ROOT) else out
    for path in stale:
        print(f"cee-gen: {'differs from its CeeSafe source' if args.check else 'wrote'}: {where}/{path}")
    if not stale:
        print(f"cee-gen: {len(files)} generated files under {where} match their CeeSafe sources (ceesafec {CEESAFEC_VERSION})")
    elif args.check:
        print(f"cee-gen: {where} is not what the pinned ceesafec makes of the CeeSafe sources")
    wrong = [] if args.no_negative else refused(exe)
    for line in wrong:
        print(f"cee-gen: {line}")
    if not wrong and not args.no_negative:
        print(f"cee-gen: {len(NEGATIVE)} negative source(s) refused as their annotations say; {TEMPLATE} compiles")
    return 1 if (args.check and stale) or wrong else 0


if __name__ == "__main__":
    sys.exit(main())
