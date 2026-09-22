#!/usr/bin/env python3
"""
sched-balance-probe.py -- measure what a balancer would have to fix.

Two measurements, both applied to a checkout as a temporary patch and
removed again. Neither ships: they are how the numbers in
docs/audit/next-subsystem-load-balancer.md were obtained, and how they
can be obtained again on another machine or after a change.

  apply    patch the tree (run queue probe + imbalance bench)
  revert   restore the snapshots `apply` took

`apply` refuses to run when any file it patches has uncommitted changes,
and `revert` restores only its own snapshots -- it never runs `git
checkout`, so it cannot discard work it did not create.

    python3 tools/sched-balance-probe.py apply
    gmake test              # x86-64; ARCH=aarch64 for the other
    grep -E 'BALPROBE|BALBENCH' out/x86_64-debug/boot-test.log
    python3 tools/sched-balance-probe.py revert

**The probe** counts, per CPU and per tick, the standing opportunity a
balancer would act on: ticks where this CPU was idle with an empty queue
while another CPU had a thread queued behind its running one
("pullable"), and ticks where this CPU had two or more queued while some
other CPU was idle and empty ("overloaded"). It reads other CPUs' queues
without their locks, which is what a balancer's scan does too: the
numbers are a hint, and the move that follows one re-selects under both
locks (`sched_migrate_from`).

**The bench** is the defect itself, made repeatable. It creates twice as
many threads as there are CPUs, one at a time, each blocking before the
next is created -- the shape `sched-spread` uses, which places them
round-robin across the CPUs. It then releases *every other one*, so the
threads that are actually runnable are the ones creation order happened
to put on every other CPU, and they spin counting iterations for 500 ms.
The control releases the same number of threads pinned one per CPU.

The gap between the two is the whole unit: same threads, same work, the
only difference being where they were placed before anyone knew which of
them would run.
"""

import os
import shutil
import subprocess
import sys

SCHED = 'kernel/scheduler/sched.c'
SELFTEST = 'kernel/core/selftest.c'
SCHED_H = 'kernel/include/kernel/sched.h'
SMPTEST = 'kernel/scheduler/smptest.c'
SELFTEST_H = 'kernel/include/kernel/selftest.h'
FILES = [SCHED, SELFTEST, SCHED_H, SMPTEST, SELFTEST_H]

PROBE = r'''
/* --- BALANCE PROBE (tools/sched-balance-probe.py; not for merge) --- */
static uint64_t g_bp_ticks[CONFIG_MAX_CPUS];
static uint64_t g_bp_pullable[CONFIG_MAX_CPUS];
static uint64_t g_bp_overloaded[CONFIG_MAX_CPUS];
static uint64_t g_bp_depth_sum[CONFIG_MAX_CPUS];
static unsigned g_bp_depth_max[CONFIG_MAX_CPUS];
static uint64_t g_bp_anyidle[CONFIG_MAX_CPUS];

static void balance_probe_tick(struct percpu *pc, struct runqueue *rq)
{
    unsigned self = pc->cpu_id;
    unsigned n = cpu_count();
    unsigned mine = rq->nr_running;
    bool i_am_idle = (rq->current == rq->idle || rq->current == NULL) && mine == 0;
    bool other_idle = false, other_has_work = false;
    for (unsigned c = 0; c < n; c++) {
        if (c == self || !cpu_online(c))
            continue;
        struct runqueue *o = &g_rqs[c];
        unsigned d = o->nr_running;
        /* An identity compare of two pointers, never a dereference: this
         * is another CPU's queue and its lock is not held. */
        if ((o->current == o->idle || o->current == NULL) && d == 0)
            other_idle = true;
        if (d >= 1)
            other_has_work = true;
    }
    g_bp_ticks[self]++;
    g_bp_depth_sum[self] += mine;
    if (mine > g_bp_depth_max[self])
        g_bp_depth_max[self] = mine;
    if (other_idle)
        g_bp_anyidle[self]++;
    if (i_am_idle && other_has_work)
        g_bp_pullable[self]++;
    if (mine >= 2 && other_idle)
        g_bp_overloaded[self]++;
}

void balance_probe_report(void);
void balance_probe_report(void)
{
    for (unsigned c = 0; c < cpu_count(); c++) {
        kprintf("BALPROBE cpu %u ticks %llu pullable %llu overloaded %llu depth_avg_x100 %llu depth_max %u other_idle %llu switches %llu\n",
                c, (unsigned long long)g_bp_ticks[c], (unsigned long long)g_bp_pullable[c],
                (unsigned long long)g_bp_overloaded[c],
                (unsigned long long)(g_bp_ticks[c] ? g_bp_depth_sum[c] * 100 / g_bp_ticks[c] : 0),
                g_bp_depth_max[c], (unsigned long long)g_bp_anyidle[c],
                (unsigned long long)g_rqs[c].switches);
    }
}
/* --- end balance probe --- */

'''

