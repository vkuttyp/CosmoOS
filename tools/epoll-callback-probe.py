#!/usr/bin/env python3
"""Remove one of the epoll-callback unit's protections and show its test fail at its own check.

Readiness reaches an epoll set by callback: each item owns a wait entry on
its member's queue, run by the member's wake under that queue's lock
(docs/kernel/io/design.md, "epoll"; invariants A10, A11). Two rules keep it
sound, and this probe builds a temporary clone with one of them taken out:

  --old no-unhook     a DEL frees the item's registration without taking its
                      callback entry off the member's queue (the item is
                      leaked rather than freed, so the next wake runs a
                      callback on a removed item instead of on freed memory).
                      `epoll-wake-race` must FAIL at one of its two checks
                      that catch it: the race loop's "the reported descriptor
                      is the registered one", or the deterministic "a write
                      of the member after the DEL puts nothing in the set".
  --old no-loop-check a set may be added to a set without the reachability
                      and depth check. `epoll-nest` must FAIL at its first
                      -ELOOP check (the loop outer -> inner -> outer).

Without --old the clone is as committed and both tests must PASS. The clone
boots to a panic right after the test, carrying its verdict:
  python3 tools/epoll-callback-probe.py --arch x86_64 --old no-unhook
  python3 tools/epoll-callback-probe.py --arch x86_64 --old no-loop-check
  python3 tools/epoll-callback-probe.py --arch x86_64
AArch64 is the same with --arch aarch64. Four CPUs; never edits the working
tree (the clone is of HEAD: commit first); logs under
out/epoll-callback-<arch>-<mode>/.

The measurement (the report's "before" column):
  python3 tools/epoll-callback-probe.py --arch x86_64 --baseline [--lockdep0]
checks out the tree before the unit (BASELINE, main at the epoll-close
merge) in the clone, adds this branch's `epoll-scale` and
`epoll-close-bench` to it, boots the standard suite and prints their lines
(`epoll-scale` FAILS there: the figures grow with the count, which is the
baseline). `--lockdep0` builds with LOCKDEP=0 for either tree, since lockdep
costs about half of every debug figure; `--measure` boots this tree the same
way (the "after" column).
"""
import argparse
import importlib.util
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


def replace_once(source, before, after):
    if source.count(before) != 1:
        raise RuntimeError('probe anchor must occur exactly once: ' + before[:80])
    return source.replace(before, after)


# mode -> (the test function, its name, the checks that may be the one to fail: (expression, a
# comment on its line)). no-unhook has two: the race loop's "the reported descriptor is the
# registered one" catches the leaked item's callback when the writer's wake lands first, and
# the deterministic "nothing in the set hears it" when it does not; both are the protection.
MODES = {
    'no-unhook': ('epoll_wake_race', 'epoll-wake-race',
                  [('got == 0 || (got == 1 && out[0].fd == h)', 'CHECK(got == 0 || (got == 1 && out[0].fd == h));'),
                   ('epoll_obj_wait(ep, out, 4, 0) == 0', 'nothing in the set hears it')]),
    'no-loop-check': ('epoll_nest', 'epoll-nest',
                      [('add(inner, ho, outer, COSMO_IO_READABLE) == -ELOOP', 'a loop: outer reaches inner')]),
}


def failing_checks(tests, checks):
    """A regex alternation of the checks' failure texts, each with its line number."""
    alts = []
    for expr, marker in checks:
        for i, l in enumerate(tests.splitlines()):
            if 'CHECK(' + expr + ');' in l and marker in l:
                alts.append(re.escape(expr) + ' at line %u' % (i + 1))
                break
        else:
            raise RuntimeError('the check is not in epolltest.c as expected: ' + expr)
    return '(' + '|'.join(alts) + ')'


def wrap_with_panic(tests, fn):
    tests = replace_once(tests, 'bool selftest_%s(const char **reason)\n{' % fn,
                         'static bool probe_body_%s(const char **reason);\n'
                         'bool selftest_%s(const char **reason)\n{\n'
                         '    bool r = probe_body_%s(reason);\n'
                         '    panic("EPOLLCBPROBE: ok=%%u reason=%%s", r, *reason);\n'
                         '}\n'
                         'static bool probe_body_%s(const char **reason)\n{' % (fn, fn, fn, fn))
    if '#include <kernel/panic.h>' not in tests:
        tests = replace_once(tests, '#include <kernel/object.h>\n', '#include <kernel/object.h>\n#include <kernel/panic.h>\n')
    return tests


