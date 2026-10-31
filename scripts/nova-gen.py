#!/usr/bin/env python3
"""
nova-gen.py - the C that CosmoOS's Nova sources generate (docs/build/nova.md).

Some kernel code is written in Nova, a memory-safe language compiled to C
(https://github.com/vkuttyp/Nova). Each NAME.nova sits beside the NAME.c it
generates, and the generated C is checked in, so building CosmoOS needs no
Nova compiler; only changing a .nova file does. Nova's runtime, which the
generated C needs, is generated too.

  kernel/block/gpt.nova        -> kernel/block/gpt.c
  kernel/process/cputime.nova  -> kernel/process/cputime.c
  kernel/process/nice.nova     -> kernel/process/nice.c
  kernel/scheduler/fair.nova   -> kernel/scheduler/fair.c
  kernel/nova/kcore_test.nova  -> kernel/nova/kcore_test.c   (with Nova's kernel library)
  (every port)                 -> kernel/include/nova/rt.h, kernel/nova/nova_rt_core.c

A port's header is normally hand-written C that the Nova source imports
(kernel/include/kernel/cputime.h). A port whose structs and API are
defined in Nova instead can name a generated header: novac's
nova_exports.h (its @repr(C) types and a prototype per @export), written
under that name with its own include guard (docs/build/nova.md).

The compiler is pinned (NOVAC_VERSION): generated C is a pure function of
the source and the compiler, so a regeneration with the pinned novac
reproduces the checked-in files byte for byte, and --check (CI's "Nova
sources" job) fails when they differ. Moving to another novac is a change
of its own: bump the pin and regenerate.

Both modes also check the sources the kernel must refuse,
tests/nova/negative/*.nova (NEGATIVE): novac must give exactly the errors
their `// error[N...]` lines name.

Usage:
    python3 scripts/nova-gen.py           # regenerate (make nova)
    python3 scripts/nova-gen.py --check   # compare only (make nova-check)
    python3 scripts/nova-gen.py --pinned  # print the pinned version
The novac used is $NOVAC, else `novac` on PATH. To install the pinned one:
    curl -fsSL https://vkuttyp.github.io/Nova/install.sh | NOVA_VERSION=v<pinned> sh
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

NOVAC_VERSION = "0.6.0"

# Each port: its Nova source, the C it generates, what that C is, the header novac generates for it (None
# when the port's header is hand-written C, imported by the source), and whether it uses Nova's kernel library
# (lib/kernel: locks, per-CPU values, kernel objects; novac --kernel cosmoos).
PORTS = [
    ("kernel/block/gpt.nova", "kernel/block/gpt.c", "The GUID Partition Table (gpt.h)", None, False),
    ("kernel/process/cputime.nova", "kernel/process/cputime.c",
     "CPU time and the CPU-time interval timers (cputime.h)", None, False),
    ("kernel/process/nice.nova", "kernel/process/nice.c", "The rules of nice values (nice.h)", None, False),
    ("kernel/scheduler/fair.nova", "kernel/scheduler/fair.c",
     "The weighted fair share at the default priority level (fair.h)", None, False),
    ("kernel/object/object.nova", "kernel/object/object.c", "The reference-counted kernel object header (object.h)", None,
     False),
    ("kernel/object/handle.nova", "kernel/object/handle.c", "The per-process handle table (handle.h)",
     "kernel/include/kernel/handle_types.h", False),
    ("kernel/scheduler/wait.nova", "kernel/scheduler/wait.c", "Wait queues (wait.h)",
     "kernel/include/kernel/wait_types.h", False),
    ("kernel/scheduler/completion.nova", "kernel/scheduler/completion.c", "One-shot completions (completion.h)",
     "kernel/include/kernel/completion_types.h", False),
    ("kernel/scheduler/semaphore.nova", "kernel/scheduler/semaphore.c", "Counting semaphores (semaphore.h)",
     "kernel/include/kernel/semaphore_types.h", False),
    ("kernel/scheduler/mutex.nova", "kernel/scheduler/mutex.c", "Sleeping mutexes with priority inheritance (mutex.h)",
     "kernel/include/kernel/mutex_types.h", False),
    ("kernel/core/quiesce.nova", "kernel/core/quiesce.c", "Quiescence: epoch-based reclamation (quiesce.h)",
     "kernel/include/kernel/quiesce_types.h", False),
    ("kernel/ipc/pipe.nova", "kernel/ipc/pipe.c", "The pipe's ring, and anonymous pipes over it (pipe.h)",
     "kernel/include/kernel/pipe_types.h", False),
    ("kernel/ipc/fifo.nova", "kernel/ipc/fifo.c", "Named pipes: the pipe's ring behind a filesystem node (fifo.h)",
     "kernel/include/kernel/fifo_types.h", False),
    ("kernel/core/fbvalid.nova", "kernel/core/fbvalid.c", "The framebuffer validator (bootinfo.h)", None, False),
    ("kernel/nova/crash_test.nova", "kernel/nova/crash_test.c",
     "The crash test crash-nova: a Nova bounds check that fails on purpose (CRASH_TEST=5)",
     "kernel/include/nova/crash_test.h", False),
    ("kernel/nova/kcore_test.nova", "kernel/nova/kcore_test.c",
     "The boot self-test of Nova's kernel library (selftest nova-kcore)", None, True),
]
RUNTIME = [("nova/rt.h", "kernel/include/nova/rt.h"), ("nova_rt_core.c", "kernel/nova/nova_rt_core.c")]

BANNER = """/*
 * {name} - {title}, generated from {source}.
 *
 * GENERATED: do not edit. Change {source} and run `make nova`
 * (scripts/nova-gen.py), which compiles it with the pinned novac. It needs
 * Nova's runtime (kernel/include/nova/rt.h, kernel/nova/nova_rt_core.c, also
 * generated) and a nova_panic for the environment it is linked into
 * (kernel/nova/nova_panic.c in the kernel).
 */
