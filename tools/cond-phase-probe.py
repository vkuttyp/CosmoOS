#!/usr/bin/env python3
"""
cond-phase-probe.py -- why did thrtest step 25 read the mutex word as
something other than 1 at the broadcast's probe?

One sighting (docs/testing/flakes.md, "thrtest step 25: the mutex word was
not 1 at the broadcast's probe"), CI, aarch64, on PR #262:

    thrtest: FAIL bp_state_seen == 1u at line 2424

Step 25 runs twice. A holder takes cv_m uncontended, three waiters sleep on
cv_c, and main broadcasts with libc's probe running at phase `at`: 0 after
`seq` moved and before the requeue, 1 after the requeue. The probe asserts
that cv_m's word reads 1 (held, uncontended). But the requeue wakes one
waiter, and that waiter "relocks at 2 whatever the word says" (the step's
own comment): if it reaches cv_m -- still held -- before the probe reads
the word at phase 1, the word is 2 on a correct libc. At phase 0 nobody has
been woken, and 1 is the only answer.

The probe prints each iteration's phase and the word it read (CPPROBE
lines, from the user-mode suite).

    python3 tools/cond-phase-probe.py apply [--force] [--fix]
    gmake ARCH=x86_64 test
    grep CPPROBE out/x86_64-debug/boot-test.log
    python3 tools/cond-phase-probe.py revert

--force makes the probe, at phase 1, wait (bounded at 1 s, yielding) until
the word reads 2 before it reads it: the woken waiter has reached the held
mutex first, which is the order the sighting needs, made certain. --fix is
the candidate check: phase 0 reads 1; phase 1 reads 1 or 2, the word held
either way.

`apply` and `revert` are those of tools/nvme-admin-probe.py: stamp first,
every file replaced atomically, the stamp removed last, finished by
running revert (again).
"""

import hashlib
import os
import subprocess
import sys

THR = 'userland/tests/thrtest.c'
BACKUP = '.cond-phase-probe.orig'
STAMP = '.cond-phase-probe.applied'

OLD_READ = """    bp_state_seen = cv_m.state;                 /* 1: the holder, uncontended */"""
NEW_READ_FORCE = """    if (phase == 1) {   /* CPPROBE --force: the woken waiter reaches the held mutex first */
        uint64_t cp_deadline = cosmo_clock_ns() + 1000000000ull;
        while (__atomic_load_n(&cv_m.state, __ATOMIC_ACQUIRE) != 2u && cosmo_clock_ns() < cp_deadline)
            cosmo_yield();
    }
    bp_state_seen = cv_m.state;                 /* 1: the holder, uncontended */"""

OLD_CHECK = """        cosmo_cond_broadcast(&cv_c);
        CHECK(bp_phase_seen == 3u);              /* both phases ran */
        CHECK(bp_state_seen == 1u);              /* the holder held it uncontended */"""
def new_check(fix):
    s = """        cosmo_cond_broadcast(&cv_c);
        printf("CPPROBE: step 25 at phase %u: the word read %u at the probe\\n", at, bp_state_seen);
        CHECK(bp_phase_seen == 3u);              /* both phases ran */
"""
    if fix:
        s += """        CHECK(at == 0u ? bp_state_seen == 1u : (bp_state_seen == 1u || bp_state_seen == 2u));   /* CPPROBE --fix */"""
    else:
        s += """        CHECK(bp_state_seen == 1u);              /* the holder held it uncontended */"""
    return s


def sha(p):
    return hashlib.sha256(open(p, 'rb').read()).hexdigest()


def write_atomic(path, data):
    # Whole or not at all: a probe interrupted mid-write must leave every
    # file either as it was or as intended, never half of each.
    tmp = path + '.probe-tmp'
    with open(tmp, 'wb') as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)


def git_clean(path):
    r = subprocess.run(['git', 'status', '--porcelain', '--', path], capture_output=True, text=True)
    if r.returncode != 0:     # a failed check is not a clean tree
        sys.exit(f'git status failed for {path}: {r.stderr.strip() or r.returncode}')
    return not r.stdout.strip()


