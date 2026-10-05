#!/usr/bin/env python3
"""Negative controls for the two-CPU validation increment.

docs/audit/2026-10-05-two-cpu-validation-report.md, "Regression Tests".
Each mode builds HEAD with one deliberate defect in a throwaway git
worktree (never the working tree), boots it with the given CPU count,
and requires the named test to fail for the stated reason:

  teardown-late   virtio_blk releases the queue's interrupt AFTER the slot
                  walk (the original defect): virtio-remove-inflight must
                  fail its walk-after-section check, at two CPUs.
  share-cpu       the irq-order pass does not park the submitter when the
                  holder must share its CPU: at two CPUs every attempt must
                  miss the overlap, with the unregister drain seen waiting.
  no-rotate       pick_cpu scans from CPU 0 every time, ties never rotate:
                  sched-spread must report every worker on one CPU.
  no-balance      SCHED_BALANCE=0, nothing pulls the yielding pair apart:
                  sched-balance-pair must fail its yielding half.

    tools/two-cpu-probe.py --arch x86_64 --mode no-rotate --smp 3

Prints the test's SELFTEST line and PROBE: PASS when the required failure
was seen, PROBE: FAIL otherwise. The worktree and its logs stay under
out/two-cpu-probe/<arch>-<mode>-smp<N>/ for inspection.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

TEARDOWN_FREE = "    virtq_free(vb->vq);\n    vb->vq = NULL;\n"
WALK_END = "    /*\n     * The slots that are left, one at a time under the lock and"

MODES = {
    'teardown-late': dict(test='virtio-remove-inflight', need=r'held_until < walk'),
    'share-cpu': dict(test='virtio-remove-inflight', need=r'order_attempts < 4',
                      serial=r'did not overlap.*drain waited [1-9]'),
    'no-rotate': dict(test='sched-spread', need=r'piled onto one CPU'),
    'no-balance': dict(test='sched-balance-pair', need=r'y == PAIR_APART'),
}


def mutate(tree, mode):
    if mode == 'teardown-late':
        p = os.path.join(tree, 'drivers/virtio/virtio_blk.c')
        s = open(p).read()
        assert s.count(TEARDOWN_FREE) == 1, 'teardown anchor'
        s = s.replace(TEARDOWN_FREE, '')
        # Back after the walk, where the defect had it: just before the
        # boundary stamp that follows the loop.
        anchor = "#if CONFIG_DEBUG\n    /* The boundary: every completion"
        assert s.count(anchor) == 1, 'boundary anchor'
        s = s.replace(anchor, TEARDOWN_FREE + anchor)
        open(p, 'w').write(s)
        check = open(p).read()
        assert check.index(TEARDOWN_FREE) > check.index(WALK_END), 'mutation did not apply'
    elif mode == 'share-cpu':
        p = os.path.join(tree, 'kernel/device/devtest.c')
        s = open(p).read()
        old = '    if (hold_cpu == cpu) {\n        __atomic_store_n(&s.pause, 1u'
        assert s.count(old) == 1, 'pause anchor'
        s = s.replace(old, '    if (0) {\n        __atomic_store_n(&s.pause, 1u')
        open(p, 'w').write(s)
        assert 'if (0) {' in open(p).read(), 'mutation did not apply'
    elif mode == 'no-rotate':
        p = os.path.join(tree, 'kernel/scheduler/sched.c')
        s = open(p).read()
        old = 'unsigned start = n ? __atomic_fetch_add(&g_pick_rotor, 1u, __ATOMIC_RELAXED) % n : 0;'
        assert s.count(old) == 1, 'rotor anchor'
        s = s.replace(old, 'unsigned start = 0; (void)g_pick_rotor;')
        open(p, 'w').write(s)
        assert 'unsigned start = 0;' in open(p).read(), 'mutation did not apply'
    # no-balance is a build flag, not a source change.


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--arch', required=True, choices=['x86_64', 'aarch64'])
    ap.add_argument('--mode', required=True, choices=sorted(MODES))
    ap.add_argument('--smp', required=True)
    args = ap.parse_args()
    m = MODES[args.mode]

    work = os.path.join(ROOT, 'out', 'two-cpu-probe', f'{args.arch}-{args.mode}-smp{args.smp}')
    tree = os.path.join(work, 'tree')
    if os.path.exists(tree):
        subprocess.run(['git', '-C', ROOT, 'worktree', 'remove', '--force', tree], check=False)
        shutil.rmtree(tree, ignore_errors=True)
    os.makedirs(work, exist_ok=True)
    subprocess.run(['git', '-C', ROOT, 'worktree', 'add', '--detach', tree, 'HEAD'], check=True,
                   stdout=subprocess.DEVNULL)
    try:
        mutate(tree, args.mode)
        flags = ['SCHED_BALANCE=0'] if args.mode == 'no-balance' else []
        log = os.path.join(work, 'boot.serial')
        with open(os.path.join(work, 'build.log'), 'w') as f:
            rc = subprocess.run(['gmake', '-C', tree, '-j6', 'ARCH=' + args.arch, *flags, 'image'],
                                stdout=f, stderr=subprocess.STDOUT).returncode
        if rc != 0:
            print(f'PROBE: FAIL (build failed; {work}/build.log)')
            return 1
        with open(os.path.join(work, 'boot.result'), 'w') as f:
            subprocess.run(['gmake', '-C', tree, 'ARCH=' + args.arch, *flags, 'QEMU_SMP=' + args.smp,
                            'BOOT_LOG=' + log, 'test'], stdout=f, stderr=subprocess.STDOUT)
        serial = open(log, errors='replace').read() if os.path.exists(log) else ''
        line = next((l for l in serial.splitlines() if re.match(rf'SELFTEST: {re.escape(m["test"])} +\.\.\. ', l)),
                    None)
        print(line or f'(no SELFTEST line for {m["test"]}: did it boot?)')
        ok = line is not None and ' FAIL' in line and re.search(m['need'], line) is not None
        if ok and 'serial' in m:
            ok = re.search(m['serial'], serial) is not None
            print(f'serial evidence {m["serial"]!r}: {"seen" if ok else "MISSING"}')
        print(f'PROBE: {"PASS" if ok else "FAIL"} (required: {m["test"]} fails on {m["need"]!r})')
        return 0 if ok else 1
    finally:
        subprocess.run(['git', '-C', ROOT, 'worktree', 'remove', '--force', tree], check=False)


if __name__ == '__main__':
    sys.exit(main())
