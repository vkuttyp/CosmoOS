#!/usr/bin/env python3
"""Negative controls for lockdep's callback classes.

docs/kernel/lockdep/testing.md, "lockdep-callback". Each mode builds HEAD
with one part of the mechanism removed, in a throwaway git worktree (never
the working tree), boots it, and requires `lockdep-callback` to fail at
its first case's expectation -- the wait on another timer of the same
function went unreported:

  no-wait    lockdep_callback_wait checks nothing: only the old per-object
             profile remains, and it cannot see a timer that is not
             running, so the case goes unreported.
  no-class   callbacks never enter their class, so no callback -> lock edge
             exists for a wait to close a cycle through.

    tools/lockdep-callback-probe.py --arch x86_64 --mode no-wait

Prints the test's SELFTEST line and PROBE: PASS when the required failure
was seen. Logs stay under out/lockdep-callback-probe/<arch>-<mode>/.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NEED = r'hits == 1'

ENTER = 'void lockdep_callback_enter(const void *fn, uint16_t *class_slot)\n{\n'
EXIT = 'void lockdep_callback_exit(const void *fn)\n{\n'
WAIT = 'bool lockdep_callback_wait(const void *fn, uint16_t *class_slot, uintptr_t ip)\n{\n'


def mutate(tree, mode):
    p = os.path.join(tree, 'kernel/core/lockdep.c')
    s = open(p).read()
    if mode == 'no-wait':
        edits = [(WAIT, WAIT + '    (void)fn; (void)class_slot; (void)ip;\n    return true;   /* PROBE */\n')]
    else:
        edits = [(ENTER, ENTER + '    (void)fn; (void)class_slot;\n    return;   /* PROBE */\n'),
                 (EXIT, EXIT + '    (void)fn;\n    return;   /* PROBE */\n')]
    for old, new in edits:
        assert s.count(old) == 1, f'anchor: {old!r}'
        s = s.replace(old, new)
    open(p, 'w').write(s)
    assert open(p).read().count('/* PROBE */') == len(edits), 'mutation did not apply'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--arch', required=True, choices=['x86_64', 'aarch64'])
    ap.add_argument('--mode', required=True, choices=['no-wait', 'no-class'])
    args = ap.parse_args()

    work = os.path.join(ROOT, 'out', 'lockdep-callback-probe', f'{args.arch}-{args.mode}')
    tree = os.path.join(work, 'tree')
    if os.path.exists(tree):
        subprocess.run(['git', '-C', ROOT, 'worktree', 'remove', '--force', tree], check=False)
        shutil.rmtree(tree, ignore_errors=True)
    os.makedirs(work, exist_ok=True)
    subprocess.run(['git', '-C', ROOT, 'worktree', 'add', '--detach', tree, 'HEAD'], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        mutate(tree, args.mode)
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
        line = next((l for l in serial.splitlines() if re.match(r'SELFTEST: lockdep-callback +\.\.\. ', l)), None)
        print(line or '(no SELFTEST line for lockdep-callback: did it boot?)')
        ok = line is not None and ' FAIL' in line and re.search(re.escape(NEED), line) is not None
        print(f'PROBE: {"PASS" if ok else "FAIL"} (required: lockdep-callback fails on {NEED!r})')
        return 0 if ok else 1
    finally:
        subprocess.run(['git', '-C', ROOT, 'worktree', 'remove', '--force', tree], check=False)


if __name__ == '__main__':
    sys.exit(main())
