#!/usr/bin/env python3
"""Force a delay between `entered` and the sync in irq-sync and timer-cancel-sync.

Both tests assert that a synchronous unregister/cancel against a callback
holding for 20 ms on another CPU spans at least 10 ms. The old code
started its clock when *this* thread returned from `wait_flag(&entered)`,
so a host that held the vCPU for part of the callback's 20 ms between
that return and the clock read made a correct sync read short
(docs/testing/flakes.md, two sightings). The fixed code measures from the
callback's own entry stamp.

This probe builds a temporary clone, inserts a 15 ms spin at exactly that
point in both tests, and boots to a panic that carries both results:
  python3 tools/sync-lower-bound-probe.py --arch x86_64 --old   # must FAIL both, on `sync_ns >= MS(10)`
  python3 tools/sync-lower-bound-probe.py --arch x86_64         # must PASS both
AArch64 is the same with --arch aarch64. Four CPUs; never edits the
working tree; logs under out/sync-lower-bound-<arch>-{old,fixed}/.
"""
import argparse
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def replace_once(source, before, after):
    if source.count(before) != 1:
        raise RuntimeError('probe anchor must occur exactly once: ' + before[:80])
    return source.replace(before, after)


DELAY = '''        {   /* SYNCPROBE: the host holds this vCPU for 15 of the callback's 20 ms */
            uint64_t probe_t = clock_now_ns();
            while (clock_since_ns(probe_t) < MS(15))
                arch_cpu_relax();
        }
'''


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--arch', choices=['x86_64', 'aarch64'], default='x86_64')
    ap.add_argument('--old', action='store_true', help='time from the point this thread reaches, as before the fix')
    args = ap.parse_args()
    root = Path(__file__).resolve().parents[1]
    tag = 'sync-lower-bound-' + args.arch + ('-old' if args.old else '-fixed')
    out = root / 'out' / tag
    out.mkdir(parents=True, exist_ok=True)
    src = (root / 'kernel/core/quiescetest.c').read_text()

    # irq-sync: the delay goes between wait_flag(&entered) and the sync.
    irq_anchor = '''        CHECK(wait_flag(&p->entered, 1000));
        /* The handler is running on `cpu` right now, for ~20 ms. */
        CHECK(interrupt_unregister_sync((unsigned)vec, irq_probe_handler) == 0);
'''
    irq_fixed = '''        CHECK(wait_flag(&p->entered, 1000));
''' + DELAY + '''        CHECK(interrupt_unregister_sync((unsigned)vec, irq_probe_handler) == 0);
'''
    irq_old = '''        CHECK(wait_flag(&p->entered, 1000));
''' + DELAY + '''        uint64_t probe_t0 = clock_now_ns();
        CHECK(interrupt_unregister_sync((unsigned)vec, irq_probe_handler) == 0);
'''
    src = replace_once(src, irq_anchor, irq_old if args.old else irq_fixed)
    if args.old:
        src = replace_once(src, '        uint64_t sync_ns = clock_since_ns(p->entered_ns);\n        CHECK(__atomic_load_n(&p->done, __ATOMIC_ACQUIRE) == 1);   /* returned only after the handler */',
                           '        uint64_t sync_ns = clock_since_ns(probe_t0);\n        CHECK(__atomic_load_n(&p->done, __ATOMIC_ACQUIRE) == 1);   /* returned only after the handler */')

    # timer-cancel-sync, step 2: the same point.
    timer_anchor = '''    CHECK(wait_flag(&p->entered, 1000));
    bool was_pending = timer_cancel_sync(&p->t);
'''
    timer_delay = DELAY.replace('        ', '    ')
    timer_fixed = '''    CHECK(wait_flag(&p->entered, 1000));
''' + timer_delay + '''    bool was_pending = timer_cancel_sync(&p->t);
'''
    timer_old = '''    CHECK(wait_flag(&p->entered, 1000));
''' + timer_delay + '''    uint64_t probe_t0 = clock_now_ns();
    bool was_pending = timer_cancel_sync(&p->t);
'''
    src = replace_once(src, timer_anchor, timer_old if args.old else timer_fixed)
    if args.old:
        src = replace_once(src, '    uint64_t sync_ns = clock_since_ns(p->entered_ns);\n    CHECK(!was_pending);',
                           '    uint64_t sync_ns = clock_since_ns(probe_t0);\n    CHECK(!was_pending);')

    # Carry both verdicts out in one panic line, right after the second test.
    src = replace_once(src, '''bool selftest_irq_sync(const char **reason)
{
    cpumask_t saved = thread_pin_self();
    bool r = selftest_irq_sync_pinned(reason);
    thread_set_affinity_self(saved);
    return r;
}''', '''static bool syncprobe_irq_ok;
static const char *syncprobe_irq_reason = "";
bool selftest_irq_sync(const char **reason)
{
    cpumask_t saved = thread_pin_self();
    bool r = selftest_irq_sync_pinned(reason);
    thread_set_affinity_self(saved);
    syncprobe_irq_ok = r;
    syncprobe_irq_reason = *reason;
    return r;
}''')
    tail_anchor = '''bool selftest_timer_cancel_sync(const char **reason)
{
    cpumask_t saved = thread_pin_self();
    bool r = selftest_timer_cancel_sync_pinned(reason);
    thread_set_affinity_self(saved);
'''
    if src.count(tail_anchor) != 1:
        raise RuntimeError('timer-cancel-sync wrapper anchor changed')
    src = src.replace(tail_anchor, tail_anchor + '''    panic("SYNCPROBE: irq-sync=%u timer-cancel-sync=%u irq-reason=%s timer-reason=%s", syncprobe_irq_ok, r,
          syncprobe_irq_reason, *reason);
''')
    if '#include <kernel/panic.h>' not in src:
        src = replace_once(src, '#include <kernel/quiesce.h>', '#include <kernel/panic.h>\n#include <kernel/quiesce.h>')

    with tempfile.TemporaryDirectory(prefix='cosmo-sync-lower-bound-') as tmp:
        subprocess.run(['git', 'clone', '--quiet', '--shared', str(root), tmp], check=True)
        work = Path(tmp)
        (work / 'kernel/core/quiescetest.c').write_text(src)
        with (out / 'build.log').open('w') as log:
            subprocess.run(['gmake', '-j4', 'ARCH=' + args.arch, 'BUILD=debug', 'HAVE_MUSL=0',
                            'OUT=' + str(out), 'image'], cwd=work, stdout=log, stderr=subprocess.STDOUT, check=True)
        spec = importlib.util.spec_from_file_location('sync_boot', root / 'tests/boot/run_boot_test.py')
        harness = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(harness)
        if args.old:
            outcome = (r'irq-sync=0 timer-cancel-sync=0 irq-reason=check failed: sync_ns >= MS\(10\) at line \d+ '
                       r'timer-reason=check failed: sync_ns >= MS\(10\) at line \d+')
        else:
            outcome = r'irq-sync=1 timer-cancel-sync=1 irq-reason= timer-reason='
        harness.PANIC_REQUIRED_MARKERS = [r'^KERNEL PANIC: SYNCPROBE: ' + outcome + '$']
        harness.PANIC_FORBIDDEN_MARKERS = [r'^SELFTEST: timer-cancel-sync ', r'KERNEL PANIC \(recursive\)']
        os.environ.update(COSMO_ARCH=args.arch, QEMU_ARCH=args.arch, QEMU_SMP='4')
        sys.argv = ['sync-lower-bound-probe', '--expect-panic', 'fault', '--timeout', '120',
                    '--image', str(out / 'cosmoos.img'), '--log', str(out / 'boot.log')]
        return harness.main()


if __name__ == '__main__':
    sys.exit(main())
