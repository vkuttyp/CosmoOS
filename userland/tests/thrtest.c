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
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cosmo/auxv.h>
#include <cosmo/syscall.h>
#include <cosmo/tcb.h>
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

/*
 * (3) Two threads make progress against each other.
 *
 * This waited with **a loop count standing in for a duration** -- two
 * million iterations, yielding every four thousand -- which is the
 * substitution this tree has been bitten by twice and which
 * docs/audit/next-subsystem-condvar.md cited as a reason to build the
 * primitive. A count measures the host's speed, not the property.
 *
 * Both sides now wait on one condition variable against a real deadline,
 * so a side that never runs makes this step **fail with a message**
 * rather than spin for two million iterations or hang for the boot
 * deadline. Five seconds is absurdly generous for two threads setting a
 * flag, which is what a budget should be: large enough that a loaded host
 * never reaches it, finite so a broken one says so.
 *
 * `broadcast` rather than `signal`, because the two parties wait on the
 * same variable for *different* predicates -- `signal` may wake exactly
 * the one that has nothing to look at.
 */
#define PAIR_BUDGET_NS (5ull * 1000ull * 1000ull * 1000ull)
static cosmo_mutex_t pair_m = COSMO_MUTEX_INIT;
static cosmo_cond_t pair_c = COSMO_COND_INIT;
static volatile unsigned flag_a, flag_b;

/* Wait for `*flag` under `pair_m`, which the caller must already hold. */
static void pair_wait(volatile unsigned *flag)
{
    uint64_t deadline = cosmo_clock_ns() + PAIR_BUDGET_NS;
    while (!*flag) {
        uint64_t now = cosmo_clock_ns();
        if (now >= deadline)
            break;
        /* Relative, so recomputed each pass from the deadline: passing the
         * budget itself each time would wait five seconds per iteration
         * (cosmo/thread.h). */
        cosmo_cond_timedwait(&pair_c, &pair_m, deadline - now);
    }
}

static void *pair_a(void *arg)
{
    (void)arg;
    cosmo_mutex_lock(&pair_m);
    flag_a = 1;
    cosmo_cond_broadcast(&pair_c);
    pair_wait(&flag_b);
    unsigned saw = flag_b;
    cosmo_mutex_unlock(&pair_m);
    return (void *)(unsigned long)saw;
}

/* (11) The allocator and stdio, from several threads at once: what an
 * unlocked free list corrupts silently and an unlocked stream garbles.
 * Each thread allocates, writes a pattern, frees, and prints -- and checks
 * its own blocks, so a lost or shared block shows up as a wrong byte
 * rather than only as a crash. */
/*
 * The heap step's constants, named because the barrier's bound is derived
 * from the retry budget rather than guessed. A worker starts waiting the
 * moment it is created, and main may still spend its whole retry budget on
 * the workers after it: HEAP_WORKERS - 1 creates, each up to
 * HEAP_RETRY_ATTEMPTS attempts with HEAP_RETRY_SLEEP_NS between them. A
 * bound that did not cover that would report the first worker late during
 * exactly the refusal the retry exists to tolerate -- turning a tolerated
 * create failure into a test failure. Hence the budget plus two seconds.
 */
#define HEAP_WORKERS        3u
#define HEAP_RETRY_ATTEMPTS 20u
#define HEAP_RETRY_SLEEP_NS 20000000ull
#define HEAP_BARRIER_NS \
    ((unsigned long long)(HEAP_WORKERS - 1u) * HEAP_RETRY_ATTEMPTS * HEAP_RETRY_SLEEP_NS \
     + 2000000000ull)

/*
 * Step 14's pair. Each provokes a *different* failure in a loop and reads
 * its own errno back every iteration. One shared errno loses this within a
 * few iterations -- whichever thread wrote last wins -- so the assertion is
 * not "errno is right once" but "errno is right every time, while another
 * thread is writing a different value into its own".
 */
static volatile unsigned errno_bad[2], errno_done;
static void *errno_ebadf(void *arg)
{
    (void)arg;
    for (unsigned i = 0; i < 2000u; i++) {
        errno = 0;
        if (close(-1) != -1 || errno != EBADF)
            errno_bad[0]++;
    }
    __atomic_fetch_add(&errno_done, 1, __ATOMIC_ACQ_REL);
    return NULL;
}
static void *errno_erange(void *arg)
{
    (void)arg;
    for (unsigned i = 0; i < 2000u; i++) {
        errno = 0;
        /* A different code from the other thread's, from a different call:
         * strtoll's overflow is ERANGE and touches no kernel at all, so the
         * two threads are not merely racing inside one syscall path. */
        (void)strtoll("99999999999999999999", NULL, 10);
        if (errno != ERANGE)
            errno_bad[1]++;
    }
    __atomic_fetch_add(&errno_done, 1, __ATOMIC_ACQ_REL);
    return NULL;
}

/*
 * Step 19's thread-local storage. `tls_init` has an initialiser, so it
 * lives in `.tdata` and every thread must see 0x5eed -- which is what
 * distinguishes a *copied* template from one shared image. `tls_zero` is
 * large and uninitialised, so it lives in `.tbss` and must be zero in a
 * thread even after an earlier one filled it. `tls_aligned` asks for more
 * alignment than the thread pointer's own, which is the case that catches
 * an implementation that rounds the image's address away.
 *
 * These three are also the whole proof that the offset formula is right:
 * the linker resolved their addresses relative to the thread pointer, and
 * reading back an initialiser this library placed is the only way to know
 * that libc and the linker agree. No amount of reading the ABI documents
 * establishes that.
 */
static __thread int tls_init = 0x5eed;
static __thread char tls_zero[512];
static __thread _Alignas(64) long tls_aligned;
/*
 * An alignment larger than the block's own offset, which is the case that
 * made the storage size disagree with the placement: the thread pointer and
 * the image are aligned independently, so each rounding costs up to an
 * alignment. Having a program in the suite with one means the worst case is
 * exercised on every boot rather than reasoned about.
 */
static __thread _Alignas(256) long tls_overaligned;

