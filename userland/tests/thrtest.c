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

/* (11) A thread that waits to be told to stop, for the bound. */
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
    /* (1) */
    CHECK(cosmo_thread_id() == (unsigned)getpid());   /* a first thread's id is the pid */
    CHECK(cosmo_thread_start(&t, simple, (void *)7ul, 0) == 0);
    CHECK(t.tid != 0 && t.tid != (unsigned)getpid());
    CHECK(cosmo_thread_join(&t, &ret) == 0);
    CHECK(ran == 7);
    CHECK(ret == (void *)0x5eed);


    /* (2) */
    CHECK(cosmo_thread_start(&t, (void *(*)(void *))entry_probe, (void *)0x1234ul, 0) == 0);
    CHECK(cosmo_thread_join(&t, NULL) == 0);
    CHECK(entry_arg_ok);
#if defined(__x86_64__)
    CHECK(entry_sp % 16 == 8);
#else
    CHECK(entry_sp % 16 == 0);
#endif


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
    }


    /* (5) */
    CHECK(cosmo_thread_start(&t, quick, NULL, 0) == 0);
    CHECK(t.done == t.tid);            /* the kernel wrote it before the thread could run */
    cosmo_sleep_ns(50000000ull);       /* let it finish first: the join must still work */
    CHECK(t.done == 0);
    CHECK(cosmo_thread_join(&t, NULL) == 0);


    /* (6) */
    CHECK(cosmo_thread_start(&t, exiter, NULL, 0) == 0);
    CHECK(cosmo_thread_join(&t, NULL) == 0);
    CHECK(cosmo_thread_id() == (unsigned)getpid());   /* still here */


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
    }


    /* (9) */
    counter = 0;
    CHECK(cosmo_thread_start(&t, bump, NULL, 0) == 0);
    CHECK(cosmo_thread_start(&t2, bump, NULL, 0) == 0);
    CHECK(cosmo_thread_join(&t, NULL) == 0);
    CHECK(cosmo_thread_join(&t2, NULL) == 0);
    CHECK(counter == 40000ul);           /* no lost update: the mutex held */
    CHECK(cosmo_mutex_trylock(&mx) == 0);
    CHECK(cosmo_mutex_trylock(&mx) == -EBUSY);
    cosmo_mutex_unlock(&mx);


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

    /* (11) The bound holds, and the process survives reaching it.
     * PROCESS_MAX_THREADS is 256 per process; the stacks are one page each
     * so that 256 of them cost little. */
    {
        static cosmo_thread_t many[300];
        unsigned made = 0;
        int rc = 0;
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
        /* and the process is healthy: the next create works */
        CHECK(cosmo_thread_start(&t, quick, NULL, 0) == 0);
        CHECK(cosmo_thread_join(&t, NULL) == 0);
    }

    if (failures == 0)
        printf("THREADTEST: PASS\n");
    else
        printf("THREADTEST: FAIL %d\n", failures);
    return failures ? 1 : 0;
}
