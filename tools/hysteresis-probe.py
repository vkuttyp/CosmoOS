#!/usr/bin/env python3
"""
hysteresis-probe.py -- why does sched-balance-hysteresis see a worker move
"for a difference of one"?

Two sightings (docs/testing/flakes.md, "sched-balance-hysteresis: moved a
thread for a difference of one"), one per architecture, both in boots that
change no scheduler code. The test holds two yielding workers on CPU A and
one on CPU B, each allowed only {A, B}, and fails if a worker changes CPU
while its sampler -- which reads both CPUs' loads every ~8 ms (a 5 ms sleep
on a 4 ms tick) -- never saw the gap reach two.

But the balancer re-decides every pull under both runqueue locks
(sched_migrate_from, min_gap 2): it can only pull on an exact difference of
two. So a move means a real 3-vs-1 (or 2-vs-0) existed at the pull, and the
sampler missed it: a third thread runnable on A for less than the sampling
interval, landing on B's scan (every SCHED_BALANCE_TICKS = 16 ticks = 64 ms
for a busy CPU). With a third thread running on A, one of A's workers sits
in the queue having *yielded*, not been preempted, so pick_migratable may
take it.

The probe records every pull of a "hyst" worker at the moment of decision,
under both locks: the two loads, what the source CPU was running, and what
its queue held (HYPROBE lines). It logs a summary of each window.

    python3 tools/hysteresis-probe.py apply [--force] [--fix] [--threshold1]
    gmake ARCH=x86_64 test
    grep HYPROBE out/x86_64-debug/boot-test.log
    python3 tools/hysteresis-probe.py revert

--force adds an intruder pinned to A that runs ~3 ms right after each of
the sampler's samples -- the next sample waits for the burst to end -- so it
is runnable between samples and never at one:
a transient the sampler cannot see, built from the mechanism rather than
waited for. --fix is the candidate: the window is judged by the balancer's
own locked loads at every pull of a worker, not by the sampler. Any pull
below a locked difference of two fails, whether or not the sampler saw a
move, and so does a move with no pull recorded; otherwise every move was
the rule obeyed.

--threshold1 is the defect the test exists to catch: the balancer pulls on
a difference of one (the scan's `mine + 2` and the locked re-check's
min_gap both lowered to one).

`apply` and `revert` are those of tools/nvme-admin-probe.py: stamp first,
every file replaced atomically, the stamp removed last, finished by
running revert (again).
"""

import hashlib
import os
import subprocess
import sys

SCHED = 'kernel/scheduler/sched.c'
SMPT = 'kernel/scheduler/smptest.c'
BACKUP = '.hysteresis-probe.orig'
STAMP = '.hysteresis-probe.applied'

REC_STRUCT = """struct hyprobe_rec {   /* HYPROBE */
    unsigned from, to, lf, lt, min_gap, nq;
    int cur_prio;
    char cur[16];
    char q[6][16];
    int qp[6];
    unsigned qflags[6];
};
"""