static volatile unsigned tls_bad, tls_done;
static void *tlsvar_user(void *arg)
{
    unsigned id = (unsigned)(unsigned long)arg;
    if (tls_init != 0x5eed)
        tls_bad++;                       /* .tdata was not copied for this thread */
    for (unsigned i = 0; i < sizeof(tls_zero); i++)
        if (tls_zero[i] != 0)
            tls_bad++;                   /* .tbss was not zeroed for this thread */
    if (((unsigned long)&tls_aligned % 64u) != 0)
        tls_bad++;                       /* the image ignored its own alignment */
    if (((unsigned long)&tls_overaligned % 256u) != 0)
        tls_bad++;                       /* ...including one past the block's offset */
    /* Now make this thread's copy distinctive, and check it stays so while
     * the others do the same: a shared image loses this immediately. */
    tls_init = (int)id;
    memset(tls_zero, (int)(id & 0xff), sizeof(tls_zero));
    tls_aligned = (long)id;
    for (unsigned i = 0; i < 200u; i++) {
        cosmo_yield();
        if (tls_init != (int)id || tls_aligned != (long)id)
            tls_bad++;
        if (tls_zero[0] != (char)(id & 0xff) || tls_zero[sizeof(tls_zero) - 1] != (char)(id & 0xff))
            tls_bad++;
    }
    /*
     * `strerror`'s buffer, from a thread: the message for an unknown code
     * is built in per-thread storage now, so two threads asking about
     * different codes must each read their own. A shared buffer gives
     * whichever wrote last, to both.
     */
    for (unsigned i = 0; i < 100u; i++) {
        char want[32];
        snprintf(want, sizeof(want), "Unknown error %u", 9000u + id);
        if (strcmp(strerror((int)(9000u + id)), want) != 0)
            tls_bad++;
        cosmo_yield();
    }
    __atomic_fetch_add(&tls_done, 1, __ATOMIC_ACQ_REL);
    return NULL;
}

/* Step 17: reports what the cache says its own id is, which the creator
 * compares against the tid `thread_create` gave it. */
static void *id_reporter(void *arg)
{
    (void)arg;
    return (void *)(unsigned long)cosmo_thread_id();
}

/*
 * Step 17's other half: **the block is not in the thread's stack.** A
 * thread can find its own block without any new interface -- `&errno` is a
 * field of it -- and compare it against a local, which is on the stack by
 * definition. The block is mapped in the page *above* the stack, so it must
 * be above the deepest thing the stack holds.
 *
 * This assertion exists because the obvious proof does not work: putting
 * the block inside the stack corrupts `err` silently on AArch64, where the
 * `self` word is unused, and every assertion that sets `errno` and reads it
 * straight back still passes. The layout has to be checked as a layout.
 */
static volatile unsigned layout_ok, layout_ran, layout_tp_ok, layout_head_ok;
static volatile unsigned long layout_blk;
static void *layout_probe(void *arg)
{
    char local;
    (void)arg;
    const char *blk = (const char *)&errno - offsetof(struct __cosmo_tcb, err);
    layout_ok = (unsigned)(blk > &local);
    layout_blk = (unsigned long)blk;

    /*
     * The thread pointer sits at `COSMO_TCB_TP_OFFSET` into the storage,
     * which is the block itself on x86-64 and 128 bytes above it on
     * AArch64 -- because variant I reserves 16 bytes at the thread pointer
     * and puts `__thread` variables above them.
     *
     * Asserted because it is silent on the architecture that needs no
     * change: x86-64's thread pointer *is* the block and always was, so
     * an AArch64 mistake here passes every x86 test. This is the check
     * that differs between them.
     */
#if defined(__aarch64__)
    char *tp = (char *)__builtin_thread_pointer();
    layout_tp_ok = (unsigned)(tp - blk == (long)COSMO_TCB_TP_OFFSET);
    /*
     * And the ABI's 16 reserved bytes at the thread pointer are real
     * memory that is **not** the block: writing them must leave `errno`,
     * the cached tid and the `self` word alone. Under the old layout the
     * thread pointer was the block, so these sixteen bytes were `self`,
     * `err` and `tid` -- this is that collision, in miniature, before any
     * TLS image exists to cause it in earnest.
     */
    struct __cosmo_tcb *b = (struct __cosmo_tcb *)(tp - COSMO_TCB_TP_OFFSET);
    unsigned want_tid = cosmo_thread_id();
    errno = ERANGE;
    for (unsigned i = 0; i < 16u; i++)
        ((volatile char *)tp)[i] = (char)0xA5;
    layout_head_ok = (unsigned)(errno == ERANGE && b->self == b && b->tid == want_tid);
#else
    layout_tp_ok = (unsigned)(COSMO_TCB_TP_OFFSET == 0u);   /* the block is the thread pointer */
    layout_head_ok = 1u;                                    /* no reserved head to write */
#endif
    layout_ran = 1;
    return NULL;
}

/* --- steps 18 to 22: the condition variable ------------------------------
 *
 * Shared between the steps because the shapes repeat: a mutex, a
 * condition variable, a predicate, and a count of waiters that got
 * through. Every waiter here loops on its predicate, which is the
 * contract `cosmo/thread.h` states -- a waiter written with `if` would
 * pass most of these on an unloaded machine, which is why the contract is
 * a documented invariant and not a suggestion.
 */
/*
 * libc's test seam, declared here because **no public header offers it**
 * -- it is libc's own (`libc/src/libc.h`), and a program has no business
 * with it. This test is the exception that the seam exists for, and
 * reaching for the symbol by hand is the honest way to say so.
 */
extern void (*__cosmo_cond_probe)(void);

static cosmo_mutex_t cv_m = COSMO_MUTEX_INIT;
static cosmo_cond_t  cv_c = COSMO_COND_INIT;
/*
 * A second variable, so that a test waiting for its waiters to *arrive*
 * waits with the primitive too rather than spinning on a yield. Every
 * `cosmo_yield()` poll in these steps was one, and a spin inside the
 * condition variable's own tests would be the clearest possible statement
 * that the author did not believe in it.
 */
static cosmo_cond_t  cv_enter_c = COSMO_COND_INIT;
static volatile unsigned cv_ready;      /* the predicate */
static volatile unsigned cv_woke;       /* waiters that returned with it true */
static volatile unsigned cv_held_ok;    /* waiters that found the mutex re-taken */
static volatile unsigned cv_entered;    /* waiters that have reached the wait */

/* Wait, under `cv_m`, for `cv_entered` to reach `n`. */
static void cv_await_entered(unsigned n)
{
    while (cv_entered < n)
        cosmo_cond_wait(&cv_enter_c, &cv_m);
}

static void *cv_waiter(void *arg)
{
    (void)arg;
    cosmo_mutex_lock(&cv_m);
    cv_entered++;
    cosmo_cond_broadcast(&cv_enter_c);   /* whoever is waiting for us to arrive */
    while (!cv_ready)
        cosmo_cond_wait(&cv_c, &cv_m);
    /*
     * The predicate is true -- but the signaller made it true before this
     * returned, so the predicate alone cannot tell a correct wait from one
     * that never re-acquired the mutex. `trylock` can: the mutex is not
     * recursive, so a thread that holds it is refused, and a thread that
     * does not holds it after this call. -EBUSY is the assertion.
     */
    if (cosmo_mutex_trylock(&cv_m) == -EBUSY)
        cv_held_ok++;
    cv_woke++;
    cosmo_mutex_unlock(&cv_m);
    return NULL;
}

