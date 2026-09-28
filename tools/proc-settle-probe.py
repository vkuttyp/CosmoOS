#!/usr/bin/env python3
"""
proc-settle-probe.py -- why does run_module's process count not settle?

Three sightings of one check (docs/testing/flakes.md), the same helper
each time (run_module, kernel/process/proctest.c):

    dev-tty-none ... FAIL: check failed: process_count() == before at line 254 (2107 ms)
    signal-group ... FAIL: check failed: process_count() == before at line 254 (2477 ms)
    tty-isatty   ... (the same check, under the chaos migrator, at 500 ms)

run_module takes `before = process_count()`, spawns init, waits for it to
exit, then waits up to 2 s for process_count() to equal `before` again.
process_count() is machine-wide: the check holds only if no *other*
process is created or released inside the window.

The probe instruments every run_module call:
- the pids (and names, states) in the table at `before`, and again when
  the settle ends;
- how long the settle took after init exited;
- every difference between the two sets other than the process the call
  spawned, as `PSPROBE foreign`.

    python3 tools/proc-settle-probe.py apply [--fix | --harness] [--delay-reap MS [PID]]
    gmake ARCH=aarch64 test
    grep PSPROBE out/aarch64-debug/boot-test.log
    python3 tools/proc-settle-probe.py revert

`apply` and `revert` are those of tools/nvme-admin-probe.py: stamp first,
every file replaced atomically, the stamp removed last, finished by
running revert (again).
"""

import hashlib
import os
import subprocess
import sys

PROC_C = 'kernel/process/process.c'
PROCTEST = 'kernel/process/proctest.c'
REG = 'kernel/core/selftest.c'
THREAD_C = 'kernel/scheduler/thread.c'
BACKUP = '.proc-settle-probe.orig'

# Every test: a process still in the table when it ends is left to the next.
REG_EDITS = [
    ('''            failed++;
        }
    }

    sched_watchdog_disarm();''', '''            failed++;
        }
        {   /* PSPROBE: a process this test left to the next */
            extern unsigned process_count(void);
            struct psprobe_ent { int pid; int ppid; int state; unsigned refs; unsigned thr; char name[16]; };
            extern unsigned psprobe_table(struct psprobe_ent *out, unsigned max);
            static struct psprobe_ent lk[4];
            unsigned ln = psprobe_table(lk, 4);
            for (unsigned q = 0; q < ln && q < 4; q++)
                kinfo("PSPROBE leak: '%s' ended with pid %d '%s' (parent %d, state %d, refs %u, threads %u) in the table",
                      tests[i].name, lk[q].pid, lk[q].name, lk[q].ppid, lk[q].state, lk[q].refs, lk[q].thr);
        }
    }

    sched_watchdog_disarm();'''),
]


# --fix: the candidate. A test waits for *its own* process to leave the
# table (pids are never reused, so a pid names one process), not for a
# machine-wide count to come back.
FIX_PROC = [('''unsigned process_count(void)
{
    return g_process_count;
}
''', '''unsigned process_count(void)
{
    return g_process_count;
}

/* PSPROBE --fix: is pid still in the table -- its release not yet finished? */
bool psfix_in_table(pid_t pid);
bool psfix_in_table(pid_t pid)
{
    bool in = false;
    arch_irq_state_t s = spin_lock_irqsave(&g_process_table_lock);
    struct process *p;
    list_for_each_entry(p, &g_processes, all_link)
        if (p->pid == pid) {
            in = true;
            break;
        }
    spin_unlock_irqrestore(&g_process_table_lock, s);
    return in;
}
''')]
FIX_TEST = [
    ('''/* --- run the boot module --- */
''', '''/* --- run the boot module --- */

bool psfix_in_table(pid_t pid);   /* PSPROBE --fix */
static bool psfix_wait_gone(pid_t pid)
{
    uint64_t deadline = clock_deadline_ns(2000000000ULL);
    while (psfix_in_table(pid) && !clock_deadline_passed(deadline))
        thread_sleep_ms(1);
    return !psfix_in_table(pid);
}
'''),
    ('''    int status = process_wait_exit(p);
    process_put(p);
    if (status != 0) {
        kwarn("selftest: %s: the probe failed check %d", kind, status);
        *reason = "the terminal-mode probe reported a failure";''',
     '''    int status = process_wait_exit(p);
    process_put(p);
    CHECK(psfix_wait_gone(pid));   /* PSPROBE --fix: leave nothing to the next test */
    if (status != 0) {
        kwarn("selftest: %s: the probe failed check %d", kind, status);
        *reason = "the terminal-mode probe reported a failure";'''),
]
# run_module's settle, as the probe has already rewritten it: the count
# check becomes a check on its own pid.
FIX_RUN_MODULE = [('''    while (process_count() != before && !clock_deadline_passed(deadline))
        sched_yield();
    {   /* PSPROBE */''', '''    (void)deadline;
    bool ps_gone = psfix_wait_gone(ps_mine);   /* PSPROBE --fix: its own process, not the count */
    {   /* PSPROBE */'''),
                  ('''    CHECK(process_count() == before);
    *status_out = status;''', '''    CHECK(ps_gone);
    *status_out = status;''')]