SCHED_OLD = """enum sched_migrate_result sched_migrate_from(unsigned from, unsigned to, unsigned min_gap, struct thread **moved)
{"""
SCHED_NEW = REC_STRUCT + """struct hyprobe_rec g_hyprobe[8];
unsigned g_hyprobe_n, g_hyprobe_below;

static void hyprobe_name(char *dst, const char *src)
{
    unsigned i = 0;
    for (; i < 15 && src[i]; i++)
        dst[i] = src[i];
    dst[i] = '\\0';
}

static void hyprobe_fill(struct hyprobe_rec *r, unsigned from, unsigned to, unsigned min_gap);
void hyprobe_peek(unsigned cpu, struct hyprobe_rec *r);

/* Both locks held: the loads are exact, the queue is stable. */
static void hyprobe_record(struct thread *t, unsigned from, unsigned to, unsigned min_gap)
{
    if (strncmp(t->name, "hyst", THREAD_NAME_MAX) != 0)
        return;
    if (load_locked(&g_rqs[from]) < load_locked(&g_rqs[to]) + 2)   /* every pull, not just the first 8 */
        __atomic_fetch_add(&g_hyprobe_below, 1u, __ATOMIC_RELAXED);
    unsigned i = __atomic_fetch_add(&g_hyprobe_n, 1u, __ATOMIC_RELAXED);
    if (i >= 8)
        return;
    hyprobe_fill(&g_hyprobe[i], from, to, min_gap);
}

/* What `cpu` holds, under its lock (the test's first broken sample). */
void hyprobe_peek(unsigned cpu, struct hyprobe_rec *r)
{
    arch_irq_state_t s = arch_irq_save();
    spin_lock(&g_rqs[cpu].lock);
    hyprobe_fill(r, cpu, cpu, 0);
    spin_unlock(&g_rqs[cpu].lock);
    arch_irq_restore(s);
}

static void hyprobe_fill(struct hyprobe_rec *r, unsigned from, unsigned to, unsigned min_gap)
{
    struct runqueue *rq = &g_rqs[from];
    r->from = from;
    r->to = to;
    r->lf = load_locked(rq);
    r->lt = load_locked(&g_rqs[to]);
    r->min_gap = min_gap;
    hyprobe_name(r->cur, rq->current ? rq->current->name : "-");
    r->cur_prio = rq->current ? rq->current->priority : -1;
    r->nq = 0;
    for (unsigned p = 0; p < SCHED_PRIO_COUNT && r->nq < 6; p++) {
        struct thread *x;
        list_for_each_entry(x, &rq->ready[p], rq_link) {
            if (r->nq >= 6)
                break;
            hyprobe_name(r->q[r->nq], x->name);
            r->qp[r->nq] = x->priority;
            r->qflags[r->nq] = x->flags;
            r->nq++;
        }
    }
}

enum sched_migrate_result sched_migrate_from(unsigned from, unsigned to, unsigned min_gap, struct thread **moved)
{"""

SCHED_OLD2 = """    struct thread *t = g_policy->pick_migratable(&g_rqs[from], CPUMASK_OF(to));
    if (t != NULL) {
        migrate_locked(t, from, to);
        *moved = t;"""
SCHED_NEW2 = """    struct thread *t = g_policy->pick_migratable(&g_rqs[from], CPUMASK_OF(to));
    if (t != NULL) {
        hyprobe_record(t, from, to, min_gap);   /* HYPROBE */
        migrate_locked(t, from, to);
        *moved = t;"""

# --threshold1: the defect the test exists to catch (a difference of one pulls).
SCHED_OLD_T1 = """        if (busiest == self || busiest_load < mine + 2) {"""
SCHED_NEW_T1 = """        if (busiest == self || busiest_load < mine + 1) {   /* HYPROBE --threshold1 */"""
SCHED_OLD_T1B = """        enum sched_migrate_result r = sched_migrate_from(busiest, self, 2, &moved);"""
SCHED_NEW_T1B = """        enum sched_migrate_result r = sched_migrate_from(busiest, self, 1, &moved);   /* HYPROBE --threshold1 */"""

SMPT_OLD_DECL = """static bool sched_balance_hysteresis_pinned(const char **reason)
{"""
SMPT_NEW_DECL = REC_STRUCT + """extern struct hyprobe_rec g_hyprobe[8];
extern unsigned g_hyprobe_n, g_hyprobe_below;
void hyprobe_peek(unsigned cpu, struct hyprobe_rec *r);
static struct hyprobe_rec g_hypeek;

/* HYPROBE --force: runnable on A between the sampler's samples, never at one. */
struct hyprobe_intr {
    struct semaphore go;        /* counting: one up per burst (a completion latches) */
    unsigned busy, stop, bursts;
};

__attribute__((unused)) static void hyprobe_intruder(void *arg)
{
    struct hyprobe_intr *x = arg;
    for (;;) {
        semaphore_down(&x->go);
        if (__atomic_load_n(&x->stop, __ATOMIC_ACQUIRE))
            break;
        uint64_t until = clock_now_ns() + 3000000ull;
        while (clock_now_ns() < until && !__atomic_load_n(&x->stop, __ATOMIC_ACQUIRE))
            arch_cpu_relax();
        x->bursts++;
        __atomic_store_n(&x->busy, 0u, __ATOMIC_RELEASE);
    }
    thread_exit(0);
}

static bool sched_balance_hysteresis_pinned(const char **reason)
{"""

