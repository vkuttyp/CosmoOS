#!/usr/bin/env python3
"""priority-inheritance-probe.py -- mutex.c has no priority inheritance.

A sleeping mutex (kernel/scheduler/mutex.c) records its owner and parks
waiters on a wait queue, but never raises the owner's priority to a blocked
waiter's. So the classic priority inversion is reachable: a low-priority
thread holding a mutex is preempted by a medium-priority thread, and a
high-priority thread blocked on that mutex waits for the medium one to finish
-- unbounded, priority defeated. See docs/audit/next-subsystem-priority-inheritance.md.

This probe adds a self-test, `prio-inversion`, that stages the inversion
deterministically on one CPU (the low thread parks holding the mutex, the high
thread blocks on it, then the medium thread and the low thread are made
runnable together and the scheduler's own priority order decides who runs) and
records, at the instant the high thread acquires the mutex, whether the medium
thread had already run to completion. Without inheritance it always has.

    python3 tools/priority-inheritance-probe.py apply
    gmake ARCH=x86_64 test > run.txt 2>&1
    grep PRIOINV out/x86_64-debug/boot-test.log
    python3 tools/priority-inheritance-probe.py revert

`apply` and `revert` are those of tools/net-rx-dup-probe.py.
"""

import hashlib
import os
import stat
import subprocess
import sys

SCHEDTEST = 'kernel/scheduler/schedtest.c'
DECL = 'kernel/include/kernel/selftest.h'
REG = 'kernel/core/selftest.c'
BACKUP = '.priority-inheritance-probe.orig'
STAMP = '.priority-inheritance-probe.applied'

ANCHOR = "/* --- the other shape: a direct sched_wake, no wait-queue wake ---"

TEST = r"""/* --- priority inversion: no priority inheritance in mutex.c -----------------
 *
 * The classic three-thread inversion, staged on one CPU. A low-priority
 * thread L holds a mutex; a high-priority thread H blocks on it; a
 * medium-priority thread Mid is runnable. mutex.c never raises L to H's
 * priority, so Mid (higher than L) runs to completion before L can release
 * the mutex, and H -- the highest of the three -- waits for the medium
 * thread: priority inversion.
 *
 * Made deterministic rather than raced: L parks holding the mutex, H blocks
 * on it, Mid parks; then Mid and L are released together and the scheduler's
 * priority order alone decides who runs. At the instant H acquires the mutex
 * it records whether Mid had already finished. Without inheritance it always
 * has. Every wait is bounded, so a regression is a failed check, not a hang.
 */
struct pi_probe {
    struct mutex m;
    struct semaphore l_run, mid_run;
    volatile int held;        /* L holds the mutex */
    volatile int mid_done;    /* Mid ran to completion */
    volatile int h_got;       /* H acquired the mutex */
    volatile int h_saw_mid;   /* mid_done, read at H's acquisition */
};

static void pi_low(void *arg)
{
    struct pi_probe *p = arg;
    mutex_lock(&p->m);
    __atomic_store_n(&p->held, 1, __ATOMIC_RELEASE);
    semaphore_down(&p->l_run);   /* parked holding the mutex until released */
    mutex_unlock(&p->m);         /* the critical section ends here */
}

static void pi_mid(void *arg)
{
    struct pi_probe *p = arg;
    semaphore_down(&p->mid_run);                 /* released together with L */
    for (volatile unsigned i = 0; i < 2000000u; i++)
        arch_cpu_relax();                        /* a bounded run that occupies the CPU */
    __atomic_store_n(&p->mid_done, 1, __ATOMIC_RELEASE);
}

static void pi_high(void *arg)
{
    struct pi_probe *p = arg;
    mutex_lock(&p->m);                           /* blocks: L holds it */
    __atomic_store_n(&p->h_saw_mid, __atomic_load_n(&p->mid_done, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE);
    __atomic_store_n(&p->h_got, 1, __ATOMIC_RELEASE);
    mutex_unlock(&p->m);
}

/* One absolute deadline for the whole test, so a failure path cannot sum
 * several waits past the per-test watchdog budget (8 s): the first wait that
 * cannot complete burns the rest of the budget and the test then fails. */
static bool pi_wait_state(struct thread *t, enum thread_state st, uint64_t deadline)
{
    while (__atomic_load_n(&t->state, __ATOMIC_ACQUIRE) != st) {
        if (clock_now_ns() >= deadline)
            return false;
        thread_sleep_ms(1);
    }
    return true;
}

static bool pi_wait_flag(volatile int *flag, uint64_t deadline)
{
    while (!__atomic_load_n(flag, __ATOMIC_ACQUIRE)) {
        if (clock_now_ns() >= deadline)
            return false;
        thread_sleep_ms(1);
    }
    return true;
}

static bool selftest_prio_inversion_pinned(const char **reason)
{
    unsigned before = thread_count();
    unsigned here = arch_cpu_id();
    int base = thread_current()->priority;
    CHECK(base + 12 < SCHED_PRIO_COUNT);   /* room for H/Mid/L strictly below this thread */
    int ph = base + 4, pm = base + 8, pl = base + 12;   /* H > Mid > L, all below the test thread */

    static struct pi_probe p;
    memset(&p, 0, sizeof(p));
    mutex_init(&p.m, "pi-probe");
    semaphore_init(&p.l_run, 0, "pi-l");
    semaphore_init(&p.mid_run, 0, "pi-mid");
    uint64_t dl = clock_now_ns() + MS(6000);   /* whole-test bound, under the 8 s budget */

    /* No CHECK after a thread is created: a mid-setup failure must still
     * release the parked workers and join them, or L is left holding the
     * mutex and the runner moves on with orphaned threads. Record the
     * reason, fall to `out`, unwind there. */
    struct thread *L = NULL, *H = NULL, *Mid = NULL;
    const char *why = NULL;

    L = thread_create_on(pi_low, &p, "pi-low", pl, CPUMASK_OF(here));
    if (L == NULL) { why = "pi-low: thread_create_on"; goto out; }
    if (!pi_wait_flag(&p.held, dl)) { why = "L did not take the mutex"; goto out; }
    if (!pi_wait_state(L, THREAD_BLOCKED, dl)) { why = "L did not park holding the mutex"; goto out; }

    H = thread_create_on(pi_high, &p, "pi-high", ph, CPUMASK_OF(here));
    if (H == NULL) { why = "pi-high: thread_create_on"; goto out; }
    if (!pi_wait_state(H, THREAD_BLOCKED, dl)) { why = "H did not block on the mutex"; goto out; }

    Mid = thread_create_on(pi_mid, &p, "pi-mid", pm, CPUMASK_OF(here));
    if (Mid == NULL) { why = "pi-mid: thread_create_on"; goto out; }
    if (!pi_wait_state(Mid, THREAD_BLOCKED, dl)) { why = "Mid did not park"; goto out; }

    /* Release Mid and L together: both runnable, priority alone decides. */
    semaphore_up(&p.mid_run);
    semaphore_up(&p.l_run);
    if (!pi_wait_flag(&p.h_got, dl)) { why = "H never acquired the mutex"; goto out; }
    if (p.h_saw_mid != 1)
        why = "no inversion: the high thread was not delayed by the medium one";

out:
    /* Release both parks (idempotent: an extra up on a consumed semaphore
     * just leaves a count nothing will down) so any worker still waiting
     * runs to exit, then join everything created. */
    semaphore_up(&p.l_run);
    semaphore_up(&p.mid_run);
    if (L != NULL) thread_join(L);
    if (H != NULL) thread_join(H);
    if (Mid != NULL) thread_join(Mid);
    if (why != NULL) {
        *reason = why;
        return false;
    }
    CHECK(threads_settle(before));
    kprintf("PRIOINV: a medium-priority thread ran to completion before a high-priority thread blocked on a "
            "mutex held by a low-priority one could acquire it; mutex.c has no priority inheritance\n");
    return true;
}

bool selftest_prio_inversion(const char **reason)
{
    cpumask_t saved = thread_pin_self();
    bool r = selftest_prio_inversion_pinned(reason);
    thread_set_affinity_self(saved);
    return r;
}

"""

