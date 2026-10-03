#!/usr/bin/env python3
"""Force replies to arrive during the reporter's deadline read.

Builds a temporary clone; never edits the working tree. Run both controls:
  python3 tools/lockup-deadline-probe.py --arch x86_64 --old-order
  python3 tools/lockup-deadline-probe.py --arch x86_64
AArch64 is supported too. Logs/images remain under out/. The old polling
order must omit the spinner; the fixed order must validate its full stack.
This is a targeted probe, not a replacement for the complete boot suite.
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
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--arch', choices=['x86_64', 'aarch64'], default='x86_64')
    ap.add_argument('--old-order', action='store_true')
    args = ap.parse_args()
    root = Path(__file__).resolve().parents[1]
    tag = 'lockup-deadline-' + args.arch + ('-old' if args.old_order else '-fixed')
    out = root / 'out' / tag
    out.mkdir(parents=True, exist_ok=True)
    source = (root / 'kernel/core/lockup.c').read_text()
    # Hold the first sample's responders until the deadline read. They
    # complete during the simulated interruption of that clock read.
    helpers = '''
static unsigned deadline_probe_hold;
static bool deadline_probe_used;
static void deadline_probe_pause(uint64_t seq, cpumask_t targets)
{
    __atomic_store_n(&deadline_probe_hold, 0u, __ATOMIC_RELEASE);
    uint64_t start = clock_now_ns();
    for (;;) {
        cpumask_t got = 0;
        for (unsigned c = 0; c < cpu_count(); c++)
            if ((targets & CPUMASK_OF(c)) &&
                __atomic_load_n(&percpu_get(c)->sample.seq, __ATOMIC_ACQUIRE) == seq)
                got |= CPUMASK_OF(c);
        if (got == targets)
            break;
        if (clock_since_ns(start) > 1000000000ULL)
            panic("DEADLINEPROBE: responder guard expired");
        arch_cpu_relax();
    }
    /* Model the reporter resuming after its original 5 ms deadline. */
    while (clock_since_ns(start) < 10000000ULL)
        arch_cpu_relax();
}
'''
    source = replace_once(source, '/* --- the sample --- */', helpers + '\n/* --- the sample --- */')
    source = replace_once(source, '    record(&pc->sample, frame, want, nmi);', '''    uint64_t probe_start = clock_now_ns();
    while (__atomic_load_n(&deadline_probe_hold, __ATOMIC_ACQUIRE)) {
        if (clock_since_ns(probe_start) > 1000000000ULL)
            panic("DEADLINEPROBE: reporter guard expired");
        arch_cpu_relax();
    }
    record(&pc->sample, frame, want, nmi);''')
    anchor = '    cpumask_t targets = cpu_online_mask() & ~CPUMASK_OF(me);'
    source = replace_once(source, anchor, anchor + '''
    bool force = !deadline_probe_used;
    deadline_probe_used = true;
    if (force)
        __atomic_store_n(&deadline_probe_hold, 1u, __ATOMIC_RELEASE);
''')
    # Only the all-CPU loop is forced. The single-CPU API shares the fix.
    anchor = '        bool expired = clock_now_ns() >= deadline;'
    if source.count(anchor) != 2:
        raise RuntimeError('expected both fixed polling loops')
    if args.old_order:
        source = source.replace(anchor + '\n', '', 1)
        source = replace_once(source, 'if (got == targets || expired)',
                              'if (got == targets || (force ? (deadline_probe_pause(seq, targets), force = false, 0) : 0) || clock_now_ns() >= deadline)')
    else:
        source = source.replace(anchor, '''        if (force) {
            deadline_probe_pause(seq, targets);
            force = false;
        }
''' + anchor, 1)
    tests = (root / 'kernel/core/lockuptest.c').read_text()
    tests = replace_once(tests, '#include <kernel/lockup.h>', '#include <kernel/lockup.h>\n#include <kernel/panic.h>')
    tests = replace_once(tests, '    bool r = selftest_lockup_sample_pinned(reason);', '''    bool r = selftest_lockup_sample_pinned(reason);
    panic("DEADLINEPROBE: result=%u reason=%s", r, *reason);''')
    with tempfile.TemporaryDirectory(prefix='cosmo-lockup-deadline-') as tmp:
        subprocess.run(['git', 'clone', '--quiet', '--shared', str(root), tmp], check=True)
        work = Path(tmp)
        (work / 'kernel/core/lockup.c').write_text(source)
        (work / 'kernel/core/lockuptest.c').write_text(tests)
        with (out / 'build.log').open('w') as log:
            subprocess.run(['gmake', '-j4', 'ARCH=' + args.arch, 'BUILD=debug',
                            'HAVE_MUSL=0', 'OUT=' + str(out), 'image'], cwd=work,
                           stdout=log, stderr=subprocess.STDOUT, check=True)
        sys.path.insert(0, str(root / 'tests/boot'))
        spec = importlib.util.spec_from_file_location('deadline_boot', root / 'tests/boot/run_boot_test.py')
        harness = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(harness)
        outcome = (r'result=0 reason=check failed: m & CPUMASK_OF' if args.old_order else r'result=1 reason=')
        harness.PANIC_REQUIRED_MARKERS = [r'^KERNEL PANIC: DEADLINEPROBE: ' + outcome]
        harness.PANIC_FORBIDDEN_MARKERS = [r'^KERNEL PANIC: DEADLINEPROBE: (reporter|responder) guard expired']
        os.environ.update(COSMO_ARCH=args.arch, QEMU_ARCH=args.arch, QEMU_SMP='4')
        sys.argv = ['deadline-probe', '--expect-panic', 'fault', '--timeout', '60',
                    '--image', str(out / 'cosmoos.img'), '--log', str(out / 'boot.log')]
        return harness.main()


if __name__ == '__main__':
    sys.exit(main())