SMPT_OLD_START = """    struct sched_balance_stats s0, s1;
    sched_balance_stats(&s0);
    unsigned moved = 0, load_a = 0, load_b = 0;"""
def smpt_new_start(force):
    s = """    static struct hyprobe_intr hyx;   /* HYPROBE */
    struct thread *hyt = NULL;
    unsigned hy_bla = 0, hy_blb = 0;
    uint64_t hy_t0 = 0, hy_bms = 0;
"""
    if force:
        s += """    memset(&hyx, 0, sizeof(hyx));
    semaphore_init(&hyx.go, 0, "hyprobe-go");
    if (ok) {
        hyt = thread_create_on(hyprobe_intruder, &hyx, "hyintr", SCHED_PRIO_DEFAULT, CPUMASK_OF(a_cpu));
        if (hyt == NULL) {   /* a forced window without its intruder would measure nothing */
            kerror("HYPROBE: --force: the intruder could not be created");
            ok = false;
        }
    }
"""
    return s + SMPT_OLD_START

SMPT_OLD_WIN = """        uint64_t deadline = clock_deadline_ns(500ull * 1000000ull);
        while (!clock_deadline_passed(deadline)) {"""
SMPT_NEW_WIN = """        __atomic_store_n(&g_hyprobe_n, 0u, __ATOMIC_RELAXED);   /* HYPROBE: this window's pulls */
        __atomic_store_n(&g_hyprobe_below, 0u, __ATOMIC_RELAXED);
        hy_t0 = clock_now_ns();
        uint64_t deadline = clock_deadline_ns(500ull * 1000000ull);
        while (!clock_deadline_passed(deadline)) {"""

SMPT_OLD_LOOPEND = """            thread_sleep_ms(5);
        }
    }
    sched_balance_stats(&s1);
"""
def smpt_new_loopend(fix):
    s = """            if (hyt != NULL && !__atomic_load_n(&hyx.busy, __ATOMIC_ACQUIRE)) {   /* HYPROBE --force */
                __atomic_store_n(&hyx.busy, 1u, __ATOMIC_RELEASE);
                semaphore_up(&hyx.go);
                /* Invisible at every sample by construction: the next
                 * sample waits for the burst to end. */
                while (__atomic_load_n(&hyx.busy, __ATOMIC_ACQUIRE) && !clock_deadline_passed(deadline))
                    thread_sleep_ms(1);
            }
            thread_sleep_ms(5);
        }
    }
    sched_balance_stats(&s1);
    if (hyt != NULL) {
        __atomic_store_n(&hyx.stop, 1u, __ATOMIC_RELEASE);
        semaphore_up(&hyx.go);
        thread_join(hyt);
    }
    unsigned hy_n = __atomic_load_n(&g_hyprobe_n, __ATOMIC_RELAXED);
    unsigned hy_below = __atomic_load_n(&g_hyprobe_below, __ATOMIC_RELAXED);
    for (unsigned i = 0; i < hy_n && i < 8; i++) {
        struct hyprobe_rec *r = &g_hyprobe[i];
        char qs[160];
        unsigned o = 0;
        for (unsigned j = 0; j < r->nq && o < sizeof(qs) - 40; j++)
            o += (unsigned)ksnprintf(qs + o, sizeof(qs) - o, "%s%s/%d%s", j ? "," : "", r->q[j], r->qp[j],
                                    (r->qflags[j] & THREAD_FLAG_PREEMPTED) ? "P" : "");
        qs[o < sizeof(qs) ? o : sizeof(qs) - 1] = '\\0';
        kinfo("HYPROBE: pull %u: cpu %u -> %u, locked loads %u vs %u (min_gap %u); running %s/%d; queued [%s]",
              i, r->from, r->to, r->lf, r->lt, r->min_gap, r->cur, r->cur_prio, qs);
    }
    if (premise_broken) {
        struct hyprobe_rec *r = &g_hypeek;
        char qs[160];
        unsigned o = 0;
        for (unsigned j = 0; j < r->nq && o < sizeof(qs) - 40; j++)
            o += (unsigned)ksnprintf(qs + o, sizeof(qs) - o, "%s%s/%d%s", j ? "," : "", r->q[j], r->qp[j],
                                     (r->qflags[j] & THREAD_FLAG_PREEMPTED) ? "P" : "");
        qs[o < sizeof(qs) ? o : sizeof(qs) - 1] = '\\0';
        kinfo("HYPROBE: the first broken sample's cpu %u, a moment later under its lock: load %u; running %s/%d; queued [%s]",
              r->from, r->lf, r->cur, r->cur_prio, qs);
    }
    kinfo("HYPROBE: window: moved %u, premise_broken %d, pulls recorded %u (%u below a locked gap of two), "
          "intruder bursts %u, scans %llu, locked re-checks refused (GAP) %llu; first broken sample %u vs %u at %llu ms",
          moved, premise_broken ? 1 : 0, hy_n, hy_below, hyx.bursts,
          (unsigned long long)(s1.scans - s0.scans),
          (unsigned long long)(s1.refused[SCHED_MIGRATE_GAP] - s0.refused[SCHED_MIGRATE_GAP]),
          hy_bla, hy_blb, (unsigned long long)hy_bms);
"""
    if fix:
        s += """    /* HYPROBE --fix: judged by the balancer's locked loads at each pull
     * alone. A pull below two fails, whatever the sampler saw; with none,
     * every move was the rule obeyed. */
    if (hy_below != 0 || (moved != 0 && hy_n == 0)) {
        for (unsigned i = 0; i < W; i++)
            if (t[i] != NULL)
                __atomic_store_n(&w[i].stop, 1u, __ATOMIC_RELEASE);
        for (unsigned i = 0; i < W; i++)
            if (t[i] != NULL)
                thread_join(t[i]);
        *reason = hy_below != 0 ? "the balancer pulled a thread for a locked difference below two"
                                : "a worker moved with no balancer pull recorded";
        return false;
    }
    bool hy_legit = moved != 0;   /* every move had a recorded pull, all at two or more */
"""
    else:
        s += """    bool hy_legit = false;
"""
    return s

