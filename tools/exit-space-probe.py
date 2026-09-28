#!/usr/bin/env python3
"""
exit-space-probe.py -- does a program that waits for its child still find
the child's binary busy?

An exited process keeps its address space -- and with it the text
mapping that holds its program file busy (-ETXTBSY) -- until its last
reference drops (`vm_space_destroy` runs in `process_release`). The
native `waitpid` returns once the child is EXITED, which the reaper sets
before it drops the exited thread's reference. The proc-settle unit
forced the window with a reap hold (docs/audit/next-subsystem-proc-settle.md);
this probe asks whether a user program meets it *unforced*.

`init --txtbsy-race N` copies /boot/init to /tmp/txbsy, then N times:
spawns /tmp/txbsy (`--exit0`, which returns at once), waitpid()s it, and
rewrites the file's last byte with its own value. It counts the rounds
whose write (or open) was refused with ETXTBSY and, for those, how long
the file stayed busy. A kernel test runs it; it is registered REPS times.

    python3 tools/exit-space-probe.py apply [--fix] [--parts | --fast] [ROUNDS [REPS]]
    gmake ARCH=aarch64 test
    grep TXBPROBE out/aarch64-debug/boot-test.log
    python3 tools/exit-space-probe.py revert

`apply` and `revert` are those of tools/nvme-admin-probe.py: stamp first,
every file replaced atomically, the stamp removed last, finished by
running revert (again).
"""

import hashlib
import os
import subprocess
import sys

INIT = 'userland/init/init.c'
PROCTEST = 'kernel/process/proctest.c'
REG = 'kernel/core/selftest.c'
BACKUP = '.exit-space-probe.orig'
STAMP = '.exit-space-probe.applied'


def init_edits():
    return [
        ("""int main(int argc, char **argv)
{""", """/* TXBPROBE: after waitpid, is the child's binary still busy? */
static int txtbsy_rewrite_last(const char *path)
{
    int fd = open(path, O_RDWR);
    if (fd < 0)
        return -errno;
    off_t end = lseek(fd, 0, SEEK_END);
    char b;
    int rc = 0;
    if (end <= 0 || lseek(fd, end - 1, SEEK_SET) < 0 || read(fd, &b, 1) != 1 || lseek(fd, end - 1, SEEK_SET) < 0)
        rc = -EIO;
    else if (write(fd, &b, 1) != 1)
        rc = -errno;
    close(fd);
    return rc;
}

static int txtbsy_race(unsigned n)
{
    const char *path = "/tmp/txbsy";
    int in = open("/boot/init", O_RDONLY), out = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (in < 0 || out < 0)
        return 2;
    char buf[4096];
    ssize_t k;
    while ((k = read(in, buf, sizeof(buf))) > 0)
        if (write(out, buf, (size_t)k) != k)
            return 3;
    close(in);
    close(out);
    unsigned busy = 0, other = 0;
    uint64_t longest = 0, total = 0;
    for (unsigned i = 0; i < n; i++) {
        const char *cargv[] = { "txbsy", "--exit0", NULL };
        long pid = cosmo_spawn(&(struct cosmo_spawn){ .path = path, .argv = cargv });
        if (pid < 0)
            return 4;
        int st = 0;
        if (waitpid((pid_t)pid, &st, 0) != (pid_t)pid || st != 0)
            return 5;
        int rc = txtbsy_rewrite_last(path);
        if (rc == -ETXTBSY) {
            busy++;
            uint64_t t0 = cosmo_clock_ns();
            while (txtbsy_rewrite_last(path) == -ETXTBSY && cosmo_clock_since_ns(t0) < 5000000000ull)
                ;
            uint64_t d = cosmo_clock_since_ns(t0);
            total += d;
            if (d > longest)
                longest = d;
        } else if (rc != 0) {
            other++;
        }
    }
    unlink(path);
    /* A rewrite that failed for another reason measured nothing: say so
     * with a status the kernel test fails on, not only in the line. */
    printf("TXBPROBE: %u of %u rewrites after waitpid refused with ETXTBSY (%u other errors); busy for %llu us at most, %llu us in all\\n",
           busy, n, other, (unsigned long long)(longest / 1000), (unsigned long long)(total / 1000));
    fflush(stdout);
    return other != 0 ? 6 : 0;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "--exit0") == 0)
        return 0;   /* TXBPROBE: the child */
    if (argc >= 3 && strcmp(argv[1], "--txtbsy-race") == 0)
        return txtbsy_race((unsigned)atoi(argv[2]));   /* TXBPROBE */"""),
    ]