/*
 * Step 20's second half. `cv_tickets` is *work*, not a flag: a waiter that
 * wakes takes one if there is one and otherwise goes back to waiting.
 *
 * This shape exists because the obvious assertion is wrong. A first draft
 * of step 20 checked that `cosmo_cond_signal` wakes **exactly one**
 * waiter -- and the interface does not promise that. Spurious wakeups are
 * permitted (cosmo/thread.h), step 22 exists because callers must
 * tolerate them, and waking more waiters than necessary is a cost rather
 * than a defect. The number of threads a signal makes runnable is also
 * not observable from here without a race. What *is* guaranteed, and what
 * a caller depends on, is that **no more work is taken than was made
 * available** however many threads wake -- so that is what is asserted.
 */
static volatile unsigned cv_tickets, cv_taken;
static void *cv_ticket_waiter(void *arg)
{
    (void)arg;
    cosmo_mutex_lock(&cv_m);
    cv_entered++;
    cosmo_cond_broadcast(&cv_enter_c);
    while (cv_tickets == 0)
        cosmo_cond_wait(&cv_c, &cv_m);
    cv_tickets--;
    cv_taken++;
    cosmo_cond_broadcast(&cv_enter_c);   /* the counter the test waits on */
    cosmo_mutex_unlock(&cv_m);
    return NULL;
}

/*
 * Step 19's probe: the signal, performed *inside* `cosmo_cond_wait`'s
 * window, on the waiting thread itself. `__cosmo_cond_probe` is called
 * after the wait has released the mutex and before it sleeps, so taking
 * the mutex here cannot deadlock -- and it means the signal lands in the
 * window by construction rather than by asking the scheduler nicely.
 *
 * Two earlier designs of this test tried to do it with a second thread
 * and could not: unlocking the mutex makes a blocked signaller runnable,
 * not running, so the waiter reaches `futex_wait` first and a
 * read-after-unlock implementation passes.
 */
static volatile unsigned cv_probe_ran;
static void cv_probe(void)
{
    cosmo_mutex_lock(&cv_m);
    cv_ready = 1;
    cosmo_cond_signal(&cv_c);
    cosmo_mutex_unlock(&cv_m);
    __atomic_fetch_add(&cv_probe_ran, 1, __ATOMIC_ACQ_REL);
}

/*
 * Step 21's second half: a waiter that uses `cosmo_cond_timedwait` and
 * reports what it returned and how long it took, so that a successful
 * timed wait is actually exercised rather than described.
 */
#define CV_TIMED_BUDGET_NS (1000ull * 1000ull * 1000ull)
static volatile int cv_timed_rc;
static volatile unsigned long long cv_timed_ns;
static void *cv_timed_waiter(void *arg)
{
    (void)arg;
    cosmo_mutex_lock(&cv_m);
    cv_entered++;
    cosmo_cond_broadcast(&cv_enter_c);
    uint64_t t0 = cosmo_clock_ns();
    uint64_t deadline = t0 + CV_TIMED_BUDGET_NS;
    int rc = 0;
    while (!cv_ready) {
        uint64_t now = cosmo_clock_ns();
        if (now >= deadline) { rc = -ETIMEDOUT; break; }
        rc = cosmo_cond_timedwait(&cv_c, &cv_m, deadline - now);
        if (rc == -ETIMEDOUT)
            break;
    }
    cv_timed_ns = cosmo_clock_since_ns(t0);
    cv_timed_rc = rc;
    if (cosmo_mutex_trylock(&cv_m) == -EBUSY)
        cv_held_ok++;
    cv_woke++;
    cosmo_mutex_unlock(&cv_m);
    return NULL;
}

/* Step 22: a waiter whose predicate never becomes true, so that
 * broadcasts at it prove only that a correct caller survives them. */
static volatile unsigned cv_spur_stop, cv_spur_returns;
static void *cv_spurious_waiter(void *arg)
{
    (void)arg;
    cosmo_mutex_lock(&cv_m);
    cv_entered++;
    cosmo_cond_broadcast(&cv_enter_c);
    while (!cv_spur_stop) {
        cosmo_cond_wait(&cv_c, &cv_m);
        cv_spur_returns++;
        cosmo_cond_broadcast(&cv_enter_c);   /* the counter the test watches */
    }
    cosmo_mutex_unlock(&cv_m);
    return NULL;
}

/* Step 15: a thread that sets its errno and exits, so main can show that
 * its own survived. */
static void *errno_setter(void *arg)
{
    (void)arg;
    errno = 0;
    (void)close(-1);
    return (void *)(unsigned long)(unsigned)errno;
}

/*
 * Step 16: the path a program outside libc's wrapper must take. This thread
 * is made by a raw SYS_thread_create with tls = 0, so it has no block and
 * must not touch libc until it installs one -- which is the contract
 * cosmo/tcb.h states. It installs one from storage of its own and only then
 * uses errno.
 */
/* A page: enough for this program's block, the ABI head and its own TLS
 * image, whatever `cosmo_tcb_storage()` turns out to be. A program outside
 * libc's wrapper has to ask rather than assume, because the size follows
 * the program's own `__thread` variables. */
static __attribute__((aligned(16))) char raw_blk[4096];
static volatile int raw_rc[4];
static volatile unsigned raw_err, raw_done;
static void raw_entry(void *arg)
{
    (void)arg;
    /* Too short, and misaligned: refused before anything is installed, and
     * both refusals happen while this thread still has no block -- which is
     * why cosmo_tcb_install must not itself touch errno. */
    raw_rc[0] = cosmo_tcb_install(raw_blk, cosmo_tcb_storage() - 1u);
    raw_rc[1] = cosmo_tcb_install(raw_blk + 1, cosmo_tcb_storage());
    /*
     * Exactly the documented size, at exactly the documented alignment and
     * no more. This is the case that costs the most, because the thread
     * pointer is then rounded up to the template's alignment inside the
     * storage *and* the image is rounded up again above it -- and a caller
     * that asked `cosmo_tcb_storage()` and allocated the answer has
     * followed the contract, so a refusal here is libc's bug, not the
     * caller's. The size formula charged for one of those two roundings
     * until a review noticed; the reason `raw_rc[1]` did not catch it is
     * that it asks for the exact size at a *misaligned* address, so it is
     * refused for the alignment before the size is ever weighed. This
     * program's 256-byte-aligned thread-local is what makes the two
     * roundings cost more than the block's own offset.
     */
    raw_rc[3] = cosmo_tcb_install(raw_blk, cosmo_tcb_storage());
    raw_rc[2] = cosmo_tcb_install(raw_blk, sizeof(raw_blk));
    if (raw_rc[2] == 0 && tls_init != 0x5eed)
        raw_rc[2] = -1;   /* installed, but its TLS image was not placed */
    if (raw_rc[2] == 0) {
        errno = 0;
        (void)close(-1);
        raw_err = (unsigned)errno;
        raw_done = cosmo_thread_id();   /* the cache install wrote */
    }
    cosmo_thread_exit(0);
}

/*
 * Step 13's worker. It clobbers its *own* thread pointer, which is why the
 * successful sets happen here and never on the main thread: from the unit's
 * later steps the main thread's pointer is libc's own block, and a test
 * that took it away would break `errno` for everything after it. Nothing
 * this worker does after the first set touches libc -- it returns, and the
 * trampoline's store and `thread_exit` are an atomic and a raw syscall.
 */
