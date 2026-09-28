#!/usr/bin/env python3
"""
migrate-stress-probe.py -- which worker makes no progress in
sched-migrate-stress, and why?

Two CI sightings, both aarch64 (docs/testing/flakes.md):

    SELFTEST: sched-migrate-stress ... FAIL: a worker made no progress under migration (268 ms)

The test starts 22 workers (8 spinners, 8 sleepers, 4 ping-pong pairs, 2
mutex contenders) and a random migrator, sleeps 200 ms, stops them, and
requires every worker to have finished at least one round. The failure
says only that one did not.

The probe instruments it and repeats it:
- each worker records when its thread first entered and when its first
  round finished (relative to the test's start);
- every repetition logs the minimum rounds per kind (the margin);
- a zero-round worker is named: kind, index, entered, first round, and the
  CPU and state of its thread when the window closed;
- the test is registered REPS times (default 40), so one boot runs it that
  many times and gives a rate.

    python3 tools/migrate-stress-probe.py apply [REPS]
    gmake ARCH=aarch64 test          # quiet, and with other boots beside it
    grep MSPROBE out/aarch64-debug/boot-test.log
    python3 tools/migrate-stress-probe.py revert

`apply` and `revert` are those of tools/nvme-admin-probe.py: stamp first,
every file replaced atomically, the stamp removed last, finished by
running revert (again).
"""

import hashlib
import os
import shutil
import subprocess
import sys

SMP = 'kernel/scheduler/smptest.c'
REG = 'kernel/core/selftest.c'
BACKUP = '.migrate-stress-probe.orig'
STAMP = '.migrate-stress-probe.applied'

