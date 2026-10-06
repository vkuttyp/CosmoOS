#!/usr/bin/env python3
"""Negative controls for lockdep's raw interrupt-state pairing check.

docs/kernel/lockdep/testing.md, "lockdep-irq-pairing". Each mode builds HEAD
with one check removed, in a throwaway git worktree (never the working
tree), boots it, and requires `lockdep-irq-pairing` to fail at the very
case that check exists for -- the case's `lockdep_expected_hits() == 1`,
found by line in the test's source, not just any failure:

  no-tracking    arch_irq_save/restore track nothing    -> case 1 (restore without a save)
  no-order       the innermost-save comparison is gone  -> case 2 (out of order)
  no-enabled     the enabled-inside-region check is gone -> case 3
  no-exit        the outstanding-at-exit check is gone  -> case 4

    tools/lockdep-irq-pairing-probe.py --arch x86_64 --mode no-order

Prints the SELFTEST line and PROBE: PASS when the required failure was
seen. Logs stay under out/lockdep-irq-pairing-probe/<arch>-<mode>/.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CASE = {'no-tracking': 1, 'no-order': 2, 'no-enabled': 3, 'no-exit': 4}


def edit(path, old, new):
    s = open(path).read()
    assert s.count(old) == 1, f'anchor not found exactly once in {path}: {old!r}'
    s = s.replace(old, new)
    open(path, 'w').write(s)
    assert new in open(path).read(), 'mutation did not apply'


def mutate(tree, mode):
    p = os.path.join(tree, 'kernel/core/lockdep.c')
    if mode == 'no-tracking':
        edit(p, '    if (!__atomic_load_n(&g_off, __ATOMIC_ACQUIRE) && (st = irq_saves_here()) != NULL) {',
             '    if (0 /* PROBE */ && (st = irq_saves_here()) != NULL) {')
        edit(p, '    if (!__atomic_load_n(&g_off, __ATOMIC_ACQUIRE))\n        irq_restore_track(',
             '    if (0 /* PROBE */)\n        irq_restore_track(')
    elif mode == 'no-order':
        edit(p, '    if (saved != state || enabled_inside) {',
             '    if (/* PROBE */ enabled_inside) {')
    elif mode == 'no-enabled':
        edit(p, '    bool enabled_inside = arch_irq_enabled();',
             '    bool enabled_inside = false;   /* PROBE */')
    elif mode == 'no-exit':
        edit(p, '    if (saves != 0) {\n        char detail[96];',
             '    if (saves != 0 && false /* PROBE */) {\n        char detail[96];')


def case_line(tree, n):
    """The line of the n-th expectation check in the pairing test."""
    lines = open(os.path.join(tree, 'kernel/core/lockdeptest.c')).read().splitlines()
    start = next(i for i, l in enumerate(lines) if l.startswith('static bool selftest_lockdep_irq_pairing_pinned'))
    hits = [i + 1 for i in range(start, len(lines)) if 'CHECK(lockdep_expected_hits() == 1);' in lines[i]]
    return hits[n - 1]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--arch', required=True, choices=['x86_64', 'aarch64'])
    ap.add_argument('--mode', required=True, choices=sorted(CASE))
    args = ap.parse_args()

    work = os.path.join(ROOT, 'out', 'lockdep-irq-pairing-probe', f'{args.arch}-{args.mode}')
    tree = os.path.join(work, 'tree')
    if os.path.exists(tree):
        subprocess.run(['git', '-C', ROOT, 'worktree', 'remove', '--force', tree], check=False)
        shutil.rmtree(tree, ignore_errors=True)
    os.makedirs(work, exist_ok=True)
    subprocess.run(['git', '-C', ROOT, 'worktree', 'add', '--detach', tree, 'HEAD'], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        mutate(tree, args.mode)
        need = f'at line {case_line(tree, CASE[args.mode])} '
        log = os.path.join(work, 'boot.serial')
        with open(os.path.join(work, 'build.log'), 'w') as f:
            if subprocess.run(['gmake', '-C', tree, '-j6', 'ARCH=' + args.arch, 'image'],
                              stdout=f, stderr=subprocess.STDOUT).returncode != 0:
                print(f'PROBE: FAIL (build failed; {work}/build.log)')
                return 1
        with open(os.path.join(work, 'boot.result'), 'w') as f:
            subprocess.run(['gmake', '-C', tree, 'ARCH=' + args.arch, 'BOOT_LOG=' + log, 'test'],
                           stdout=f, stderr=subprocess.STDOUT)
        serial = open(log, errors='replace').read() if os.path.exists(log) else ''
        line = next((l for l in serial.splitlines() if re.match(r'SELFTEST: lockdep-irq-pairing +\.\.\. ', l)), None)
        print(line or '(no SELFTEST line for lockdep-irq-pairing: did it boot?)')
        ok = line is not None and ' FAIL' in line and 'lockdep_expected_hits() == 1' in line and need in line
        print(f'PROBE: {"PASS" if ok else "FAIL"} (required: case {CASE[args.mode]}, the check {need.strip()})')
        return 0 if ok else 1
    finally:
        subprocess.run(['git', '-C', ROOT, 'worktree', 'remove', '--force', tree], check=False)


if __name__ == '__main__':
    sys.exit(main())