static volatile long tls_rc[3];
static void *tls_user(void *arg)
{
    static __attribute__((aligned(16))) char a[128], b[128];
    (void)arg;
    tls_rc[0] = cosmo_set_tls((unsigned long long)(unsigned long)a);
    tls_rc[1] = cosmo_set_tls((unsigned long long)(unsigned long)b);   /* re-set wins */
    tls_rc[2] = cosmo_set_tls(0);                                      /* and zero is legal */
    return NULL;
}

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
    uint64_t deadline = cosmo_clock_ns() + HEAP_BARRIER_NS;
    while (!__atomic_load_n(&heap_go, __ATOMIC_ACQUIRE)) {
        uint64_t now = cosmo_clock_ns();
        if (now >= deadline)
            break;
        /* Sleep on the word rather than spin on a yield count: a count is
         * not a duration, and main may be sleeping between refused
         * attempts while this worker burns through it. */
        cosmo_futex_wait(&heap_go, 0, deadline - now);
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
/*
 * This thread's wait was `while (!worker_stop) cosmo_yield();` -- an
 * **unbounded** spin for the whole life of the step. The terminal-modes
 * unit established why that is worse than inelegant: on a single-CPU boot
 * the thing being waited for needs the CPU the polling loop is spinning
 * on, so the wait is paid for out of the progress it is waiting for.
 *
 * It sleeps now. Untimed, because the only thing that ends this step is
 * main setting the flag, and if main never did, the *join* below would
 * wait forever either way -- a deadline here would report a failure whose
 * cause is elsewhere.
 */
static cosmo_mutex_t worker_m = COSMO_MUTEX_INIT;
static cosmo_cond_t worker_c = COSMO_COND_INIT;
static volatile unsigned worker_blocked, worker_stop;
static void *masked(void *arg)
{
    (void)arg;
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    sigprocmask(SIG_BLOCK, &set, NULL);   /* the mask is this thread's */
    cosmo_mutex_lock(&worker_m);
    worker_blocked = 1;
    while (!worker_stop)
        cosmo_cond_wait(&worker_c, &worker_m);
    cosmo_mutex_unlock(&worker_m);
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


/* ---- the environment and the atexit list, under threads --------------- *
 *
 * docs/audit/next-subsystem-libc-shared-tables.md. These two tables were
 * the ones the threads unit did not lock, while it locked the allocator
 * and stdio and made errno per-thread.
 */

#define ENV_READERS 3
#define ENV_ROUNDS  400

static volatile int env_stop;
static volatile unsigned env_misses;

/* A reader that must always find a name nobody ever removes. */
static void *env_reader(void *arg)
{
    (void)arg;
    while (!env_stop) {
        const char *v = getenv("STABLE");
        if (v == NULL || strcmp(v, "yes") != 0)
            __atomic_fetch_add(&env_misses, 1, __ATOMIC_RELAXED);
    }
    return NULL;
}

/*
 * (A) setenv grows the array while readers walk it. Before the lock
 * this freed `environ` under them -- a use-after-free in the
 * allocator. This one is PROBABILISTIC by nature: the only thing that
 * distinguishes it is a read of freed memory, so the window is made
 * wide (a long environment, many growths) rather than iterated and
 * hoped for. The report says so rather than calling it a proof.
 */
static void env_grow_under_readers(void)
{
    cosmo_thread_t r[ENV_READERS];
    char name[32];

    CHECK(setenv("STABLE", "yes", 1) == 0);
    for (unsigned i = 0; i < 60; i++) {      /* a long array to walk */
        snprintf(name, sizeof(name), "PAD%u", i);
        CHECK(setenv(name, "x", 1) == 0);
    }
    env_stop = 0;
    env_misses = 0;
    for (unsigned i = 0; i < ENV_READERS; i++)
        CHECK(cosmo_thread_start(&r[i], env_reader, NULL, 0) == 0);
    for (unsigned i = 0; i < ENV_ROUNDS; i++) {
        snprintf(name, sizeof(name), "GROW%u", i);
        CHECK(setenv(name, "v", 1) == 0);    /* each one reallocates */
    }
    env_stop = 1;
    for (unsigned i = 0; i < ENV_READERS; i++)
        CHECK(cosmo_thread_join(&r[i], NULL) == 0);
    CHECK(env_misses == 0);
    printf("thrtest: env-grow-under-readers: %u readers over %u growths, %u misses\n",
           (unsigned)ENV_READERS, (unsigned)ENV_ROUNDS, env_misses);
}

/*
 * (B) unsetenv shifts the array while readers are inside `getenv`.
 *
 * This is PROBABILISTIC, and an earlier design of it claimed to be
 * deterministic by having the test walk `environ` itself and pause
 * mid-array. That does not work, and the reason is worth keeping: a
 * walker the test owns never takes the library's lock, so the lock
 * the fix adds cannot protect it -- the test fails identically with
 * and without the fix, which makes it a test of nothing. A
 * deterministic version needs to pause INSIDE `getenv`, which needs a
 * test seam libc does not have.
 *
 * So the reader here is the real `getenv`, and the window is widened
 * rather than forced: a long environment to walk, and many removals.
 */
static void *env_unset_reader(void *arg)
{
    (void)arg;
    while (!env_stop) {
        const char *v = getenv("STABLE");
        if (v == NULL || strcmp(v, "yes") != 0)
            __atomic_fetch_add(&env_misses, 1, __ATOMIC_RELAXED);
    }
    return NULL;
}

static void env_unset_under_readers(void)
{
    cosmo_thread_t r[ENV_READERS];
    char name[32];

    CHECK(setenv("STABLE", "yes", 1) == 0);
    for (unsigned i = 0; i < 200; i++) {
        snprintf(name, sizeof(name), "DEL%u", i);
        CHECK(setenv(name, "x", 1) == 0);
    }
    env_stop = 0;
    env_misses = 0;
    for (unsigned i = 0; i < ENV_READERS; i++)
        CHECK(cosmo_thread_start(&r[i], env_unset_reader, NULL, 0) == 0);
    for (unsigned i = 0; i < 200; i++) {
        snprintf(name, sizeof(name), "DEL%u", i);
        CHECK(unsetenv(name) == 0);      /* each one shifts the tail */
    }
    env_stop = 1;
    for (unsigned i = 0; i < ENV_READERS; i++)
        CHECK(cosmo_thread_join(&r[i], NULL) == 0);
    CHECK(env_misses == 0);
    printf("thrtest: env-unset-under-readers: %u readers over 200 removals, %u misses\n",
           (unsigned)ENV_READERS, env_misses);
}

/*
 * (C) and (D) atexit. Countable: register N from N threads and count
 * how many run. A lost update is a number, not a timing.
 */
#define AT_THREADS 8
static volatile unsigned at_ran;
static void at_handler(void) { __atomic_fetch_add(&at_ran, 1, __ATOMIC_RELAXED); }

static volatile int at_go;

/*
 * The barrier is the point. Without it the threads are started in a
 * loop and each finishes before the next exists, so they never
 * contend and an unlocked `atexit` passes the test -- which is what
 * the first build measured.
 */
static void *at_registrar(void *arg)
{
    unsigned *ok = arg;
    while (!at_go)
        cosmo_yield();
    *ok = (atexit(at_handler) == 0) ? 1u : 0u;
    return NULL;
}

static unsigned at_registered;

static void atexit_concurrent(void)
{
    cosmo_thread_t t[AT_THREADS];
    unsigned ok[AT_THREADS] = { 0 };

    at_go = 0;
    for (unsigned i = 0; i < AT_THREADS; i++)
        CHECK(cosmo_thread_start(&t[i], at_registrar, &ok[i], 0) == 0);
    at_go = 1;                       /* all eight enter atexit together */
    for (unsigned i = 0; i < AT_THREADS; i++)
        CHECK(cosmo_thread_join(&t[i], NULL) == 0);
    for (unsigned i = 0; i < AT_THREADS; i++)
        at_registered += ok[i];
    /* Every one that said it registered must have been kept: the count
     * the drain runs is checked at exit, below. */
    CHECK(at_registered == AT_THREADS);
    printf("thrtest: atexit-concurrent: %u of %u registrations accepted\n",
           at_registered, (unsigned)AT_THREADS);
}

/* (E) A handler that calls back into the library must not deadlock --
 * the case exit's take/pop/release/call shape exists for. */
static volatile int reentrant_ran;
static void reentrant_handler(void)
{
    (void)getenv("STABLE");     /* takes the same lock exit was holding */
    reentrant_ran = 1;
    __atomic_fetch_add(&at_ran, 1, __ATOMIC_RELAXED);   /* counted with the rest */
}

/*
 * Registered FIRST so the LIFO drain runs it LAST, and it prints the
 * program's verdict -- so a drain that loses a handler or deadlocks
 * fails the run. Printing `THREADTEST: PASS` from `main` instead would
 * have published the verdict before the thing under test had run: the
 * first build did exactly that, and the drain's own failure could not
 * reach the marker.
 */
static void atexit_checks_at_exit(void)
{
    if (at_ran != at_registered) {
        printf("thrtest: FAIL atexit drain ran %u of %u\n", at_ran, at_registered);
        failures++;
    } else if (!reentrant_ran) {
        printf("thrtest: FAIL the re-entrant handler did not run\n");
        failures++;
    } else {
        printf("thrtest: atexit-drain: %u handlers ran, and the re-entrant one returned\n", at_ran);
    }
    if (failures == 0)
        printf("THREADTEST: PASS\n");
    else
        printf("THREADTEST: FAIL %d\n", failures);
    fflush(stdout);
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
    cosmo_mutex_lock(&pair_m);
    pair_wait(&flag_a);
    CHECK(flag_a == 1);
    flag_b = 1;
    cosmo_cond_broadcast(&pair_c);
    cosmo_mutex_unlock(&pair_m);
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
        cosmo_mutex_lock(&worker_m);
        worker_stop = 1;
        cosmo_cond_broadcast(&worker_c);
        cosmo_mutex_unlock(&worker_m);
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
        cosmo_thread_t h[HEAP_WORKERS];
        unsigned char started[HEAP_WORKERS] = { 0, 0, 0 };
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
        for (unsigned i = 0; i < HEAP_WORKERS; i++) {
            int rc = -1;
            for (unsigned attempt = 0; attempt < HEAP_RETRY_ATTEMPTS && rc != 0; attempt++) {
                rc = cosmo_thread_start(&h[i], heap_user, (void *)(unsigned long)(i + 1), 16u * 1024u);
                if (rc != 0) {
                    printf("thrtest: heap thread %u refused rc=%d (attempt %u)\n", i + 1, rc, attempt);
                    fflush(stdout);
                    cosmo_sleep_ns(HEAP_RETRY_SLEEP_NS);   /* let the reaper catch up */
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
        uint64_t ready_by = cosmo_clock_ns() + HEAP_BARRIER_NS;
        while (__atomic_load_n(&heap_ready, __ATOMIC_ACQUIRE) < made && cosmo_clock_ns() < ready_by)
            cosmo_yield();
        CHECK(heap_ready == made);            /* all of them, together */
        __atomic_store_n(&heap_go, 1, __ATOMIC_RELEASE);
        cosmo_futex_wake(&heap_go, HEAP_WORKERS);
        /* Join exactly the slots that started. A handle whose start was
         * refused is safe to join now, but says nothing -- and joining it
         * by a miscounted index would join a *different* slot's thread
         * twice and leave a real one running. */
        for (unsigned i = 0; i < HEAP_WORKERS; i++)
            if (started[i])
                CHECK(cosmo_thread_join(&h[i], NULL) == 0);
        CHECK(made == HEAP_WORKERS);
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
     * (13) `SYS_set_tls`, the syscall the per-thread `errno` is built on.
     * What it promises is checked here; that it *took effect* is checked by
     * `errno` itself in the steps that follow, because there is no
     * architecture-independent way to read a thread pointer back -- x86-64
     * cannot read the FS base without `rdfsbase`, which is why libc's block
     * points to itself at all.
     *
     * A refused call leaves the pointer alone, so the refusals are safe on
     * any thread, including this one. The successful ones are not, and run
     * on a worker.
     */
    {
        static __attribute__((aligned(16))) char blk[128];
        CHECK(cosmo_set_tls((unsigned long long)(unsigned long)blk + 8u) == -EINVAL);  /* 16-byte aligned */
        CHECK(cosmo_set_tls(0x400000ull - 16u) == -EFAULT);          /* below the user range */
        CHECK(cosmo_set_tls(0x7FFFFFFFF000ull) == -EFAULT);          /* and at the top, which is past it */
        /*
         * A base whose eight bytes straddle the top of the range is not
         * checked here because the alignment makes it unreachable: the
         * range ends 16-byte aligned, so the last 16-byte-aligned address
         * below it has its whole word inside. An earlier version of this
         * step asserted otherwise, and the assertion failed -- by
         * *succeeding*, which set the main thread's pointer to an address
         * the rest of this unit relies on libc owning.
         */

        cosmo_thread_t tt;
        tls_rc[0] = tls_rc[1] = tls_rc[2] = -1;
        CHECK(cosmo_thread_start(&tt, tls_user, NULL, 16u * 1024u) == 0);
        CHECK(cosmo_thread_join(&tt, NULL) == 0);
        CHECK(tls_rc[0] == 0);     /* a thread can set its own */
        CHECK(tls_rc[1] == 0);     /* and set it again */
        CHECK(tls_rc[2] == 0);     /* and zero is what every thread starts with */
    }

    STEP("13");
    /*
     * (14) **Two threads, two `errno`s.** The point of the unit. Each
     * thread provokes its own failure two thousand times and reads its own
     * value back each time; a shared `errno` loses one of the two within a
     * few iterations. Main provokes a third code while they run, so all
     * three are live at once.
     */
    {
        cosmo_thread_t a, b;
        errno_bad[0] = errno_bad[1] = errno_done = 0;
        CHECK(cosmo_thread_start(&a, errno_ebadf, NULL, 16u * 1024u) == 0);
        CHECK(cosmo_thread_start(&b, errno_erange, NULL, 16u * 1024u) == 0);
        unsigned mine_bad = 0;
        for (unsigned i = 0; i < 2000u; i++) {
            errno = 0;
            if (close(-2) != -1 || errno != EBADF)
                mine_bad++;      /* counted, not printed: 2000 CHECKs would bury the log */
        }
        CHECK(cosmo_thread_join(&a, NULL) == 0);
        CHECK(cosmo_thread_join(&b, NULL) == 0);
        CHECK(errno_done == 2);
        CHECK(mine_bad == 0);       /* main's own, while both threads ran */
        CHECK(errno_bad[0] == 0);   /* EBADF never became ERANGE */
        CHECK(errno_bad[1] == 0);   /* nor the other way */
    }

    STEP("14");
    /* (15) Main's `errno` survives a thread's: it sets one code, a thread
     * sets another and exits, and main's is still its own afterwards. */
    {
        errno = 0;
        CHECK(strtoll("99999999999999999999", NULL, 10) == 0x7fffffffffffffffll);
        CHECK(errno == ERANGE);
        void *theirs = NULL;
        CHECK(cosmo_thread_start(&t, errno_setter, NULL, 16u * 1024u) == 0);
        CHECK(cosmo_thread_join(&t, &theirs) == 0);
        CHECK((unsigned)(unsigned long)theirs == EBADF);   /* the thread's own */
        CHECK(errno == ERANGE);                         /* and main's is untouched */
    }

    STEP("15");
    /*
     * (16) A thread libc did not make, installing its own block. The
     * *un*installed case is deliberately not tested: the contract is that
     * it faults, and a test asserting a fault would be asserting the
     * absence of a fallback this design does not have.
     */
    {
        void *stk = mmap(NULL, 16u * 1024u, PROT_READ | PROT_WRITE,
                         MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
        CHECK(stk != MAP_FAILED);
        raw_rc[0] = raw_rc[1] = raw_rc[2] = raw_rc[3] = 1;
        raw_err = 0;
        raw_done = 0;
        unsigned clear = 0;
        struct cosmo_thread req;
        memset(&req, 0, sizeof(req));
        req.entry = (unsigned long)raw_entry;
        req.stack_top = ((unsigned long)stk + 16u * 1024u) & ~15ul;
        req.tls = 0;                       /* no block: the contract's case */
        req.clear_tid = (unsigned long)&clear;
        long tid = cosmo_thread_create(&req);
        CHECK(tid > 0);
        if (tid > 0) {
            while (__atomic_load_n(&clear, __ATOMIC_ACQUIRE) != 0)
                cosmo_yield();
            CHECK(raw_rc[0] == -EINVAL);       /* shorter than the prefix */
            CHECK(raw_rc[1] == -EINVAL);       /* and misaligned */
            CHECK(raw_rc[3] == 0);             /* exactly the documented size, at 16 */
            CHECK(raw_rc[2] == 0);
            CHECK(raw_err == EBADF);           /* errno works once installed */
            CHECK(raw_done == (unsigned)tid);  /* and so does the tid cache */
        }
        CHECK(munmap(stk, 16u * 1024u) == 0);
    }

    STEP("16");
    /* (17) The cached tid agrees with the syscall, for the first thread and
     * for a created one, and `strerror`/`perror` still answer for the
     * calling thread -- both still shared, which L8 still records. */
    {
        CHECK(cosmo_thread_id() == (cosmo_tid_t)cosmo_thread_self());
        CHECK(cosmo_thread_id() == (cosmo_tid_t)getpid());   /* the first thread answers its pid */
        void *said = NULL;
        CHECK(cosmo_thread_start(&t, id_reporter, NULL, 16u * 1024u) == 0);
        CHECK(cosmo_thread_join(&t, &said) == 0);
        CHECK((unsigned long)said == (unsigned long)t.tid);   /* the cache is this thread's own */
        layout_ok = layout_ran = 0;
        CHECK(cosmo_thread_start(&t, layout_probe, NULL, 16u * 1024u) == 0);
        CHECK(cosmo_thread_join(&t, NULL) == 0);
        CHECK(layout_ran == 1);
        CHECK(layout_ok == 1);      /* the block is above the stack, not in it */
        CHECK(layout_tp_ok == 1);   /* and the thread pointer is where the ABI wants it */
        CHECK(layout_head_ok == 1); /* writing the ABI's reserved head leaves the block alone */
        /*
         * And the join freed it. The report proposed counting the address
         * space across a thousand cycles; this asserts the thing itself
         * instead, because a leak is directly observable: a write from an
         * unmapped address is -EFAULT, and this address was a live block
         * one statement ago. (A count would also need a limit to count
         * against -- `getrlimit` reports the limit, not the usage.)
         */
        int nul = open("/dev/null", O_WRONLY);
        CHECK(nul >= 0);
        if (nul >= 0) {
            errno = 0;
            CHECK(write(nul, (const void *)layout_blk, 1) == -1);
            CHECK(errno == EFAULT);     /* the block page went with the stack */
            CHECK(close(nul) == 0);
        }
        errno = EBADF;
        CHECK(strcmp(strerror(EBADF), "Bad file descriptor") == 0);
        CHECK(strcmp(strerror(ERANGE), "Result out of range") == 0);
        /*
         * And an *unknown* code, which is the only path with a buffer:
         * two threads asking about two unknown codes used to share one
         * and get one answer. L8's last line (see step 17's worker).
         */
        CHECK(strcmp(strerror(4242), "Unknown error 4242") == 0);
    }

    STEP("17");
    /*
     * (17) **`__thread` works**, which is what this unit is for. Four
     * threads' worth of per-thread storage: the first thread's and three
     * created ones, each writing its own value and checking it survives
     * while the others write theirs.
     */
    {
        cosmo_thread_t tt[3];
        tls_bad = tls_done = 0;
        /* The first thread's own copy, before any other exists. */
        CHECK(tls_init == 0x5eed);
        CHECK(tls_zero[0] == 0 && tls_zero[sizeof(tls_zero) - 1] == 0);
        CHECK(((unsigned long)&tls_aligned % 64u) == 0);
        CHECK(((unsigned long)&tls_overaligned % 256u) == 0);
        tls_init = 0x1111;
        memset(tls_zero, 0x11, sizeof(tls_zero));
        for (unsigned i = 0; i < 3u; i++)
            CHECK(cosmo_thread_start(&tt[i], tlsvar_user, (void *)(unsigned long)(i + 2), 32u * 1024u) == 0);
        for (unsigned i = 0; i < 3u; i++)
            CHECK(cosmo_thread_join(&tt[i], NULL) == 0);
        CHECK(tls_done == 3);
        CHECK(tls_bad == 0);
        /* And the first thread's copy is still its own, which a shared
         * image would have lost three times over. */
        CHECK(tls_init == 0x1111);
        CHECK(tls_zero[0] == 0x11 && tls_zero[sizeof(tls_zero) - 1] == 0x11);
        printf("thrtest: tls_init at %p, aligned at %p\n", (void *)&tls_init, (void *)&tls_aligned);
    }

    STEP("18");
    /*
     * (18) **A signal is seen, and the mutex comes back.** The first of
     * the condition variable's steps
     * (docs/audit/next-subsystem-condvar.md). One waiter, one signaller;
     * the waiter must return with the predicate true *and* holding the
     * mutex again.
     */
    {
        cosmo_thread_t w;
        cv_ready = cv_woke = cv_held_ok = cv_entered = 0;
        CHECK(cosmo_thread_start(&w, cv_waiter, NULL, 32u * 1024u) == 0);
        /* Wait for it to be *in* the wait, by its own report -- and wait
         * for it on a condition variable, not a yield loop. "N things
         * after a fixed settle" is the flake family this tree keeps
         * re-learning, and a spin here would be the primitive's own test
         * declining to use it. */
        cosmo_mutex_lock(&cv_m);
        cv_await_entered(1);
        cv_ready = 1;
        cosmo_cond_signal(&cv_c);
        cosmo_mutex_unlock(&cv_m);
        CHECK(cosmo_thread_join(&w, NULL) == 0);
        CHECK(cv_woke == 1);
        CHECK(cv_held_ok == 1);   /* trylock said -EBUSY: the wait re-took it */
    }

    STEP("19");
    /*
     * (19) **A signal delivered inside the sleep window is not lost.**
     * The property the unit exists for, and the one no arrangement of
     * threads can test: the window between `cosmo_cond_wait`'s read of
     * `seq` and its `futex_wait` is a few instructions wide, and
     * releasing a blocked signaller only makes it *runnable*.
     *
     * So libc's probe is armed, and it signals from inside the window on
     * this very thread. A correct wait read `seq` before unlocking, so the
     * probe's increment makes `futex_wait` return without sleeping and the
     * `while` sees the predicate. A wait that read `seq` after unlocking
     * reads the value the probe already published and sleeps on it
     * forever -- which is a hang, caught by the boot deadline.
     *
     * The probe is armed immediately before the wait it instruments, and
     * libc *takes* it rather than reading it, so it cannot fire inside a
     * later step's waiter.
     */
    {
        cv_ready = cv_probe_ran = 0;
        __atomic_store_n(&__cosmo_cond_probe, cv_probe, __ATOMIC_RELEASE);
        cosmo_mutex_lock(&cv_m);
        while (!cv_ready)
            cosmo_cond_wait(&cv_c, &cv_m);
        cosmo_mutex_unlock(&cv_m);
        CHECK(cv_probe_ran == 1);                       /* the window was entered */
        CHECK(__atomic_load_n(&__cosmo_cond_probe, __ATOMIC_ACQUIRE) == NULL);
    }

    STEP("20");
    /*
     * (20) **A broadcast reaches every waiter, and a signal delivers one
     * ticket's worth of work.** Four waiters either way.
     *
     * Not "a signal wakes exactly one": the interface does not promise
     * that. Spurious wakeups are permitted, step 22 exists because callers
     * must tolerate them, and how many threads a signal makes runnable is
     * not observable from here without a race. What a caller depends on is
     * that no more work is taken than was made available, and that is what
     * the second half asserts.
     */
    {
        cosmo_thread_t w[4];
        cv_ready = cv_woke = cv_held_ok = cv_entered = 0;
        for (unsigned i = 0; i < 4u; i++)
            CHECK(cosmo_thread_start(&w[i], cv_waiter, NULL, 32u * 1024u) == 0);
        cosmo_mutex_lock(&cv_m);
        cv_await_entered(4);
        cv_ready = 1;
        cosmo_cond_broadcast(&cv_c);
        cosmo_mutex_unlock(&cv_m);
        for (unsigned i = 0; i < 4u; i++)
            CHECK(cosmo_thread_join(&w[i], NULL) == 0);
        CHECK(cv_woke == 4);
        CHECK(cv_held_ok == 4);

        /*
         * And a signal with four waiting hands out **one ticket's worth of
         * work**, however many threads it happens to wake. That -- not
         * "exactly one thread wakes" -- is what the interface promises;
         * see `cv_ticket_waiter` for why the obvious assertion was wrong
         * and had to be replaced.
         */
        /*
         * The ticket is published **after** all four are waiting, not
         * before they start. A first version set it first, so the first
         * worker to run found work waiting and took it without ever
         * calling `cosmo_cond_wait` -- by the time main saw four arrivals
         * `cv_taken` was already 1, and deleting the signal entirely would
         * still have passed. Review found it. A test for what a signal
         * delivers must have nothing to deliver until the signal.
         */
        cv_entered = cv_taken = 0;
        cv_tickets = 0;
        for (unsigned i = 0; i < 4u; i++)
            CHECK(cosmo_thread_start(&w[i], cv_ticket_waiter, NULL, 32u * 1024u) == 0);
        cosmo_mutex_lock(&cv_m);
        cv_await_entered(4);
        CHECK(cv_taken == 0);       /* nothing taken before there was anything to take */
        cv_tickets = 1;
        cosmo_cond_signal(&cv_c);
        cosmo_mutex_unlock(&cv_m);
        /* Whoever wakes, exactly one ticket exists, so exactly one is
         * taken -- and this stays true however long we look, which is what
         * makes it an assertion rather than a sample. */
        cosmo_mutex_lock(&cv_m);
        while (cv_taken < 1u)
            cosmo_cond_wait(&cv_enter_c, &cv_m);
        CHECK(cv_taken == 1);
        CHECK(cv_tickets == 0);
        /* Release the other three with work of their own. */
        cv_tickets = 3;
        cosmo_cond_broadcast(&cv_c);
        cosmo_mutex_unlock(&cv_m);
        for (unsigned i = 0; i < 4u; i++)
            CHECK(cosmo_thread_join(&w[i], NULL) == 0);
        CHECK(cv_taken == 4 && cv_tickets == 0);
    }

    STEP("21");
    /*
     * (21) **Timeouts.** One wait that expires and one that does not,
     * both bounded by the clock rather than by a loop count.
     */
    {
        cv_ready = 0;
        cosmo_mutex_lock(&cv_m);
        uint64_t t0 = cosmo_clock_ns();
        int rc = cosmo_cond_timedwait(&cv_c, &cv_m, 20ull * 1000ull * 1000ull);
        uint64_t elapsed = cosmo_clock_since_ns(t0);
        /*
         * The mutex, **before** releasing it. A first version unlocked and
         * then checked that `trylock` succeeded, which proves only that
         * the mutex is free -- and `cosmo_mutex_unlock` silently stores 0
         * over an already-unlocked mutex, so that check passed whether or
         * not `timedwait` had retaken it. Review found it. Asking for
         * -EBUSY while still holding it is the assertion that can fail.
         */
        CHECK(cosmo_mutex_trylock(&cv_m) == -EBUSY);
        cosmo_mutex_unlock(&cv_m);
        CHECK(rc == -ETIMEDOUT);
        CHECK(elapsed >= 20ull * 1000ull * 1000ull);   /* it waited at least what it was asked */

        /*
         * And a timed wait that is *signalled* returns 0, well inside its
         * deadline. This needs a waiter that actually calls
         * `cosmo_cond_timedwait`: a first version used the untimed
         * `cv_waiter` here, so the timed success path this paragraph
         * claims to cover was never executed and an implementation
         * reporting `-ETIMEDOUT` after a successful wake would have
         * passed.
         */
        cosmo_thread_t w;
        cv_ready = cv_woke = cv_held_ok = cv_entered = 0;
        cv_timed_rc = 1; cv_timed_ns = 0;
        CHECK(cosmo_thread_start(&w, cv_timed_waiter, NULL, 32u * 1024u) == 0);
        cosmo_mutex_lock(&cv_m);
        cv_await_entered(1);
        cv_ready = 1;
        cosmo_cond_signal(&cv_c);
        cosmo_mutex_unlock(&cv_m);
        CHECK(cosmo_thread_join(&w, NULL) == 0);
        CHECK(cv_woke == 1);
        CHECK(cv_timed_rc == 0);                        /* woken, not timed out */
        CHECK(cv_timed_ns < CV_TIMED_BUDGET_NS);        /* and well inside the budget */
    }

    STEP("22");
    /*
     * (22) **A correct caller survives spurious wakeups.** A waiter whose
     * predicate never becomes true, broadcast at repeatedly: every
     * `cosmo_cond_wait` returns and every one goes back round the
     * `while`, so the waiter is still waiting and the predicate is still
     * false. This is the step that makes the loop contract a tested
     * property rather than a comment -- an implementation that returned
     * "the predicate is true" would strand this waiter's caller.
     */
    {
        cosmo_thread_t w;
        cv_spur_stop = cv_spur_returns = cv_entered = 0;
        CHECK(cosmo_thread_start(&w, cv_spurious_waiter, NULL, 32u * 1024u) == 0);
        cosmo_mutex_lock(&cv_m);
        cv_await_entered(1);
        for (unsigned i = 0; i < 20u; i++) {
            unsigned before = cv_spur_returns;
            cosmo_cond_broadcast(&cv_c);
            /* Wait for the wakeup to be observed rather than assuming it
             * was: the count is the thing, not the interval. */
            while (cv_spur_returns == before)
                cosmo_cond_wait(&cv_enter_c, &cv_m);
        }
        cosmo_mutex_unlock(&cv_m);
        CHECK(cv_spur_returns >= 20u);   /* it woke, repeatedly */
        CHECK(cv_spur_stop == 0);        /* and its predicate never became true */
        /* Release it. */
        cosmo_mutex_lock(&cv_m);
        cv_spur_stop = 1;
        cosmo_cond_broadcast(&cv_c);
        cosmo_mutex_unlock(&cv_m);
        CHECK(cosmo_thread_join(&w, NULL) == 0);
    }

    STEP("23");
    /*
     * (23) The bound holds, and the process survives reaching it. This is
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

    STEP("24");
    /*
     * (24) **A program can find its own program headers**, which is how it
     * will find its own `PT_TLS` (docs/audit/next-subsystem-pt-tls.md). The
     * kernel passes the standard trio in the auxiliary vector and knows
     * nothing about thread-local storage; everything above that is the
     * program's own reading of its own ELF.
     *
     * The header layout is declared here rather than included: ELF's
     * program header is a fixed format, and no public header in this tree
     * describes it yet -- libc will need its own copy when it starts
     * placing images, and a test that waited for that would be testing
     * nothing now.
     */
    {
        struct phdr {
            uint32_t p_type, p_flags;
            uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
        };
        unsigned long at_phdr = cosmo_getauxval(COSMO_AT_PHDR);
        unsigned long at_phent = cosmo_getauxval(COSMO_AT_PHENT);
        unsigned long at_phnum = cosmo_getauxval(COSMO_AT_PHNUM);
        CHECK(at_phdr != 0);                      /* the headers are mapped */
        CHECK(at_phent == sizeof(struct phdr));   /* 56, and the kernel refuses anything else */
        CHECK(at_phnum > 0 && at_phnum < 64);
        /* And the tags that were already there, so a wrong offset into the
         * vector shows up as these failing rather than as a plausible
         * address. */
        CHECK(cosmo_getauxval(COSMO_AT_PAGESZ) == 4096);
        CHECK(cosmo_getauxval(COSMO_AT_ENTRY) != 0);
        CHECK(cosmo_getauxval(0xdead) == 0);      /* an unknown tag is absent, not garbage */

        /*
         * The headers must describe *this* program: some PT_LOAD has to
         * cover the address of this function. A vector that pointed at
         * another image, or at nothing, would satisfy every check above.
         */
        const struct phdr *ph = (const struct phdr *)at_phdr;
        unsigned long self = (unsigned long)(void *)&main;
        int covers = 0, nr_tls = 0;
        for (unsigned long i = 0; i < at_phnum; i++) {
            if (ph[i].p_type == 1u) {   /* PT_LOAD */
                if (self >= ph[i].p_vaddr && self < ph[i].p_vaddr + ph[i].p_memsz)
                    covers = 1;
            }
            if (ph[i].p_type == 7u)     /* PT_TLS */
                nr_tls++;
        }
        CHECK(covers == 1);
        /* At most one, which the ABI requires and every later step relies
         * on. This program has none yet; the count is printed so that the
         * step which gives it a `__thread` variable can be seen to change
         * it rather than asserted to. */
        CHECK(nr_tls <= 1);
        printf("thrtest: phdr at 0x%lx, %lu entries, %d PT_TLS\n", at_phdr, at_phnum, nr_tls);
    }

    /*
     * The two tables the threads unit did not lock
     * (docs/audit/next-subsystem-libc-shared-tables.md). Last, because
     * the atexit ones leave handlers registered and the drain's own
     * check runs after main returns.
     */
    /* FIRST, so the LIFO drain runs it LAST and it can see every
     * other handler's result. It prints the verdict. */
    CHECK(atexit(atexit_checks_at_exit) == 0);

    env_grow_under_readers();
    env_unset_under_readers();
    atexit_concurrent();
    CHECK(atexit(reentrant_handler) == 0);
    at_registered++;             /* the drain must run this one too */

    /*
     * `exit`, not `return`: the drain is part of what is under test,
     * and the verdict is printed from the last handler rather than
     * from here.
     */
    exit(0);
}
