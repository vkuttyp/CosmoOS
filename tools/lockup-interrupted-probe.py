#!/usr/bin/env python3
"""
lockup-interrupted-probe.py -- why does lockup-sample's sampled PC sometimes
lie outside spin_here?

Three sightings (docs/testing/flakes.md, "`lockup-sample`, and a failure
that was not a flake at all"), all x86-64, all on commits that change no
kernel code:

    SELFTEST: lockup-sample ... FAIL: check failed: in_fn(pc, (const void *)spin_here, SPIN_FN_BOUND)

The test starts a spinner on CPU k, with interrupts enabled, and samples
every CPU. On x86-64 the sample is an NMI. An NMI can arrive while an
ordinary interrupt is being handled on the spinner's CPU, and then the
sampled PC is in that handler. The second sighting's trace showed exactly
that: an interrupt's tail, then isr.S, then spinner_main. The check has no
allowance for a spinner that is interrupted, which a spinner with
interrupts enabled will be.

    python3 tools/lockup-interrupted-probe.py apply [--force] [--fix]
    gmake ARCH=x86_64 test > run.txt 2>&1
    grep LIPROBE out/x86_64-debug/boot-test.log
    python3 tools/lockup-interrupted-probe.py revert

The instrumentation classifies the sampled PC and each trace entry:
P (the probe's parked handler), R (exactly spin_here's return address
into spinner_main, which spin_here records), S (spin_here), or ?
(anything else). A first version classified spinner_main by the test's
512-byte MAIN_FN_BOUND, and the probe's own handler, placed after it,
fell inside that range and read as spinner_main.

--force makes the sample land inside an interrupt on the spinner,
deterministically. A helper thread on a third CPU sends CPU k a cross call
(smp_call_function_single) whose handler parks, bounded at 50 ms, until
the test has sampled. The test waits for the handler to be parked, then
samples. On x86-64 the NMI interrupts the parked handler. On AArch64 there
is no NMI: the sample is an ordinary interrupt, which the parked handler
(interrupts masked) holds off, so the sample should go unanswered within
its 5 ms timeout. Each architecture's outcome is printed. If the forcing
cannot happen -- fewer than three online CPUs, the helper thread fails to
start, or the handler never parks -- the test fails on CHECK(lp_was_parked)
rather than silently sampling the plain case, so a forced run that could
not force never reads as a pass.

--fix is the candidate check. The sample names the spinner if either:
- the PC is in spin_here and trace[1] is exactly spin_here's return
  address (uninterrupted); or
- the PC is elsewhere and that exact return address appears deeper in the
  trace (the spinner, interrupted).

`apply` and `revert` are those of tools/cond-phase-probe.py.
"""

import hashlib
import os
import stat
import subprocess
import sys

LT = 'kernel/core/lockuptest.c'
BACKUP = '.lockup-interrupted-probe.orig'
STAMP = '.lockup-interrupted-probe.applied'

OLD_INC = """#include <kernel/timer.h>
"""
NEW_INC = """#include <kernel/timer.h>
#include <kernel/smp.h>   /* LIPROBE */
"""

OLD_SPIN = """    volatile bool masked;           /* interrupts off while spinning */"""
NEW_SPIN = """    volatile bool masked;           /* interrupts off while spinning */
    volatile uintptr_t ret;         /* LIPROBE: spin_here's return address, exactly */"""
OLD_HERE = """    s->running = true;
    while (!s->stop)"""
NEW_HERE = """    s->ret = (uintptr_t)__builtin_return_address(0);   /* LIPROBE */
    s->running = true;
    while (!s->stop)"""

OLD_FN = """static bool selftest_lockup_sample_pinned(const char **reason)
{"""
NEW_FN = r"""/* LIPROBE: a cross call parked on the spinner's CPU until the test has sampled. */
static volatile int lp_parked, lp_release;
static __noinline void lp_park(void *arg)
{
    (void)arg;
    lp_parked = 1;
    /* No call in the loop, so the sample's PC is in here: bounded by a count. */
    for (unsigned long i = 0; !lp_release && i < 400000000ul; i++)
        ;
    lp_parked = 2;
}
static unsigned lp_target;
static __attribute__((unused)) void lp_caller_main(void *arg)
{
    (void)arg;
    smp_call_function_single(lp_target, lp_park, NULL);
}
/* P the parked handler, R exactly spin_here's return into spinner_main,
 * S spin_here, ? anything else. P first: it sits near spinner_main. */
static char lp_class(uintptr_t a, uintptr_t ret)
{
    if (in_fn(a, (const void *)lp_park, 256))
        return 'P';
    if (a == ret)
        return 'R';
    if (in_fn(a, (const void *)spin_here, SPIN_FN_BOUND))
        return 'S';
    return '?';
}

static bool selftest_lockup_sample_pinned(const char **reason)
{"""

OLD_START = """    CHECK(t != NULL);

    uint64_t t0 = clock_now_ns();
    cpumask_t m = 0;
    bool ok = lockup_sample_all(NULL, LOCKUP_SAMPLE_TIMEOUT_NS, &m);"""

