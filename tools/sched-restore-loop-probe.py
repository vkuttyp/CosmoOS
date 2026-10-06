#!/usr/bin/env python3
"""Negative controls for schedule()'s restore loop (S31).

docs/kernel/scheduler/testing.md, "sched-restore-loop". Each mode builds HEAD
with an edit, in a throwaway git worktree (never the working tree):

  recursive  the old structure: schedule_internal's loop leaves after one
             pass and ends in arch_irq_restore, whose preemption point
             takes the pending reschedule by entering schedule() again;
             plus the forced call below; with S31's entry assertion and
             check-kernel-elf.sh's schedule_internal check removed so the
             self-test is what judges it.
             -> boots; `sched-restore-loop` must FAIL at its stack-bound
                CHECK, the line found in the test's source.
  loop       the loop as built, under the same forced call.
             -> boots; `sched-restore-loop` must pass.
  guard      the old structure (as `recursive`) with the guard left in.
             -> the kernel link must be refused by check-kernel-elf.sh,
                naming schedule_internal.

The forced call is the shape that double faulted a one-CPU boot
(docs/audit/2026-10-06-lockdep-irq-pairing-report.md): lockdep's
arch_irq_restore keeps a 160-byte buffer live across the hardware restore,
so it calls arch_irq_restore_hw instead of tail-calling it. The probe
confirms in the disassembly that it is a call before booting.

    tools/sched-restore-loop-probe.py --arch x86_64 --mode recursive

Prints the SELFTEST line and PROBE: PASS when the required outcome was seen.
Logs stay under out/sched-restore-loop-probe/<arch>-<mode>/.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEST = 'sched-restore-loop'


def edit(path, old, new):
    s = open(path).read()
    assert s.count(old) == 1, f'anchor not found exactly once in {path}: {old!r}'
    s = s.replace(old, new)
    open(path, 'w').write(s)
    assert new in open(path).read(), 'mutation did not apply'


def force_call(tree):
    edit(os.path.join(tree, 'kernel/core/lockdep.c'),
         '''void arch_irq_restore(arch_irq_state_t state)
{
    if (!__atomic_load_n(&g_off, __ATOMIC_ACQUIRE))
        irq_restore_track(state, (uintptr_t)__builtin_return_address(0));
    arch_irq_restore_hw(state);
}''',
         '''void arch_irq_restore(arch_irq_state_t state)
{
    volatile char probe_frame[160];   /* PROBE: the pre-fix wrapper's buffer, live across the call */
    probe_frame[0] = 0;
    if (!__atomic_load_n(&g_off, __ATOMIC_ACQUIRE))
        irq_restore_track(state, (uintptr_t)__builtin_return_address(0));
    arch_irq_restore_hw(state);
    probe_frame[1] = probe_frame[0];
}''')


def recursive(tree, keep_guard):
    p = os.path.join(tree, 'kernel/scheduler/sched.c')
    edit(p, '        if (!preempt_point_due(s))\n            break;\n',
         '        break;   /* PROBE: one pass; the restore below takes the reschedule */\n')
    edit(p, '    thread_current()->sched_nest--;\n#endif\n    arch_irq_restore_nopoint(s);\n}',
         '    thread_current()->sched_nest--;\n#endif\n    arch_irq_restore(s);   /* PROBE: the preempting restore */\n}')
    if keep_guard:
        return
    edit(p, '''    if (t->sched_nest != 0)
        panic("schedule() entered on thread '%s' from inside its own (S31)", t->name);
''', '    /* PROBE: S31 assertion removed */\n')
    edit(os.path.join(tree, 'scripts/check-kernel-elf.sh'),
         'never_reaches required schedule_internal arch_irq_restore arch_irq_restore_hw preempt_point sched_preempt\n',
         '# PROBE: schedule_internal check removed\n')


def bound_line(tree):
    lines = open(os.path.join(tree, 'kernel/scheduler/schedtest.c')).read().splitlines()
    start = next(i for i, l in enumerate(lines) if l.startswith('static bool selftest_sched_restore_loop_pinned'))
    return next(i + 1 for i in range(start, len(lines)) if 'CHECK(worst <= base + RESTORE_BOUND);' in lines[i])


def objdump():
    for c in ('llvm-objdump', os.path.expanduser('~/.swiftly/bin/llvm-objdump')):
        if shutil.which(c) or os.path.exists(c):
            return c
    return 'llvm-objdump'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--arch', required=True, choices=['x86_64', 'aarch64'])
    ap.add_argument('--mode', required=True, choices=['recursive', 'loop', 'guard'])
    args = ap.parse_args()

    work = os.path.join(ROOT, 'out', 'sched-restore-loop-probe', f'{args.arch}-{args.mode}')
    tree = os.path.join(work, 'tree')
    if os.path.exists(tree):
        subprocess.run(['git', '-C', ROOT, 'worktree', 'remove', '--force', tree], check=False)
        shutil.rmtree(tree, ignore_errors=True)
    os.makedirs(work, exist_ok=True)
    subprocess.run(['git', '-C', ROOT, 'worktree', 'add', '--detach', tree, 'HEAD'], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        force_call(tree)
        if args.mode in ('recursive', 'guard'):
            recursive(tree, keep_guard=args.mode == 'guard')
        build_log = os.path.join(work, 'build.log')
        with open(build_log, 'w') as f:
            built = subprocess.run(['gmake', '-C', tree, '-j6', 'ARCH=' + args.arch, 'image'],
                                   stdout=f, stderr=subprocess.STDOUT).returncode == 0
        blog = open(build_log, errors='replace').read()
        if args.mode == 'guard':
            refused = 'check-kernel-elf: schedule_internal reaches arch_irq_restore' in blog
            print(next((l for l in blog.splitlines() if l.startswith('check-kernel-elf:')), '(no check-kernel-elf line)'))
            ok = not built and refused
            print(f'PROBE: {"PASS" if ok else "FAIL"} (required: the link refused, naming schedule_internal)')
            return 0 if ok else 1
        if not built:
            print(f'PROBE: FAIL (build failed; {build_log})')
            return 1
        elf = os.path.join(tree, 'out', f'{args.arch}-debug', 'kernel', 'kernel.elf')
        dis = subprocess.run([objdump(), '-d', '--no-show-raw-insn', '--disassemble-symbols=arch_irq_restore', elf],
                             capture_output=True, text=True).stdout
        if not re.search(r'\s(call|callq|bl)\s.*<arch_irq_restore_hw>', dis):
            print('PROBE: FAIL (the forced link is not a call in the disassembly; the probe would prove nothing)')
            return 1
        log = os.path.join(work, 'boot.serial')
        with open(os.path.join(work, 'boot.result'), 'w') as f:
            subprocess.run(['gmake', '-C', tree, 'ARCH=' + args.arch, 'BOOT_LOG=' + log, 'test'],
                           stdout=f, stderr=subprocess.STDOUT)
        serial = open(log, errors='replace').read() if os.path.exists(log) else ''
        line = next((l for l in serial.splitlines() if re.match(rf'SELFTEST: {TEST} +\.\.\. ', l)), None)
        info = next((l for l in serial.splitlines() if f'selftest: {TEST}:' in l), None)
        print(info or f'(no {TEST} measurement line)')
        print(line or f'(no SELFTEST line for {TEST}: did it boot?)')
        if args.mode == 'recursive':
            need = f'at line {bound_line(tree)}'
            ok = line is not None and ' FAIL' in line and 'worst <= base + RESTORE_BOUND' in line and need in line
            print(f'PROBE: {"PASS" if ok else "FAIL"} (required: the stack-bound check, {need})')
        else:
            ok = line is not None and ' ok ' in line + ' '
            print(f'PROBE: {"PASS" if ok else "FAIL"} (required: {TEST} ok under the forced call)')
        return 0 if ok else 1
    finally:
        subprocess.run(['git', '-C', ROOT, 'worktree', 'remove', '--force', tree], check=False)


if __name__ == '__main__':
    sys.exit(main())
