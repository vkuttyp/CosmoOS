/*
 * thrtest - the native thread interface, from userland
 * (docs/kernel/process/design.md, "Native threads";
 *  docs/kernel/process/testing.md).
 *
 * A kernel self-test cannot create a *user* thread, so this is the proof,
 * run from /etc/rc.test and gated by the "THREADTEST: PASS" marker the
 * boot harness requires. Every check prints what failed and why, because
 * a marker that simply never appears says nothing about which step broke.
 */

#include <errno.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cosmo/syscall.h>
#include <cosmo/thread.h>

#define PAGE 4096u

static int failures;

/* Printed before each step, so a hang or a fault names the step it was in
 * -- two of this unit's bug-proofs kill the process outright, and "no
 * output" would not say where. */
#define STEP(n)                                                              \
    do {                                                                     \
        printf("thrtest: step %s\n", (n));                                   \
        fflush(stdout);                                                      \
    } while (0)

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("thrtest: FAIL %s at line %d\n", #cond, __LINE__);        \
            failures++;                                                      \
        }                                                                    \
    } while (0)

/* (1) A thread runs and is joined. */
static volatile unsigned ran;
static void *simple(void *arg)
{
    ran = (unsigned)(unsigned long)arg;
    return (void *)0x5eed;
}

/*
 * (2) The entry conditions the ABI promises. The kernel enters a thread at
 * a function with no `call` having happened, so what the entry sees is the
 * thing to check: SysV wants rsp % 16 == 8 (a call having pushed eight
 * bytes), AAPCS wants sp % 16 == 0. It has to be captured *before* any
 * prologue runs, so the entry is a naked stub that records the stack
 * pointer and tail-jumps to the C body -- reading rsp inside the C
 * function measures the frame, not the entry, which an earlier version of
 * this test did and x86-64 caught.
 */
volatile unsigned long entry_sp;
static volatile unsigned entry_arg_ok;
void *entry_probe_body(void *arg);
extern void *entry_probe(void *arg);   /* the naked stub below */
#if defined(__x86_64__)
__asm__(".text\n"
        ".globl entry_probe\n"
        "entry_probe:\n"
        "  movq %rsp, entry_sp(%rip)\n"
        "  jmp entry_probe_body\n");
#else
__asm__(".text\n"
        ".globl entry_probe\n"
        "entry_probe:\n"
        "  mov x1, sp\n"
        "  adrp x2, entry_sp\n"
        "  add x2, x2, :lo12:entry_sp\n"
        "  str x1, [x2]\n"
        "  b entry_probe_body\n");
#endif
void *entry_probe_body(void *arg)
{
    entry_arg_ok = arg == (void *)0x1234;
    /* A 16-byte-aligned store is what actually faults if the alignment is
     * wrong rather than merely unusual. */
    typedef unsigned long long v16 __attribute__((vector_size(16)));
    volatile v16 v = { 1, 2 };
    return (void *)(unsigned long)(v[0] + v[1]);
}

/* (3) Two threads make progress against each other. */
static volatile unsigned flag_a, flag_b;
static void *pair_a(void *arg)
{
    (void)arg;
    flag_a = 1;
    for (unsigned i = 0; i < 2000000u && !flag_b; i++)
        if ((i & 0xfffu) == 0xfffu)
            cosmo_yield();
    return (void *)(unsigned long)flag_b;
}

/* (11) The allocator and stdio, from several threads at once: what an
 * unlocked free list corrupts silently and an unlocked stream garbles.
 * Each thread allocates, writes a pattern, frees, and prints -- and checks
 * its own blocks, so a lost or shared block shows up as a wrong byte
 * rather than only as a crash. */