def apply_files(fl):
    """Patch every file in `fl`, or none. Every patch is built in memory
    first; then the stamp is written, recording each file's original and
    patched hash; then each backup and each file is replaced atomically.
    From the stamp on, every file is exactly its original or its patched
    bytes, so revert can finish whatever an interruption left."""
    if os.path.exists(STAMP):
        sys.exit('already applied (or an apply was interrupted): run revert first')
    if os.path.exists(STAMP + '.probe-tmp'):
        os.remove(STAMP + '.probe-tmp')  # the stamp is written first: without it nothing was patched
    plan = []
    for path, edits in fl:
        if os.path.exists(path + BACKUP):
            sys.exit(f'{path + BACKUP} exists from an earlier run; restore or remove it by hand first')
        if not os.path.isfile(path):
            sys.exit(f'{path} not found: run from the top of the tree')
        if not git_clean(path):
            sys.exit(f'{path} has uncommitted changes')
        orig = open(path, 'rb').read()
        s = orig.decode()
        for a, b in edits:
            if s.count(a) != 1:
                sys.exit(f'{path}: anchor not found exactly once: {a[:50]!r}')
            s = s.replace(a, b)
        plan.append((path, orig, s.encode()))
    write_atomic(STAMP, ''.join(f'{p} {hashlib.sha256(n).hexdigest()} {hashlib.sha256(o).hexdigest()}\n'
                                for p, o, n in plan).encode())
    for path, orig, new in plan:
        write_atomic(path + BACKUP, orig)
        write_atomic(path, new)


def revert():
    if not os.path.exists(STAMP):
        if os.path.exists(STAMP + '.probe-tmp'):
            os.remove(STAMP + '.probe-tmp')   # an apply interrupted before its stamp: nothing was patched
            sys.exit('not applied (removed a partial stamp an interrupted apply left)')
        sys.exit('not applied')
    entries = [line.split() for line in open(STAMP).read().split('\n') if line]
    for path, patched, orig in entries:
        cur = sha(path)
        if cur == orig:
            continue                     # never patched, or already restored
        if cur != patched:
            sys.exit(f'{path} changed since apply; restore by hand from {path + BACKUP}')
        if not os.path.exists(path + BACKUP) or sha(path + BACKUP) != orig:
            sys.exit(f'{path} is still patched and {path + BACKUP} is missing or not its original; restore by hand')
    for path, patched, orig in entries:
        if sha(path) == patched:
            write_atomic(path, open(path + BACKUP, 'rb').read())
    # Every file is original now. The backups and every temp write_atomic can
    # leave go first and the stamp last: while the stamp exists a revert can
    # be run again and finish, and once it is gone nothing is left behind.
    for path, _, _ in entries:
        for leftover in (path + BACKUP, path + '.probe-tmp', path + BACKUP + '.probe-tmp'):
            if os.path.exists(leftover):
                os.remove(leftover)
    if os.path.exists(STAMP + '.probe-tmp'):
        os.remove(STAMP + '.probe-tmp')
    os.remove(STAMP)
    print('reverted')





def apply():
    args = sys.argv[2:]
    force = '--force' in args
    fix = '--fix' in args
    if [a for a in args if a not in ('--force', '--fix')]:
        sys.exit('usage: apply [--force] [--fix]')
    edits = [(OLD_CHECK, new_check(fix))]
    if force:
        edits.append((OLD_READ, NEW_READ_FORCE))
    apply_files([(THR, edits)])
    print('applied: thrtest step 25 instrumented'
          + ('; the phase-1 probe waits for the woken waiter to contend' if force else '')
          + ('; phase 1 accepts 1 or 2' if fix else ''))


if __name__ == '__main__':
    {'apply': apply, 'revert': revert}.get(sys.argv[1] if len(sys.argv) > 1 else '',
                                           lambda: sys.exit(__doc__))()