EDITS = [
    ("""    volatile uint64_t rounds;
    struct completion *wait_on;   /* ping-pong: what I wait for */""",
     """    volatile uint64_t rounds;
    volatile uint64_t entered_ns, first_ns;   /* MSPROBE */
    struct completion *wait_on;   /* ping-pong: what I wait for */"""),
    ("""static void stress_check_here(struct stress_worker *w)
{
    preempt_disable();""",
     """static void stress_check_here(struct stress_worker *w)
{
    if (w->first_ns == 0)
        w->first_ns = clock_now_ns();   /* MSPROBE: the first round's check */
    preempt_disable();"""),
]
for fn in ('stress_spinner', 'stress_sleeper', 'stress_pingpong', 'stress_mutexer'):
    EDITS.append((f"""static void {fn}(void *arg)
{{
    struct stress_worker *w = arg;
""", f"""static void {fn}(void *arg)
{{
    struct stress_worker *w = arg;
    w->entered_ns = clock_now_ns();   /* MSPROBE */
"""))
EDITS += [
    ("""    volatile unsigned bad_affinity;   /* a worker found itself on a CPU its mask excludes */
    struct mutex mtx;
};""", """    volatile unsigned bad_affinity;   /* a worker found itself on a CPU its mask excludes */
    struct mutex mtx;
    struct thread *msp_thr[32];                 /* MSPROBE: each worker's thread, for the migrator */
    struct stress_worker *msp_w[32];
    volatile unsigned msp_moved[32];            /* how often the migrator moved it */
    volatile unsigned msp_moved_before_run[32]; /* of those, before its first round */
};"""),
    ("""        if (sched_migrate_from(from, to, 0, &moved) == SCHED_MIGRATED)   /* no gap: move for no reason, as the adversary does */
            w->rounds++;""", """        if (sched_migrate_from(from, to, 0, &moved) == SCHED_MIGRATED) {   /* no gap: move for no reason, as the adversary does */
            w->rounds++;
            for (unsigned q = 0; q < 32; q++)   /* MSPROBE: whose move was it */
                if (w->sh->msp_thr[q] == moved) {
                    w->sh->msp_moved[q]++;
                    if (w->sh->msp_w[q] != NULL && w->sh->msp_w[q]->first_ns == 0)
                        w->sh->msp_moved_before_run[q]++;
                    break;
                }
        }"""),
    ("""    t[made++] = thread_create(stress_migrator, &mig, "mig-migrator", SCHED_PRIO_DEFAULT);""",
     """    {   /* MSPROBE: the migrator's table, filled before it starts */
        struct stress_worker *ws[] = { spin, sleep, pp, mx };
        unsigned ns[] = { SPIN, SLEEP, 2 * PAIRS, MUTEX }, q = 0;
        for (unsigned a = 0; a < 4; a++)
            for (unsigned b = 0; b < ns[a]; b++, q++) {
                sh.msp_thr[q] = t[q];
                sh.msp_w[q] = &ws[a][b];
            }
    }
    t[made++] = thread_create(stress_migrator, &mig, "mig-migrator", SCHED_PRIO_DEFAULT);"""),
    ("""    unsigned before = thread_count();
    uint64_t moves = sched_migration_count();
    enum { SPIN = 8, SLEEP = 8, PAIRS = 4, MUTEX = 2 };""",
     """    unsigned before = thread_count();
    uint64_t moves = sched_migration_count();
    uint64_t msp_t0 = clock_now_ns();   /* MSPROBE */
    enum { SPIN = 8, SLEEP = 8, PAIRS = 4, MUTEX = 2 };"""),
    ("""    thread_sleep_ms(200);
    __atomic_store_n(&sh.stop, 1u, __ATOMIC_RELEASE);""",
     """    thread_sleep_ms(200);
    uint64_t msp_woke = clock_now_ns() - msp_t0;   /* MSPROBE: how long the 200 ms sleep really took */
    /* MSPROBE: each thread's CPU and state as the window closes, before stop. */
    int msp_cpu[SPIN + SLEEP + 2 * PAIRS + MUTEX + 1];
    int msp_state[SPIN + SLEEP + 2 * PAIRS + MUTEX + 1];
    for (unsigned i = 0; i < made; i++) {
        msp_cpu[i] = t[i] ? t[i]->cpu : -1;
        msp_state[i] = t[i] ? (int)t[i]->state : -1;
    }
    unsigned msp_load[CONFIG_MAX_CPUS], msp_maxload = 0;   /* MSPROBE: the queues at the close */
    for (unsigned c = 0; c < cpu_count() && c < CONFIG_MAX_CPUS; c++) {
        msp_load[c] = sched_cpu_load(c);
        if (msp_load[c] > msp_maxload)
            msp_maxload = msp_load[c];
    }
    __atomic_store_n(&sh.stop, 1u, __ATOMIC_RELEASE);"""),
    ("""    uint64_t moved = sched_migration_count() - moves;
    bool progress = true;""",
     """    uint64_t moved = sched_migration_count() - moves;
    {   /* MSPROBE: the margin, and every zero-round worker by name */
        struct { const char *kind; struct stress_worker *w; unsigned n, base; } k[4] = {
            { "spin", spin, SPIN, 0 }, { "sleep", sleep, SLEEP, SPIN },
            { "pingpong", pp, 2 * PAIRS, SPIN + SLEEP }, { "mutex", mx, MUTEX, SPIN + SLEEP + 2 * PAIRS },
        };
        uint64_t mins[4];
        for (unsigned a = 0; a < 4; a++) {
            mins[a] = ~0ull;
            for (unsigned b = 0; b < k[a].n; b++) {
                struct stress_worker *w = &k[a].w[b];
                if (w->rounds < mins[a])
                    mins[a] = w->rounds;
                if (w->rounds == 0)
                    kinfo("MSPROBE zero: %s %u entered %lld us first-round %lld us; at the window's close cpu %d state %d, "
                          "that cpu's load %u; migrated %u times, %u before its first round",
                          k[a].kind, b, w->entered_ns ? (long long)((w->entered_ns - msp_t0) / 1000) : -1ll,
                          w->first_ns ? (long long)((w->first_ns - msp_t0) / 1000) : -1ll,
                          msp_cpu[k[a].base + b], msp_state[k[a].base + b],
                          msp_cpu[k[a].base + b] >= 0 ? msp_load[msp_cpu[k[a].base + b]] : 0u,
                          sh.msp_moved[k[a].base + b], sh.msp_moved_before_run[k[a].base + b]);
            }
        }
        kinfo("MSPROBE rep: sleep took %llu us; min rounds spin %llu sleep %llu pingpong %llu mutex %llu; migrations %llu; "
              "max cpu load %u",
              (unsigned long long)(msp_woke / 1000), (unsigned long long)mins[0], (unsigned long long)mins[1],
              (unsigned long long)mins[2], (unsigned long long)mins[3], (unsigned long long)moved, msp_maxload);
    }
    bool progress = true;"""),
]


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


def files(reps):
    reg = '    { "sched-migrate-stress", selftest_sched_migrate_stress },\n'
    return [(SMP, EDITS), (REG, [(reg, reg * reps)])]


def apply():
    reps = int(sys.argv[2]) if len(sys.argv) > 2 and sys.argv[2].isdigit() else 40
    apply_files(files(reps))
    print(f'applied: sched-migrate-stress instrumented, registered {reps} times')


if __name__ == '__main__':
    {'apply': apply, 'revert': revert}.get(sys.argv[1] if len(sys.argv) > 1 else '',
                                           lambda: sys.exit(__doc__))()