# --harness: the design. run_module checks its own pid (FIX_RUN_MODULE), and
# the runner, after every test, waits up to 2 s for the table to empty and
# fails the test that left a process, naming it. No spawner is edited.
HARNESS_REG = [('''        uint64_t dt = clock_since_ns(t0);
        total_ns += dt;''', '''        {   /* PSPROBE --harness: nothing a test made outlives it */
            extern unsigned process_count(void);
            uint64_t hw0 = clock_now_ns(), hdl = clock_deadline_ns(2000000000ULL);
            while (process_count() != 0 && !clock_deadline_passed(hdl))
                thread_sleep_ms(1);
            if (clock_now_ns() - hw0 > 1000000)
                kinfo("PSPROBE harness: '%s' waited %llu us for its processes", tests[i].name,
                      (unsigned long long)((clock_now_ns() - hw0) / 1000));
            if (process_count() != 0 && ok) {
                ok = false;
                reason = "a process it made outlived it (PSPROBE --harness)";
            }
        }
        uint64_t dt = clock_since_ns(t0);
        total_ns += dt;''')]


def delay_edits(ms, pid):
    # --delay-reap MS [PID]: the reaper, having torn the last thread's process
    # down to a zombie (the exit is complete, the waiter is woken), waits MS
    # before dropping the thread's reference -- for every process, or only
    # for PID. The reaper is one thread, so a delay on PID also holds every
    # exit queued behind it: PID's release always comes first.
    cond = 'strcmp(thread_current()->name, "reaper") == 0' + (f' && p->pid == {pid}' if pid is not None else '')
    return [(THREAD_C, [('''        if (last)
            process_last_thread_gone(p);
        process_put(p);''', f'''        if (last)
            process_last_thread_gone(p);
        if ({cond})
            thread_sleep_ms({ms});   /* PSPROBE --delay-reap */
        process_put(p);''')])]
STAMP = '.proc-settle-probe.applied'

PROC_EDITS = [
    ("""static void process_release(struct kobject *obj)
{
    struct process *p = container_of(obj, struct process, obj);
    KASSERT(p->state == PROCESS_EXITED);
    KASSERT(p->nr_threads == 0);
""", """volatile uint64_t psprobe_rel_start_ns, psprobe_rel_end_ns, psprobe_exit_ns;   /* PSPROBE: stages */
volatile uint64_t psprobe_rel_h_ns, psprobe_rel_vm_ns, psprobe_rel_pages;
volatile int psprobe_rel_pid, psprobe_exit_pid;
char psprobe_rel_who[16];
static void process_release(struct kobject *obj)
{
    struct process *p = container_of(obj, struct process, obj);
    KASSERT(p->state == PROCESS_EXITED);
    KASSERT(p->nr_threads == 0);
    psprobe_rel_start_ns = clock_now_ns();   /* PSPROBE */
    psprobe_rel_pid = (int)p->pid;
    strlcpy(psprobe_rel_who, thread_current()->name, sizeof(psprobe_rel_who));
    psprobe_rel_pages = p->space ? p->space->mapped_pages : 0;
"""),
    ("""    handle_table_destroy(&p->handles);
    if (p->space != NULL)
        vm_space_destroy(p->space);
    if (p->cwd_locked)
        vnode_put(p->cwd_locked);
    if (p->root)
        vnode_put(p->root);
""", """    handle_table_destroy(&p->handles);
    psprobe_rel_h_ns = clock_now_ns();   /* PSPROBE */
    if (p->space != NULL)
        vm_space_destroy(p->space);
    psprobe_rel_vm_ns = clock_now_ns();   /* PSPROBE */
    if (p->cwd_locked)
        vnode_put(p->cwd_locked);
    if (p->root)
        vnode_put(p->root);
"""),
    ("""    kdebug("process: pid %u '%s' released", p->pid, p->name);
""", """    kdebug("process: pid %u '%s' released", p->pid, p->name);
    psprobe_rel_end_ns = clock_now_ns();   /* PSPROBE */
"""),
    ("""    complete(&p->exited);
    if (zombie)""", """    psprobe_exit_ns = clock_now_ns();   /* PSPROBE */
    psprobe_exit_pid = (int)p->pid;
    complete(&p->exited);
    if (zombie)"""),
    ("""unsigned process_count(void)
{
    return g_process_count;
}
""", """unsigned process_count(void)
{
    return g_process_count;
}

/* PSPROBE: the table, for run_module's settle */
struct psprobe_ent { pid_t pid; pid_t ppid; int state; unsigned refs; unsigned thr; char name[16]; };
unsigned psprobe_table(struct psprobe_ent *out, unsigned max);
unsigned psprobe_table(struct psprobe_ent *out, unsigned max)
{
    unsigned n = 0;
    arch_irq_state_t ts = spin_lock_irqsave(&g_process_table_lock);
    struct process *p;
    list_for_each_entry(p, &g_processes, all_link) {
        if (n < max) {
            out[n].pid = p->pid;
            out[n].ppid = p->parent_pid;
            out[n].state = (int)p->state;
            out[n].refs = __atomic_load_n(&p->obj.refcount, __ATOMIC_RELAXED);
            out[n].thr = p->nr_threads;
            strlcpy(out[n].name, p->name, sizeof(out[n].name));
        }
        n++;
    }
    spin_unlock_irqrestore(&g_process_table_lock, ts);
    return n;
}
"""),
]

