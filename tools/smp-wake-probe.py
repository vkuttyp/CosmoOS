#!/usr/bin/env python3
"""
smp-wake-probe.py -- why did smp-wake see no reschedule IPI on the target?

One sighting (docs/testing/flakes.md, "smp-wake: no reschedule IPI counted
on the target"), CI, x86-64, 2026-09-23:

    CHECK(cw.ipis_after > cw.ipis_before)

The test pins a waiter to another CPU, waits until it reads BLOCKED, posts
the semaphore, and asserts the target handled an IPI_RESCHEDULE between
the waiter's block and its return. But sched_wake sends that IPI only
when the target is running its idle thread or something of lower
priority (request_resched). Two ways it is not owed:

- **The waiter itself is still current.** waitqueue_prepare sets BLOCKED
  under the wait-queue lock and returns; the waiter switches out later,
  in sched_block_current. A post in between finds rq->current == the
  waiter, equal priority: no IPI, and schedule() picks the READY waiter
  straight back ("woken between blocking and reaching here").
- **Another thread of equal priority is running on the target**
  (flakes.md's candidate).

The probe records, at the waiter's wake and under the target's run-queue
lock, what the target was running and whether request_resched was called
(SWPROBE lines).

    python3 tools/smp-wake-probe.py apply [--window MS] [--busy] [--fix]
    gmake ARCH=x86_64 test
    grep SWPROBE out/x86_64-debug/boot-test.log
    python3 tools/smp-wake-probe.py revert

--window MS holds the waiter MS ms between setting BLOCKED and switching
out (a spin in waitqueue_prepare, for the thread named cross-waiter only),
so the test's post lands in that window. --busy runs a spinner at the
waiter's priority on the target through the post. --fix is the candidate
check: after BLOCKED, the test also waits until the target reads idle
(sched_cpu_load == 0, which cannot be read while the waiter is still
current), then asserts that the wake requested a reschedule and that the
target counted an IPI.

`apply` and `revert` are those of tools/nvme-admin-probe.py: stamp first,
every file replaced atomically, the stamp removed last, finished by
running revert (again).
"""

import hashlib
import os
import subprocess
import sys

SCHED = 'kernel/scheduler/sched.c'
WAIT = 'kernel/scheduler/wait.c'
SMPT = 'kernel/scheduler/smptest.c'
BACKUP = '.smp-wake-probe.orig'
STAMP = '.smp-wake-probe.applied'

SCHED_OLD = """bool sched_wake(struct thread *t)
{"""
SCHED_NEW = """/* SWPROBE: the cross-waiter's last wake, under the target's lock. */
unsigned g_swp_seen, g_swp_requested, g_swp_cpu, g_swp_cur_is_idle, g_swp_cur_is_self;
int g_swp_cur_prio;
char g_swp_cur[16];

bool sched_wake(struct thread *t)
{"""
SCHED_OLD2 = """        if (rq->current == rq->idle || t->priority < rq->current->priority)
            request_resched(rq);
        woke = true;"""
SCHED_NEW2 = """        bool swp_req = rq->current == rq->idle || t->priority < rq->current->priority;
        if (strncmp(t->name, "cross-waiter", THREAD_NAME_MAX) == 0) {   /* SWPROBE */
            g_swp_seen = 1;
            g_swp_requested = swp_req;
            g_swp_cpu = rq->cpu;
            g_swp_cur_is_idle = rq->current == rq->idle;
            g_swp_cur_is_self = rq->current == t;
            g_swp_cur_prio = rq->current ? rq->current->priority : -1;
            unsigned i = 0;
            const char *n = rq->current ? rq->current->name : "-";
            for (; i < 15 && n[i]; i++)
                g_swp_cur[i] = n[i];
            g_swp_cur[i] = '\\0';
        }
        if (swp_req)
            request_resched(rq);
        woke = true;"""

WAIT_INC_OLD = """#include <kernel/timer.h>
"""
WAIT_INC_NEW = """#include <kernel/timer.h>
#include <kernel/string.h>   /* SWPROBE */
#include <arch/cpu.h>
"""

WAIT_OLD = """    cur->state = THREAD_BLOCKED;
    spin_unlock_irqrestore(&wq->lock, s);
}"""
def wait_new(ms):
    return f"""    cur->state = THREAD_BLOCKED;
    spin_unlock_irqrestore(&wq->lock, s);
    if (strncmp(cur->name, "cross-waiter", THREAD_NAME_MAX) == 0) {{   /* SWPROBE --window */
        uint64_t until = clock_now_ns() + {ms}ull * 1000000ull;
        while (clock_now_ns() < until)
            arch_cpu_relax();
    }}
}}"""

