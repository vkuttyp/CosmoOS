#!/usr/bin/env python3
"""lockup-interrupted-probe.py -- reproduce, and prove the fix for, the
sighting where lockup-sample's sampled PC lies outside spin_here.

This probe applies to the *current* tree, after the fix (the unit
docs/audit/next-subsystem-lockup-interrupted.md, PR #271): spin_here
records the exact address it returns to in spinner_main (s->ret), and
lockup-sample accepts the spinner by that frame -- the leaf in spin_here
with trace[1] that return (uninterrupted), or the leaf elsewhere with that
return deeper in the trace (interrupted). The earlier version of this
probe, shipped with the report (PR #270), added those pieces itself and no
longer applies now that the kernel carries them.

    python3 tools/lockup-interrupted-probe.py apply [--force] [--mut-leaf | --mut-noret]
    gmake ARCH=x86_64 test > run.txt 2>&1
    grep LIPROBE out/x86_64-debug/boot-test.log
    python3 tools/lockup-interrupted-probe.py revert

The instrumentation classifies the sampled PC and each trace entry:
P (the probe's parked handler), R (exactly spin_here's return address into
spinner_main, which spin_here records), S (spin_here), or ? (anything
else). P is tested first: the parked handler sits near spinner_main.

--force makes the sample land inside an interrupt on the spinner,
deterministically. A helper thread on a third CPU sends CPU k a cross call
(smp_call_function_single) whose handler parks, bounded, until the test
has sampled. On x86-64 the NMI interrupts the parked handler, so the
sampled PC is in it (class P) and spin_here's return is deeper (R): the
interrupted case. On AArch64 there is no NMI -- the sample is an ordinary
interrupt the parked handler (interrupts masked) holds off, so it goes
unanswered and the test fails earlier, at the answered check; the
interrupted-leaf hazard is x86-64's alone, which is why every sighting is.
If the forcing cannot happen (fewer than three online CPUs, the helper
thread fails to start, or the handler never parks) the test fails on
CHECK(lp_was_parked) rather than silently sampling the plain case, so a
forced run that could not force never reads as a pass.

With --force alone the shipped check accepts the interrupted sample (ok).
The mutations show the check is load-bearing, on that same forced sample:
--mut-leaf reverts the check to `pc in spin_here` alone -> it fails
exactly as the sightings did; --mut-noret stops spin_here recording its
return -> the frame scan finds no matching address and fails.

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

# The forcing helpers and the sample classifier, before the pinned test.
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

# --force: park a cross call before the sample, capture whether it was still
# parked at the sample. Anchored on the unique setup-through-sample block.
OLD_FORCE = """    CHECK(t != NULL);

    uint64_t t0 = clock_now_ns();
    cpumask_t m = 0;
    bool ok = lockup_sample_all(NULL, LOCKUP_SAMPLE_TIMEOUT_NS, &m);
"""
NEW_FORCE = r"""    CHECK(t != NULL);
    struct thread *lp_th = NULL;   /* LIPROBE --force */
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

    uint64_t t0 = clock_now_ns();
    cpumask_t m = 0;
    bool ok = lockup_sample_all(NULL, LOCKUP_SAMPLE_TIMEOUT_NS, &m);
    bool lp_was_parked = lp_parked == 1;   /* LIPROBE --force: still inside the handler at the sample */
    lp_release = 1;
    if (lp_th)
        thread_join(lp_th);
"""

# The classification print, after the trace is copied out of the slot.
OLD_READ = """    memcpy(trace, sm->trace, sizeof(trace));
"""
def new_read(force):
    s = OLD_READ + r"""    {   /* LIPROBE */
        char lp_cls[LOCKUP_TRACE_MAX + 1];
        unsigned lp_n = depth < LOCKUP_TRACE_MAX ? depth : LOCKUP_TRACE_MAX;
        for (unsigned i = 0; i < lp_n; i++)
            lp_cls[i] = lp_class(trace[i], s.ret);
        lp_cls[lp_n] = 0;
        kprintf("LIPROBE: cpu %d answered %d, nmi %d, pc %c, depth %u, trace %s\n", k,
                (int)((m >> k) & 1), (int)sm->nmi, lp_class(pc, s.ret), depth, lp_cls);
    }
"""
    if force:
        s += r"""    kprintf("LIPROBE: forced: the handler was %s at the sample\n", lp_was_parked ? "still parked" : "NOT PARKED");
"""
    return s

# --force guard: a run that could not force is a plain run in disguise, not a
# pass. After stop_spinner/CHECK(own) so a failure does not leak the spinner.
OLD_OWN = """    CHECK(own);
"""
NEW_OWN = """    CHECK(own);
    CHECK(lp_was_parked);   /* LIPROBE --force */
"""

# --mut-leaf: the check back to the sighting's leaf-PC assertion.
OLD_CHECK = """    {
        uintptr_t ret = s.ret;
        bool ret_below = false;
        for (unsigned i = 1; i < depth && i < LOCKUP_TRACE_MAX; i++)
            ret_below |= trace[i] == ret;
        CHECK(in_fn(pc, (const void *)spin_here, SPIN_FN_BOUND) ? tr1 == ret : ret_below);
    }"""
MUT_LEAF = """    (void)tr1;
    CHECK(in_fn(pc, (const void *)spin_here, SPIN_FN_BOUND));   /* LIPROBE --mut-leaf: the sighting's check */"""

# --mut-noret: spin_here records no return address.
OLD_HERE_RET = """    s->ret = (uintptr_t)__builtin_return_address(0);"""
MUT_NORET = """    s->ret = 0;   /* LIPROBE --mut-noret */"""


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
    force = '--force' in args
    mut_leaf = '--mut-leaf' in args
    mut_noret = '--mut-noret' in args
    if [a for a in args if a not in ('--force', '--mut-leaf', '--mut-noret')]:
        sys.exit('usage: apply [--force] [--mut-leaf | --mut-noret]')
    if mut_leaf and mut_noret:
        sys.exit('usage: --mut-leaf and --mut-noret are exclusive')
    edits = [(OLD_INC, NEW_INC), (OLD_FN, NEW_FN)]
    if force:
        edits.append((OLD_FORCE, NEW_FORCE))
    edits.append((OLD_READ, new_read(force)))
    if force:
        edits.append((OLD_OWN, NEW_OWN))
    if mut_leaf:
        edits.append((OLD_CHECK, MUT_LEAF))
    if mut_noret:
        edits.append((OLD_HERE_RET, MUT_NORET))
    apply_files([(LT, edits)])
    print('applied: lockup-sample classifies its sample'
          + ('; a cross call parked on the spinner\'s CPU at the sample' if force else '')
          + ('; the check reverted to the leaf PC (the sighting)' if mut_leaf else '')
          + ('; spin_here records no return' if mut_noret else ''))


if __name__ == '__main__':
    {'apply': apply, 'revert': revert}.get(sys.argv[1] if len(sys.argv) > 1 else '',
                                           lambda: sys.exit(__doc__))()