BASELINE = '95c635e2'   # main at the epoll-close merge (PR #324): the snapshot-and-pin wait
MEASURE_LINES = [r'selftest: epoll-scale', r'^SELFTEST: epoll-scale ', r'selftest: epoll-close-bench',
                 r'^SELFTEST: epoll-close-bench ', r'^SELFTEST: (PASS|FAIL)', r'^boot-test: (PASS|FAIL)']


def baseline_tests(ours, theirs):
    """The baseline's epolltest.c: theirs, plus our epoll-scale and epoll-close-bench
    (the helpers they need and nothing that needs this unit's API)."""
    def block(start_marker, end_marker):
        a = ours.index(start_marker)
        b = ours.index(end_marker) if end_marker else len(ours)
        return ours[a:b]
    scale = block('/* --- epoll-scale:', '/* --- epoll-wake-race:')
    bench = block('/* --- epoll-close-bench:', None)
    includes = '#include <kernel/log.h>\n#include <kernel/percpu.h>\n'
    theirs = replace_once(theirs, '#include <kernel/timer.h>\n', includes + '#include <kernel/timer.h>\n')
    return theirs + '\n' + scale + '\n' + bench


def measure(args):
    root = Path(__file__).resolve().parents[1]
    tree = 'baseline' if args.baseline else 'measure'
    tag = 'epoll-callback-' + args.arch + '-' + tree + ('-lockdep0' if args.lockdep0 else '')
    out = root / 'out' / tag
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True, exist_ok=True)
    ours_tests = (root / 'kernel/io/epolltest.c').read_text()
    with tempfile.TemporaryDirectory(prefix='cosmo-epoll-measure-') as tmp:
        subprocess.run(['git', 'clone', '--quiet', '--shared', str(root), tmp], check=True)
        work = Path(tmp)
        if args.baseline:
            subprocess.run(['git', 'checkout', '--quiet', BASELINE], cwd=work, check=True)
            theirs = (work / 'kernel/io/epolltest.c').read_text()
            (work / 'kernel/io/epolltest.c').write_text(baseline_tests(ours_tests, theirs))
            decl = (work / 'kernel/include/kernel/selftest.h').read_text()
            decl = replace_once(decl, 'bool selftest_epoll_close(const char **reason);',
                                'bool selftest_epoll_close(const char **reason);\n'
                                'bool selftest_epoll_scale(const char **reason);\n'
                                'bool selftest_epoll_close_bench(const char **reason);')
            (work / 'kernel/include/kernel/selftest.h').write_text(decl)
            reg = (work / 'kernel/core/selftest.c').read_text()
            reg = replace_once(reg, '    { "epoll-close",     selftest_epoll_close },\n',
                               '    { "epoll-close",     selftest_epoll_close },\n'
                               '    { "epoll-scale",     selftest_epoll_scale },\n'
                               '    { "epoll-close-bench", selftest_epoll_close_bench },\n')
            (work / 'kernel/core/selftest.c').write_text(reg)
        make = ['gmake', '-j4', 'ARCH=' + args.arch, 'BUILD=debug', 'HAVE_MUSL=0', 'OUT=' + str(out)]
        if args.lockdep0:
            make.append('LOCKDEP=0')
        with (out / 'build.log').open('w') as log:
            subprocess.run(make + ['image'], cwd=work, stdout=log, stderr=subprocess.STDOUT, check=True)
        os.environ.update(COSMO_ARCH=args.arch, QEMU_ARCH=args.arch, QEMU_SMP='4')
        spec = importlib.util.spec_from_file_location('epoll_measure_boot', root / 'tests/boot/run_boot_test.py')
        harness = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(harness)
        timeout = '240' if args.arch == 'aarch64' else '180'
        log_path = out / 'boot.log'
        sys.argv = ['epoll-callback-probe', '--timeout', timeout, '--image', str(out / 'cosmoos.img'),
                    '--log', str(log_path), '--kernel', str(out / 'kernel/kernel.elf')]
        rc = harness.main()
    print('--- %s: the measurement lines (%s) ---' % (tag, log_path))
    try:
        text = log_path.read_text(errors='replace')
    except OSError:
        text = ''
    for line in text.splitlines():
        if any(re.search(p, line) for p in MEASURE_LINES):
            print(line.rstrip())
    if args.baseline:
        print('(--baseline: `epoll-scale` must FAIL its bound there -- the figures grow with the count -- and nothing else)')
    return rc


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--arch', choices=['x86_64', 'aarch64'], default='x86_64')
    ap.add_argument('--old', choices=sorted(MODES), default=None, help='the protection to take out')
    ap.add_argument('--test', choices=['epoll-wake-race', 'epoll-nest'], default=None,
                    help='without --old: which test carries the verdict (default: both, one boot each)')
    ap.add_argument('--baseline', action='store_true', help='measure the tree before the unit (with the benches added)')
    ap.add_argument('--measure', action='store_true', help='measure this tree')
    ap.add_argument('--lockdep0', action='store_true', help='build with LOCKDEP=0')
    args = ap.parse_args()
    if args.baseline or args.measure:
        return measure(args)
    root = Path(__file__).resolve().parents[1]
    modes = [args.old] if args.old else (['no-unhook', 'no-loop-check'] if args.test is None else
                                         [m for m, v in MODES.items() if v[1] == args.test])
    rc_all = 0
    for mode in modes:
        fn, test_name, checks = MODES[mode]
        tag = 'epoll-callback-' + args.arch + ('-' + mode if args.old else '-fixed-' + test_name)
        out = root / 'out' / tag
        shutil.rmtree(out, ignore_errors=True)
        out.mkdir(parents=True, exist_ok=True)

        epoll = (root / 'kernel/io/epoll.c').read_text()
        if args.old == 'no-unhook':
            epoll = replace_once(epoll, '''    unhook_item(it);          /* no callback runs on this item after this */
    unready_item(ep, it);
    mutex_unlock(&ep->lock);
    mutex_unlock(&g_watch_lock);
    kobject_put(it->obj);
    kfree(it);
    return 0;''', '''    /* EPOLLCBPROBE --old no-unhook: the callback entries stay on the member's
     * queues and the item is leaked instead of freed, so the next wake runs
     * a callback on a removed registration. */
    unready_item(ep, it);
    mutex_unlock(&ep->lock);
    mutex_unlock(&g_watch_lock);
    return 0;''')
        elif args.old == 'no-loop-check':
            epoll = replace_once(epoll, '''    if (member_set) {
        int rc = nesting_allowed(ep, epoll_of(target));
        if (rc) {''', '''    if (member_set) {
        int rc = 0;   /* EPOLLCBPROBE --old no-loop-check: no reachability or depth check */
        (void)nesting_allowed;
        if (rc) {''')
        tests = wrap_with_panic((root / 'kernel/io/epolltest.c').read_text(), fn)

        with tempfile.TemporaryDirectory(prefix='cosmo-epoll-callback-') as tmp:
            subprocess.run(['git', 'clone', '--quiet', '--shared', str(root), tmp], check=True)
            work = Path(tmp)
            (work / 'kernel/io/epoll.c').write_text(epoll)
            (work / 'kernel/io/epolltest.c').write_text(tests)
            with (out / 'build.log').open('w') as log:
                subprocess.run(['gmake', '-j4', 'ARCH=' + args.arch, 'BUILD=debug', 'HAVE_MUSL=0',
                                'OUT=' + str(out), 'image'], cwd=work, stdout=log, stderr=subprocess.STDOUT, check=True)
            os.environ.update(COSMO_ARCH=args.arch, QEMU_ARCH=args.arch, QEMU_SMP='4')
            spec = importlib.util.spec_from_file_location('epoll_cb_boot', root / 'tests/boot/run_boot_test.py')
            harness = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(harness)
            if args.old:
                outcome = r'ok=0 reason=check failed: ' + failing_checks(tests, checks)
            else:
                outcome = r'ok=1 reason='
            harness.PANIC_REQUIRED_MARKERS = [r'^KERNEL PANIC: EPOLLCBPROBE: ' + outcome + '$']
            harness.PANIC_FORBIDDEN_MARKERS = [r'^SELFTEST: ' + test_name + ' ', r'KERNEL PANIC \(recursive\)']
            sys.argv = ['epoll-callback-probe', '--expect-panic', 'fault', '--timeout', '170',
                        '--image', str(out / 'cosmoos.img'), '--log', str(out / 'boot.log')]
            rc = harness.main()
            print('--- %s: %s' % (tag, 'PASS' if rc == 0 else 'FAIL (rc %d)' % rc))
            rc_all = rc_all or rc
    return rc_all


if __name__ == '__main__':
    sys.exit(main())