static volatile unsigned heap_bad, heap_done, heap_ready, heap_go, heap_late;
static void *heap_user(void *arg)
{
    unsigned id = (unsigned)(unsigned long)arg;
    /*
     * Wait until every worker exists before doing any work. Without this
     * the retries below can let one worker finish before the last one is
     * created, and the step would pass without ever running three threads
     * through the allocator at once -- which is the whole property. The
     * wait is bounded so a worker cannot hang if a create was refused for
     * good: main releases the barrier either way.
     *
     * A bound that expires silently, though, is the flaw the barrier was
     * added to remove: the worker would go on to allocate alone, every
     * assertion below would still hold, and the step would once more pass
     * without having tested anything. So expiry is *counted*, and main
     * asserts it never happened -- the wait is bounded for safety, and
     * observable so that the safety cannot be mistaken for the property.
     */
    __atomic_fetch_add(&heap_ready, 1, __ATOMIC_ACQ_REL);
    unsigned w = 0;
    while (w < 2000u && !__atomic_load_n(&heap_go, __ATOMIC_ACQUIRE)) {
        cosmo_yield();
        w++;
    }
    if (!__atomic_load_n(&heap_go, __ATOMIC_ACQUIRE)) {
        printf("thrtest: heap %u gave up waiting for the barrier\n", id);
        fflush(stdout);
        __atomic_fetch_add(&heap_late, 1, __ATOMIC_ACQ_REL);
    }
    for (unsigned i = 0; i < 400u; i++) {
        size_t n = 16u + ((i * 37u + id) % 700u);
        unsigned char *p = malloc(n);
        if (p == NULL) {
            printf("thrtest: heap %u iter %u malloc(%u) NULL\n", id, i, (unsigned)n);
            heap_bad++;
            break;
        }
        memset(p, (int)(id & 0xff), n);
        for (size_t k = 0; k < n; k++)
            if (p[k] != (unsigned char)(id & 0xff)) {
                printf("thrtest: heap %u iter %u byte %u is %u not %u\n", id, i,
                       (unsigned)k, p[k], id & 0xff);
                heap_bad++;
                break;
            }
        void *q = realloc(p, n * 2);
        if (q == NULL) {
            printf("thrtest: heap %u iter %u realloc(%u) NULL\n", id, i, (unsigned)(n * 2));
            free(p);
            heap_bad++;
            break;
        }
        free(q);
        if ((i % 100u) == 0)
            printf("thrtest: heap thread %u at %u\n", id, i);
    }
    heap_done++;
    return NULL;
}

/* (12) A thread that waits to be told to stop, for the bound. */
static volatile unsigned spin_stop;
static void *spinner(void *arg)
{
    (void)arg;
    while (!spin_stop)
        cosmo_yield();
    return NULL;
}

/* (5) clear_tid is a join: a thread that has already finished. */
static void *quick(void *arg)
{
    (void)arg;
    return NULL;
}

/* (6) Exit semantics: a worker's exit must not end the process. */
static void *exiter(void *arg)
{
    (void)arg;
    cosmo_thread_exit(0);
    return NULL;   /* not reached */
}

/* (7) The signal mask is per-thread. */
static volatile unsigned handler_tid;
static void on_usr1(int sig)
{
    (void)sig;
    handler_tid = cosmo_thread_id();
}
static volatile unsigned worker_blocked, worker_stop;
static void *masked(void *arg)
{
    (void)arg;
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    sigprocmask(SIG_BLOCK, &set, NULL);   /* the mask is this thread's */
    worker_blocked = 1;
    while (!worker_stop)
        cosmo_yield();
    return NULL;
}

/* (9) A mutex over the futex, which is what the futex is for. */
static cosmo_mutex_t mx = COSMO_MUTEX_INIT;
static volatile unsigned long counter;
static void *bump(void *arg)
{
    (void)arg;
    for (unsigned i = 0; i < 20000u; i++) {
        cosmo_mutex_lock(&mx);
        counter++;
        cosmo_mutex_unlock(&mx);
    }
    return NULL;
}

#define SELF_PATH "/boot/tests/native/thrtest"

/*
 * The two filtered children of step (10). Each installs a filter on
 * itself and then makes one call; what happens next is the assertion, and
 * only the parent can see it.
 */
