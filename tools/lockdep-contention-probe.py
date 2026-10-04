#!/usr/bin/env python3
"""Exercise lockdep-contention failure cleanup in a temporary clone.

Run missed-timer with and without --old-order to compare cleanup state.
The readiness and exit modes require the corresponding timeout panic.
Both architectures are supported; logs/images remain under out/. These
targeted expected-failure boots do not replace the full self-test suite.
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--arch', choices=['x86_64', 'aarch64'], default='x86_64')
    parser.add_argument('--mode', choices=['missed-timer', 'readiness', 'exit'], default='missed-timer')
    parser.add_argument('--old-order', action='store_true')
    args = parser.parse_args()
    if args.old_order and args.mode != 'missed-timer':
        parser.error('--old-order applies only to missed-timer')
    root = Path(__file__).resolve().parent.parent
    tag = 'lockdep-cont-probe-' + args.arch + '-' + args.mode + ('-old' if args.old_order else '-fixed')
    parent = root / 'out' / tag
    parent.mkdir(parents=True, exist_ok=True)
    out = Path(tempfile.mkdtemp(prefix='run-', dir=parent))
    print('contention-probe: artifacts: ' + str(out), flush=True)
    source = (root / 'kernel/core/lockdeptest.c').read_text()
    if args.mode == 'missed-timer':
        source = replace_once(source, 'static unsigned g_cont_holding, g_cont_timer_ran;',
                              'static unsigned g_cont_holding, g_cont_timer_ran;\n'
                              'static bool g_probe_cancelled, g_probe_joined;')
        source = replace_once(source, 'timer_start(&t, 5000000ULL);', 'timer_start(&t, 5000000000ULL);')
        source = replace_once(source, 'timer_cancel_sync(&t);        /* stack timer cannot outlive this frame */',
                              'g_probe_cancelled = timer_cancel_sync(&t);')
        source = replace_once(source, '    thread_join(h);', '    thread_join(h);\n    g_probe_joined = true;')
        if args.old_order:
            anchor = '    bool timer_ran = __atomic_load_n(&g_cont_timer_ran, __ATOMIC_ACQUIRE) == 1;'
            source = replace_once(source, anchor, anchor + '\n    CHECK(timer_ran); /* reproduce the old early return */')
        anchor = '    bool r = selftest_lockdep_contention_pinned(reason);'
        source = replace_once(source, anchor, anchor + '''
    panic("CONTPROBE: result=%u held=%u cancelled=%u joined=%u irq=%u preempt=%d reason=%s",
          r, spin_is_held(&g_cont_l), g_probe_cancelled, g_probe_joined,
          arch_irq_enabled(), raw_this_cpu()->preempt_count, *reason);
''')
        state = 'held=1 cancelled=0 joined=0 irq=1 preempt=1' if args.old_order else 'held=0 cancelled=1 joined=1 irq=1 preempt=0'
        expected = '^KERNEL PANIC: CONTPROBE: result=0 ' + state + ' reason=check failed: timer_ran'
    else:
        if args.mode == 'readiness':
            anchor = '    __atomic_store_n(&g_cont_holding, 1u, __ATOMIC_RELEASE);'
            source = replace_once(source, anchor, '    for (;;) arch_cpu_relax(); /* withheld readiness */\n' + anchor)
        else:
            anchor = '    spin_unlock_irqrestore(&g_cont_l, s);\n}'
            source = replace_once(source, anchor, '    spin_unlock_irqrestore(&g_cont_l, s);\n'
                                  '    for (;;) arch_cpu_relax(); /* withheld exit */\n}')
        expected = '^KERNEL PANIC: selftest lockdep-contention: holder ' + args.mode + ' timeout; retaining thread'

    with tempfile.TemporaryDirectory(prefix='cosmo-lockdep-contention-') as tmp:
        subprocess.run(['git', 'clone', '--quiet', '--shared', str(root), tmp], check=True)
        work = Path(tmp)
        for name in ('kernel/core/spinlock.c', 'kernel/include/kernel/spinlock.h',
                     'kernel/include/kernel/selftest.h', 'kernel/core/selftest.c'):
            (work / name).write_text((root / name).read_text())
        (work / 'kernel/core/lockdeptest.c').write_text(source)
        with (out / 'build.log').open('w') as log:
            subprocess.run(['gmake', '-j4', 'ARCH=' + args.arch, 'BUILD=debug', 'LOCKDEP=1',
                            'HAVE_MUSL=0', 'OUT=' + str(out), 'image'], cwd=work,
                           stdout=log, stderr=subprocess.STDOUT, check=True)
        sys.path.insert(0, str(root / 'tests/boot'))
        spec = importlib.util.spec_from_file_location('contention_boot', root / 'tests/boot/run_boot_test.py')
        harness = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(harness)
        harness.PANIC_REQUIRED_MARKERS = [expected]
        harness.PANIC_FORBIDDEN_MARKERS = [r'^KERNEL PANIC: CONTPROBE: result=1']
        os.environ.update(COSMO_ARCH=args.arch, QEMU_ARCH=args.arch, QEMU_SMP='4')
        sys.argv = ['contention-probe', '--expect-panic', 'fault', '--timeout', '60',
                    '--image', str(out / 'cosmoos.img'), '--log', str(out / 'boot.log')]
        return harness.main()


if __name__ == '__main__':
    sys.exit(main())
