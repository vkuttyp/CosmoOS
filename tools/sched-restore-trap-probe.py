#!/usr/bin/env python3
"""The seam of sched-restore-loop spent on a trap's preemption (flakes.md,
"sched-restore-loop: one trip short").

docs/kernel/scheduler/testing.md, "sched-restore-loop". Builds HEAD in a
throwaway git worktree (never the working tree) and boots `make test`:

  (default)  as built: the seam spends an armed resumption only when the
             restore loop can take it (interrupts on in the state it
             restores).
             -> `sched-restore-loop` must pass, all 32 armed resumptions
                left after the forced preemption.
  --old      the seam as it was, spending one at every resumption, under
             the test's forced trap-path preemption of its first run.
             -> `sched-restore-loop` must FAIL at its check that the forced
                preemption spent none (the line found in the test's
                source), with 31 of 32 left. That run is then one trip
                short, which the trips check (at least 3200) can miss
                when a stray reschedule adds one elsewhere: on x86-64 it
                did, so the probe asks for the direct check.

    tools/sched-restore-trap-probe.py --arch x86_64 [--old]

Prints the measurement and SELFTEST lines and PROBE: PASS when the
required outcome was seen. Logs stay under
out/sched-restore-trap-probe/<arch>-<mode>/.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEST = 'sched-restore-loop'
FIXED = '        if (cur->test_resume_resched != 0 && arch_irq_state_enabled(s)) {\n'
OLD = '        if (cur->test_resume_resched != 0) {   /* PROBE: spent at every resumption */\n'


def edit(path, old, new):
    s = open(path).read()
    assert s.count(old) == 1, f'anchor not found exactly once in {path}: {old!r}'
    open(path, 'w').write(s.replace(old, new))
    assert new in open(path).read(), 'mutation did not apply'


def check_line(tree):
    lines = open(os.path.join(tree, 'kernel/scheduler/schedtest.c')).read().splitlines()
    start = next(i for i, l in enumerate(lines) if l.startswith('static bool selftest_sched_restore_loop_pinned'))
    return next(i + 1 for i in range(start, len(lines)) if 'CHECK(trap_left == RESTORE_RUN);' in lines[i])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--arch', required=True, choices=['x86_64', 'aarch64'])
    ap.add_argument('--old', action='store_true')
    args = ap.parse_args()
    mode = 'old' if args.old else 'fixed'

    work = os.path.join(ROOT, 'out', 'sched-restore-trap-probe', f'{args.arch}-{mode}')
    tree = os.path.join(work, 'tree')
    if os.path.exists(tree):
        listed = subprocess.run(['git', '-C', ROOT, 'worktree', 'list', '--porcelain'],
                                capture_output=True, text=True).stdout
        if f'worktree {os.path.realpath(tree)}\n' not in listed:
            sys.exit(f'{tree} exists and is not a worktree of this probe; remove it yourself')
        subprocess.run(['git', '-C', ROOT, 'worktree', 'remove', '--force', tree], check=False)
        shutil.rmtree(tree, ignore_errors=True)
    os.makedirs(work, exist_ok=True)
    subprocess.run(['git', '-C', ROOT, 'worktree', 'add', '--detach', tree, 'HEAD'], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        sched = os.path.join(tree, 'kernel/scheduler/sched.c')
        if FIXED not in open(sched).read():
            print('PROBE: FAIL (the seam as built is not in HEAD; the probe is stale)')
            return 1
        if args.old:
            edit(sched, FIXED, OLD)
        log = os.path.join(work, 'boot.serial')
        with open(os.path.join(work, 'boot.result'), 'w') as f:
            built = subprocess.run(['gmake', '-C', tree, '-j6', 'ARCH=' + args.arch, 'BOOT_LOG=' + log, 'test'],
                                   stdout=f, stderr=subprocess.STDOUT).returncode
        serial = open(log, errors='replace').read() if os.path.exists(log) else ''
        line = next((l for l in serial.splitlines() if re.match(rf'SELFTEST: {TEST} +\.\.\. ', l)), None)
        info = next((l for l in serial.splitlines() if f'selftest: {TEST}:' in l), None)
        print(info or f'(no {TEST} measurement line; make test exited {built})')
        print(line or f'(no SELFTEST line for {TEST}: did it boot?)')
        if args.old:
            need = f'at line {check_line(tree)}'
            ok = (line is not None and ' FAIL' in line and 'trap_left == RESTORE_RUN' in line and need in line and
                  info is not None and '; 31 of 32 armed left after a trap' in info)
            print(f'PROBE: {"PASS" if ok else "FAIL"} (required: the trap check, {need}, 31 of 32 left)')
        else:
            ok = (line is not None and ' ok ' in line + ' ' and info is not None and
                  '; 32 of 32 armed left after a trap' in info)
            print(f'PROBE: {"PASS" if ok else "FAIL"} (required: {TEST} ok, 32 of 32 left)')
        return 0 if ok else 1
    finally:
        subprocess.run(['git', '-C', ROOT, 'worktree', 'remove', '--force', tree], check=False)


if __name__ == '__main__':
    sys.exit(main())
