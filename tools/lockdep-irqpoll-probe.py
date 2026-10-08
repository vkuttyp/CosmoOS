#!/usr/bin/env python3
"""Negative controls for irq_poll's lockdep callback class.

docs/kernel/lockdep/testing.md, "lockdep-irqpoll". Each mode builds HEAD
with one part of the annotation removed, in a throwaway git worktree (never
the working tree), boots it, and requires `lockdep-irqpoll` to fail:

  no-wait          irq_poll_disable / irq_poll_synchronize do not acquire
                   the class (lockdep_callback_wait removed).
  no-class         no call of `poll` runs inside the class (enter/exit
                   removed), in the handler or the worker.
  no-worker-class  the worker calls `poll` outside the class; the handler's
                   call keeps it, so only the worker case can fail.

Without the class check the wait holding a lock its poll takes is no longer
named as the deadlock it is: the only check left is might_sleep behind it,
so the test, which expects a callback report there, dies on an unexpected
"sleeping call in atomic context" report instead -- lockdep reports are
fatal unless the test expected that very kind. The probe requires that
report, inside lockdep-irqpoll, and no `ok` line for the test; for
no-worker-class it also requires the handler case to have passed (the
report comes from the worker case's wait).

    tools/lockdep-irqpoll-probe.py --arch x86_64 --mode no-wait

Prints the evidence and PROBE: PASS when the required failure was seen.
Logs stay under out/lockdep-irqpoll-probe/<arch>-<mode>/.
"""
import argparse
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MODES = ('no-wait', 'no-class', 'no-worker-class')

EDITS = {
    'no-wait': [('    if (!lockdep_callback_wait(ip->class_fn, &ip->lockdep_class, ip_caller))\n        return false;\n',
                 '    (void)ip_caller;   /* PROBE */\n')],
    'no-class': [('    lockdep_callback_enter(fn, &ip->lockdep_class);\n', '    (void)fn;   /* PROBE */\n'),
                 ('    lockdep_callback_exit(fn);\n', '    /* PROBE */\n')],
    'no-worker-class': [('        unsigned n = run_poll(ip);\n        preempt_enable();\n',
                         '        unsigned n = ip->poll(ip, IRQ_POLL_BUDGET);   /* PROBE */\n'
                         '        note_one(ip, n);\n        preempt_enable();\n')],
}


def mutate(tree, mode):
    p = os.path.join(tree, 'kernel/core/irqpoll.c')
    s = open(p).read()
    for old, new in EDITS[mode]:
        assert s.count(old) == 1, f'anchor: {old!r}'
        s = s.replace(old, new)
    open(p, 'w').write(s)
    assert open(p).read().count('/* PROBE */') == len(EDITS[mode]), 'mutation did not apply'


def judge(serial, mode):
    lines = serial.splitlines()
    start = next((i for i, l in enumerate(lines) if l.startswith('SELFTEST: lockdep-callback ') and ' ok' in l), None)
    if start is None:
        return False, 'the boot never reached lockdep-irqpoll (no ok line for lockdep-callback before it)'
    after = lines[start + 1:]
    if any(l.startswith('SELFTEST: lockdep-irqpoll ') for l in after):
        return False, 'lockdep-irqpoll printed a result line: ' + next(
            l for l in after if l.startswith('SELFTEST: lockdep-irqpoll '))
    rep = next((i for i, l in enumerate(after) if l.strip() == 'lockdep: sleeping call in atomic context'), None)
    if rep is None:
        return False, 'no "sleeping call in atomic context" report after lockdep-callback'
    print('evidence:', after[rep].strip(), '|', after[rep + 1].strip() if rep + 1 < len(after) else '')
    if mode == 'no-worker-class':
        # The handler case passed first: its two expected callback reports precede the failure.
        hits = sum(1 for l in after[:rep] if 'expected report: a callback wait' in l)
        print(f'evidence: {hits} expected callback report(s) before the failure (the handler case)')
        if hits < 2:
            return False, 'the failure came before the handler case passed'
    return True, 'lockdep-irqpoll died on an unexpected sleep report at the wait'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--arch', required=True, choices=['x86_64', 'aarch64'])
    ap.add_argument('--mode', required=True, choices=MODES)
    args = ap.parse_args()

    work = os.path.join(ROOT, 'out', 'lockdep-irqpoll-probe', f'{args.arch}-{args.mode}')
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
        ok, why = judge(serial, args.mode)
        print(f'PROBE: {"PASS" if ok else "FAIL"} ({why})')
        return 0 if ok else 1
    finally:
        subprocess.run(['git', '-C', ROOT, 'worktree', 'remove', '--force', tree], check=False)


if __name__ == '__main__':
    sys.exit(main())
