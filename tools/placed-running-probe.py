#!/usr/bin/env python3
"""
placed-running-probe.py -- do the scheduler tests' spinners run when the
tests say they do?

Three tests use one pinned spinner (`mig_spinner_main`, smptest.c) and
each assumes it is RUNNING on its CPU when the test looks:

  sched-load             waits for `sched_cpu_load(busy) != 0`, which a
                         spinner merely queued already satisfies;
  sched-migrate          waits for its worker to be READY on the CPU,
                         which it is from the moment it is created;
  sched-migrate-refuses  the same wait, then expects the spinner to be
                         refused as running (NOT_READY).

Two sightings each of the first and last failing that way (docs/testing/flakes.md):

  sched-load ... FAIL: a CPU running a compute-bound thread reported no load (0 ms)
  sched-migrate-refuses: the running spinner: affinity

The probe gives the spinner an `entered` flag it sets as it starts, and
at each test's check logs whether the spinner had entered and its state
(PRPROBE lines). Each test is registered REPS times (default 100).
`--fix` makes each test wait for `entered` before its check.

    python3 tools/placed-running-probe.py apply [--fix] [REPS]
    gmake ARCH=x86_64 test
    grep PRPROBE out/x86_64-debug/boot-test.log
    python3 tools/placed-running-probe.py revert

`apply` and `revert` are those of tools/nvme-admin-probe.py: stamp first,
every file replaced atomically, the stamp removed last, finished by
running revert (again).
"""

import hashlib
import os
import subprocess
import sys

SMP = 'kernel/scheduler/smptest.c'
REG = 'kernel/core/selftest.c'
BACKUP = '.placed-running-probe.orig'
STAMP = '.placed-running-probe.applied'

BASE = [
    ("""struct mig_spinner {
    volatile unsigned stop;
};""", """struct mig_spinner {
    volatile unsigned stop;
    volatile unsigned entered;   /* PRPROBE: it has run */
};"""),
    ("""    struct mig_spinner *s = arg;
    while (!__atomic_load_n(&s->stop, __ATOMIC_ACQUIRE))""", """    struct mig_spinner *s = arg;
    __atomic_store_n(&s->entered, 1u, __ATOMIC_RELEASE);   /* PRPROBE */
    while (!__atomic_load_n(&s->stop, __ATOMIC_ACQUIRE))"""),
    # sched-load: the check
    ("""    unsigned busy_load = sched_cpu_load(busy);
    unsigned spare_load = sched_cpu_load(spare);""", """    unsigned pr_entered = __atomic_load_n(&sp.entered, __ATOMIC_ACQUIRE);   /* PRPROBE */
    int pr_state = (int)__atomic_load_n(&ts->state, __ATOMIC_ACQUIRE);
    unsigned busy_load = sched_cpu_load(busy);
    unsigned spare_load = sched_cpu_load(spare);
    kinfo("PRPROBE load: spinner entered %u, state %d at the check; busy load %u", pr_entered, pr_state, busy_load);"""),
    # sched-migrate: after the wait
    ("""    bool ready = wait_ready_on(tw, a);
    /* The worker may leave for b now; nothing else has touched its queue""", """    bool ready = wait_ready_on(tw, a);
    kinfo("PRPROBE migrate: spinner entered %u, state %d when the worker was READY",   /* PRPROBE */
          __atomic_load_n(&sp.entered, __ATOMIC_ACQUIRE), (int)__atomic_load_n(&ts->state, __ATOMIC_ACQUIRE));
    /* The worker may leave for b now; nothing else has touched its queue"""),
    # sched-migrate-refuses: before the spinner's migrate
    ("""        r = sched_migrate(ts, b);   /* the spinner: RUNNING on a */""", """        kinfo("PRPROBE refuses: spinner entered %u, state %d before its migrate",   /* PRPROBE */
              __atomic_load_n(&sp.entered, __ATOMIC_ACQUIRE), (int)__atomic_load_n(&ts->state, __ATOMIC_ACQUIRE));
        r = sched_migrate(ts, b);   /* the spinner: RUNNING on a */"""),
]

# --fix: each test waits for the spinner to have entered before its check.
FIX = [
    ("""    /* Wait for it to be the running thread there, not merely placed. */
    uint64_t deadline = clock_deadline_ns(2000000000ULL);
    while (!clock_deadline_passed(deadline) && sched_cpu_load(busy) == 0)
        thread_sleep_ms(1);""", """    /* Wait for it to be the running thread there, not merely placed. */
    uint64_t deadline = clock_deadline_ns(2000000000ULL);
    while (!clock_deadline_passed(deadline) && !__atomic_load_n(&sp.entered, __ATOMIC_ACQUIRE))   /* PRPROBE --fix */
        thread_sleep_ms(1);
    if (!__atomic_load_n(&sp.entered, __ATOMIC_ACQUIRE))
        kinfo("PRPROBE timeout: load: the spinner never entered in 2 s");"""),
]
for anchor in ("""    struct thread *tw = thread_create_on(mig_worker_main, &w, "mig-worker", SCHED_PRIO_DEFAULT, CPUMASK_OF(a));""",
               """    struct thread *tw = thread_create_on(mig_worker_main, &w, "mig-pinned", SCHED_PRIO_DEFAULT, CPUMASK_OF(a));"""):
    FIX.append((anchor, """    {   /* PRPROBE --fix: the spinner holds a before the worker is placed */
        uint64_t pr_dl = clock_deadline_ns(2000000000ULL);
        while (!__atomic_load_n(&sp.entered, __ATOMIC_ACQUIRE) && !clock_deadline_passed(pr_dl))
            thread_sleep_ms(1);
        if (!__atomic_load_n(&sp.entered, __ATOMIC_ACQUIRE))
            kinfo("PRPROBE timeout: migrate: the spinner never entered in 2 s");
    }
""" + anchor))


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


def registry_lines():
    s = open(REG).read()
    lines = []
    for name in ('sched-load', 'sched-migrate', 'sched-migrate-refuses'):
        for l in s.split('\n'):
            if l.strip().startswith('{ "%s",' % name):
                lines.append(l + '\n')
                break
        else:
            sys.exit(f'{REG}: no registry line for {name}')
    return lines


def apply():
    args = sys.argv[2:]
    fix = '--fix' in args
    args = [a for a in args if a != '--fix']
    if any(not a.isdigit() for a in args) or len(args) > 1:
        sys.exit('usage: apply [--fix] [REPS]')
    reps = int(args[0]) if args else 100
    reg = [(l, l * reps) for l in registry_lines()]
    apply_files([(SMP, BASE + (FIX if fix else [])), (REG, reg)])
    print(f'applied: three spinner tests instrumented, each registered {reps} times'
          + ('; each waits for its spinner to enter' if fix else ''))


if __name__ == '__main__':
    {'apply': apply, 'revert': revert}.get(sys.argv[1] if len(sys.argv) > 1 else '',
                                           lambda: sys.exit(__doc__))()
