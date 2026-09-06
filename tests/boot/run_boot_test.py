#!/usr/bin/env python3
"""
QEMU boot test for CosmoOS.

Boots the disk image under QEMU with serial output captured, then decides
PASS/FAIL from two independent signals:

  1. the kernel's exit status via the isa-debug-exit device
     (QEMU exits with (value << 1) | 1; the kernel writes 0x10 for success
     and 0x11 for failure), and
  2. required markers in the serial log (loader banner, kernel banner,
     SELFTEST verdict when self-tests are enabled).

Both must agree. A timeout, a panic marker, or a missing marker is a
failure. The full serial log is always written to --log and echoed on
failure so CI output is self-explanatory.
"""

import argparse
import os
import sys
import threading
import re
import subprocess
import sys
import time

EXIT_SUCCESS_VALUE = 0x10
EXIT_FAILURE_VALUE = 0x11

ARCH = os.environ.get("COSMO_ARCH", "x86_64")
# The machine has an IOMMU unless the runner was told to leave it out
# (scripts/qemu-run.sh, docs/kernel/iommu/testing.md).
IOMMU = os.environ.get("QEMU_IOMMU", "1") != "0"
# AArch64: firmware hands over at EL2 unless QEMU_EL2=0
# (docs/kernel/arch/aarch64/design.md, "Exception level 2").
EL2 = ARCH == "aarch64" and os.environ.get("QEMU_EL2", "1") != "0"

BOOT_MARKERS = [
    r"^cosmoboot-uefi v\d+",
    r"^jumping to kernel entry",
    r"^CosmoOS kernel ",
    r"^Architecture: " + re.escape(ARCH) + r"$",
    r"^Boot: UEFI",
]

# Normal run: must reach the end cleanly, nothing alarming in the log.
# The user-mode markers come from the init program delivered as the boot
# module: the self-test run prints USERTEST: PASS and the real run
# prints its banner and exits 0.
REQUIRED_MARKERS = BOOT_MARKERS + [
    r"^\[ INFO\] module: loaded hello 1\.0 ",
    r"^\[ INFO\] module: loaded virtio 1\.0 ",
    r"^\[ INFO\] module: loaded virtio_blk 1\.0 ",
    r"^\[ INFO\] module: loaded virtio_rng 1\.0 ",
    r"^\[ INFO\] module: loaded virtio_console 1\.0 ",
    r"^\[ INFO\] module: loaded virtio_net 1\.0 ",
    r"^\[ INFO\] module: loaded nvme 1\.0 ",
    r"^\[ INFO\] net: eth0 registered ",
    r"^\[ INFO\] blk: vda: 16384 sectors of 512 bytes",
    r"^\[ INFO\] blk: nvme0n1: 16384 sectors of 512 bytes",
    r"^\[ INFO\] nvme0: .* 1 namespace\(s\) of \d+, \d+ I/O queue\(s\) of depth 32",
    r"^\[ INFO\] virtio-console: virtio\d+: registered as a console sink",
    r"^\[ INFO\] hello: module init \(ABI v3, load 1\)",
    r"^init: CosmoOS userland, pid \d+",
    r"^CosmoOS userland ready",
    r"^init: rc exited with status 0",
    r"^interactive-ok$",
    r"^init: shell exited with status 0",
    r"^\[ INFO\] init exited with status 0",
    r"^\[ INFO\] boot complete",
]
# The loader kept EL2 and the kernel can reach it.
if EL2:
    REQUIRED_MARKERS += [
        r"^cosmoboot: EL2 stub at 0x[0-9a-f]+ \(\d+ bytes\)$",
        r"^\[ INFO\] el2: stub v\d+ at 0x[0-9a-f]+; EL2 available$",
    ]
# DMA remapping is on before the drivers load, and every bus-mastering
# device is in a domain (docs/kernel/iommu/testing.md).
if IOMMU:
    REQUIRED_MARKERS += [
        r"^\[ INFO\] iommu: (intel-vtd0|arm-smmuv3) at .*; translation on$",
        r"^\[ INFO\] iommu: (intel-vtd0|arm-smmuv3): pci:[0-9a-f]{2}:[0-9a-f]{2}\.[0-7] \(requester [0-9a-f]{4}\) in domain \d+",
    ]