BENCH = r'''
/* --- IMBALANCE BENCH (tools/sched-balance-probe.py; not for merge) --- */
struct bwork {
    struct completion started, release, done;
    unsigned stop;          /* atomics only: written here, read on another CPU */
    unsigned runs;
    uint64_t iters;
    unsigned cpu;
    cpumask_t pin;
};

static void bwork_main(void *arg)
{
    struct bwork *w = arg;
    w->cpu = raw_cpu_id();
    complete(&w->started);
    wait_for_completion(&w->release);
    w->cpu = raw_cpu_id();
    uint64_t n = 0;
    while (__atomic_load_n(&w->stop, __ATOMIC_ACQUIRE) == 0) {
        for (volatile unsigned k = 0; k < 64; k++)
            ;
        n++;
    }
    w->iters = n;
    complete(&w->done);
}

/* Two per CPU is the widest round below, and the machine's CPU count is
 * what the workload is defined in terms of: a fixed cap smaller than
 * that would quietly measure a different benchmark on a bigger machine
 * (found in review of this report). */
#define NB (CONFIG_MAX_CPUS * 2u)

static bool bench_round2(const char **reason, const char *label, unsigned n_created,
                         unsigned stride, bool pin_round, unsigned ncpu)
{
    static struct bwork w[NB];
    static struct thread *t[NB];
    unsigned made = 0;
    unsigned n_workers = n_created;
    if (n_workers > NB) {
        *reason = "the bench wants more workers than it has room for";
        return false;
    }
    for (unsigned i = 0; i < n_workers; i++) {
        memset(&w[i], 0, sizeof(w[i]));
        completion_init(&w[i].started, "bw-start");
        completion_init(&w[i].release, "bw-rel");
        completion_init(&w[i].done, "bw-done");
        w[i].pin = pin_round ? CPUMASK_OF((i / stride) % ncpu) : CPUMASK_ALL;
        t[i] = thread_create_on(bwork_main, &w[i], "bwork", SCHED_PRIO_DEFAULT, w[i].pin);
        if (t[i] == NULL)
            break;
        made++;
        wait_for_completion(&w[i].started);
        /* And until it is observably off its run queue, as sched-spread
         * does: the completion says "running", not "blocked". A worker
         * still runnable when the next is created breaks the one shape
         * this bench depends on -- each thread placed against a machine
         * whose queues have drained -- so the wait expiring fails the
         * bench rather than measuring something else under its name. */
        for (unsigned k = 0; k < 2000 && __atomic_load_n(&t[i]->state, __ATOMIC_ACQUIRE) != THREAD_BLOCKED; k++)
            thread_sleep_ms(1);
        if (__atomic_load_n(&t[i]->state, __ATOMIC_ACQUIRE) != THREAD_BLOCKED) {
            kprintf("BALBENCH %s: worker %u never blocked; the round is void\n", label, i);
            for (unsigned j = 0; j <= i; j++)
                complete(&w[j].release);
            for (unsigned j = 0; j <= i; j++) {
                __atomic_store_n(&w[j].stop, 1u, __ATOMIC_RELEASE);
            }
            for (unsigned j = 0; j <= i; j++)
                thread_join(t[j]);
            *reason = "a bench worker never reached THREAD_BLOCKED";
            return false;
        }
    }
    unsigned runners = 0;
    for (unsigned i = 0; i < made; i += stride) {
        w[i].runs = 1;
        runners++;
        complete(&w[i].release);
    }
    thread_sleep_ms(500);
    for (unsigned i = 0; i < made; i++)
        __atomic_store_n(&w[i].stop, 1u, __ATOMIC_RELEASE);
    for (unsigned i = 0; i < made; i += stride)
        wait_for_completion(&w[i].done);
    unsigned hist[CONFIG_MAX_CPUS] = {0};
    uint64_t total = 0, lo = ~0ull, hi = 0;
    for (unsigned i = 0; i < made; i += stride) {
        if (w[i].cpu < CONFIG_MAX_CPUS)
            hist[w[i].cpu]++;
        total += w[i].iters;
        if (w[i].iters < lo) lo = w[i].iters;
        if (w[i].iters > hi) hi = w[i].iters;
    }
    for (unsigned i = 0; i < made; i++)
        if (!w[i].runs)
            complete(&w[i].release);
    kprintf("BALBENCH %s created %u runners %u placement", label, made, runners);
    for (unsigned c = 0; c < ncpu; c++)
        kprintf(" cpu%u=%u", c, hist[c]);
    kprintf(" total_iters %llu min %llu max %llu\n",
            (unsigned long long)total, (unsigned long long)lo, (unsigned long long)hi);
    for (unsigned i = 0; i < made; i++)
        thread_join(t[i]);
    return true;
}

bool selftest_bench_imbalance(const char **reason);
bool selftest_bench_imbalance(const char **reason)
{
    unsigned n = cpu_count();
    if (n < 2) {
        kinfo("selftest: bench-imbalance: one CPU; skipping");
        return true;
    }
    return bench_round2(reason, "as-placed-full", n, 1, false, n) &&
           bench_round2(reason, "pinned-full", n, 1, true, n) &&
           bench_round2(reason, "as-placed-alternate", n * 2, 2, false, n) &&
           bench_round2(reason, "pinned-alternate", n * 2, 2, true, n);
}
/* --- end imbalance bench --- */

'''