SMPT_OLD_BROKEN = """            if (la > lb + 1 || lb > la + 1)
                premise_broken = true;"""
SMPT_NEW_BROKEN = """            if (la > lb + 1 || lb > la + 1) {
                if (!premise_broken) {   /* HYPROBE: the first broken sample */
                    hy_bla = la;
                    hy_blb = lb;
                    hy_bms = (clock_now_ns() - hy_t0) / 1000000ull;
                    hyprobe_peek(la > lb ? a_cpu : b_cpu, &g_hypeek);
                }
                premise_broken = true;
            }"""

SMPT_OLD_DECIDE = """    if (moved != 0 && premise_broken) {"""
SMPT_NEW_DECIDE = """    if (moved != 0 && (premise_broken || hy_legit)) {"""


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
    t1 = '--threshold1' in args
    if [a for a in args if a not in ('--force', '--fix', '--threshold1')]:
        sys.exit('usage: apply [--force] [--fix] [--threshold1]')
    apply_files([
        (SCHED, [(SCHED_OLD, SCHED_NEW), (SCHED_OLD2, SCHED_NEW2)]
                + ([(SCHED_OLD_T1, SCHED_NEW_T1), (SCHED_OLD_T1B, SCHED_NEW_T1B)] if t1 else [])),
        (SMPT, [(SMPT_OLD_DECL, SMPT_NEW_DECL), (SMPT_OLD_START, smpt_new_start(force)),
                (SMPT_OLD_WIN, SMPT_NEW_WIN), (SMPT_OLD_BROKEN, SMPT_NEW_BROKEN),
                (SMPT_OLD_LOOPEND, smpt_new_loopend(fix)),
                (SMPT_OLD_DECIDE, SMPT_NEW_DECIDE)]),
    ])
    print('applied: sched-balance-hysteresis pulls recorded'
          + ('; an intruder on A between samples' if force else '')
          + ('; moves judged by the locked gap' if fix else '')
          + ('; the balancer pulls on a difference of one' if t1 else ''))


if __name__ == '__main__':
    {'apply': apply, 'revert': revert}.get(sys.argv[1] if len(sys.argv) > 1 else '',
                                           lambda: sys.exit(__doc__))()