# Phase 9: the shell's own test script runs from /etc/rc in self-test builds.
SHTEST_MARKER = r"^SHTEST: PASS"
# The package system's script checks: output lines the harness also requires in self-test builds.
PKGTEST_MARKERS = [
    r"^pkg: index updated: \d+ packages",
    r"^pkg: installing fortunes-1\.0",
    r"^pkg: installing fortune-1\.0",
    r"bad signature|unknown signing key",
    r"checksum does not match the index",
    r"^hello, world \(hello 1\.0\)$",
    r"^hello, world \(hello 1\.1\)$",
    r"^pkg: fortunes: fortune depends on it",
    r"^pkg: verify: 0 problems",
    r"^installed: 2\.5$",
    r"^2\.5$",
]
# Phase 11: Linux programs run from /etc/rc.test; hello_musl only when the build had musl-gcc.
LINUXTEST_MARKERS = [
    r"^hello from linux abi$",
    r"^LINUXTEST: PASS$",
    r"^lxinterp: ok$",
    r"^lxdyn: ok$",
    # lxsig (docs/compat/linux/testing.md): each mode dies by its signal.
    r"^lxsig term: 143$",
    r"^lxsig segv: 139$",
    r"^lxsig ill: 132$",
    r"^lxsig badret: 139$",
    r"^lxsig badstack: 139$",
    r"^lxsig group: 7$",
    r"^lxsig lastthread: 5$",
]
MUSL_MARKER = r"^hello from musl on Linux x86_64 \(pid \d+\)$"

# Virtualization (docs/kernel-services/virtualization/testing.md): the QEMU
# CPU model carries SVM + NPT, so the guest self-tests must run (not skip)
# and vmctl must run the sample guest from the shell.
HVTEST_MARKERS = [
    r"^HVTEST: PASS$",
]
HV_FORBIDDEN_MARKERS = [
    r"selftest: hv: skipped",
    r"^HVTEST: skipped",
]

# Only produced by the self-test run of init (debug builds); required
# whenever self-tests ran at all.
USERTEST_MARKER = r"^USERTEST: PASS"
FORBIDDEN_MARKERS = [
    r"KERNEL PANIC",
    r"BUG:",
    r"SELFTEST: FAIL",
    r"cosmoboot: FATAL",
]

# --expect-panic run (CRASH_TEST=1 kernel): the panic report must be
# complete and the failure exit code must be delivered.
# The page-fault vector and the register dump are architecture specific
# (docs/kernel/arch/aarch64/testing.md).
if ARCH == "aarch64":
    PANIC_ARCH_MARKERS = [
        r"^trap 1029 ",
        r"^ELR=[0-9a-f]{16} SPSR=",
        r"^FAR=ffff900000000000 \(not-present write kernel\)",
    ]
else:
    PANIC_ARCH_MARKERS = [
        r"^trap 14 ",
        r"^RIP=[0-9a-f]{16} CS=",
        r"^CR2=ffff900000000000 \(not-present write kernel\)",
    ]
PANIC_REQUIRED_MARKERS = BOOT_MARKERS + [
    r"^\[ INFO\] crash test: writing to an unmapped address",
    r"^KERNEL PANIC: page fault: kernel write at 0xffff900000000000 \(not present\): no region",
] + PANIC_ARCH_MARKERS + [
    r"^stack trace:",
    r"^  #0 +0xffffffff8[0-9a-f]{7}",
    r"^halting\.",
]
PANIC_FORBIDDEN_MARKERS = [
    r"^\[ INFO\] boot complete",
    r"crash test: write did not fault",
    r"KERNEL PANIC \(recursive\)",
    r"cosmoboot: FATAL",
]


