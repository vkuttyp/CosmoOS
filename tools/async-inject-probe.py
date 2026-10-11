#!/usr/bin/env python3
"""Prove trap-async-inject readies EL2 on the CPU it runs on (AArch64 only).

The test asks EL2 for a virtual SError. EL2 answers that only through the
switch's vectors, which a CPU has once something readied it (S25); the
test passed or failed by which CPU it ran on (entropy boots, PR #351, run
38119192127: "EL2 refused the virtual SError"). Since its proof commit the
test hands EL2 back on its own CPU first, so it always meets that case.
--old reverses the fix commit: the test fails on every boot of a CPU with
FEAT_RAS. The boot is on cortex-a76 (the test skips without FEAT_RAS);
x86-64 has no EL2, so there is nothing to probe there.
"""
import os
import sys
import fixprobe

os.environ.setdefault("QEMU_CPU", "cortex-a76")
if "--arch" in sys.argv and sys.argv[sys.argv.index("--arch") + 1] != "aarch64":
    sys.exit("async-inject-probe: AArch64 only (x86-64 has no EL2)")
sys.exit(fixprobe.run(
    "async-inject-probe", __doc__,
    "aarch64: trap-async-inject readies EL2 on its own CPU",
    ["kernel/arch/aarch64/trap.c", "kernel/arch/aarch64/hv_el2.c", "kernel/arch/aarch64/include/aarch64/trapframe.h"],
    ["trap-async-inject"],
    [],
    ["trap-async-inject ... FAIL: EL2 refused the virtual SError"],
    ["selftest: trap-async-inject: a corrected SError was taken at EL1 and execution continued"]))