TEST_EDITS = [
    ("""/* --- run the boot module --- */
""", """/* --- run the boot module --- */

/* PSPROBE */
struct psprobe_ent { pid_t pid; pid_t ppid; int state; unsigned refs; unsigned thr; char name[16]; };
unsigned psprobe_table(struct psprobe_ent *out, unsigned max);
enum { PSPROBE_MAX = 32 };
extern volatile uint64_t psprobe_rel_start_ns, psprobe_rel_end_ns, psprobe_exit_ns;
extern volatile int psprobe_rel_pid, psprobe_exit_pid;
extern char psprobe_rel_who[16];
extern volatile uint64_t psprobe_rel_h_ns, psprobe_rel_vm_ns, psprobe_rel_pages;

static bool psprobe_has(const struct psprobe_ent *t, unsigned n, pid_t pid)
{
    for (unsigned i = 0; i < n && i < PSPROBE_MAX; i++)
        if (t[i].pid == pid)
            return true;
    return false;
}

static void psprobe_diff(const char *what, const char *call, pid_t mine,
                         const struct psprobe_ent *a, unsigned na, const struct psprobe_ent *b, unsigned nb)
{
    for (unsigned i = 0; i < na && i < PSPROBE_MAX; i++)
        if (a[i].pid != mine && !psprobe_has(b, nb, a[i].pid))
            kinfo("PSPROBE foreign %s: call '%s' pid %d '%s' (parent %d, state %d, refs %u, threads %u) was in the table at before and is gone",
                  what, call, (int)a[i].pid, a[i].name, (int)a[i].ppid, a[i].state, a[i].refs, a[i].thr);
    for (unsigned i = 0; i < nb && i < PSPROBE_MAX; i++)
        if (b[i].pid != mine && !psprobe_has(a, na, b[i].pid))
            kinfo("PSPROBE foreign %s: call '%s' pid %d '%s' (parent %d, state %d, refs %u, threads %u) appeared",
                  what, call, (int)b[i].pid, b[i].name, (int)b[i].ppid, b[i].state, b[i].refs, b[i].thr);
}
"""),
    ("""    struct process *p = NULL;
    unsigned before = process_count();
    int rc = process_create_from_elf(image, image_size, argv[0], argv, NULL, NULL, &p);""",
     """    struct process *p = NULL;
    static struct psprobe_ent ps_a[PSPROBE_MAX], ps_b[PSPROBE_MAX];   /* PSPROBE: self-tests run one at a time */
    unsigned ps_na = psprobe_table(ps_a, PSPROBE_MAX);
    unsigned before = process_count();
    const char *ps_call = argv[1] && argv[2] ? argv[2] : argv[1] ? argv[1] : argv[0];
    int rc = process_create_from_elf(image, image_size, argv[0], argv, NULL, NULL, &p);"""),
    ("""     * still says "stuck" and no longer says "busy".
     */
    uint64_t t0 = clock_now_ns();
    int status = process_wait_exit(p);
    CHECK(clock_since_ns(t0) < 15000000000ULL);
    process_put(p);
""", """     * still says "stuck" and no longer says "busy".
     */
    uint64_t t0 = clock_now_ns();
    int status = process_wait_exit(p);
    CHECK(clock_since_ns(t0) < 15000000000ULL);
    pid_t ps_mine = p->pid;
    unsigned ps_nx = psprobe_table(ps_b, PSPROBE_MAX);   /* PSPROBE: as init exits */
    psprobe_diff("at-exit", ps_call, ps_mine, ps_a, ps_na, ps_b, ps_nx);
    uint64_t ps_t1 = clock_now_ns();
    process_put(p);
"""),
    ("""    while (process_count() != before && !clock_deadline_passed(deadline))
        sched_yield();
    CHECK(process_count() == before);""",
     """    while (process_count() != before && !clock_deadline_passed(deadline))
        sched_yield();
    {   /* PSPROBE */
        unsigned ps_nb = psprobe_table(ps_b, PSPROBE_MAX);
        kinfo("PSPROBE call '%s': pid %d, table %u at before (count %u), %u after (count %u), settle %llu us%s",
              ps_call, (int)ps_mine, ps_na, before, ps_nb, process_count(),
              (unsigned long long)((clock_now_ns() - ps_t1) / 1000),
              process_count() == before ? "" : " -- NOT SETTLED");
        kinfo("PSPROBE stages '%s': exit-completed pid %d at %lld us, test resumed %lld us, release pid %d by '%s' began %lld us, took %lld us",
              ps_call, psprobe_exit_pid, 0ll, (long long)((ps_t1 - psprobe_exit_ns) / 1000),
              psprobe_rel_pid, psprobe_rel_who,
              psprobe_rel_pid == (int)ps_mine ? (long long)((long long)(psprobe_rel_start_ns - psprobe_exit_ns) / 1000) : -1ll,
              psprobe_rel_pid == (int)ps_mine ? (long long)((psprobe_rel_end_ns - psprobe_rel_start_ns) / 1000) : -1ll);
        kinfo("PSPROBE parts '%s': handles %llu us, vm %llu us (%llu mapped pages), rest %llu us",
              ps_call, (unsigned long long)((psprobe_rel_h_ns - psprobe_rel_start_ns) / 1000),
              (unsigned long long)((psprobe_rel_vm_ns - psprobe_rel_h_ns) / 1000), (unsigned long long)psprobe_rel_pages,
              (unsigned long long)((psprobe_rel_end_ns - psprobe_rel_vm_ns) / 1000));
        psprobe_diff("at-settle", ps_call, ps_mine, ps_a, ps_na, ps_b, ps_nb);
    }
    CHECK(process_count() == before);"""),
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


def apply():
    args = sys.argv[2:]
    fix = '--fix' in args
    harness = '--harness' in args
    if fix and harness:
        sys.exit('--fix and --harness are two candidates: one at a time')
    args = [a for a in args if a not in ('--fix', '--harness')]
    delay = pid = None
    if args[:1] == ['--delay-reap'] and len(args) in (2, 3) and all(a.isdigit() for a in args[1:]):
        delay = int(args[1])
        pid = int(args[2]) if len(args) == 3 else None
    elif args:
        sys.exit('usage: apply [--fix | --harness] [--delay-reap MS [PID]]')
    fl = [(PROC_C, PROC_EDITS + (FIX_PROC if fix or harness else [])),
          (PROCTEST, TEST_EDITS + (FIX_TEST + FIX_RUN_MODULE if fix else FIX_TEST[:1] + FIX_RUN_MODULE if harness else [])),
          (REG, REG_EDITS + (HARNESS_REG if harness else []))]
    if delay is not None:
        fl += delay_edits(delay, pid)
    apply_files(fl)
    print('applied: every run_module call logs its settle and any foreign process; every test, a process it left'
          + ('; the candidate fix (each test waits for its own pid)' if fix else '')
          + ('; the design (run_module checks its own pid; the runner waits out and names what a test left)' if harness else '')
          + (f'; the reaper waits {delay} ms before dropping a thread\'s process'
             + (f' (pid {pid} only)' if pid is not None else '') if delay is not None else ''))


if __name__ == '__main__':
    {'apply': apply, 'revert': revert}.get(sys.argv[1] if len(sys.argv) > 1 else '',
                                           lambda: sys.exit(__doc__))()
