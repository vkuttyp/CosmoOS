#!/usr/bin/env python3
"""Require lockdep's raw-lock re-entry panic in an isolated kernel build.

Direct recursion works on both architectures; NMI mode uses real x86 APIC
delivery in trap-paranoid. --busy interrupts an unfinished held-stack
update as well. --old-lock restores the old lock and is a negative control:
the boot harness must FAIL (normally by timeout), not accept the hang.
Ring mode checks the prerequisite panic-output boundary with the log ring
lock already held, including LOCKDEP=0. --old-ring restores the unsafe
logging path as another deliberately failing control. Images and logs are
retained in a fresh out/ subdirectory on every run.
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
    parser.add_argument('--mode', choices=['direct', 'nmi', 'ring'], default='direct')
    parser.add_argument('--operation', choices=['stats', 'acquire'], default='stats')
    parser.add_argument('--busy', action='store_true')
    parser.add_argument('--old-lock', action='store_true')
    parser.add_argument('--old-ring', action='store_true')
    parser.add_argument('--lockdep', choices=['0', '1'], default='1')
    args = parser.parse_args()
    if args.mode == 'nmi' and args.arch != 'x86_64':
        parser.error('hardware NMI delivery is only available on x86_64')
    if args.lockdep == '0' and args.mode != 'ring':
        parser.error('raw-lock probes require lockdep')
    if args.mode == 'ring' and (args.busy or args.old_lock or args.operation != 'stats'):
        parser.error('ring mode does not use --busy, --old-lock or --operation')
    root = Path(__file__).resolve().parent.parent
    tag = ('lockdep-reentry-' + args.arch + '-' + args.mode + '-' + args.operation
           + ('-busy' if args.busy else '') + ('-old' if args.old_lock else '-fixed')
           + ('-old-ring' if args.old_ring else '') + '-ld' + args.lockdep)
    parent = root / 'out' / tag
    parent.mkdir(parents=True, exist_ok=True)
    # Dependencies contain the temporary clone's absolute paths. A unique
    # build directory prevents stale .d files from naming a deleted clone.
    out = Path(tempfile.mkdtemp(prefix='run-', dir=parent))
    print('reentry-probe: artifacts: ' + str(out), flush=True)
    operation = ('struct lockdep_stats stats;\n    lockdep_get_stats(&stats);'
                 if args.operation == 'stats' else
                 'static spinlock_t lock = SPINLOCK_INIT("reentry-probe");\n'
                 '    spin_lock(&lock);\n    spin_unlock(&lock);')
    source = (root / 'kernel/core/lockdeptest.c').read_text()
    trap = (root / 'kernel/arch/x86_64/trap.c').read_text()
    log_source = (root / 'kernel/core/log.c').read_text()
    selftest = (root / 'kernel/core/selftest.c').read_text()
    if args.old_ring:
        log_source = replace_once(log_source, '    if (console_in_panic_mode())\n        return;\n', '')
    if args.mode == 'direct':
        anchor = 'bool selftest_lockdep_order(const char **reason)\n{'
        callback = ('static void reentry_probe(void *arg)\n{\n    (void)arg;\n    '
                    + operation + '\n    panic("REENTRYPROBE: unexpected return");\n}\n\n')
        source = replace_once(source, anchor, callback + anchor + '\n'
                              '    kinfo("REENTRYPROBE: direct raw-lock recursion");\n'
                              '    lockdep_test_snapshot_context('
                              + ('true' if args.busy else 'false') + ', reentry_probe, NULL);')
    elif args.mode == 'nmi':
        anchor = '    p->snapshot_ok = lockdep_snapshot_held_cpu(raw_cpu_id(), p->held, &p->held_count);'
        # The first two calls are software int $2. Re-enter only on the
        # real NMI while the graph lock is held; busy mode lets the stable
        # reader finish and attacks the next, interrupted-writer case.
        injection = ('    if (__atomic_load_n(&p->hits, __ATOMIC_ACQUIRE) == '
                     + ('3' if args.busy else '2') + ') {\n    ' + operation
                     + '\n        panic("REENTRYPROBE: unexpected return");\n    }\n')
        trap = replace_once(trap, anchor, injection + anchor)
        anchor = '        static spinlock_t held = SPINLOCK_INIT("nmi-snapshot-probe");'
        trap = replace_once(trap, anchor,
                            '        kinfo("REENTRYPROBE: hardware NMI raw-lock recursion");\n' + anchor)
    else:
        log_source = '#include <kernel/panic.h>\n' + log_source
        anchor = 'int selftest_run_all(void)\n{'
        selftest = replace_once(selftest, anchor, anchor + '\n'
                                '    kinfo("REENTRYPROBE: held-ring panic armed");\n'
                                '    kprintf("REENTRYPROBE: take ring and panic\\n");')
        anchor = '    arch_irq_state_t st = spin_lock_irqsave(&g_ring_lock);'
        # This anchor also occurs in klog_copy; only inject the writer.
        pos = log_source.index(anchor)
        end = pos + len(anchor)
        log_source = log_source[:end] + '''
    if (n == sizeof("REENTRYPROBE: take ring and panic\\n") - 1 &&
        memcmp(s, "REENTRYPROBE: take ring and panic\\n", n) == 0)
        panic("REENTRYPROBE: panic while log ring held");
''' + log_source[end:]
    lockdep = (root / 'kernel/core/lockdep.c').read_text()
    if args.old_lock:
        start = lockdep.index('    unsigned cpu = arch_cpu_id();   /* IRQ masking prevents migration */')
        end = lockdep.index('    return s;', start)
        lockdep = lockdep[:start] + ('    while (__atomic_exchange_n(&g_raw, 1u, __ATOMIC_ACQUIRE) != 0)\n'
                                    '        arch_cpu_relax();\n') + lockdep[end:]

    with tempfile.TemporaryDirectory(prefix='cosmo-lockdep-reentry-') as tmp:
        subprocess.run(['git', 'clone', '--quiet', '--shared', str(root), tmp], check=True)
        work = Path(tmp)
        for name, contents in [('kernel/core/lockdep.c', lockdep),
                               ('kernel/core/lockdeptest.c', source),
                               ('kernel/core/log.c', log_source),
                               ('kernel/core/selftest.c', selftest),
                               ('kernel/arch/x86_64/trap.c', trap)]:
            (work / name).write_text(contents)
        for name in ['kernel/core/console.c', 'kernel/include/kernel/console.h',
                     'drivers/virtio/virtio_console.c']:
            (work / name).write_text((root / name).read_text())
        with (out / 'build.log').open('w') as log:
            subprocess.run(['gmake', '-j4', 'ARCH=' + args.arch, 'BUILD=debug', 'LOCKDEP=' + args.lockdep,
                            'HAVE_MUSL=0', 'OUT=' + str(out), 'image'], cwd=work,
                           stdout=log, stderr=subprocess.STDOUT, check=True)
        sys.path.insert(0, str(root / 'tests/boot'))
        spec = importlib.util.spec_from_file_location('reentry_boot', root / 'tests/boot/run_boot_test.py')
        harness = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(harness)
        harness.PANIC_REQUIRED_MARKERS = [
            'REENTRYPROBE: ' + ('hardware NMI' if args.mode == 'nmi' else 'direct') + ' raw-lock recursion',
            r'^KERNEL PANIC: lockdep: graph raw lock re-entry on CPU [0-9]+$',
            r'^CPU: .*context: ' + ('interrupt' if args.mode == 'nmi' else 'thread'),
            r'^halting\.$',
        ]
        harness.PANIC_FORBIDDEN_MARKERS = [r'REENTRYPROBE: unexpected return',
                                           r'KERNEL PANIC \((recursive|concurrent)\)']
        if args.mode == 'ring':
            harness.PANIC_REQUIRED_MARKERS = [
                r'REENTRYPROBE: held-ring panic armed',
                r'^KERNEL PANIC: REENTRYPROBE: panic while log ring held$',
                r'^CPU: .*context: thread', r'^halting\.$',
            ]
        if args.busy:
            harness.PANIC_REQUIRED_MARKERS.append(
                r'^  held by this CPU: unavailable \(stack busy, changed, or invalid\)$')
        os.environ.update(COSMO_ARCH=args.arch, QEMU_ARCH=args.arch, QEMU_SMP='4')
        sys.argv = ['reentry-probe', '--expect-panic', 'fault', '--timeout', '30',
                    '--image', str(out / 'cosmoos.img'), '--log', str(out / 'boot.log')]
        return harness.main()


if __name__ == '__main__':
    sys.exit(main())
