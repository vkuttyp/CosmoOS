#!/usr/bin/env python3
"""Show a completion handler refilled while it runs stop its CPU's tick, and the budget end it.

docs/testing/flakes.md, "`virtio-remove-inflight` held for 184 s, CPU 0 in
virtio-blk's completion loop": vblk_done drained the used ring until it
found it empty, with interrupts off, and a submitter kept it from ever
being empty. `blk-irq-budget` builds that on purpose: every bio's
completion resubmits it through the block layer's own pending queue and
waits for the device's next completion, so the handler always has one
more. The fix bounds every completion handler with an irq_poll
(kernel/include/kernel/irqpoll.h): IRQ_POLL_BUDGET per call, the rest on
the CPU's irqpoll worker.

  python3 tools/irq-budget-probe.py --tree before            # the commit before the fix: the test fails
  python3 tools/irq-budget-probe.py --tree before --storm-ms 12000   # the same, long enough for a hard-lockup warning
  python3 tools/irq-budget-probe.py --old                    # this tree, the budget removed: the test fails
  python3 tools/irq-budget-probe.py                          # this tree: the test passes
--arch aarch64 for the other architecture; --smp N for N CPUs (default 4).
Never edits the working tree (the clone is of HEAD: commit first); logs
under out/irq-budget-<arch>-<tree>[-old][-stormN]-smpN/.
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

TEST_SUBJECT = 'selftest: blk-irq-budget, a completion handler refilled while it runs'
LINES = [r'blk-irq-budget', r'no tick for', r'hard lockup', r'popped \d+, more than the ring', r'^boot-test: (PASS|FAIL)']


def replace_once(path, before, after):
    source = path.read_text()
    if source.count(before) != 1:
        raise RuntimeError('probe anchor must occur exactly once in %s: %s' % (path, before[:80]))
    path.write_text(source.replace(before, after))


def sha_of(work, subject):
    sha = subprocess.run(['git', 'log', '--format=%H', '--fixed-strings', '--grep', subject, '-1'], cwd=work,
                         check=True, capture_output=True, text=True).stdout.strip()
    if not sha:
        raise RuntimeError('no commit with subject: ' + subject)
    return sha


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--arch', choices=['x86_64', 'aarch64'], default='x86_64')
    ap.add_argument('--tree', choices=['before', 'this'], default='this',
                    help='before: the reproduction test\'s own commit, without the fix')
    ap.add_argument('--old', action='store_true', help='remove the budget (IRQ_POLL_BUDGET = UINT32_MAX), this tree')
    ap.add_argument('--storm-ms', type=int, default=0, help='the storm\'s length (default the test\'s 1000)')
    ap.add_argument('--smp', type=int, default=4)
    args = ap.parse_args()
    if args.old and args.tree != 'this':
        ap.error('--old applies to --tree this')
    root = Path(__file__).resolve().parents[1]
    tag = 'irq-budget-%s-%s%s%s-smp%d' % (args.arch, args.tree, '-old' if args.old else '',
                                          '-storm%d' % args.storm_ms if args.storm_ms else '', args.smp)
    out = root / 'out' / tag
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='cosmo-irq-budget-') as tmp:
        subprocess.run(['git', 'clone', '--quiet', '--shared', str(root), tmp], check=True)
        work = Path(tmp)
        if args.tree == 'before':
            sha = sha_of(work, TEST_SUBJECT)
            subprocess.run(['git', 'checkout', '--quiet', sha], cwd=work, check=True)
            print('probe: %s, the reproduction test without the fix' % sha[:12])
        if args.old:
            replace_once(work / 'kernel/include/kernel/irqpoll.h', '#define IRQ_POLL_BUDGET 32u',
                         '#define IRQ_POLL_BUDGET 0xffffffffu   /* PROBE: no budget */')
        if args.storm_ms:
            replace_once(work / 'kernel/device/devtest.c', '#define BIRQ_STORM_MS 1000u',
                         '#define BIRQ_STORM_MS %du   /* PROBE */' % args.storm_ms)
        make = ['gmake', '-j8', 'ARCH=' + args.arch, 'BUILD=debug', 'HAVE_MUSL=0', 'OUT=' + str(out)]
        with (out / 'build.log').open('w') as log:
            subprocess.run(make + ['image'], cwd=work, stdout=log, stderr=subprocess.STDOUT, check=True)
        os.environ.update(COSMO_ARCH=args.arch, QEMU_ARCH=args.arch, QEMU_SMP=str(args.smp))
        spec = importlib.util.spec_from_file_location('irq_budget_boot', work / 'tests/boot/run_boot_test.py')
        harness = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(harness)
        log_path = out / 'boot.log'
        sys.argv = ['irq-budget-probe', '--timeout', '400', '--image', str(out / 'cosmoos.img'), '--log',
                    str(log_path), '--kernel', str(out / 'kernel/kernel.elf')]
        try:
            harness.main()
        except SystemExit:
            pass
    print('--- %s (%s) ---' % (tag, log_path))
    try:
        text = log_path.read_text(errors='replace')
    except OSError:
        text = ''
    for line in text.splitlines():
        if any(re.search(p, line) for p in LINES):
            print(line.rstrip()[:400])
    passed = re.search(r'^SELFTEST: blk-irq-budget +\.\.\. ok', text, re.M) is not None
    failed = re.search(r'^SELFTEST: blk-irq-budget +\.\.\. FAIL', text, re.M) is not None
    if not (passed or failed):
        print('probe: blk-irq-budget did not run to a verdict')
        return 1
    want_fail = args.old or args.tree == 'before'
    ok = failed if want_fail else passed
    print('probe: blk-irq-budget %s; %s' % ('FAILED' if failed else 'passed',
                                           'the expected verdict' if ok else 'NOT the expected verdict'))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