def new_start(force):
    s = """    CHECK(t != NULL);
"""
    if force:
        s += r"""    struct thread *lp_th = NULL;   /* LIPROBE --force */
    {
        unsigned me = arch_cpu_id(), third = ~0u;
        for (unsigned c = 0; c < cpu_count() && third == ~0u; c++)
            if (c != me && c != (unsigned)k && cpu_online(c))
                third = c;
        lp_parked = 0;
        lp_release = 0;
        lp_target = (unsigned)k;
        if (third != ~0u)
            lp_th = thread_create_on(lp_caller_main, NULL, "lp-caller", SCHED_PRIO_DEFAULT, CPUMASK_OF(third));
        uint64_t d = clock_now_ns() + 1000ull * 1000 * 1000;
        while (lp_th && lp_parked != 1 && clock_now_ns() < d)
            thread_sleep_ms(1);
        kprintf("LIPROBE: forced: the cross call %s on cpu %d (caller on cpu %d)\n",
                lp_parked == 1 ? "parked" : "DID NOT PARK", k, (int)third);
    }
"""
    s += """
    uint64_t t0 = clock_now_ns();
    cpumask_t m = 0;
    bool ok = lockup_sample_all(NULL, LOCKUP_SAMPLE_TIMEOUT_NS, &m);"""
    if force:
        s += r"""
    bool lp_was_parked = lp_parked == 1;   /* LIPROBE --force: still inside the handler at the sample */
    lp_release = 1;
    if (lp_th)
        thread_join(lp_th);"""
    return s

OLD_OWN = """    CHECK(own);
"""
NEW_OWN = """    CHECK(own);
    CHECK(lp_was_parked);   /* LIPROBE --force: a run that could not force -- no third CPU, thread_create_on failed, or the handler never parked -- is a plain run in disguise, not a pass. Placed after stop_spinner so a failure does not leak the spinner. */
"""

OLD_READ = """    bool own = (m & CPUMASK_OF(arch_cpu_id())) != 0;
"""
def new_read(force):
    s = OLD_READ + r"""    uintptr_t lp_tr[LOCKUP_TRACE_MAX];   /* LIPROBE */
    memcpy(lp_tr, sm->trace, sizeof(lp_tr));
    {
        char cls[LOCKUP_TRACE_MAX + 1];
        unsigned n = depth < LOCKUP_TRACE_MAX ? depth : LOCKUP_TRACE_MAX;
        for (unsigned i = 0; i < n; i++)
            cls[i] = lp_class(lp_tr[i], s.ret);
        cls[n] = 0;
        kprintf("LIPROBE: cpu %d answered %d, nmi %d, pc %c, depth %u, trace %s\n", k,
                (int)((m >> k) & 1), (int)sm->nmi, lp_class(pc, s.ret), depth, cls);
    }
"""
    if force:
        s += r"""    kprintf("LIPROBE: forced: the handler was %s at the sample\n", lp_was_parked ? "still parked" : "NOT PARKED");
"""
    return s

OLD_CHECK = """    CHECK(in_fn(pc, (const void *)spin_here, SPIN_FN_BOUND));
    CHECK(depth >= 2);
    CHECK(in_fn(tr1, (const void *)spinner_main, MAIN_FN_BOUND));
    CHECK(when >= t0 && when <= t1);"""
NEW_CHECK_FIX = r"""    CHECK(depth >= 2);
    {   /* LIPROBE --fix: the spinner's own frame, exactly: in its loop, or interrupted */
        bool lp_ret_below = false;
        for (unsigned i = 1; i < depth && i < LOCKUP_TRACE_MAX; i++)
            lp_ret_below |= lp_tr[i] == s.ret;
        CHECK(in_fn(pc, (const void *)spin_here, SPIN_FN_BOUND) ? tr1 == s.ret : lp_ret_below);
    }
    CHECK(when >= t0 && when <= t1);"""


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
    if os.path.exists(path):   # keep the file's mode: the umask must not change it
        os.chmod(tmp, stat.S_IMODE(os.stat(path).st_mode))
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
            sys.exit(f'{path} changed since apply. {path + BACKUP} is the pre-probe '
                     f'original, so copying it back would erase those changes. Keep your '
                     f'edits and remove the LIPROBE lines by hand, then delete {path + BACKUP} '
                     f'and {STAMP}.')
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
    force, fix = '--force' in args, '--fix' in args
    if [a for a in args if a not in ('--force', '--fix')]:
        sys.exit('usage: apply [--force] [--fix]')
    edits = [(OLD_INC, NEW_INC), (OLD_SPIN, NEW_SPIN), (OLD_HERE, NEW_HERE), (OLD_FN, NEW_FN),
             (OLD_START, new_start(force)), (OLD_READ, new_read(force))]
    if force:
        edits.append((OLD_OWN, NEW_OWN))
    if fix:
        edits.append((OLD_CHECK, NEW_CHECK_FIX))
    apply_files([(LT, edits)])
    print('applied: lockup-sample classifies its sample'
          + ('; a cross call parked on the spinner\'s CPU at the sample' if force else '')
          + ('; the check accepts the spinner interrupted' if fix else ''))


if __name__ == '__main__':
    {'apply': apply, 'revert': revert}.get(sys.argv[1] if len(sys.argv) > 1 else '',
                                           lambda: sys.exit(__doc__))()