SMPT_OLD_DECL = """static bool selftest_smp_wake_pinned(const char **reason)
{"""
SMPT_NEW_DECL = """extern unsigned g_swp_seen, g_swp_requested, g_swp_cpu, g_swp_cur_is_idle, g_swp_cur_is_self;   /* SWPROBE */
extern int g_swp_cur_prio;
extern char g_swp_cur[16];

struct swp_spinner {
    unsigned stop;
};

__attribute__((unused)) static void swp_spin(void *arg)
{
    struct swp_spinner *x = arg;
    while (!__atomic_load_n(&x->stop, __ATOMIC_ACQUIRE))
        arch_cpu_relax();
    thread_exit(0);
}

static bool selftest_smp_wake_pinned(const char **reason)
{"""

SMPT_OLD_POST = """    uint64_t sent = clock_now_ns();
    semaphore_up(&cw.sem); /* from CPU 0: wake + IPI to CPU 1 */
    thread_join(t);
"""
def smpt_new_post(busy, fix=False):
    s = """    g_swp_seen = 0;   /* SWPROBE */
    __attribute__((unused)) static struct swp_spinner sps;
    struct thread *spt = NULL;
"""
    if busy:
        s += """    __atomic_store_n(&sps.stop, 0u, __ATOMIC_RELAXED);
    spt = thread_create_on(swp_spin, &sps, "swp-busy", SCHED_PRIO_DEFAULT, CPUMASK_OF(target));
    CHECK(spt != NULL);
    thread_sleep_ms(20);   /* SWPROBE --busy: the spinner is running on the target */
"""
    if fix:
        s += """    /* SWPROBE --fix: BLOCKED is set before the waiter switches out; wait
     * until the target reads idle. A load of 0 cannot be read while the
     * waiter is still its current thread. */
    while (sched_cpu_load(target) != 0) {
        CHECK(clock_now_ns() < deadline + MS(1000));
        thread_sleep_ms(1);
    }
"""
    s += """    uint64_t sent = clock_now_ns();
    semaphore_up(&cw.sem); /* from CPU 0: wake + IPI to CPU 1 */
"""
    if busy:
        s += """    thread_sleep_ms(10);
    __atomic_store_n(&sps.stop, 1u, __ATOMIC_RELEASE);
    thread_join(spt);
"""
    s += """    thread_join(t);
    kinfo("SWPROBE: wake seen %u on cpu %u: target running %s (prio %d; idle %u, the waiter itself %u), "
          "reschedule requested %u; IPIs on the target %llu -> %llu",
          g_swp_seen, g_swp_cpu, g_swp_cur, g_swp_cur_prio, g_swp_cur_is_idle, g_swp_cur_is_self,
          g_swp_requested, (unsigned long long)cw.ipis_before, (unsigned long long)cw.ipis_after);
    (void)spt;
"""
    return s

SMPT_OLD_CHECK = """    CHECK(cw.ipis_after > cw.ipis_before);"""
SMPT_NEW_FIX = """    CHECK(g_swp_requested && cw.ipis_after > cw.ipis_before);   /* SWPROBE --fix: the wake of an idle target requested it, and it arrived */"""


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
    fix = '--fix' in args
    busy = '--busy' in args
    args = [a for a in args if a not in ('--fix', '--busy')]
    window = 0
    if args[:1] == ['--window']:
        if len(args) != 2 or not args[1].isdigit():
            sys.exit('usage: apply [--window MS] [--busy] [--fix]')
        window = int(args[1])
    elif args:
        sys.exit('usage: apply [--window MS] [--busy] [--fix]')
    fl = [(SCHED, [(SCHED_OLD, SCHED_NEW), (SCHED_OLD2, SCHED_NEW2)]),
          (SMPT, [(SMPT_OLD_DECL, SMPT_NEW_DECL), (SMPT_OLD_POST, smpt_new_post(busy, fix))]
                 + ([(SMPT_OLD_CHECK, SMPT_NEW_FIX)] if fix else []))]
    if window:
        fl.append((WAIT, [(WAIT_INC_OLD, WAIT_INC_NEW), (WAIT_OLD, wait_new(window))]))
    apply_files(fl)
    print('applied: smp-wake instrumented'
          + (f'; the waiter held {window} ms between BLOCKED and switching out' if window else '')
          + ('; a spinner at its priority on the target' if busy else '')
          + ('; the IPI asserted only when requested' if fix else ''))


if __name__ == '__main__':
    {'apply': apply, 'revert': revert}.get(sys.argv[1] if len(sys.argv) > 1 else '',
                                           lambda: sys.exit(__doc__))()
