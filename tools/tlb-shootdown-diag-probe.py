#!/usr/bin/env python3
"""Make one CPU miss a TLB shootdown's deadline and check the report names it.

`arch_mmu_shootdown` (x86-64) gives every other CPU one second to
acknowledge its flush IPI and panics otherwise. Three sightings of that
panic (docs/testing/flakes.md, "the TLB shootdown deadline") said only
"acknowledged by 2 of 3 CPUs". The report now names each CPU that did
not answer and prints what it was doing: its current thread, interrupt
and preemption state, its last tick, and an NMI sample of its frames.

This probe builds a temporary clone whose self-test runner, before its
first test, arms the debug knob `arch_mmu_shootdown_test_hold` so that
CPU 1's next flush handler spins before acknowledging, then issues a
shootdown of one kernel page:
  python3 tools/tlb-shootdown-diag-probe.py            # 1.5 s hold: the deadline passes; the panic must name cpu 1
  python3 tools/tlb-shootdown-diag-probe.py --control  # 100 ms hold: late but in time; no missing CPU, the probe's own panic
The deadline itself is not changed. x86-64 only: AArch64 broadcasts its
invalidations (`tlbi ... is`) and has no acknowledgement to wait for.
Four CPUs; never edits the working tree; logs under
out/tlb-shootdown-diag-{forced,control}/.
"""
import argparse
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def replace_once(source, before, after):
    if source.count(before) != 1:
        raise RuntimeError('probe anchor must occur exactly once: ' + before[:80])
    return source.replace(before, after)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--control', action='store_true', help='hold for 100 ms: inside the deadline')
    args = ap.parse_args()
    root = Path(__file__).resolve().parents[1]
    tag = 'tlb-shootdown-diag-' + ('control' if args.control else 'forced')
    out = root / 'out' / tag
    shutil.rmtree(out, ignore_errors=True)   # a previous run's dependency files name a temporary clone that is gone
    out.mkdir(parents=True, exist_ok=True)
    hold_ns = 100 * 1000 * 1000 if args.control else 1500 * 1000 * 1000
    src = (root / 'kernel/core/selftest.c').read_text()
    anchor = '''int selftest_run_all(void)
{
    int failed = 0;
'''
    src = replace_once(src, anchor, anchor + '''
    {   /* TLBPROBE: CPU 1's next flush handler holds its acknowledgement */
        if (cpu_count() < 2 || !cpu_online(1))
            panic("TLBPROBE: needs CPU 1 online");
        arch_mmu_shootdown_test_hold(1, %dull);
        uint64_t probe_t0 = clock_now_ns();
        arch_mmu_shootdown(NULL, (vaddr_t)&failed & ~(vaddr_t)0xfff, 0x1000);
        panic("TLBPROBE: shootdown returned after %%llu ms, every CPU acknowledged",
              (unsigned long long)(clock_since_ns(probe_t0) / 1000000));
    }
''' % hold_ns)
    for inc in ('#include <arch/mmu.h>', '#include <kernel/panic.h>', '#include <kernel/percpu.h>'):
        if inc not in src:
            src = replace_once(src, '#include <kernel/selftest.h>', inc + '\n#include <kernel/selftest.h>')

    with tempfile.TemporaryDirectory(prefix='cosmo-tlb-shootdown-diag-') as tmp:
        subprocess.run(['git', 'clone', '--quiet', '--shared', str(root), tmp], check=True)
        work = Path(tmp)
        (work / 'kernel/core/selftest.c').write_text(src)
        with (out / 'build.log').open('w') as log:
            subprocess.run(['gmake', '-j4', 'ARCH=x86_64', 'BUILD=debug', 'HAVE_MUSL=0',
                            'OUT=' + str(out), 'image'], cwd=work, stdout=log, stderr=subprocess.STDOUT, check=True)
        spec = importlib.util.spec_from_file_location('tlb_boot', root / 'tests/boot/run_boot_test.py')
        harness = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(harness)
        if args.control:
            harness.PANIC_REQUIRED_MARKERS = [
                r'^KERNEL PANIC: TLBPROBE: shootdown returned after ([1-9]\d\d|\d{4,}) ms, every CPU acknowledged$',
            ]
            harness.PANIC_FORBIDDEN_MARKERS = [r'did not acknowledge', r'^KERNEL PANIC: mmu: TLB shootdown']
        else:
            harness.PANIC_REQUIRED_MARKERS = [
                r'^mmu: TLB shootdown of 0x[0-9a-f]+\+0x1000: 2 of 3 CPUs acknowledged within 1 s$',
                r"^cpu 1: did not acknowledge; running thread \d+ '[^']*', irq_depth [1-9]\d*, preempt_count \d+, "
                r'last tick \d+ ms ago at pc 0x[0-9a-f]+$',
                r'^cpu 1: pc 0x[0-9a-f]+ sp 0x[0-9a-f]+ \(nmi, \d+ us ago\)$',
                r'^  #0  0x[0-9a-f]+$',
                r'^KERNEL PANIC: mmu: TLB shootdown of 0x[0-9a-f]+\+0x1000 acknowledged by 2 of 3 CPUs; not by cpu 1$',
                r'^stack trace:',
                r'^halting\.',
            ]
            harness.PANIC_FORBIDDEN_MARKERS = [r'^KERNEL PANIC: TLBPROBE', r'^cpu [023]: did not acknowledge',
                                               r'KERNEL PANIC \(recursive\)']
        os.environ.update(COSMO_ARCH='x86_64', QEMU_ARCH='x86_64', QEMU_SMP='4')
        sys.argv = ['tlb-shootdown-diag-probe', '--expect-panic', 'fault', '--timeout', '90',
                    '--image', str(out / 'cosmoos.img'), '--log', str(out / 'boot.log')]
        return harness.main()


if __name__ == '__main__':
    sys.exit(main())
