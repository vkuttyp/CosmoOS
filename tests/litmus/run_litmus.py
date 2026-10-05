#!/usr/bin/env python3
"""Check the quiescence protocol's litmus tests against the RC11 model.

docs/kernel/quiesce/testing.md, "Memory ordering". Every litmus file in
tests/litmus/quiesce/ states one outcome the protocol must forbid (or, for
online-old, the outcome the old onlining order allowed). herd7 enumerates
every execution the C11 model (rc11.cat: Lahav et al., PLDI 2017)
permits, so a verdict here is exhaustive, not a sample.

Three kinds of check, all required:

  verdict   the file as written: Never (forbidden), Sometimes (allowed),
            or NoRace -- no execution of the file has a data race (RC11
            makes a racy execution undefined) and its condition, the
            point where the racing access would happen, is reachable.
  witness   each conjunct of a forbidden outcome must be reachable on its
            own, so a Never cannot come from a condition no execution
            could ever approach (a vacuous test).
  control   the same file with one order weakened, or one step removed,
            must flip (Never to Sometimes, NoRace to Race): the verdict
            depends on exactly the ordering the protocol claims it does.

    tests/litmus/run_litmus.py [--herd herd7]

Exits non-zero on any mismatch, or if herd7 is not installed.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
DIR = os.path.join(HERE, 'quiesce')

# file -> (expected verdict, the conjuncts that must each be reachable)
VERDICTS = {
    'gp.litmus': ('NoRace', []),
    'unlink.litmus': ('Never', ['1:e=1', '1:r=0']),
    'two-waiters.litmus': ('Never', ['0:t=0 /\\ 1:t=1 /\\ 2:e=2', '2:r=0']),
    'online.litmus': ('Never', ['0:m=0', '1:r=0']),
    'online-old.litmus': ('Sometimes', []),
    'wake.litmus': ('Never', ['((0:a=0 /\\ 1:a=2) \\/ (1:a=0 /\\ 0:a=2)) /\\ 0:w=0',
                              '((0:a=0 /\\ 1:a=2) \\/ (1:a=0 /\\ 0:a=2)) /\\ 1:s=0']),
}

# (file, what the control removes, [(old, new), ...]); each must flip:
# a Never file to Sometimes, a NoRace file to Race.
CONTROLS = [
    ('gp.litmus', 'Q2 relaxed',
     [('atomic_exchange_explicit(seen, e, memory_order_acq_rel)',
       'atomic_exchange_explicit(seen, e, memory_order_relaxed)')]),
    ('gp.litmus', 'W2 relaxed',
     [('atomic_load_explicit(seen, memory_order_acquire)',
       'atomic_load_explicit(seen, memory_order_relaxed)')]),
    ('unlink.litmus', 'Q1 relaxed',
     [('atomic_load_explicit(epoch, memory_order_acquire)',
       'atomic_load_explicit(epoch, memory_order_relaxed)')]),
    ('unlink.litmus', 'W1 relaxed',
     [('atomic_fetch_add_explicit(epoch, 1, memory_order_seq_cst)',
       'atomic_fetch_add_explicit(epoch, 1, memory_order_relaxed)')]),
    ('two-waiters.litmus', 'second waiter stores instead of RMW (no release sequence)',
     [('P1 (atomic_int* epoch) {\n  int t = atomic_fetch_add_explicit(epoch, 1, memory_order_seq_cst);',
       'P1 (atomic_int* epoch) {\n  int t = atomic_load_explicit(epoch, memory_order_relaxed);\n'
       '  atomic_store_explicit(epoch, 2, memory_order_relaxed);')]),
    ('online.litmus', 'no W1b fence (waiter)',
     [('  int t = atomic_fetch_add_explicit(epoch, 1, memory_order_seq_cst);\n'
       '  atomic_thread_fence(memory_order_seq_cst);\n',
       '  int t = atomic_fetch_add_explicit(epoch, 1, memory_order_seq_cst);\n')]),
    ('online.litmus', 'no Q0 fence (new CPU)',
     [('  atomic_store_explicit(online, 1, memory_order_release);\n'
       '  atomic_thread_fence(memory_order_seq_cst);\n',
       '  atomic_store_explicit(online, 1, memory_order_release);\n')]),
    ('wake.litmus', 'waitqueue_empty without the lock',
     [('  int a = atomic_fetch_add_explicit(lk, 1, memory_order_acquire);\n'
       '  int w = atomic_load_explicit(wq, memory_order_relaxed);\n'
       '  int b = atomic_fetch_add_explicit(lk, 1, memory_order_release);\n',
       '  int w = atomic_load_explicit(wq, memory_order_relaxed);\n'),
      # Nothing constrains the publisher's side any more: drop its half
      # of the non-overlap condition, or the control is vacuous.
      ('exists (((0:a=0 /\\ 1:a=2) \\/ (1:a=0 /\\ 0:a=2)) /\\ 0:w=0 /\\ 1:s=0)',
       'exists (0:w=0 /\\ 1:s=0)')]),
]


def herd(herd7, text):
    """Run herd7 under rc11.cat on litmus text; return Never or Sometimes."""
    with tempfile.NamedTemporaryFile('w', suffix='.litmus', delete=False) as f:
        f.write(text)
        path = f.name
    try:
        out = subprocess.run([herd7, '-model', 'rc11.cat', path], capture_output=True, text=True)
    finally:
        os.unlink(path)
    m = re.search(r'^Observation \S+ (Never|Sometimes|Always)', out.stdout, re.M)
    if m is None:
        raise RuntimeError(f'herd7 gave no observation:\n{out.stdout}\n{out.stderr}')
    return m.group(1), re.search(r'^Flag \*undef\*', out.stdout, re.M) is not None


def verdict(herd7, text, kind):
    """The file's verdict in the vocabulary of `kind` (NoRace/Race or Never/Sometimes)."""
    obs, racy = herd(herd7, text)
    if kind in ('NoRace', 'Race'):
        if obs == 'Never':
            return 'Unreachable'   # the racing access never happens: says nothing
        return 'Race' if racy else 'NoRace'
    return 'Sometimes' if obs == 'Always' else obs


