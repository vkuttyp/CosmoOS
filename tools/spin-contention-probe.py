#!/usr/bin/env python3
"""Exercise real spin-wait observation and cleanup in isolated kernel builds.

Early modes force a timer callback before contention, then delay the
contender 50 ms. early-short-hold restores a 20 ms holder guard. Other modes
withhold observation, lose nested restoration, or fail benchmark cleanup.
An exact diagnostic and completed panic shutdown are required.
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
    parser.add_argument('--mode', choices=['early', 'early-short-hold', 'missing-wait', 'nested-restore',
                                         'bench-unobserved', 'bench-irqguard', 'bench-exit', 'bench-ready',
                                         'bench-startup', 'bench-startup-old'], default='early')
    args = parser.parse_args()
    if args.mode == 'bench-startup-old' and args.arch != 'x86_64':
        parser.error('bench-startup-old exercises x86 IPI-based TLB shootdown')
    root = Path(__file__).resolve().parent.parent
    parent = root / 'out' / ('spin-probe-' + args.arch + '-' + args.mode)
    parent.mkdir(parents=True, exist_ok=True)
    out = Path(tempfile.mkdtemp(prefix='run-', dir=parent))
    print('spin-contention-probe: artifacts: ' + str(out), flush=True)
    source = (root / 'kernel/core/lockdeptest.c').read_text()
    spin = (root / 'kernel/core/spinlock.c').read_text()
    runner = (root / 'kernel/core/selftest.c').read_text()
    bench = args.mode.startswith('bench-')
    if args.mode.startswith('early'):
        source = once(source, 'static unsigned g_cont_cpu, g_cont_nested_seen;',
                      'static unsigned g_cont_cpu, g_cont_nested_seen, probe_early;')
        source = once(source, '        timer_start(t, 1000000ULL); /* a callback before the wait proves nothing */',
                      '        __atomic_store_n(&probe_early, 1u, __ATOMIC_RELEASE);\n'
                      '        timer_start(t, 1000000ULL); /* a callback before the wait proves nothing */')
        anchor = '    timer_start(&t, 5000000ULL);   /* fires on this CPU while we spin below */'
        source = once(source, anchor, anchor + '''
    uint64_t probe_deadline = clock_deadline_ns(1000000000ULL);
    while (!__atomic_load_n(&probe_early, __ATOMIC_ACQUIRE)) {
        if (clock_deadline_passed(probe_deadline))
            panic("SPINPROBE: early callback guard expired");
        arch_cpu_relax();
    }
    udelay(50000); /* owner startup pin is nonpreemptible, IRQs remain on */
    kinfo("SPINPROBE: early callback observed before contender");
''')
        if args.mode == 'early-short-hold':
            start = source.index('static void cont_holder(void *arg)')
            end = source.index('static void cont_timer(', start)
            source = source[:start] + source[start:end].replace('1000000000ULL', '20000000ULL') + source[end:]
    elif args.mode == 'missing-wait':
        spin = once(spin, '    return cpu < CONFIG_MAX_CPUS && lock != NULL &&',
                    '    return false && cpu < CONFIG_MAX_CPUS && lock != NULL &&')
    elif args.mode == 'nested-restore':
        spin = once(spin, '__atomic_store_n(&g_test_waiting[cpu], previous, __ATOMIC_RELEASE);',
                    '__atomic_store_n(&g_test_waiting[cpu], NULL, __ATOMIC_RELEASE);\n            (void)previous;')
    elif args.mode in ('bench-unobserved', 'bench-irqguard'):
        source = once(source, 'waiting = spin_test_waiting_on(other, &p.lock)', 'waiting = false')
        if args.mode == 'bench-irqguard':
            source = once(source, '    for (unsigned path = 0; path < 2; path++) {',
                          '    for (unsigned path = 1; path < 2; path++) {')
            source = once(source, '    struct spin_bench_probe p = {0};',
                          '    clock_test_force_uncommon(true);\n    struct spin_bench_probe p = {0};')
    elif args.mode == 'bench-ready':
        source = once(source, '    preempt_disable();\n    __atomic_store_n(&p->ready, 1u, __ATOMIC_RELEASE);',
                      '    preempt_disable();\n    /* deliberately withhold readiness */')
    elif args.mode.startswith('bench-startup'):
        source = once(source, '#include <kernel/interrupt.h>',
                      '#include <kernel/vmm.h>\n#include <kernel/interrupt.h>')
        source = once(source, '    unsigned ready, go, acquired;',
                      '    unsigned ready, go, acquired;\n    vaddr_t probe_map;')
        source = once(source, '    struct spin_bench_probe *p = arg;', '''    struct spin_bench_probe *p = arg;
    thread_sleep_ms(20); /* let the owner reach its startup protocol */
    kinfo("SPINPROBE: startup free owner-held=%u", __atomic_load_n(&p->lock.locked, __ATOMIC_ACQUIRE));
    vm_kernel_free(p->probe_map); /* real shootdown, before waiter readiness */''')
        source = once(source, '    for (unsigned path = 0; path < 2; path++) {',
                      '    for (unsigned path = 1; path < 2; path++) {')
        anchor = '            struct thread *waiter = thread_create_on(spin_bench_waiter, &p, "spin-bench",'
        source = once(source, anchor, '''            p.probe_map = vm_kernel_alloc(4096, VM_KALLOC_POPULATE, VM_PROT_RW);
            KASSERT(p.probe_map != 0);
''' + anchor)
        if args.mode.endswith('-old'):
            start = source.index('            uint64_t ready_end = clock_deadline_ns(')
            end = source.index('            arch_irq_state_t s = 0;', start)
            source = source[:start] + source[end:] # only remove the owner readiness gate
    else:
        anchor = '    p->context_ok = p->context_ok && arch_irq_enabled() && raw_this_cpu()->preempt_count == 0;'
        source = once(source, anchor, anchor + '\n    for (;;) arch_cpu_relax(); /* deliberately withhold exit */')
    if bench:
        anchor = '            thread_join(waiter);\n            CHECK(waiting && excluded'
        source = once(source, anchor, '''            thread_join(waiter);
            kinfo("SPINPROBE: bench cleanup locked=%u waiting=%u acquired=%u irq=%u preempt=%d",
                  __atomic_load_n(&p.lock.locked, __ATOMIC_RELAXED), spin_test_waiting_on(other, &p.lock),
                  __atomic_load_n(&p.acquired, __ATOMIC_ACQUIRE), arch_irq_enabled(), raw_this_cpu()->preempt_count);
            CHECK(waiting && excluded''')
        call = '    bool ok = spin_contention_bench_pinned(reason);'
        source = once(source, call, call + '\n    panic("SPINPROBE: result=%u reason=%s", ok, ok ? "none" : *reason);')
        entry = '    { "lockdep-spin-bench", selftest_lockdep_spin_bench },\n'
    else:
        anchor = '    thread_join(h);'
        source = once(source, anchor, anchor + '''
    kinfo("SPINPROBE: contention cleanup held=%u timer-idle=%u waiting=%u irq=%u preempt=%d",
          spin_is_held(&g_cont_l), t.state == TIMER_IDLE, spin_test_waiting_on(me, &g_cont_l),
          arch_irq_enabled(), raw_this_cpu()->preempt_count);
''')
        call = '    bool r = selftest_lockdep_contention_pinned(reason);'
        source = once(source, call, call + '\n    panic("SPINPROBE: result=%u reason=%s", r, r ? "none" : *reason);')
        entry = '    { "lockdep-contention", selftest_lockdep_contention },\n'
    runner = once(runner, entry, '')
    runner = once(runner, 'static const struct selftest tests[] = {\n',
                  'static const struct selftest tests[] = {\n' + entry)
    required = [r'^halting\.$']
    if args.mode in ('early', 'bench-startup'):
        required += [r'^KERNEL PANIC: SPINPROBE: result=1 reason=none$']
    elif args.mode in ('early-short-hold', 'missing-wait'):
        required += [r'^KERNEL PANIC: SPINPROBE: result=0 reason=check failed: timer_ran at line [0-9]+$']
    elif args.mode == 'nested-restore':
        required += [r'^KERNEL PANIC: assertion failed: ' + re.escape('spin_test_waiting_on(arch_cpu_id(), &g_cont_l)') + r' at .* \(cont_timer\)$']
    elif args.mode == 'bench-exit':
        required += [r'^KERNEL PANIC: selftest lockdep-spin-bench: waiter exit timeout; retaining thread and probe$']
    elif args.mode == 'bench-ready':
        required += [r'^KERNEL PANIC: selftest lockdep-spin-bench: waiter readiness timeout; retaining thread and probe$']
    else:
        required += [r'^KERNEL PANIC: SPINPROBE: result=0 reason=check failed: waiting && excluded .* at line [0-9]+$',
                     r'SPINPROBE: bench cleanup locked=0 waiting=0 acquired=1 irq=1 preempt=0$']
    if args.mode in ('early', 'early-short-hold', 'missing-wait'):
        required += [r'SPINPROBE: contention cleanup held=0 timer-idle=1 waiting=0 irq=1 preempt=0$']
    if args.mode.startswith('early'):
        required += [r'SPINPROBE: early callback observed before contender$']
    if args.mode.startswith('bench-startup'):
        required += [r'SPINPROBE: startup free owner-held=' + ('1' if args.mode.endswith('-old') else '0') + r'$']
    with tempfile.TemporaryDirectory(prefix='cosmo-spin-contention-') as tmp:
        subprocess.run(['git', 'clone', '--quiet', '--shared', str(root), tmp], check=True)
        work = Path(tmp)
        for name in ('kernel/include/kernel/spinlock.h', 'kernel/include/kernel/selftest.h'):
            (work / name).write_text((root / name).read_text())
        for name, content in [('kernel/core/lockdeptest.c', source), ('kernel/core/spinlock.c', spin),
                              ('kernel/core/selftest.c', runner)]:
            (work / name).write_text(content)
        with (out / 'build.log').open('w') as log:
            subprocess.run(['gmake', '-j4', 'ARCH=' + args.arch, 'BUILD=debug', 'LOCKDEP=1',
                            'HAVE_MUSL=0', 'OUT=' + str(out), 'image'], cwd=work,
                           stdout=log, stderr=subprocess.STDOUT, check=True)
        sys.path.insert(0, str(root / 'tests/boot'))
        spec = importlib.util.spec_from_file_location('spin_boot', root / 'tests/boot/run_boot_test.py')
        harness = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(harness)
        harness.PANIC_REQUIRED_MARKERS = required
        harness.PANIC_FORBIDDEN_MARKERS = [r'^KERNEL PANIC: SPINPROBE: early callback guard expired$']
        os.environ.update(COSMO_ARCH=args.arch, QEMU_ARCH=args.arch,
                          QEMU_SMP='2' if args.mode == 'bench-irqguard' else '4')
        sys.argv = ['spin-probe', '--expect-panic', 'fault', '--timeout', '30',
                    '--image', str(out / 'cosmoos.img'), '--log', str(out / 'boot.log')]
        return harness.main()


if __name__ == '__main__':
    sys.exit(main())