def qemu_exit_value(returncode):
    """Map QEMU's return code back to the value written by the guest."""
    if returncode <= 0 or (returncode & 1) == 0:
        return None
    return returncode >> 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--image", required=True)
    ap.add_argument("--log", required=True)
    ap.add_argument("--timeout", type=float, default=180.0, help="seconds before the run is killed")
    ap.add_argument("--expect-selftest", choices=["auto", "yes", "no"], default="auto",
                    help="require a SELFTEST: PASS line (auto: only if a SELFTEST line appears)")
    ap.add_argument("--expect-panic", action="store_true",
                    help="the kernel was built with CRASH_TEST=1: require a full panic report "
                         "and the failure exit code instead of a clean boot")
    args = ap.parse_args()

    if args.expect_panic:
        required, forbidden = PANIC_REQUIRED_MARKERS, PANIC_FORBIDDEN_MARKERS
        expected_exit = EXIT_FAILURE_VALUE
        args.expect_selftest = "no"
    else:
        required, forbidden = REQUIRED_MARKERS, FORBIDDEN_MARKERS
        expected_exit = EXIT_SUCCESS_VALUE

    here = os.path.dirname(os.path.abspath(__file__))
    runner = os.path.join(here, "..", "..", "scripts", "qemu-run.sh")

    env = dict(os.environ)
    env.setdefault("QEMU_ACCEL", "tcg")

    os.makedirs(os.path.dirname(os.path.abspath(args.log)), exist_ok=True)

    # Phase 6 devices: a fresh 8 MiB scratch disk for virtio-blk (the blk
    # self-test writes to it) and a file for the virtio console output,
    # both next to the serial log.
    testdisk = args.log + ".testdisk.img"
    with open(testdisk, "wb") as f:
        f.truncate(8 * 1024 * 1024)
    vcon = args.log + ".vcon"
    nvmedisk = args.log + ".nvme.img"
    with open(nvmedisk, "wb") as f:
        f.truncate(8 * 1024 * 1024)
    env["QEMU_TESTDISK"] = testdisk
    env["QEMU_NVMEDISK"] = nvmedisk
    env["QEMU_VCON"] = vcon

    # Phase 8: the network harness (only for normal runs with self-tests).
    nettest = None
    if not args.expect_panic and args.expect_selftest != "no":
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        from nettest import NetTest
        nettest = NetTest()
        env.update(nettest.env())

    # Phase 9: the interactive shell harness types at the console prompt
    # (normal runs only; the panic run never reaches a prompt).
    shelltest = None
    if not args.expect_panic:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        from shelltest import ShellTest
        shelltest = ShellTest()
    print(f"boot-test: booting {args.image} (timeout {args.timeout:.0f}s)")
    start = time.monotonic()
    with open(args.log, "wb") as log:
        proc = subprocess.Popen(
            [runner, args.image],
            stdin=subprocess.PIPE if shelltest is not None else subprocess.DEVNULL,
            stdout=log,
            stderr=subprocess.STDOUT,
            env=env,
        )
        net_thread = None
        if nettest is not None:
            net_thread = threading.Thread(target=nettest.run_when_ready, args=(args.log, proc, args.timeout - 30),
                                          daemon=True)
            net_thread.start()
        shell_thread = None
        if shelltest is not None:
            shell_thread = threading.Thread(target=shelltest.run, args=(args.log, proc, args.timeout - 10),
                                            daemon=True)
            shell_thread.start()
        try:
            returncode = proc.wait(timeout=args.timeout)
            timed_out = False
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
            returncode = None
            timed_out = True
    elapsed = time.monotonic() - start

    with open(args.log, "rb") as f:
        text = f.read().decode("utf-8", errors="replace")
    lines = text.replace("\r\n", "\n").replace("\r", "\n").split("\n")

    failures = []

    if timed_out:
        failures.append(f"timed out after {args.timeout:.0f}s")

    value = None if returncode is None else qemu_exit_value(returncode)
    if not timed_out and value != expected_exit:
        if value == EXIT_FAILURE_VALUE:
            failures.append("kernel reported failure via debug-exit")
        elif value == EXIT_SUCCESS_VALUE:
            failures.append("kernel reported success via debug-exit but a panic was expected")
        else:
            failures.append(f"unexpected QEMU exit code {returncode} (debug-exit value {value})")

    for pat in required:
        if not any(re.search(pat, ln) for ln in lines):
            failures.append(f"missing marker /{pat}/")

    for pat in forbidden:
        hits = [ln for ln in lines if re.search(pat, ln)]
        if hits:
            failures.append(f"forbidden marker /{pat}/: {hits[0].strip()}")

    selftest_lines = [ln for ln in lines if ln.startswith("SELFTEST: ")]
    # Per-test durations (docs/verification/design.md, "Per-test timing"):
    # report the slowest and fail one that nears the hang watchdog.
    budget_ms = int(os.environ.get("SELFTEST_BUDGET_MS", "8000"))
    timings = []
    for ln in selftest_lines:
        m = re.match(r"SELFTEST: (\S+)\s+\.\.\. (?:ok|FAIL.*) \((\d+) ms\)", ln)
        if m:
            timings.append((int(m.group(2)), m.group(1)))
    if timings:
        timings.sort(reverse=True)
        total = sum(t for t, _ in timings)
        print(f"boot-test: {len(timings)} self-tests, {total} ms total; slowest: "
              + ", ".join(f"{name} {ms} ms" for ms, name in timings[:5]))
        for ms, name in timings:
            if ms > budget_ms:
                failures.append(f"self-test {name} took {ms} ms (budget {budget_ms} ms)")
    want_selftest = args.expect_selftest == "yes" or (args.expect_selftest == "auto" and selftest_lines)
    if want_selftest and not any(ln.startswith("SELFTEST: PASS") for ln in selftest_lines):
        failures.append("no 'SELFTEST: PASS' line")
    if want_selftest and not any(re.search(USERTEST_MARKER, ln) for ln in lines):
        failures.append(f"missing marker /{USERTEST_MARKER}/ (user-mode self-test)")

    # The network exchange happens inside the self-tests; without them
    # (release builds) the harness only provided the devices.
    if nettest is not None and want_selftest:
        if net_thread is not None:
            net_thread.join(5)
        failures.extend(nettest.failures())
        for pat in (r"^NETTEST: client ok", r"^NETTEST: done .*quit=1"):
            if not any(re.search(pat, ln) for ln in lines):
                failures.append(f"missing marker /{pat}/ (network harness)")

    if shelltest is not None:
        if shell_thread is not None:
            shell_thread.join(5)
        failures.extend(shelltest.failures(lines))
    if want_selftest and not any(re.search(SHTEST_MARKER, ln) for ln in lines):
        failures.append(f"missing marker /{SHTEST_MARKER}/ (shell test script)")
    if want_selftest:
        for pat in PKGTEST_MARKERS:
            if not any(re.search(pat, ln) for ln in lines):
                failures.append(f"missing marker /{pat}/ (package test)")
        if ARCH == "x86_64":
            # The virtualization backend and the Linux table exist on x86-64 only
            # (docs/kernel/arch/aarch64/design.md, "Stubs and exclusions").
            for pat in HVTEST_MARKERS:
                if not any(re.search(pat, ln) for ln in lines):
                    failures.append(f"missing marker /{pat}/ (virtualization test)")
            for pat in HV_FORBIDDEN_MARKERS:
                hits = [ln for ln in lines if re.search(pat, ln)]
                if hits:
                    failures.append(f"forbidden marker /{pat}/: {hits[0].strip()}")
            if os.environ.get("HAVE_MUSL") == "1" and not any(re.search(MUSL_MARKER, ln) for ln in lines):
                failures.append(f"missing marker /{MUSL_MARKER}/ (musl static program)")
        else:
            for pat in (r"^HVTEST: skipped$",):
                if not any(re.search(pat, ln) for ln in lines):
                    failures.append(f"missing marker /{pat}/ (x86-only sections must report skipped)")
        # The Linux ABI programs run on both architectures (milestone 10).
        for pat in LINUXTEST_MARKERS:
            if not any(re.search(pat, ln) for ln in lines):
                failures.append(f"missing marker /{pat}/ (Linux ABI test)")
    # The virtio console must have carried the kernel's output too.
    if not args.expect_panic:
        try:
            with open(vcon, "rb") as f:
                vlines = f.read().decode("utf-8", "replace").splitlines()
        except OSError:
            vlines = []
        if not any(re.search(r"^\[ INFO\] boot complete", ln) for ln in vlines):
            failures.append("virtio console output lacks the boot-complete line (" + vcon + ")")

    if failures:
        print(f"boot-test: FAIL after {elapsed:.1f}s")
        for f in failures:
            print(f"  - {f}")
        print("---- serial log ----")
        sys.stdout.write(text)
        if not text.endswith("\n"):
            print()
        print("---- end of log ----")
        return 1

    print(f"boot-test: PASS in {elapsed:.1f}s (log: {args.log})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