def with_exists(text, cond):
    return re.sub(r'^exists .*$', f'exists ({cond})', text, flags=re.M)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--herd', default='herd7')
    args = ap.parse_args()
    herd7 = shutil.which(args.herd)
    if herd7 is None:
        print(f'litmus: {args.herd} not found (herdtools7: opam install herdtools7, or the Debian package)')
        return 2

    failures = 0

    def check(label, got, want):
        nonlocal failures
        ok = got == want
        print(f'{"ok  " if ok else "FAIL"} {label}: {got} (want {want})')
        if not ok:
            failures += 1

    on_disk = sorted(f for f in os.listdir(DIR) if f.endswith('.litmus'))
    if on_disk != sorted(VERDICTS):
        print(f'FAIL the litmus files on disk {on_disk} differ from the table {sorted(VERDICTS)}')
        failures += 1

    for name, (want, witnesses) in sorted(VERDICTS.items()):
        text = open(os.path.join(DIR, name)).read()
        check(f'verdict  {name}', verdict(herd7, text, want), want)
        for w in witnesses:
            check(f'witness  {name}: {w}', verdict(herd7, with_exists(text, w), 'Sometimes'), 'Sometimes')

    for name, what, edits in CONTROLS:
        text = open(os.path.join(DIR, name)).read()
        for old, new in edits:
            if text.count(old) != 1:
                print(f'FAIL control {name} ({what}): its anchor does not match the file exactly once')
                failures += 1
                break
            text = text.replace(old, new)
        else:
            flipped = 'Race' if VERDICTS[name][0] == 'NoRace' else 'Sometimes'
            check(f'control  {name}: {what}', verdict(herd7, text, flipped), flipped)

    print(f'litmus: {"PASS" if failures == 0 else f"FAIL ({failures})"}')
    return 0 if failures == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