PROC_C = 'kernel/process/process.c'

# --fix: the candidate. The address space goes at exit, first thing in
# process_last_thread_gone: before the state waitpid reaps on (EXITED) is
# published, and so before the exit is completed or the parent woken --
# as Linux's do_exit releases the mm before exit_notify. (Placed after the
# state is published, one rewrite in 900 was still refused on each
# architecture: a parent already looking reaped it mid-teardown.)
FIX = [(PROC_C, [("""void process_last_thread_gone(struct process *p)
{
    LIST_HEAD(orphans);""", """void process_last_thread_gone(struct process *p)
{
    /* TXBPROBE --fix: the address space goes first, while the process is
     * still EXITING -- before the state a waitpid reaps on is published,
     * and before anyone is told. */
    if (p->space != NULL) {
        vm_space_destroy(p->space);
        p->space = NULL;
    }
    LIST_HEAD(orphans);""")])]


VMM_C = 'kernel/memory/vmm.c'

# --parts: where a space's teardown goes. Every vm_space_destroy logs its
# regions, pages walked, frames freed and chunks, and the time in the
# walk (query + unmap under the lock), the shootdowns and the frees.
PARTS = [('kernel/include/kernel/vmm.h', [("""struct vm_space {
    struct arch_mmu_context mmu;""", """struct vm_space {
    uint64_t esp[6];   /* ESPARTS: walk, shoot, free ns; pages, frames, chunks -- per space, so */
    bool esp_on;       /* two teardowns at once never share a counter */
    struct arch_mmu_context mmu;""")]),
         (VMM_C, [
    ("""        arch_irq_state_t s = spin_lock_irqsave(&space->lock);
        for (vaddr_t p = va; p < va + chunk; p += PAGE_SIZE) {
            paddr_t pa;
            if (!arch_mmu_query(&space->mmu, p, &pa, NULL, NULL, NULL))
                continue;""", """        uint64_t esp_t0 = clock_now_ns();   /* ESPARTS */
        arch_irq_state_t s = spin_lock_irqsave(&space->lock);
        for (vaddr_t p = va; p < va + chunk; p += PAGE_SIZE) {
            paddr_t pa;
            space->esp[3]++;
            if (!arch_mmu_query(&space->mmu, p, &pa, NULL, NULL, NULL))
                continue;
            space->esp[4]++;"""),
    ("""        spin_unlock_irqrestore(&space->lock, s);

        user_shootdown(space, va, chunk);

        /* The mapping's reference: the last one frees, and for a cache
         * frame the cache's own is never the mapping's to drop. */
        for (unsigned i = 0; i < n; i++)
            pmm_page_put(frames[i]);""", """        spin_unlock_irqrestore(&space->lock, s);
        uint64_t esp_t1 = clock_now_ns();

        user_shootdown(space, va, chunk);
        uint64_t esp_t2 = clock_now_ns();

        /* The mapping's reference: the last one frees, and for a cache
         * frame the cache's own is never the mapping's to drop. */
        for (unsigned i = 0; i < n; i++)
            pmm_page_put(frames[i]);
        if (space->esp_on) {   /* ESPARTS */
            space->esp[0] += esp_t1 - esp_t0;
            space->esp[1] += esp_t2 - esp_t1;
            space->esp[2] += clock_now_ns() - esp_t2;
            space->esp[5]++;
        }"""),
    ("""    KASSERT(raw_this_cpu()->cur_space != space);

    for (;;) {""", """    KASSERT(raw_this_cpu()->cur_space != space);
    for (unsigned q = 0; q < 6; q++)   /* ESPARTS */
        space->esp[q] = 0;
    space->esp_on = true;
    unsigned esp_regions = 0;
    uint64_t esp_start = clock_now_ns();

    for (;;) {"""),
    ("""        user_range_teardown(space, base, size);
        region_put(r);
    }""", """        user_range_teardown(space, base, size);
        region_put(r);
        esp_regions++;   /* ESPARTS */
    }
    space->esp_on = false;
    kinfo("ESPARTS: %u regions, %llu pages walked, %llu frames, %llu chunks: walk %llu us, shootdown %llu us, free %llu us, all %llu us",
          esp_regions, (unsigned long long)space->esp[3], (unsigned long long)space->esp[4], (unsigned long long)space->esp[5],
          (unsigned long long)(space->esp[0] / 1000), (unsigned long long)(space->esp[1] / 1000),
          (unsigned long long)(space->esp[2] / 1000), (unsigned long long)((clock_now_ns() - esp_start) / 1000));"""),
])]