static int filter_child(const char *mode)
{
    uint64_t mask[COSMO_SYSCALL_MASK_WORDS];
    cosmo_thread_t t;
    if (strcmp(mode, "filter-deny") == 0) {
        memset(mask, 0xff, sizeof(mask));   /* everything but thread_create */
        mask[SYS_thread_create / 64] &= ~(1ull << (SYS_thread_create % 64));
        if (cosmo_syscall2(SYS_syscall_filter, (long)mask, COSMO_SYSCALL_MASK_WORDS) != 0)
            return 2;
        cosmo_thread_start(&t, simple, NULL, 0);   /* must not come back */
        return 3;
    }
    memset(mask, 0, sizeof(mask));          /* deny everything there is */
    if (cosmo_syscall2(SYS_syscall_filter, (long)mask, COSMO_SYSCALL_MASK_WORDS) != 0)
        return 2;
    cosmo_thread_exit(7);                   /* and yet this one works */
    return 3;
}

int main(int argc, char **argv)
{
    cosmo_thread_t t, t2;
    void *ret;

    if (argc > 1)
        return filter_child(argv[1]);
    STEP("1");
    /* (1) */
    CHECK(cosmo_thread_id() == (unsigned)getpid());   /* a first thread's id is the pid */
    CHECK(cosmo_thread_start(&t, simple, (void *)7ul, 0) == 0);
    CHECK(t.tid != 0 && t.tid != (unsigned)getpid());
    CHECK(cosmo_thread_join(&t, &ret) == 0);
    CHECK(ran == 7);
    CHECK(ret == (void *)0x5eed);


    STEP("2");
    /* (2) */
    CHECK(cosmo_thread_start(&t, (void *(*)(void *))entry_probe, (void *)0x1234ul, 0) == 0);
    CHECK(cosmo_thread_join(&t, NULL) == 0);
    CHECK(entry_arg_ok);
#if defined(__x86_64__)
    CHECK(entry_sp % 16 == 8);
#else
    CHECK(entry_sp % 16 == 0);
#endif


    STEP("3");
    /* (3) */
    flag_a = flag_b = 0;
    CHECK(cosmo_thread_start(&t, pair_a, NULL, 0) == 0);
    for (unsigned i = 0; i < 2000000u && !flag_a; i++)
        if ((i & 0xfffu) == 0xfffu)
            cosmo_yield();
    CHECK(flag_a == 1);
    flag_b = 1;
    CHECK(cosmo_thread_join(&t, &ret) == 0);
    CHECK(ret == (void *)1ul);   /* it saw ours: both ran */


    STEP("4");
    /* (4) The futex closes the race it exists for. */
    {
        volatile unsigned word = 1;
        CHECK(cosmo_futex_wait(&word, 0, 0) == -EAGAIN);          /* value differs: no sleep */
        CHECK(cosmo_futex_wait(&word, 1, 1000000ull) == -ETIMEDOUT);
        CHECK(cosmo_futex_wake(&word, 1) == 0);                   /* nobody waiting */
        unsigned unaligned_holder[2];
        volatile unsigned *bad = (volatile unsigned *)((char *)unaligned_holder + 1);
        CHECK(cosmo_futex_wait(bad, 0, 0) == -EINVAL);            /* alignment is the futex's rule */
        CHECK(cosmo_futex_wake((volatile unsigned *)16ul, 1) == -EFAULT);
        /* A duration that would wrap the deadline is refused, not turned
         * into an immediate timeout. */
        CHECK(cosmo_futex_wait(&word, 1, 0xffffffffffffffffull) == -EINVAL);
        CHECK(cosmo_futex_wait(&word, 1, 0x8000000000000000ull) == -EINVAL);
    }


    STEP("5");
    /* (5) */
    /* A hundred of them, because the property is a race: the word must
     * hold the tid before the child can run, and a child that exits
     * *first* must still leave it zero. One attempt almost never contests
     * that window -- an earlier version of this step used one, and the
     * bug-proof for the ordering passed. */
    /*
     * First, deterministically: a child that waits to be told to stop
     * cannot have exited, so the word must still hold its tid. Asserting
     * that against a child that returns at once is wrong, and CI proved
     * it -- on a machine with more parallelism the child finishes, the
     * kernel zeroes the word, and the parent reads 0. (This test passed
     * five local runs and failed the first CI one.)
     */
    spin_stop = 0;
    CHECK(cosmo_thread_start(&t, spinner, NULL, PAGE) == 0);
    CHECK(t.done == t.tid);            /* written before it could run, and it is still running */
    spin_stop = 1;
    CHECK(cosmo_thread_join(&t, NULL) == 0);
    CHECK(t.done == 0);                /* zeroed and woken at its exit */

    /*
     * Then the contested part, a hundred times, with a child that returns
     * at once: what is checked here is the *join*, because the word may
     * legitimately read either the tid or zero depending on who won. A
     * stale tid -- written after the child had already zeroed it -- is
     * what hangs the join, which is how the bug-proof for the ordering
     * shows up.
     */
    for (unsigned i = 0; i < 100u; i++) {
        CHECK(cosmo_thread_start(&t, quick, NULL, PAGE) == 0);
        unsigned d = t.done;
        CHECK(d == t.tid || d == 0);   /* never a value neither side wrote */
        if (i % 10 == 0)
            cosmo_sleep_ns(2000000ull);   /* sometimes let it finish first */
        CHECK(cosmo_thread_join(&t, NULL) == 0);
        CHECK(t.done == 0);
        if (failures)
            break;
    }


    STEP("6");
    /* (6) */
    CHECK(cosmo_thread_start(&t, exiter, NULL, 0) == 0);
    CHECK(cosmo_thread_join(&t, NULL) == 0);
    CHECK(cosmo_thread_id() == (unsigned)getpid());   /* still here */


    STEP("7");
    /* (7) */
    {
        CHECK(signal(SIGUSR1, on_usr1) != SIG_ERR);
        worker_blocked = worker_stop = handler_tid = 0;
        CHECK(cosmo_thread_start(&t, masked, NULL, 0) == 0);
        for (unsigned i = 0; i < 200u && !worker_blocked; i++)
            cosmo_sleep_ns(1000000ull);
        CHECK(worker_blocked == 1);
        CHECK(kill(getpid(), SIGUSR1) == 0);
        for (unsigned i = 0; i < 200u && handler_tid == 0; i++)
            cosmo_sleep_ns(1000000ull);
        CHECK(handler_tid == (unsigned)getpid());   /* the thread that does not block it */
        worker_stop = 1;
        CHECK(cosmo_thread_join(&t, NULL) == 0);
    }


    STEP("8");
    /* (8) The argument checks. */
    {
        struct cosmo_thread req;
        memset(&req, 0, sizeof(req));
        /* A canonical user address that nothing has mapped: the kernel's
         * range check passes and the write of the return slot is what
         * fails, which is the -EFAULT the ordering promises -- and nothing
         * is created, because that write comes before the link. (Not the
         * program's own 4 MiB base, which is mapped: a create with a stack
         * in its own text succeeds and corrupts it, as an earlier version
         * of this test found out.) */
        void *hole = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
        CHECK(hole != MAP_FAILED);
        CHECK(munmap(hole, PAGE) == 0);
        const unsigned long unmapped = ((unsigned long)hole + PAGE) & ~15ul;
        req.entry = (unsigned long)simple;
        req.stack_top = unmapped;
        CHECK(cosmo_thread_create(&req) == -EFAULT);
        req.entry = 0;
        CHECK(cosmo_thread_create(&req) == -EINVAL);
        req.entry = (unsigned long)simple;
        req.stack_top = unmapped + 1;      /* unaligned */
        CHECK(cosmo_thread_create(&req) == -EINVAL);
        req.stack_top = unmapped;
        req.flags = 1;                     /* unknown */
        CHECK(cosmo_thread_create(&req) == -EINVAL);
        req.flags = 0;
        req.clear_tid = 3;                 /* unaligned */
        CHECK(cosmo_thread_create(&req) == -EINVAL);
        CHECK(cosmo_thread_create((const struct cosmo_thread *)16ul) == -EFAULT);

        /*
         * A refused start leaves a *joinable* handle. A caller keeps its
         * handles in one array, checks each start, and then joins -- and
         * if a refused start left the handle as the caller's stack found
         * it, the join would wait on indeterminate memory or fault
         * reading it. So the handle is zeroed before the first thing that
         * can fail, and the poison below is what that promise is worth:
         * without it `join` reads garbage rather than refusing. A stack
         * of half the address space is the refusal, forced rather than
         * waited for.
         */
        cosmo_thread_t poisoned;
        memset(&poisoned, 0xa5, sizeof(poisoned));
        CHECK(cosmo_thread_start(&poisoned, bump, NULL, ~(size_t)0 / 2) < 0);
        CHECK(cosmo_thread_join(&poisoned, NULL) == -EINVAL);
    }


    STEP("9");
    /* (9) */
    /* Three contenders, not two: with two, a mutex that hands the lock over
     * as "held, no waiters" still works, because the only other thread is
     * the one being handed it. Three is what strands a sleeper -- the
     * winner leaves 1 behind, the unlock wakes nobody, and the third
     * thread's join never returns, which is how a review's finding shows
     * up here. */
    counter = 0;
    cosmo_thread_t t3;
    CHECK(cosmo_thread_start(&t, bump, NULL, 0) == 0);
    CHECK(cosmo_thread_start(&t2, bump, NULL, 0) == 0);
    CHECK(cosmo_thread_start(&t3, bump, NULL, 0) == 0);
    CHECK(cosmo_thread_join(&t, NULL) == 0);
    CHECK(cosmo_thread_join(&t2, NULL) == 0);
    CHECK(cosmo_thread_join(&t3, NULL) == 0);
    CHECK(counter == 60000ul);           /* no lost update: the mutex held */
    CHECK(cosmo_mutex_trylock(&mx) == 0);
    CHECK(cosmo_mutex_trylock(&mx) == -EBUSY);
    cosmo_mutex_unlock(&mx);


    STEP("10");
    /* (10) The filter, observed from outside. A denied call does not
     * return an error: it kills the process with SIGSYS, so the exit
     * status is 159 (docs/kernel/security/design.md), which the filtered
     * process cannot report about itself. Two children: one calls a denied
     * thread_create and must die that way, and one calls thread_exit under
     * a filter that denies *everything* and must exit cleanly with its own
     * status, because a thread that cannot exit cannot be stopped. */
    {
        static const char *const argv_deny[] = { "thrtest", "filter-deny", NULL };
        static const char *const argv_exit[] = { "thrtest", "filter-exit", NULL };
        int st = -1;
        pid_t child = spawnve(SELF_PATH, argv_deny, NULL, NULL, 0);
        CHECK(child > 0);
        CHECK(waitpid(child, &st, 0) == child);
        CHECK(WIFSIGNALED(st) && WTERMSIG(st) == 31);
        st = -1;
        child = spawnve(SELF_PATH, argv_exit, NULL, NULL, 0);
        CHECK(child > 0);
        CHECK(waitpid(child, &st, 0) == child);
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 7);
    }

    STEP("11");
    /* (11) */
    {
        cosmo_thread_t h[3];
        unsigned char started[3] = { 0, 0, 0 };
        heap_bad = heap_done = heap_ready = heap_go = heap_late = 0;
        unsigned made = 0;
        /*
         * 16 KB stacks, not the 64 KB default: these threads print and
         * allocate, they do not recurse, and a test should ask the machine
         * for what it needs.
         *
         * And a create is allowed to fail. A thread's stack is a mapping
         * and a mapping can be refused on a machine under pressure, which
         * CI's aarch64 runner has done twice here and this machine never
         * has. This step's subject is the allocator and stdio under
         * concurrency, not the proposition that a create always succeeds,
         * so it retries a bounded number of times and prints every refusal
         * with the errno the library now reports -- rather than the
         * -ENOMEM it used to flatten every mapping failure into, which is
         * why the two CI failures could not say which call had failed.
         */
        for (unsigned i = 0; i < 3u; i++) {
            int rc = -1;
            for (unsigned attempt = 0; attempt < 20u && rc != 0; attempt++) {
                rc = cosmo_thread_start(&h[i], heap_user, (void *)(unsigned long)(i + 1), 16u * 1024u);
                if (rc != 0) {
                    printf("thrtest: heap thread %u refused rc=%d (attempt %u)\n", i + 1, rc, attempt);
                    fflush(stdout);
                    cosmo_sleep_ns(20000000ull);   /* let the reaper catch up */
                }
            }
            CHECK(rc == 0);
            if (rc == 0) {
                started[i] = 1;
                made++;
            }
        }
        /*
         * Every worker that started is now at the barrier, or on its way
         * to it; wait for all of them to arrive before releasing it, so
         * the work below really does run with `made` threads inside the
         * allocator at once -- the property this step exists to test, and
         * the one a sequential retry with a sleep in it would otherwise
         * quietly drop. The wait is bounded and the release unconditional:
         * a worker must never be left parked, however the creates went.
         */
        for (unsigned w = 0; w < 2000u && __atomic_load_n(&heap_ready, __ATOMIC_ACQUIRE) < made; w++)
            cosmo_yield();
        CHECK(heap_ready == made);            /* all of them, together */
        __atomic_store_n(&heap_go, 1, __ATOMIC_RELEASE);
        /* Join exactly the slots that started. A handle whose start was
         * refused is safe to join now, but says nothing -- and joining it
         * by a miscounted index would join a *different* slot's thread
         * twice and leave a real one running. */
        for (unsigned i = 0; i < 3u; i++)
            if (started[i])
                CHECK(cosmo_thread_join(&h[i], NULL) == 0);
        CHECK(made == 3);
        CHECK(heap_late == 0);     /* every worker was released, none timed out */
        CHECK(heap_done == made);
        CHECK(heap_bad == 0);      /* no lost block, no shared block, no failed allocation */
        /* fflush(NULL) flushes every stream, and must not deadlock against
         * the lock its caller already holds -- nothing in this test called
         * it until a review pointed out that it self-deadlocked. */
        CHECK(fflush(NULL) == 0);
        CHECK(fflush(stdout) == 0);
    }

    STEP("12");
    /*
     * (12) The bound holds, and the process survives reaching it. This is
     * deliberately the LAST step: it is a resource-exhaustion test -- 256
     * threads, and the memory they hold is returned as the kernel reaps
     * them, not the instant their joins return -- so anything after it is
     * running on a machine that is still recovering. An earlier version
     * put the heap step after this one and saw malloc and thread_create
     * refused for want of memory, which is this step working, not a bug.
     * PROCESS_MAX_THREADS is 256 per process; the stacks are one page each
     * so that 256 of them cost little.
     */
    {
        static cosmo_thread_t many[300];
        unsigned made = 0;
        int rc = 0;
        spin_stop = 0;   /* these threads must stay alive to fill the table:
                            step 5 uses the same flag and leaves it set */
        while (made < 300) {
            rc = cosmo_thread_start(&many[made], spinner, NULL, PAGE);
            if (rc != 0)
                break;
            made++;
        }
        CHECK(rc == -EAGAIN);          /* the bound, not some other failure */
        CHECK(made > 1 && made < 300); /* it is a bound, and it is not one */
        spin_stop = 1;
        for (unsigned i = 0; i < made; i++)
            CHECK(cosmo_thread_join(&many[i], NULL) == 0);
        spin_stop = 0;
        /*
         * And every slot a join released is usable at once. Three, not
         * one: a join that woke before the kernel stopped counting its
         * thread left the next create refused -EAGAIN, and one retry was
         * enough to hide it. This is the assertion for "when your join
         * returns, the slot is free".
         */
        cosmo_thread_t again[3];
        for (unsigned i = 0; i < 3u; i++) {
            int rc2 = cosmo_thread_start(&again[i], quick, NULL, PAGE);
            if (rc2 != 0)
                printf("thrtest: reuse %u refused rc=%d\n", i, rc2);
            CHECK(rc2 == 0);
        }
        for (unsigned i = 0; i < 3u; i++)
            CHECK(cosmo_thread_join(&again[i], NULL) == 0);
    }

    if (failures == 0)
        printf("THREADTEST: PASS\n");
    else
        printf("THREADTEST: FAIL %d\n", failures);
    return failures ? 1 : 0;
}