BACKUP = '.sched-balance-probe.orig'


def edited(text, edits):
    """Every anchor checked against `text` before any of them is applied."""
    for old, new in edits:
        if text.count(old) != 1:
            return None, "anchor appears %d times, expected once: %r" % (text.count(old), old[:60])
        text = text.replace(old, new, 1)
    return text, None


def dirty_files():
    out = subprocess.run(['git', 'status', '--porcelain', '--'] + FILES,
                         capture_output=True, text=True, check=True).stdout
    return [line[3:] for line in out.splitlines() if line.strip()]


def apply():
    tick = "void sched_tick(uint64_t now_ns, struct arch_trap_frame *frame)\n{"
    call = "    spin_lock(&rq->lock);\n    struct thread *cur = rq->current;"
    anchor = "bool selftest_sched_spread(const char **reason)"
    decl = "bool selftest_sched_spread(const char **reason);"
    plan = {
        SCHED: [(tick, PROBE + tick), (call, "    balance_probe_tick(pc, rq);\n" + call)],
        SELFTEST: [("    return failed;\n}", "    balance_probe_report();\n\n    return failed;\n}"),
                   ('{ "sched-spread",    selftest_sched_spread },',
                    '{ "sched-spread",    selftest_sched_spread },\n'
                    '    { "bench-imbalance", selftest_bench_imbalance },')],
        SCHED_H: [("void sched_dump(void);", "void balance_probe_report(void);\nvoid sched_dump(void);")],
        SMPTEST: [(anchor, BENCH + anchor)],
        SELFTEST_H: [(decl, "bool selftest_bench_imbalance(const char **reason);\n" + decl)],
    }

    # A probe that cleans up after itself must not be able to throw away
    # work it did not create: `revert` restores the snapshots this takes,
    # never the index, so it can only ever undo what `apply` did. A file
    # already modified is refused outright -- the snapshot would carry the
    # modification and the measurement would not be of this tree.
    for path in FILES:
        if os.path.exists(path + BACKUP):
            sys.exit("%s%s exists: a previous run was not reverted. Revert first." % (path, BACKUP))
    dirty = dirty_files()
    if dirty:
        sys.exit("uncommitted changes in files this probe patches: %s\n"
                 "Commit or stash them first; cleanup restores files wholesale." % ", ".join(dirty))

    # Every anchor in every file resolved before a single byte is written,
    # so a moved anchor leaves the tree untouched rather than half
    # instrumented (found in review of this report).
    staged = {}
    for path, edits in plan.items():
        text, err = edited(open(path).read(), edits)
        if err:
            sys.exit("%s: %s" % (path, err))
        staged[path] = text

    written = []
    try:
        for path, text in staged.items():
            shutil.copyfile(path, path + BACKUP)
            written.append(path)
            open(path, 'w').write(text)
    except Exception as exc:                       # a failed write leaves nothing behind
        for path in written:
            if os.path.exists(path + BACKUP):
                shutil.move(path + BACKUP, path)
        sys.exit("apply failed, tree restored: %s" % exc)
    print("applied: build and boot, then grep BALPROBE/BALBENCH in the boot log")


def revert():
    missing = [p for p in FILES if not os.path.exists(p + BACKUP)]
    if len(missing) == len(FILES):
        sys.exit("no snapshots found: nothing to revert")
    for path in FILES:
        if os.path.exists(path + BACKUP):
            shutil.move(path + BACKUP, path)
    print("reverted" + (" (%d file(s) had no snapshot)" % len(missing) if missing else ""))


if __name__ == '__main__':
    if len(sys.argv) != 2 or sys.argv[1] not in ('apply', 'revert'):
        sys.exit(__doc__)
    (apply if sys.argv[1] == 'apply' else revert)()
