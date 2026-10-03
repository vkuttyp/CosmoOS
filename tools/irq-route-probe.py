#!/usr/bin/env python3
"""Exercise irq-route's delivery deadline and failure cleanup in an isolated clone.

old/fixed slow the real periodic source to 20 Hz; silent leaves the line
masked; broken-mask omits masking at the normal 200 Hz. The runner releases
resources and runs irq-affinity before an exact expected-panic result.
"""
import argparse
import importlib.util
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


def once(source, before, after):
    if source.count(before) != 1:
        raise RuntimeError('probe anchor must occur exactly once: ' + before[:80])
    return source.replace(before, after)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--arch', choices=['x86_64', 'aarch64'], default='x86_64')
    parser.add_argument('--mode', choices=['old', 'fixed', 'silent', 'broken-mask'], default='fixed')
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    parent = root / 'out' / ('irq-route-' + args.arch + '-' + args.mode)
    parent.mkdir(parents=True, exist_ok=True)
    out = Path(tempfile.mkdtemp(prefix='run-', dir=parent))
    print('irq-route-probe: artifacts: ' + str(out), flush=True)
    source = (root / 'kernel/scheduler/schedtest.c').read_text()
    start = source.index('bool selftest_irq_route(const char **reason)')
    end = source.index('/* --- threads --- */', start)
    test = source[start:end]
    if args.mode in ('old', 'fixed'):
        test = once(test, 'arch_test_periodic_irq_start(200)', 'arch_test_periodic_irq_start(20)')
    if args.mode == 'old':
        start_wait = test.index('    /* Prove repeated delivery,')
        end_wait = test.index('    CHECK(irq_disable(gsi) == 0);', start_wait)
        test = test[:start_wait] + '''    udelay(50000);
    unsigned hits = __atomic_load_n(&g_pit_hits, __ATOMIC_RELAXED);
    CHECK(hits >= 5);
''' + test[end_wait:]
    elif args.mode == 'silent':
        test = once(test, '    CHECK(irq_enable(gsi) == 0);', '    /* Leave the route masked: no delivery. */')
    elif args.mode == 'broken-mask':
        test = once(test, '    CHECK(irq_disable(gsi) == 0);', '    /* Deliberately leave delivery enabled. */')
    if args.mode == 'fixed':
        anchor = '    CHECK(irq_disable(gsi) == 0);'
        test = once(test, anchor, '    kinfo("IRQPROBE: delivered=%u", hits);\n' + anchor)
    source = source[:start] + test + source[end:]
    runner = (root / 'kernel/core/selftest.c').read_text()
    entries = ('    { "irq-route",       selftest_irq_route },\n'
               '    { "irq-affinity",    selftest_irq_affinity },\n')
    runner = once(runner, entries, '')
    runner = once(runner, 'static const struct selftest tests[] = {\n',
                  'static const struct selftest tests[] = {\n' + entries)
    anchor = '        unsigned released = run_defers();'
    runner = once(runner, anchor, anchor + '''
        if (strcmp(tests[i].name, "irq-route") == 0)
            kinfo("IRQPROBE: route result=%u released=%u reason=%s", ok, released, ok ? "none" : reason);
        if (strcmp(tests[i].name, "irq-affinity") == 0)
            panic("IRQPROBE: affinity result=%u reason=%s", ok, ok ? "none" : reason);
''')
    if args.mode == 'fixed':
        outcome = r'route result=1 released=0 reason=none$'
    else:
        reason = {'old': 'check failed: hits >= 5',
                  'silent': 'IRQ route did not deliver five interrupts before its deadline',
                  'broken-mask': 'check failed: __atomic_load_n(&g_pit_hits, __ATOMIC_RELAXED) == after_mask'}[args.mode]
        outcome = 'route result=0 released=2 reason=' + re.escape(reason)
        outcome += '$' if args.mode == 'silent' else r' at line [0-9]+$'
    with tempfile.TemporaryDirectory(prefix='cosmo-irq-route-') as tmp:
        subprocess.run(['git', 'clone', '--quiet', '--shared', str(root), tmp], check=True)
        work = Path(tmp)
        (work / 'kernel/scheduler/schedtest.c').write_text(source)
        (work / 'kernel/core/selftest.c').write_text(runner)
        with (out / 'build.log').open('w') as log:
            subprocess.run(['gmake', '-j4', 'ARCH=' + args.arch, 'BUILD=debug', 'HAVE_MUSL=0',
                            'OUT=' + str(out), 'image'], cwd=work,
                           stdout=log, stderr=subprocess.STDOUT, check=True)
        sys.path.insert(0, str(root / 'tests/boot'))
        spec = importlib.util.spec_from_file_location('irq_boot', root / 'tests/boot/run_boot_test.py')
        harness = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(harness)
        harness.PANIC_REQUIRED_MARKERS = [r'IRQPROBE: ' + outcome,
                                         r'^KERNEL PANIC: IRQPROBE: affinity result=1 reason=none$',
                                         r'^halting\.$']
        affinity = (r'irq-affinity: GSI [0-9]+ delivered to each of 4 CPUs in turn$'
                    if args.arch == 'aarch64' else
                    r'irq-affinity: no line this controller can raise by hand; skipping$')
        harness.PANIC_REQUIRED_MARKERS.append(affinity)
        if args.mode == 'silent':
            harness.PANIC_REQUIRED_MARKERS.append(r'irq-route: delivery deadline expired with 0 of 5 hits$')
        if args.mode == 'fixed':
            harness.PANIC_REQUIRED_MARKERS.append(r'IRQPROBE: delivered=([5-9]|[1-9][0-9]+)$')
        harness.PANIC_FORBIDDEN_MARKERS = [r'^KERNEL PANIC: IRQPROBE: affinity result=0']
        os.environ.update(COSMO_ARCH=args.arch, QEMU_ARCH=args.arch, QEMU_SMP='4')
        sys.argv = ['irq-route-probe', '--expect-panic', 'fault', '--timeout', '30',
                    '--image', str(out / 'cosmoos.img'), '--log', str(out / 'boot.log')]
        return harness.main()


if __name__ == '__main__':
    sys.exit(main())
