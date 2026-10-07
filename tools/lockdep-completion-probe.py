#!/usr/bin/env python3
"""Negative controls for lockdep's completion classes.

docs/kernel/lockdep/testing.md, "lockdep-completion". Each mode builds HEAD
with one part of the mechanism removed, in a throwaway git worktree (never
the working tree), boots it, and requires `lockdep-completion` to fail at
the check each mode names:

  no-wait          the wait records and checks nothing: the signal-first
                   case has its C -> L edge but no wait closes the cycle,
                   so `hits_signal_first == 1` fails.
  no-signal        complete() records and checks nothing: the same case has
                   no C -> L edge for the wait to find, so the same check
                   fails (the wait's own check is intact and sees nothing).
  no-signal-check  complete() records its edges but never looks for the
                   cycle: the wait-first case's report at the signal is
                   gone, so `hits_wait_first == 1` fails; the signal-first
                   case still passes on the wait-side check.
  no-self          the self-signalling rule removed (a wait no longer
                   discards the thread's own pending complete() of the same
                   object): the control that completes the object its own
                   timed wait gave up on, holding the lock it waited with,
                   is reported, so the boot panics with the completion
                   report inside the test instead of printing its SELFTEST
                   line.

    tools/lockdep-completion-probe.py --arch x86_64 --mode no-wait

Prints the test's SELFTEST line (or the panic line) and PROBE: PASS when the
required failure was seen. Logs stay under out/lockdep-completion-probe/<arch>-<mode>/.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NEED = {
    'no-wait': 'hits_signal_first == 1',
    'no-signal': 'hits_signal_first == 1',
    'no-signal-check': 'hits_wait_first == 1',
    'no-self': None,   # a panic, not a failing check
}
PANIC = 'lockdep: a completion wait holds a lock its signaller needs'

WAIT = 'void lockdep_completion_wait(const void *c, uint16_t *spin_slot, const char *name, uintptr_t ip)\n{\n'
SIGNAL = 'void lockdep_completion_signal(const void *c, uint16_t *spin_slot, const char *name, uintptr_t ip)\n{\n'
REACHES = ('        if (lockdep_core_reaches(&g_graph, &g_scratch, held_node, node, path, 8, &path_len)) {'
           '   /* M reaches C: a waiter holding M */\n')
SELF = ('    if (t != NULL && t->completion_pending.c == c) {\n'
        '        /* This thread completed `c` itself and now waits for it: the\n'
        '         * completion ran inside the waiter\'s own call chain -- a device\n'
        '         * that completes in submit, a cancel after a timed-out wait --\n'
        '         * with the waiter\'s locks merely inherited. Not a dependency. */\n'
        '        t->completion_pending.c = NULL;\n'
        '        return;\n'
        '    }\n')


def mutate(tree, mode):
    p = os.path.join(tree, 'kernel/core/lockdep.c')
    s = open(p).read()
    if mode == 'no-wait':
        edits = [(WAIT, WAIT + '    (void)c; (void)spin_slot; (void)name; (void)ip;\n    return;   /* PROBE */\n')]
    elif mode == 'no-signal':
        edits = [(SIGNAL, SIGNAL + '    (void)c; (void)spin_slot; (void)name; (void)ip;\n    return;   /* PROBE */\n')]
    elif mode == 'no-signal-check':
        edits = [(REACHES, '        if (false) {   /* PROBE */\n')]
    else:
        edits = [(SELF, '    /* PROBE: no self-signalling rule */\n')]
    for old, new in edits:
        assert s.count(old) == 1, f'anchor: {old!r}'
        s = s.replace(old, new)
    open(p, 'w').write(s)
    assert open(p).read().count('PROBE') == len(edits), 'mutation did not apply'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--arch', required=True, choices=['x86_64', 'aarch64'])
    ap.add_argument('--mode', required=True, choices=sorted(NEED))
    args = ap.parse_args()

    work = os.path.join(ROOT, 'out', 'lockdep-completion-probe', f'{args.arch}-{args.mode}')
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
        line = next((l for l in serial.splitlines() if re.match(r'SELFTEST: lockdep-completion +\.\.\. ', l)), None)
        need = NEED[args.mode]
        if need is None:
            panic = next((l for l in serial.splitlines() if PANIC in l), None)
            print(panic or '(no completion report in the serial log)')
            print(line or '(no SELFTEST line for lockdep-completion, as required)')
            ok = panic is not None and line is None
            print(f'PROBE: {"PASS" if ok else "FAIL"} (required: the self-signal control panics with the completion report, '
                  f'before any SELFTEST line for lockdep-completion)')
            return 0 if ok else 1
        print(line or '(no SELFTEST line for lockdep-completion: did it boot?)')
        ok = line is not None and ' FAIL' in line and need in line
        print(f'PROBE: {"PASS" if ok else "FAIL"} (required: lockdep-completion fails on {need!r})')
        return 0 if ok else 1
    finally:
        subprocess.run(['git', '-C', ROOT, 'worktree', 'remove', '--force', tree], check=False)


if __name__ == '__main__':
    sys.exit(main())