OLD_DECL = "bool selftest_preempt_wake_locked(const char **reason);\n"
NEW_DECL = OLD_DECL + "bool selftest_prio_inversion(const char **reason);\n"

OLD_REG = '    { "preempt-wake-locked", selftest_preempt_wake_locked },\n'
NEW_REG = OLD_REG + '    { "prio-inversion",  selftest_prio_inversion },\n'


def sha(p):
    return hashlib.sha256(open(p, 'rb').read()).hexdigest()


def write_atomic(path, data):
    tmp = path + '.probe-tmp'
    with open(tmp, 'wb') as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())
    if os.path.exists(path):
        os.chmod(tmp, stat.S_IMODE(os.stat(path).st_mode))
    os.replace(tmp, path)


def git_clean(path):
    r = subprocess.run(['git', 'status', '--porcelain', '--', path], capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f'git status failed for {path}: {r.stderr.strip() or r.returncode}')
    return not r.stdout.strip()


def apply_files(fl):
    if os.path.exists(STAMP):
        sys.exit('already applied (or an apply was interrupted): run revert first')
    if os.path.exists(STAMP + '.probe-tmp'):
        os.remove(STAMP + '.probe-tmp')
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
            os.remove(STAMP + '.probe-tmp')
            sys.exit('not applied (removed a partial stamp an interrupted apply left)')
        sys.exit('not applied')
    entries = [line.split() for line in open(STAMP).read().split('\n') if line]
    for path, patched, orig in entries:
        cur = sha(path)
        if cur == orig:
            continue
        if cur != patched:
            sys.exit(f'{path} changed since apply. {path + BACKUP} is the pre-probe '
                     f'original, so copying it back would erase those changes. Keep your '
                     f'edits and remove the PRIOINV lines by hand, then delete {path + BACKUP} '
                     f'and {STAMP}.')
        if not os.path.exists(path + BACKUP) or sha(path + BACKUP) != orig:
            sys.exit(f'{path} is still patched and {path + BACKUP} is missing or not its original; restore by hand')
    for path, patched, orig in entries:
        if sha(path) == patched:
            write_atomic(path, open(path + BACKUP, 'rb').read())
    for path, _, _ in entries:
        for leftover in (path + BACKUP, path + '.probe-tmp', path + BACKUP + '.probe-tmp'):
            if os.path.exists(leftover):
                os.remove(leftover)
    if os.path.exists(STAMP + '.probe-tmp'):
        os.remove(STAMP + '.probe-tmp')
    os.remove(STAMP)
    print('reverted')


def apply():
    apply_files([
        (SCHEDTEST, [(ANCHOR, TEST + ANCHOR)]),
        (DECL, [(OLD_DECL, NEW_DECL)]),
        (REG, [(OLD_REG, NEW_REG)]),
    ])
    print('applied: prio-inversion shows the mutex has no priority inheritance')


if __name__ == '__main__':
    {'apply': apply, 'revert': revert}.get(sys.argv[1] if len(sys.argv) > 1 else '',
                                           lambda: sys.exit(__doc__))()