VMM_FAST = [(VMM_C, [
    ("static void user_range_teardown(struct vm_space *space, vaddr_t base, size_t size)\n{\n    for (vaddr_t va = base; va < base + size; va += TEARDOWN_CHUNK_PAGES * PAGE_SIZE) {\n        size_t chunk = MIN((size_t)(TEARDOWN_CHUNK_PAGES * PAGE_SIZE), (size_t)(base + size - va));\n        struct page *frames[TEARDOWN_CHUNK_PAGES];\n        unsigned n = 0;\n\n        arch_irq_state_t s = spin_lock_irqsave(&space->lock);\n        for (vaddr_t p = va; p < va + chunk; p += PAGE_SIZE) {\n            paddr_t pa;\n            if (!arch_mmu_query(&space->mmu, p, &pa, NULL, NULL, NULL))\n                continue;\n            struct page *page = phys_to_page(pa);\n            KASSERT(page != NULL);\n            frames[n++] = page;\n            frame_uncount(space, page);\n        }\n        int rc = arch_mmu_unmap(&space->mmu, va, chunk);\n        KASSERT(rc == 0);\n        spin_unlock_irqrestore(&space->lock, s);\n\n        user_shootdown(space, va, chunk);\n\n        /* The mapping's reference: the last one frees, and for a cache\n         * frame the cache's own is never the mapping's to drop. */\n        for (unsigned i = 0; i < n; i++)\n            pmm_page_put(frames[i]);\n    }\n}\n", 'static void user_range_teardown_x(struct vm_space *space, vaddr_t base, size_t size, bool dying)\n{\n    /* ESFAST: for a dying space, an absent stretch is skipped whole, and\n     * no chunk is shot down -- nothing runs the space, and its tag is\n     * invalidated once at the end of vm_space_destroy. */\n    vaddr_t end = base + size, va = base;\n    while (va < end) {\n        if (dying) {\n            size_t sk = arch_mmu_absent_span(&space->mmu, va);\n            if (sk > PAGE_SIZE) {\n                va = sk >= (size_t)(end - va) ? end : va + sk;\n                continue;\n            }\n        }\n        size_t chunk = MIN((size_t)(TEARDOWN_CHUNK_PAGES * PAGE_SIZE), (size_t)(end - va));\n        struct page *frames[TEARDOWN_CHUNK_PAGES];\n        unsigned n = 0;\n\n        arch_irq_state_t s = spin_lock_irqsave(&space->lock);\n        for (vaddr_t p = va; p < va + chunk; p += PAGE_SIZE) {\n            paddr_t pa;\n            if (!arch_mmu_query(&space->mmu, p, &pa, NULL, NULL, NULL))\n                continue;\n            struct page *page = phys_to_page(pa);\n            KASSERT(page != NULL);\n            frames[n++] = page;\n            frame_uncount(space, page);\n        }\n        int rc = arch_mmu_unmap(&space->mmu, va, chunk);\n        KASSERT(rc == 0);\n        spin_unlock_irqrestore(&space->lock, s);\n\n        if (!dying)\n            user_shootdown(space, va, chunk);\n\n        for (unsigned i = 0; i < n; i++)\n            pmm_page_put(frames[i]);\n        va += chunk;\n    }\n}\n\nstatic void user_range_teardown(struct vm_space *space, vaddr_t base, size_t size)\n{\n    user_range_teardown_x(space, base, size, false);\n}\n'),
    ("""        user_range_teardown(space, base, size);
        region_put(r);""", """        user_range_teardown_x(space, base, size, true);   /* ESFAST */
        region_put(r);"""),
    ("""    KASSERT(raw_this_cpu()->cur_space != space);

    for (;;) {""", """    KASSERT(raw_this_cpu()->cur_space != space);
    uint64_t esf_t0 = clock_now_ns();   /* ESFAST */

    for (;;) {"""),
    ("""    arch_mmu_context_destroy(&space->mmu);
    kmem_cache_free(g_space_cache, space);""", """    arch_mmu_context_destroy(&space->mmu);
    kmem_cache_free(g_space_cache, space);
    kinfo("ESFAST: teardown %llu us", (unsigned long long)((clock_now_ns() - esf_t0) / 1000));"""),
])]
ARCH_FAST = [
    ('kernel/include/arch/mmu.h', [("""bool arch_mmu_query(const struct arch_mmu_context *ctx, vaddr_t va, paddr_t *pa,
                    vm_prot_t *prot, vm_cache_t *cache, size_t *page_size);""", """bool arch_mmu_query(const struct arch_mmu_context *ctx, vaddr_t va, paddr_t *pa,
                    vm_prot_t *prot, vm_cache_t *cache, size_t *page_size);
/* ESFAST: bytes from va that are certainly unmapped (0: va is mapped). */
size_t arch_mmu_absent_span(const struct arch_mmu_context *ctx, vaddr_t va);""")]),
]
for _arch in ('aarch64', 'x86_64'):
    ARCH_FAST.append((f'kernel/arch/{_arch}/mmu.c', [("""bool arch_mmu_query(const struct arch_mmu_context *ctx, vaddr_t va, paddr_t *pa,""", """size_t arch_mmu_absent_span(const struct arch_mmu_context *ctx, vaddr_t va)   /* ESFAST */
{
    struct walk w;
    walk(ctx, va, &w);
    if (w.present)
        return 0;
    uint64_t sz = level_size[w.level];
    return (size_t)(sz - (va & (sz - 1)));
}

bool arch_mmu_query(const struct arch_mmu_context *ctx, vaddr_t va, paddr_t *pa,""")]))
FAST = VMM_FAST + ARCH_FAST


