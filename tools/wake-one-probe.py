#!/usr/bin/env python3
"""Boot mutex-wake-bench on three trees: before the epoll-callback unit, after it, and this one.

The epoll-callback unit (PR #325) gave wait entries a callback kind and made
wake_one walk its list to the end so every callback entry ran; a contended
mutex unlock, which uses wake_one on a queue that never holds a callback,
became O(waiters) under the queue's spinlock with interrupts off. The
follow-up puts callback entries on a list of their own, so wake_one stops at
its one thread again. `mutex-wake-bench` (kernel/scheduler/schedtest.c) is
the measurement: 1, 8, 32 and 256 waiters blocked on a mutex, the unlock
call's duration and the unlock-to-first-acquire latency, medians of ten.

  python3 tools/wake-one-probe.py --arch x86_64 --tree before   # main at 95c635e2, wake_one stopped at its thread
  python3 tools/wake-one-probe.py --arch x86_64 --tree after    # main at abf63098 (PR #325): wake_one walks the whole list
  python3 tools/wake-one-probe.py --arch x86_64 --tree this     # HEAD: the two lists
AArch64 is the same with --arch aarch64; --lockdep0 builds with LOCKDEP=0.
The two older trees get this branch's bench added; the boot is the standard
suite and the bench's line is printed. Never edits the working tree (the
clone is of HEAD: commit first); logs under out/wake-one-<arch>-<tree>/.
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

TREES = {'before': '95c635e2', 'after': 'abf63098', 'this': None}
LINES = [r'selftest: mutex-wake-bench', r'^SELFTEST: mutex-wake-bench ', r'^SELFTEST: (PASS|FAIL)', r'^boot-test: (PASS|FAIL)']


def replace_once(source, before, after):
    if source.count(before) != 1:
        raise RuntimeError('probe anchor must occur exactly once: ' + before[:80])
    return source.replace(before, after)


def add_bench(work, ours):
    """Put this branch's bench into an older tree's schedtest.c and registry."""
    a = ours.index('/* --- mutex-wake-bench:')
    b = ours.index('/* --- semaphore --- */')
    bench = ours[a:b]
    theirs = (work / 'kernel/scheduler/schedtest.c').read_text()
    theirs = replace_once(theirs, '/* --- semaphore --- */\n', bench + '/* --- semaphore --- */\n')
    (work / 'kernel/scheduler/schedtest.c').write_text(theirs)
    decl = (work / 'kernel/include/kernel/selftest.h').read_text()
    decl = replace_once(decl, 'bool selftest_mutex(const char **reason);\n',
                        'bool selftest_mutex(const char **reason);\nbool selftest_mutex_wake_bench(const char **reason);\n')
    (work / 'kernel/include/kernel/selftest.h').write_text(decl)
    reg = (work / 'kernel/core/selftest.c').read_text()
    reg = replace_once(reg, '    { "mutex",           selftest_mutex },\n',
                       '    { "mutex",           selftest_mutex },\n    { "mutex-wake-bench", selftest_mutex_wake_bench },\n')
    (work / 'kernel/core/selftest.c').write_text(reg)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--arch', choices=['x86_64', 'aarch64'], default='x86_64')
    ap.add_argument('--tree', choices=sorted(TREES), default='this')
    ap.add_argument('--lockdep0', action='store_true', help='build with LOCKDEP=0')
    args = ap.parse_args()
    root = Path(__file__).resolve().parents[1]
    tag = 'wake-one-' + args.arch + '-' + args.tree + ('-lockdep0' if args.lockdep0 else '')
    out = root / 'out' / tag
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True, exist_ok=True)
    ours = (root / 'kernel/scheduler/schedtest.c').read_text()
    with tempfile.TemporaryDirectory(prefix='cosmo-wake-one-') as tmp:
        subprocess.run(['git', 'clone', '--quiet', '--shared', str(root), tmp], check=True)
        work = Path(tmp)
        if TREES[args.tree] is not None:
            subprocess.run(['git', 'checkout', '--quiet', TREES[args.tree]], cwd=work, check=True)
            add_bench(work, ours)
        make = ['gmake', '-j4', 'ARCH=' + args.arch, 'BUILD=debug', 'HAVE_MUSL=0', 'OUT=' + str(out)]
        if args.lockdep0:
            make.append('LOCKDEP=0')
        with (out / 'build.log').open('w') as log:
            subprocess.run(make + ['image'], cwd=work, stdout=log, stderr=subprocess.STDOUT, check=True)
        os.environ.update(COSMO_ARCH=args.arch, QEMU_ARCH=args.arch, QEMU_SMP='4')
        spec = importlib.util.spec_from_file_location('wake_one_boot', root / 'tests/boot/run_boot_test.py')
        harness = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(harness)
        timeout = '240' if args.arch == 'aarch64' else '180'
        log_path = out / 'boot.log'
        sys.argv = ['wake-one-probe', '--timeout', timeout, '--image', str(out / 'cosmoos.img'),
                    '--log', str(log_path), '--kernel', str(out / 'kernel/kernel.elf')]
        rc = harness.main()
    print('--- %s: the measurement lines (%s) ---' % (tag, log_path))
    try:
        text = log_path.read_text(errors='replace')
    except OSError:
        text = ''
    for line in text.splitlines():
        if any(re.search(p, line) for p in LINES):
            print(line.rstrip())
    if args.tree != 'this':
        print('(an older tree: the runner judges it by this tree\'s markers, so its verdict may say FAIL on a module ABI marker; the bench line is the product)')
    return rc


if __name__ == '__main__':
    sys.exit(main())