"""


def novac() -> str:
    exe = os.environ.get("NOVAC") or shutil.which("novac")
    if exe is None:
        sys.exit(f"nova-gen: no novac: set NOVAC or put novac {NOVAC_VERSION} on PATH (see the usage in {__file__})")
    out = subprocess.run([exe, "--version"], capture_output=True, text=True).stdout.strip()
    if out != f"novac {NOVAC_VERSION}":
        sys.exit(f"nova-gen: {exe} is '{out}', not the pinned novac {NOVAC_VERSION}")
    return exe


def generated(exe: str, work: Path) -> dict[str, str]:
    """Every generated file, by its path in the tree, as the pinned novac makes it now."""
    files: dict[str, str] = {}
    for i, (source, target, title, header, kernel) in enumerate(PORTS):
        out = work / str(i)
        # The exports header is included by its name under kernel/include (kernel/handle_types.h): the generated C
        # includes it by that name rather than defining its structs again, and its guard comes from it (Nova ADR 0030).
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
                sys.exit(f"nova-gen: {source}'s {path} differs from the other ports'")
    return files


# Nova sources the kernel must refuse (roadmap 2.0 SA3: a sleep under a spin lock's guard does not compile). Each line
# marked `// error[N…]` must be refused with that code, and nothing else may be: the rest are the cases that compile.
NEGATIVE = sorted(p.relative_to(ROOT).as_posix() for p in (ROOT / "tests" / "nova" / "negative").glob("*.nova"))


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
            wrong.append(f"{source}: novac gave no diagnostics: {(run.stdout + run.stderr).strip()[:200]}")
            continue
        wrong += [f"{source}:{n}: expected error[{c}], which novac did not give" for n, c in sorted(expected - found)]
        wrong += [f"{source}:{n}: novac gave error[{c}], which is not expected" for n, c in sorted(found - expected)]
        if not expected:
            wrong.append(f"{source}: no `// error[N…]` line: a negative test must expect a refusal")
    return wrong


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate the C of CosmoOS's Nova sources.")
    parser.add_argument("--check", action="store_true", help="compare with the checked-in files instead of writing")
    parser.add_argument("--pinned", action="store_true", help="print the pinned novac version and exit")
    args = parser.parse_args()
    if args.pinned:
        print(NOVAC_VERSION)
        return 0
    exe = novac()
    with tempfile.TemporaryDirectory() as temp:
        files = generated(exe, Path(temp))
    stale = []
    for path, text in files.items():
        target = ROOT / path
        if target.exists() and target.read_text() == text:
            continue
        stale.append(path)
        if not args.check:
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(text)
    for path in stale:
        print(f"nova-gen: {'differs from its Nova source' if args.check else 'wrote'}: {path}")
    if not stale:
        print(f"nova-gen: {len(files)} generated files match their Nova sources (novac {NOVAC_VERSION})")
    elif args.check:
        print("nova-gen: run `make nova` with the pinned novac and commit the result")
    wrong = refused(exe)
    for line in wrong:
        print(f"nova-gen: {line}")
    if not wrong:
        print(f"nova-gen: {len(NEGATIVE)} negative source(s) refused as their annotations say")
    return 1 if (args.check and stale) or wrong else 0


if __name__ == "__main__":
    sys.exit(main())