def test_edits(rounds):
    return [
        ("""bool selftest_dev_tty_none(const char **reason)
{""", f"""/* TXBPROBE */
bool selftest_txbprobe(const char **reason);
bool selftest_txbprobe(const char **reason)
{{
    const char *argv[] = {{ "init", "--txtbsy-race", "{rounds}", NULL }};
    int status;
    if (!run_module(argv, &status, reason))
        return false;
    if (status != 0 && status != -1) {{
        kwarn("TXBPROBE: the probe stopped at step %d", status);
        *reason = "the txtbsy probe failed";
        return false;
    }}
    return true;
}}

bool selftest_dev_tty_none(const char **reason)
{{"""),
    ]


def reg_edits(reps):
    anchor = '    { "process-reaped",  selftest_process_reaped },\n'
    return [(anchor, anchor + '    { "txbprobe",        selftest_txbprobe },\n' * reps),
            ('#include <kernel/thread.h>\n', '#include <kernel/thread.h>\nbool selftest_txbprobe(const char **reason);   /* TXBPROBE */\n')]


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
    parts = '--parts' in args
    fastm = '--fast' in args
    if parts and fastm:
        sys.exit('--parts and --fast patch the same function: one at a time')
    args = [a for a in args if a not in ('--fix', '--parts', '--fast')]
    if any(not a.isdigit() for a in args) or len(args) > 2:
        sys.exit('usage: apply [--fix] [--parts | --fast] [ROUNDS [REPS]]')
    rounds = int(args[0]) if args else 60
    reps = int(args[1]) if len(args) > 1 else 5
    apply_files([(INIT, init_edits()), (PROCTEST, test_edits(rounds)), (REG, reg_edits(reps))] + (FIX if fix else []) + (PARTS if parts else []) + (FAST if fastm else []))
    print(f'applied: init --txtbsy-race {rounds}, run by a kernel test registered {reps} times'
          + ('; the candidate (the space goes at exit)' if fix else '')
          + ('; every teardown timed by part' if parts else '')
          + ('; a dying space skips absent stretches and per-chunk shootdowns' if fastm else ''))


if __name__ == '__main__':
    {'apply': apply, 'revert': revert}.get(sys.argv[1] if len(sys.argv) > 1 else '',
                                           lambda: sys.exit(__doc__))()
