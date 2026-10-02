/*
 * lxtest - Exercise the Linux personality through the raw Linux ABI
 * (docs/compat/linux/testing.md). Freestanding: no libc, no CosmoOS
 * note, so the kernel runs it as a Linux program. Prints
 * "LINUXTEST: PASS" or "LINUXTEST: FAIL <what>" and exits 0 or 1.
 */

#include "lxabi.h"

static int g_failures;

static void put_num(long v)
{
    char buf[24];
    int i = sizeof(buf);
    int neg = v < 0;
    unsigned long u = neg ? (unsigned long)(-(v + 1)) + 1 : (unsigned long)v;
    buf[--i] = '\0';
    do {
        buf[--i] = (char)('0' + u % 10);
        u /= 10;
    } while (u);
    if (neg)
        buf[--i] = '-';
    lx_puts(buf + i);
}

static void check(int cond, const char *what, long value)
{
    if (cond)
        return;
    g_failures++;
    lx_puts("LINUXTEST: FAIL ");
    lx_puts(what);
    lx_puts(" (");
    put_num(value);
    lx_puts(")\n");
}
#define CHECK(c) check((c), #c, 0)
#define CHECKV(c, v) check((c), #c, (long)(v))

static int memeq(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;
    for (size_t i = 0; i < n; i++)
        if (x[i] != y[i])
            return 0;
    return 1;
}

/* select's fd_set as 16 words of 64 bits (LX_FD_SETSIZE 1024). */
static void lx_fdzero(uint64_t *set)
{
    for (int i = 0; i < 16; i++)
        set[i] = 0;
}
static void lx_fdset(uint64_t *set, long fd) { set[fd / 64] |= 1ull << (fd % 64); }
static int lx_fdisset(const uint64_t *set, long fd) { return (set[fd / 64] >> (fd % 64)) & 1u; }

/* An ARP request for the gateway of pool subnet k (10.0.(3+k).1), the shape
 * the kernel's tap test injects; the tap's stack answers its own. */
static void lx_arp_request(uint8_t *req, unsigned k)
{
    static const uint8_t guest_mac[6] = { 0x52, 0x54, 0x00, 0x00, 0x00, 0x01 };
    for (int i = 0; i < 42; i++)
        req[i] = 0;
    for (int i = 0; i < 6; i++)
        req[i] = 0xff;
    for (int i = 0; i < 6; i++)
        req[6 + i] = guest_mac[i];
    req[12] = 0x08; req[13] = 0x06;
    req[15] = 1;
    req[16] = 0x08;
    req[18] = 6; req[19] = 4;
    req[21] = 1;
    for (int i = 0; i < 6; i++)
        req[22 + i] = guest_mac[i];
    req[28] = 10; req[29] = 0; req[30] = (uint8_t)(3 + k); req[31] = 15;   /* spa */
    req[38] = 10; req[39] = 0; req[40] = (uint8_t)(3 + k); req[41] = 1;    /* tpa */
}

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/* The thread pointer: %fs:0 reads the word at the FS base. */
/* The thread pointer as user code sees it: x86-64 reads through the FS
 * base (the word the TCB starts with), AArch64 reads the register. */
#if defined(__x86_64__)
static unsigned long read_fs0(void)
{
    unsigned long v;
    __asm__ volatile("movq %%fs:0, %0" : "=r"(v));
    return v;
}
static int tls_is(const uint64_t *tcb) { return read_fs0() == tcb[0]; }
#else
static unsigned long read_tpidr(void)
{
    unsigned long v;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(v));
    return v;
}
static void write_tpidr(unsigned long v)
{
    __asm__ volatile("msr tpidr_el0, %0" : : "r"(v) : "memory");
}
static int tls_is(const uint64_t *tcb) { return read_tpidr() == (unsigned long)(uintptr_t)tcb; }
#endif

/* --- signals (milestone 10) --- */
static struct {
    volatile int count, sig, code, pid, ss_flags, ss_now, fpstate_ok;
    volatile unsigned long addr, sp, blocked;
} g_sig;

#if !defined(__x86_64__)
static void lx_clobber_v0(void);
#endif

/* The restorer every handler returns through: rt_sigreturn. */
#if defined(__x86_64__)
__asm__(".text\n"
        ".globl lx_restorer\n"
        "lx_restorer:\n"
        "    movl $15, %eax\n"
        "    syscall\n");
#else
__asm__(".text\n"
        ".globl lx_restorer\n"
        "lx_restorer:\n"
        "    mov x8, #139\n"
        "    svc #0\n");
#endif
extern void lx_restorer(void);

static void sig_handler(int sig, struct lx_siginfo *si, void *ucv)
{
#if defined(__x86_64__)
    struct lx_ucontext_x86 *uc = ucv;
#else
    struct lx_ucontext_a64 *uc = ucv;
#endif
    g_sig.count++;
    g_sig.sig = sig;
    g_sig.code = si->si_code;
    g_sig.pid = si->u.kill.pid;
    g_sig.addr = (unsigned long)si->u.fault.addr;
    g_sig.sp = (unsigned long)(uintptr_t)&uc;   /* a local: on the stack the handler runs on */
    g_sig.ss_flags = uc->uc_stack.ss_flags;   /* the interrupted context's view, as on Linux */
    struct lx_stack_t now;
    g_sig.ss_now = sc2(LX_sigaltstack, 0, &now) == 0 ? now.ss_flags : -1;
    uint64_t cur = 0;
    sc4(LX_rt_sigprocmask, LX_SIG_BLOCK, 0, &cur, 8);
    g_sig.blocked = cur;
#if defined(__x86_64__)
    g_sig.fpstate_ok = uc->uc_mcontext.fpstate != 0 && (uc->uc_mcontext.fpstate & 63) == 0 &&
                       *(volatile uint32_t *)(uintptr_t)(uc->uc_mcontext.fpstate + 24) == 0x1f80;   /* MXCSR */
    if (sig == 11)
        uc->uc_mcontext.rip += 3;   /* over the 3-byte store of sig_fault_store */
    if (sig == 10)
        __asm__ volatile("pxor %%xmm0, %%xmm0" ::: "memory");   /* the frame must carry the caller's xmm0 */
#else
    /* The reserved area is a list of records: the FP/SIMD state, then
     * the esr_context, then the terminator. Walk it rather than assume
     * an order, which is what a real libc does. */
    unsigned off = 0, seen_fp = 0, seen_esr = 0;
    for (;;) {
        struct { uint32_t magic, size; } head;
        __builtin_memcpy(&head, uc->uc_mcontext.reserved + off, sizeof(head));
        if (head.magic == 0 || head.size < sizeof(head))
            break;
        if (head.magic == LX_FPSIMD_MAGIC && head.size == 528)
            seen_fp = 1;
        if (head.magic == LX_ESR_MAGIC && head.size == 16)
            seen_esr = 1;
        off += head.size;
        if (off + sizeof(head) > sizeof(uc->uc_mcontext.reserved))
            break;
    }
    g_sig.fpstate_ok = seen_fp && seen_esr && uc->uc_mcontext.pc != 0;
    if (sig == 10)
        lx_clobber_v0();   /* the frame must carry the caller's Q0 */
    if (sig == 11)
        uc->uc_mcontext.pc += 4;    /* over the store of sig_fault_store */
#endif
}

static void sig_install(int sig, unsigned flags)
{
    struct lx_sigaction a = { .handler = (uint64_t)(uintptr_t)sig_handler, .flags = LX_SA_SIGINFO | LX_SA_RESTORER | flags,
                              .restorer = (uint64_t)(uintptr_t)lx_restorer, .mask = 0 };
    CHECKV(sc4(LX_rt_sigaction, sig, &a, 0, 8) == 0, sig);
}

/* One store instruction the SIGSEGV handler steps over: movb $1, (%rax)
 * (c6 00 01, three bytes) or strb (four bytes). */
static void sig_fault_store(unsigned long addr)
{
#if defined(__x86_64__)
    __asm__ volatile("movb $1, (%0)" : : "a"(addr) : "memory");
#else
    __asm__ volatile("strb %w1, [%0]" : : "r"(addr), "r"(1) : "memory");
#endif
}

#if !defined(__x86_64__)
/* The vector registers are off limits to the compiler here
 * (-mgeneral-regs-only), so the two places this test uses them say so to
 * the assembler and put it back. */
static void lx_clobber_v0(void)
{
    __asm__ volatile(".arch armv8-a+fp+simd\n\t"
                     "movi v0.16b, #0\n\t"
                     ".arch armv8-a" ::: "memory");
}

/* Q0 loaded, kill(pid, SIGUSR1) with the handler clobbering it, Q0 read
 * back: only the handler and the kernel's frame can change it between. */
static unsigned long sig_vreg_roundtrip(long pid)
{
    unsigned long in = 0x0123456789abcdefull, out = 0;
    register long x8 __asm__("x8") = (long)LX_kill;
    register long x0 __asm__("x0") = pid;
    register long x1 __asm__("x1") = 10;
    __asm__ volatile(".arch armv8-a+fp+simd\n\t"
                     "fmov d0, %[in]\n\t"
                     "svc #0\n\t"
                     "fmov %[out], d0\n\t"
                     ".arch armv8-a"
                     : [out] "=r"(out), "+r"(x0)
                     : [in] "r"(in), "r"(x8), "r"(x1)
                     : "memory");
    return out;
}
#endif

#if defined(__x86_64__)
/* xmm0 loaded, kill(pid, SIGUSR1) with the handler clobbering xmm0, xmm0 read back. */
static unsigned long sig_xmm_roundtrip(long pid)
{
    unsigned long in = 0x0123456789abcdefull, out;
    long ret;
    /* The compiler never touches xmm registers here (-mgeneral-regs-only):
     * only the handler and the kernel's frame can change xmm0 in between. */
    __asm__ volatile("movq %[in], %%xmm0\n\t"
                     "syscall\n\t"
                     "movq %%xmm0, %[out]"
                     : [out] "=r"(out), "=a"(ret)
                     : "a"((long)LX_kill), "D"(pid), "S"(10L), [in] "r"(in)
                     : "rcx", "r11", "memory");
    (void)ret;
    return out;
}
#endif

/* --- threads (milestone 10): clone, futex, tgkill, SA_RESTART --- */
#define THREAD_FLAGS                                                                                        \
    (LX_CLONE_VM | LX_CLONE_FS | LX_CLONE_FILES | LX_CLONE_SIGHAND | LX_CLONE_THREAD | LX_CLONE_SYSVSEM |  \
     LX_CLONE_SETTLS | LX_CLONE_PARENT_SETTID | LX_CLONE_CHILD_CLEARTID | LX_CLONE_CHILD_SETTID)
static char g_stacks[4][16384] __attribute__((aligned(16)));
static uint64_t g_tcb[4] = { 0xC0FFEE, 0, 0, 0 };
static int32_t g_tidword[4];   /* CHILD_SETTID / CLEARTID words; read with an atomic load */
static volatile int g_child_tid, g_child_fs_ok, g_handler_tid;
static volatile long g_read_rc[2];
static volatile uint32_t g_futex_a, g_futex_b;
static volatile long g_wait_rc[2];
static volatile int g_waiting;
static int32_t g_pipe[2];

static void nap_ms(long ms)
{
    struct lx_timespec ts = { 0, ms * 1000000 };
    sc2(LX_nanosleep, &ts, 0);
}

static int t_basic(void *arg)
{
    (void)arg;
    g_child_tid = (int)sc0(LX_gettid);
    g_child_fs_ok = tls_is(g_tcb);
    return 0;
}

/* Block in read(pipe); the main thread signals this thread. */
static int t_reader(void *arg)
{
    char c[8];
    g_read_rc[(int)(uintptr_t)arg] = sc3(LX_read, g_pipe[0], c, 4);
    return 0;
}

/* Writes to the pipe after a pause: the main thread's poll must wake. */
static int t_late_writer(void *arg)
{
    (void)arg;
    nap_ms(30);
    sc3(LX_write, g_pipe[1], "z", 1);
    return 0;
}

#ifdef LX_select
/* Half a second later: long enough to outlast a timeout that wrapped to 290 ms. */
static int t_later_writer(void *arg)
{
    (void)arg;
    nap_ms(500);
    sc3(LX_write, g_pipe[1], "w", 1);
    return 0;
}
#endif

static int t_waiter(void *arg)
{
    int i = (int)(uintptr_t)arg;
    /* An absolute CLOCK_REALTIME deadline two seconds out: it must not fire. */
    struct lx_timespec dl;
    sc2(LX_clock_gettime, LX_CLOCK_REALTIME, &dl);
    dl.tv_sec += 2;
    __atomic_fetch_add(&g_waiting, 1, __ATOMIC_SEQ_CST);
    g_wait_rc[i] = sc6(LX_futex, &g_futex_a, LX_FUTEX_WAIT_BITSET | LX_FUTEX_CLOCK_REALTIME, 0, &dl, 0,
                       LX_FUTEX_BITSET_MATCH_ANY);
    return 0;
}

static void thread_handler(int sig, struct lx_siginfo *si, void *uc)
{
    (void)sig; (void)si; (void)uc;
    g_handler_tid = (int)sc0(LX_gettid);
}

/* poll with a millisecond timeout: the poll call where it exists (x86-64),
 * ppoll with a timespec elsewhere (AArch64 has no poll). */
static long lx_poll_ms(struct lx_pollfd *fds, unsigned long n, int ms)
{
#ifdef LX_poll
    return sc3(LX_poll, fds, n, ms);
#else
    struct lx_timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    return sc4(LX_ppoll, fds, n, ms < 0 ? 0 : &ts, 0);
#endif
}

/* The shared-futex unit's flag test: wait on the word `arg` points at,
 * with FUTEX_PRIVATE_FLAG when the low bit of the argument's second word
 * says so. */
static volatile unsigned *g_flag_word;
static long g_flag_rc[2];
static int t_flag_waiter(void *arg)
{
    int with_flag = (int)(uintptr_t)arg;
    struct lx_timespec to = { 2, 0 };
    g_flag_rc[with_flag] = sc6(LX_futex, g_flag_word, LX_FUTEX_WAIT | (with_flag ? LX_FUTEX_PRIVATE_FLAG : 0), 0,
                               &to, 0, 0);
    return 0;
}

/* Two threads placing pages at once through the Linux door (M46). */
#define LX_PLACE_ROUNDS 300
static long g_place[2][LX_PLACE_ROUNDS];
static int t_place(void *arg)
{
    long *out = g_place[(int)(uintptr_t)arg];
    for (int i = 0; i < LX_PLACE_ROUNDS; i++)
        out[i] = sc6(LX_mmap, 0, 4096, LX_PROT_READ | LX_PROT_WRITE, LX_MAP_PRIVATE | LX_MAP_ANONYMOUS, -1, 0);
    return 0;
}

/* Join like a libc: wait while the CHILD_CLEARTID word is nonzero. */
static int lx_join(int32_t *word)
{
    for (int i = 0; i < 400; i++) {
        int32_t v = __atomic_load_n(word, __ATOMIC_ACQUIRE);
        if (v == 0)
            return 0;
        struct lx_timespec to = { 0, 50000000 };
        sc6(LX_futex, word, LX_FUTEX_WAIT, v, &to, 0, 0);
    }
    return -1;
}

int main(int argc, char **argv)
{
    CHECKV(argc >= 1 && argv[0][0] != '\0', argc);

    /* --- identity, uname, time --- */
    long pid = sc0(LX_getpid);
    CHECKV(pid > 0, pid);
    CHECKV(sc0(LX_gettid) == pid, 0);
    CHECKV(sc0(LX_getppid) > 0, 0);
    CHECKV(sc0(LX_getuid) == 0 && sc0(LX_geteuid) == 0, 0);
    struct lx_utsname u;
    CHECKV(sc1(LX_uname, &u) == 0, 0);
    CHECK(streq(u.sysname, "Linux") && streq(u.machine, LX_MACHINE));
    struct lx_timespec ts0, ts1;
    CHECKV(sc2(LX_clock_gettime, LX_CLOCK_MONOTONIC, &ts0) == 0, 0);
    struct lx_timespec nap = { 0, 5000000 };
    CHECKV(sc2(LX_nanosleep, &nap, 0) == 0, 0);
    CHECKV(sc2(LX_clock_gettime, LX_CLOCK_MONOTONIC, &ts1) == 0, 0);
    long dt = (ts1.tv_sec - ts0.tv_sec) * 1000000000L + (ts1.tv_nsec - ts0.tv_nsec);
    CHECKV(dt >= 5000000 && dt < 500000000, dt);
    /* The wall clock (milestone 10): a plausible date, the same clock behind
     * time() and gettimeofday(), distinct from the monotonic one. */
    struct lx_timespec rt;
    CHECKV(sc2(LX_clock_gettime, LX_CLOCK_REALTIME, &rt) == 0, 0);
    CHECKV(rt.tv_sec > 1600000000L && rt.tv_sec < 4000000000L, rt.tv_sec);   /* between 2020 and 2096 */
#ifdef LX_time
    long tsec = sc0(LX_time);
    CHECKV(tsec >= rt.tv_sec && tsec <= rt.tv_sec + 2, tsec);
#endif
    struct lx_timeval tv;
    CHECKV(sc2(LX_gettimeofday, &tv, 0) == 0 && tv.tv_sec >= rt.tv_sec && tv.tv_sec <= rt.tv_sec + 2, tv.tv_sec);
    CHECKV(sc2(LX_clock_gettime, LX_CLOCK_REALTIME_COARSE, &ts1) == 0 && ts1.tv_sec >= rt.tv_sec, 0);
    CHECKV(sc2(LX_clock_gettime, LX_CLOCK_BOOTTIME, &ts1) == 0 && ts1.tv_sec < 100000, ts1.tv_sec);   /* monotonic: since boot */
    CHECKV(sc2(LX_clock_gettime, 99, &ts1) == -22, 0);   /* EINVAL */

    /* --- thread pointer: arch_prctl(ARCH_SET_FS) on x86-64, the tpidr_el0
     * register itself on AArch64; either way it survives a context switch --- */
    static uint64_t tcb[4] = { 0x1234567887654321UL, 0, 0, 0 };
#if defined(__x86_64__)
    CHECKV(sc2(LX_arch_prctl, LX_ARCH_SET_FS, tcb) == 0, 0);
    unsigned long fs = 0;
    CHECKV(sc2(LX_arch_prctl, LX_ARCH_GET_FS, &fs) == 0 && fs == (unsigned long)tcb, fs);
    CHECKV(sc2(LX_arch_prctl, LX_ARCH_SET_GS, tcb) == -22, 0);
#else
    write_tpidr((unsigned long)(uintptr_t)tcb);
#endif
    CHECK(tls_is(tcb));
    long tid = sc1(LX_set_tid_address, &tcb[1]);
    CHECKV(tid == pid, tid);
    /* The pointer survives a sleep (a context switch away and back). */
    nap.tv_nsec = 2000000;
    sc2(LX_nanosleep, &nap, 0);
    CHECK(tls_is(tcb));

    /* --- brk and mmap --- */
    long brk0 = sc1(LX_brk, 0);
    CHECKV(brk0 > 0 && (brk0 & 0xfff) == 0, brk0);
    long brk1 = sc1(LX_brk, brk0 + 100000);
    CHECKV(brk1 == brk0 + 100000, brk1);
    volatile unsigned char *heap = (unsigned char *)brk0;
    heap[0] = 1;
    heap[99999] = 2;
    CHECK(heap[0] == 1 && heap[99999] == 2);
    CHECKV(sc1(LX_brk, brk0) == brk0, 0);                     /* shrink back */
    CHECKV(sc1(LX_brk, brk0 - 4096) == brk0, 0);              /* below the start: unchanged */
    long m = sc6(LX_mmap, 0, 8192, LX_PROT_READ | LX_PROT_WRITE, LX_MAP_PRIVATE | LX_MAP_ANONYMOUS, -1, 0);
    CHECKV(m > 0 && (m & 0xfff) == 0, m);
    volatile unsigned int *w = (unsigned int *)m;
    CHECK(w[0] == 0 && w[2047] == 0);
    w[0] = 7;
    w[2047] = 9;
    CHECK(w[0] == 7 && w[2047] == 9);
    CHECKV(sc3(LX_mprotect, m, 8192, LX_PROT_READ) == 0, 0);
    CHECK(w[0] == 7);
    CHECKV(sc2(LX_munmap, m, 8192) == 0, 0);
    CHECKV(sc6(LX_mmap, 0, 4096, LX_PROT_READ, LX_MAP_PRIVATE, 77, 0) == -9, 0);   /* file mapping: bad fd */
    CHECKV(sc6(LX_mmap, 0, 4096, LX_PROT_READ | LX_PROT_WRITE | LX_PROT_EXEC, LX_MAP_PRIVATE | LX_MAP_ANONYMOUS, -1, 0) == -22, 0);
    long fixed = sc6(LX_mmap, 0x30000000000UL, 4096, LX_PROT_READ | LX_PROT_WRITE, LX_MAP_PRIVATE | LX_MAP_ANONYMOUS | LX_MAP_FIXED, -1, 0);
    CHECKV(fixed == 0x30000000000L, fixed);
    if (fixed > 0) {
        *(volatile char *)fixed = 'x';
        /* MAP_FIXED over a live mapping replaces it: fresh zero contents. */
        long again = sc6(LX_mmap, fixed, 4096, LX_PROT_READ | LX_PROT_WRITE, LX_MAP_PRIVATE | LX_MAP_ANONYMOUS | LX_MAP_FIXED, -1, 0);
        CHECKV(again == fixed, again);
        CHECK(*(volatile char *)fixed == 0);
        CHECKV(sc2(LX_munmap, fixed, 4096) == 0, 0);
    }

    /* --- milestone 5: partial mprotect, PROT_NONE, brk shrink and regrow --- */
    long r8 = sc6(LX_mmap, 0, 8 * 4096, LX_PROT_READ | LX_PROT_WRITE, LX_MAP_PRIVATE | LX_MAP_ANONYMOUS, -1, 0);
    CHECKV(r8 > 0 && (r8 & 0xfff) == 0, r8);
    if (r8 > 0) {
        volatile unsigned char *b = (unsigned char *)r8;
        for (int i = 0; i < 8; i++)
            b[i * 4096] = (unsigned char)(i + 1);
        CHECKV(sc3(LX_mprotect, r8 + 2 * 4096, 2 * 4096, LX_PROT_READ) == 0, 0);   /* the middle: a split */
        b[0] = 9;                                                                  /* still writable */
        CHECK(b[2 * 4096] == 3 && b[3 * 4096] == 4);                              /* readable, contents kept */
        CHECKV(sc3(LX_mprotect, r8 + 2 * 4096, 2 * 4096, 0) == 0, 0);             /* PROT_NONE */
        CHECKV(sc3(LX_write, 1, r8 + 2 * 4096, 1) == -14, 0);                     /* the kernel gets EFAULT too */
        CHECKV(sc3(LX_mprotect, r8 + 2 * 4096, 2 * 4096, LX_PROT_READ | LX_PROT_WRITE) == 0, 0);
        CHECK(b[2 * 4096] == 3);                                                   /* the frame survived PROT_NONE */
        b[2 * 4096] = 7;
        CHECKV(sc2(LX_munmap, r8 + 4096, 4096) == 0, 0);                          /* a hole */
        CHECKV(sc3(LX_mprotect, r8, 8 * 4096, LX_PROT_READ) == -12, 0);           /* across the hole: ENOMEM */
        CHECKV(sc2(LX_munmap, r8, 8 * 4096) == 0, 0);                             /* lenient: the hole is skipped */
    }
    long brk2 = sc1(LX_brk, brk0 + 100000);
    CHECKV(brk2 == brk0 + 100000, brk2);
    heap[0] = 1;                                                            /* page 0: kept by the shrink below */
    heap[99999] = 5;                                                        /* page 24: freed by it */
    CHECKV(sc1(LX_brk, brk0 + 4096) == brk0 + 4096, 0);                    /* shrink to one page */
    CHECKV(sc1(LX_brk, brk0 + 200000) == brk0 + 200000, 0);                /* regrow past the old size */
    heap[199999] = 6;
    CHECK(heap[0] == 1 && heap[99999] == 0 && heap[199999] == 6);          /* kept, fresh zero, new */
    CHECKV(sc1(LX_brk, brk0) == brk0, 0);

    /* --- files --- */
    long fd = sc4(LX_openat, LX_AT_FDCWD, "/tmp/lxtest.txt", LX_O_RDWR | LX_O_CREAT | LX_O_TRUNC | LX_O_CLOEXEC, 0644);
    CHECKV(fd >= 3, fd);
    CHECKV(sc3(LX_write, fd, "linux abi\n", 10) == 10, 0);
    struct lx_iovec iov[2] = { { (uint64_t)(uintptr_t) "second ", 7 }, { (uint64_t)(uintptr_t) "line\n", 5 } };
    CHECKV(sc3(LX_writev, fd, iov, 2) == 12, 0);
    struct lx_stat st;
    CHECKV(sc2(LX_fstat, fd, &st) == 0, 0);
    CHECKV((st.st_mode & LX_S_IFMT) == LX_S_IFREG && st.st_size == 22 && st.st_nlink == 1, st.st_mode);
    CHECKV(sizeof(struct lx_stat) == LX_STAT_SIZE, sizeof(struct lx_stat));
    char buf[64];
    CHECKV(sc4(LX_pread64, fd, buf, 4, 6) == 4 && memeq(buf, "abi\n", 4), 0);
    CHECKV(sc3(LX_lseek, fd, 0, 0) == 0, 0);
    char b1[8], b2[16];
    struct lx_iovec riov[2] = { { (uint64_t)(uintptr_t)b1, 6 }, { (uint64_t)(uintptr_t)b2, 16 } };
    CHECKV(sc3(LX_readv, fd, riov, 2) == 22 && memeq(b1, "linux ", 6) && memeq(b2, "abi\nsecond line\n", 16), 0);
    CHECKV(sc3(LX_read, fd, buf, 8) == 0, 0);                  /* EOF */
    CHECKV(sc1(LX_close, fd) == 0, 0);
    CHECKV(sc1(LX_close, fd) == -9, 0);                        /* EBADF */
    CHECKV(sc4(LX_newfstatat, LX_AT_FDCWD, "/tmp/lxtest.txt", &st, 0) == 0 && st.st_size == 22, 0);
    /* statx returns the same fields as fstat/newfstatat for the file, with a
     * mask that reports exactly what the kernel supplies -- and not atime or
     * btime, which it does not record. */
    struct lx_statx sx;
    CHECKV(sizeof(struct lx_statx) == LX_STATX_BYTES, (long)sizeof(struct lx_statx));
    CHECKV(sc5(LX_statx, LX_AT_FDCWD, "/tmp/lxtest.txt", 0, LX_STATX_BASIC_STATS, &sx) == 0, 0);
    CHECKV(sx.stx_size == (uint64_t)st.st_size && sx.stx_nlink == st.st_nlink
           && sx.stx_mode == (uint16_t)st.st_mode, (long)sx.stx_size);
    CHECKV((sx.stx_mask & LX_STATX_SUPPORTED) == LX_STATX_SUPPORTED
           && (sx.stx_mask & (LX_STATX_ATIME | LX_STATX_BTIME)) == 0, (long)sx.stx_mask);
    /* The AT_EMPTY_PATH form on an open fd matches. */
    long sxfd = sc4(LX_openat, LX_AT_FDCWD, "/tmp/lxtest.txt", 0, 0);
    CHECKV(sxfd >= 3, sxfd);
    struct lx_statx sxe;
    CHECKV(sc5(LX_statx, sxfd, "", LX_AT_EMPTY_PATH, LX_STATX_BASIC_STATS, &sxe) == 0
           && sxe.stx_size == 22 && sxe.stx_ino == sx.stx_ino, (long)sxe.stx_size);
    CHECKV(sc1(LX_close, sxfd) == 0, 0);
    /* AT_FDCWD + empty path statx the current directory. */
    struct lx_statx sxc;
    CHECKV(sc5(LX_statx, LX_AT_FDCWD, "", LX_AT_EMPTY_PATH, LX_STATX_BASIC_STATS, &sxc) == 0
           && (sxc.stx_mode & LX_S_IFMT) == LX_S_IFDIR, (long)sxc.stx_mode);
    /* Invalid requests are rejected (-EINVAL) before the file is touched. */
    CHECKV(sc5(LX_statx, LX_AT_FDCWD, "/tmp/lxtest.txt", 0x6000, 0, &sxc) == -22, 0);       /* both sync flags */
    CHECKV(sc5(LX_statx, LX_AT_FDCWD, "/tmp/lxtest.txt", 0, 0x80000000L, &sxc) == -22, 0);  /* reserved mask bit */
    /* eventfd: a counter fd, readable while non-zero. */
    {
        uint64_t egot = 0, ev = 5;
        long efd = sc2(LX_eventfd2, 0, 0);
        CHECKV(efd >= 3, efd);
        struct lx_pollfd epf = { (int)efd, LX_POLLIN, 0 };
        CHECKV(sc3(LX_write, efd, &ev, 8) == 8, 0);
        CHECKV(lx_poll_ms(&epf, 1, 1000) == 1 && epf.revents == LX_POLLIN, epf.revents);
        CHECKV(sc3(LX_read, efd, &egot, 8) == 8 && egot == 5, (long)egot);
        epf.revents = 0;
        CHECKV(lx_poll_ms(&epf, 1, 0) == 0, 0);          /* drained: no longer readable */
        CHECKV(sc1(LX_close, efd) == 0, 0);
        /* EFD_SEMAPHORE: three reads of 1, then empty. */
        long sfd = sc2(LX_eventfd2, 3, LX_EFD_SEMAPHORE);
        CHECKV(sfd >= 3, sfd);
        for (int k = 0; k < 3; k++)
            CHECKV(sc3(LX_read, sfd, &egot, 8) == 8 && egot == 1, (long)egot);
        struct lx_pollfd spf = { (int)sfd, LX_POLLIN, 0 };
        CHECKV(lx_poll_ms(&spf, 1, 0) == 0, 0);
        CHECKV(sc1(LX_close, sfd) == 0, 0);
        /* EFD_NONBLOCK: read on empty is EAGAIN; the all-ones write is EINVAL. */
        long nfd = sc2(LX_eventfd2, 0, LX_EFD_NONBLOCK);
        CHECKV(nfd >= 3, nfd);
        CHECKV(sc3(LX_read, nfd, &egot, 8) == -11, 0);   /* -EAGAIN */
        uint64_t emax = (uint64_t)-1;
        CHECKV(sc3(LX_write, nfd, &emax, 8) == -22, 0);  /* -EINVAL */
        CHECKV(sc1(LX_close, nfd) == 0, 0);
        /* Near the ceiling: the counter saturates at UINT64_MAX-1. A write
         * that would carry it past that is refused (-EAGAIN here, a block
         * otherwise); writable readiness tracks the room, and a read makes
         * the refused write fit. */
        long ffd = sc2(LX_eventfd2, 0, LX_EFD_NONBLOCK);
        CHECKV(ffd >= 3, ffd);
        uint64_t near = (uint64_t)-1 - 2;                /* leaves room for exactly 1 */
        CHECKV(sc3(LX_write, ffd, &near, 8) == 8, 0);
        struct lx_pollfd fpf = { (int)ffd, LX_POLLOUT, 0 };
        CHECKV(lx_poll_ms(&fpf, 1, 0) == 1 && (fpf.revents & LX_POLLOUT), fpf.revents);
        uint64_t two = 2, one = 1;
        CHECKV(sc3(LX_write, ffd, &two, 8) == -11, 0);   /* -EAGAIN: no room for 2 */
        CHECKV(sc3(LX_write, ffd, &one, 8) == 8, 0);     /* 1 fits: now at the ceiling */
        fpf.revents = 0;
        CHECKV(lx_poll_ms(&fpf, 1, 0) == 0, 0);          /* full: no longer writable */
        CHECKV(sc3(LX_read, ffd, &egot, 8) == 8 && egot == (uint64_t)-1 - 1, (long)egot);
        CHECKV(sc3(LX_write, ffd, &two, 8) == 8, 0);     /* the read made room: the write proceeds */
        CHECKV(sc1(LX_close, ffd) == 0, 0);
#ifdef LX_eventfd
        /* the older x86-64 number: an initial value, no flags. */
        long ofd = sc1(LX_eventfd, 7);
        CHECKV(ofd >= 3 && sc3(LX_read, ofd, &egot, 8) == 8 && egot == 7, (long)egot);
        CHECKV(sc1(LX_close, ofd) == 0, 0);
#endif
    }
    /* timerfd: a waitable timer fd (docs/audit/next-subsystem-timerfd.md). */
    {
        struct lx_itimerspec its, cur;
        struct lx_pollfd tp;
        uint64_t tc;
        /* one-shot, non-blocking: a ~40 ms relative timer becomes readable,
         * the 8-byte read returns a count >= 1, a second read is -EAGAIN. */
        long tfd = sc2(LX_timerfd_create, LX_CLOCK_MONOTONIC, LX_TFD_NONBLOCK);
        CHECKV(tfd >= 3, tfd);
        __builtin_memset(&its, 0, sizeof(its));
        its.it_value.tv_nsec = 40000000;              /* 40 ms */
        CHECKV(sc4(LX_timerfd_settime, tfd, 0, &its, 0) == 0, 0);
        tp = (struct lx_pollfd){ (int)tfd, LX_POLLIN, 0 };
        CHECKV(lx_poll_ms(&tp, 1, 2000) == 1 && tp.revents == LX_POLLIN, tp.revents);
        tc = 0;
        CHECKV(sc3(LX_read, tfd, &tc, 8) == 8 && tc >= 1, (long)tc);
        CHECKV(sc3(LX_read, tfd, &tc, 8) == -11, 0);  /* drained -> -EAGAIN */

        /* gettime on an armed one-shot: remaining in (0, 1 s], the interval as
         * set; then a disarming settime reads 0/0 and is not readable. */
        __builtin_memset(&its, 0, sizeof(its));
        its.it_value.tv_sec = 1;                      /* 1 s */
        its.it_interval.tv_nsec = 500000000;          /* 500 ms */
        CHECKV(sc4(LX_timerfd_settime, tfd, 0, &its, 0) == 0, 0);
        __builtin_memset(&cur, 0, sizeof(cur));
        CHECKV(sc2(LX_timerfd_gettime, tfd, &cur) == 0, 0);
        uint64_t rem = (uint64_t)cur.it_value.tv_sec * 1000000000ull + (uint64_t)cur.it_value.tv_nsec;
        uint64_t itv = (uint64_t)cur.it_interval.tv_sec * 1000000000ull + (uint64_t)cur.it_interval.tv_nsec;
        CHECKV(rem > 0 && rem <= 1000000000ull, (long)rem);
        CHECKV(itv == 500000000ull, (long)itv);
        /* disarm with a non-zero it_interval: it_value == 0 disarms, but the
         * interval is kept and gettime still reports it (Linux). */
        __builtin_memset(&its, 0, sizeof(its));
        its.it_interval.tv_nsec = 250000000;          /* 250 ms, kept on disarm */
        CHECKV(sc4(LX_timerfd_settime, tfd, 0, &its, 0) == 0, 0);
        __builtin_memset(&cur, 0, sizeof(cur));
        CHECKV(sc2(LX_timerfd_gettime, tfd, &cur) == 0, 0);
        CHECKV(cur.it_value.tv_sec == 0 && cur.it_value.tv_nsec == 0, 0);
        uint64_t ditv = (uint64_t)cur.it_interval.tv_sec * 1000000000ull + (uint64_t)cur.it_interval.tv_nsec;
        CHECKV(ditv == 250000000ull, (long)ditv);     /* interval kept across the disarm */
        tp = (struct lx_pollfd){ (int)tfd, LX_POLLIN, 0 };
        CHECKV(lx_poll_ms(&tp, 1, 0) == 0, 0);        /* disarmed: not readable */

        /* periodic: accumulate expirations across reads until the count
         * reaches 2 under a generous deadline -- a wait for the count, never
         * N fires after a fixed sleep. */
        __builtin_memset(&its, 0, sizeof(its));
        its.it_value.tv_nsec = 20000000;              /* 20 ms */
        its.it_interval.tv_nsec = 20000000;           /* 20 ms */
        CHECKV(sc4(LX_timerfd_settime, tfd, 0, &its, 0) == 0, 0);
        uint64_t total = 0;
        for (int i = 0; i < 40 && total < 2; i++) {   /* up to ~2 s */
            struct lx_pollfd pp = { (int)tfd, LX_POLLIN, 0 };
            if (lx_poll_ms(&pp, 1, 100) == 1) {
                uint64_t c = 0;
                if (sc3(LX_read, tfd, &c, 8) == 8)
                    total += c;
            }
        }
        CHECKV(total >= 2, (long)total);
        CHECKV(sc1(LX_close, tfd) == 0, 0);

        /* absolute: a monotonic deadline ~40 ms ahead becomes readable; then a
         * deadline already in the past fires at once, not disarm. */
        long afd = sc2(LX_timerfd_create, LX_CLOCK_MONOTONIC, LX_TFD_NONBLOCK);
        CHECKV(afd >= 3, afd);
        struct lx_timespec now;
        CHECKV(sc2(LX_clock_gettime, LX_CLOCK_MONOTONIC, &now) == 0, 0);
        uint64_t now_ns = (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
        uint64_t at = now_ns + 40000000;              /* +40 ms */
        __builtin_memset(&its, 0, sizeof(its));
        its.it_value.tv_sec = (int64_t)(at / 1000000000ull);
        its.it_value.tv_nsec = (int64_t)(at % 1000000000ull);
        CHECKV(sc4(LX_timerfd_settime, afd, LX_TFD_TIMER_ABSTIME, &its, 0) == 0, 0);
        tp = (struct lx_pollfd){ (int)afd, LX_POLLIN, 0 };
        CHECKV(lx_poll_ms(&tp, 1, 2000) == 1 && tp.revents == LX_POLLIN, tp.revents);
        tc = 0;
        CHECKV(sc3(LX_read, afd, &tc, 8) == 8 && tc >= 1, (long)tc);
        uint64_t past = now_ns - 10000000;            /* 10 ms ago: a past, non-zero deadline */
        __builtin_memset(&its, 0, sizeof(its));
        its.it_value.tv_sec = (int64_t)(past / 1000000000ull);
        its.it_value.tv_nsec = (int64_t)(past % 1000000000ull);
        CHECKV(sc4(LX_timerfd_settime, afd, LX_TFD_TIMER_ABSTIME, &its, 0) == 0, 0);
        tp = (struct lx_pollfd){ (int)afd, LX_POLLIN, 0 };
        CHECKV(lx_poll_ms(&tp, 1, 1000) == 1 && tp.revents == LX_POLLIN, tp.revents);
        CHECKV(sc1(LX_close, afd) == 0, 0);

        /* absolute against CLOCK_REALTIME: the wall-clock path converts the
         * deadline against clock_realtime_ns, so a ~40 ms-ahead REALTIME
         * deadline becomes readable within a bound. */
        long rfd = sc2(LX_timerfd_create, LX_CLOCK_REALTIME, LX_TFD_NONBLOCK);
        CHECKV(rfd >= 3, rfd);
        struct lx_timespec rnow;
        CHECKV(sc2(LX_clock_gettime, LX_CLOCK_REALTIME, &rnow) == 0, 0);
        uint64_t rat = (uint64_t)rnow.tv_sec * 1000000000ull + (uint64_t)rnow.tv_nsec + 40000000;
        __builtin_memset(&its, 0, sizeof(its));
        its.it_value.tv_sec = (int64_t)(rat / 1000000000ull);
        its.it_value.tv_nsec = (int64_t)(rat % 1000000000ull);
        CHECKV(sc4(LX_timerfd_settime, rfd, LX_TFD_TIMER_ABSTIME, &its, 0) == 0, 0);
        tp = (struct lx_pollfd){ (int)rfd, LX_POLLIN, 0 };
        CHECKV(lx_poll_ms(&tp, 1, 2000) == 1 && tp.revents == LX_POLLIN, tp.revents);
        tc = 0;
        CHECKV(sc3(LX_read, rfd, &tc, 8) == 8 && tc >= 1, (long)tc);
        CHECKV(sc1(LX_close, rfd) == 0, 0);

        /* errors: a bad clockid, and a settime on a non-timerfd fd. */
        CHECKV(sc2(LX_timerfd_create, 99, 0) == -22, 0);   /* -EINVAL */
        long ntf = sc2(LX_eventfd2, 0, 0);
        CHECKV(ntf >= 3, ntf);
        __builtin_memset(&its, 0, sizeof(its));
        its.it_value.tv_nsec = 10000000;
        CHECKV(sc4(LX_timerfd_settime, ntf, 0, &its, 0) == -22, 0);   /* not a timerfd */
        CHECKV(sc1(LX_close, ntf) == 0, 0);
    }
    /* memfd: an anonymous memory-backed file (docs/audit/next-subsystem-memfd.md). */
    {
        char mp[64], mb[64];
        for (int i = 0; i < 64; i++)
            mp[i] = (char)(i + 1);
        long mfd = sc2(LX_memfd_create, "lxtest", LX_MFD_CLOEXEC);
        CHECKV(mfd >= 3, mfd);
        /* starts empty; ftruncate sizes it, fstat sees the size */
        CHECKV(sc2(LX_ftruncate, mfd, 4096) == 0, 0);
        struct lx_stat mst;
        CHECKV(sc2(LX_fstat, mfd, &mst) == 0 && mst.st_size == 4096 &&
               (mst.st_mode & LX_S_IFMT) == LX_S_IFREG, (long)mst.st_size);
        /* write a pattern and read it back */
        CHECKV(sc4(LX_pwrite64, mfd, mp, 64, 0) == 64, 0);
        __builtin_memset(mb, 0, sizeof(mb));
        CHECKV(sc4(LX_pread64, mfd, mb, 64, 0) == 64 && memeq(mb, mp, 64), 0);
        /* mmap MAP_SHARED: the pattern is visible through the mapping, a store
         * through the mapping is visible via pread, and a pwrite is visible
         * through the mapping -- memory-backed and shareable */
        long mm = sc6(LX_mmap, 0, 4096, LX_PROT_READ | LX_PROT_WRITE, LX_MAP_SHARED, mfd, 0);
        CHECKV(mm > 0 && (mm & 0xfff) == 0, mm);
        volatile unsigned char *mv = (unsigned char *)mm;
        CHECK(mv[0] == 1 && mv[63] == 64);
        mv[0] = 0xAA;
        CHECKV(sc4(LX_pread64, mfd, mb, 1, 0) == 1 && (unsigned char)mb[0] == 0xAA, (long)(unsigned char)mb[0]);
        unsigned char one = 0x55;
        CHECKV(sc4(LX_pwrite64, mfd, &one, 1, 100) == 1, 0);
        CHECK(mv[100] == 0x55);
        CHECKV(sc2(LX_munmap, mm, 4096) == 0, 0);
        /* a second memfd is an independent file */
        long mfd2 = sc2(LX_memfd_create, "lxtest2", 0);
        CHECKV(mfd2 >= 3 && mfd2 != mfd, mfd2);
        unsigned char two = 0x11;
        CHECKV(sc4(LX_pwrite64, mfd2, &two, 1, 0) == 1, 0);
        CHECKV(sc4(LX_pread64, mfd, mb, 1, 0) == 1 && (unsigned char)mb[0] == 0xAA, 0);   /* mfd unchanged */
        CHECKV(sc1(LX_close, mfd2) == 0, 0);
        /* errors: unsupported flags, an overlong name, and a negative length */
        CHECKV(sc2(LX_memfd_create, "x", LX_MFD_ALLOW_SEALING) == -22, 0);   /* -EINVAL */
        CHECKV(sc2(LX_memfd_create, "x", 0x8) == -22, 0);                    /* unknown flag */
        char longname[300];
        for (int i = 0; i < 299; i++)
            longname[i] = 'a';
        longname[299] = '\0';
        CHECKV(sc2(LX_memfd_create, longname, 0) == -22, 0);                 /* > 249 bytes -> EINVAL */
        CHECKV(sc2(LX_ftruncate, mfd, -1) == -22, 0);                        /* negative length */
        CHECKV(sc1(LX_close, mfd) == 0, 0);
        /* lifetime: create + size + write + close in a loop, far more times
         * than the ramfs page budget (16384 pages), does not run out of
         * memory -- each file and its page is freed on the last close. A leaked
         * pin would exhaust the budget and the pwrite would start to fail. */
        int ok = 1;
        for (int i = 0; i < 20000 && ok; i++) {
            long t = sc2(LX_memfd_create, "loop", 0);
            if (t < 3) { ok = 0; break; }
            unsigned char z = (unsigned char)i;
            ok = sc2(LX_ftruncate, t, 4096) == 0 && sc4(LX_pwrite64, t, &z, 1, 0) == 1;
            sc1(LX_close, t);
        }
        CHECKV(ok, 0);
    }
    /* epoll (docs/audit/next-subsystem-epoll.md): level- and edge-triggered
     * (EPOLLET, docs/audit/next-subsystem-epollet.md). Uses epoll_pwait (wired
     * on both arches) with a NULL sigmask. */
    {
        long ep = sc1(LX_epoll_create1, LX_EPOLL_CLOEXEC);
        CHECKV(ep >= 3, ep);
        long efd = sc2(LX_eventfd2, 0, 0);
        CHECKV(efd >= 3, efd);
        struct lx_epoll_event ee = { .events = LX_EPOLLIN, .data = 0xCAFE };
        struct lx_epoll_event out[4];
        uint64_t one = 1, sink;
        CHECKV(sc4(LX_epoll_ctl, ep, LX_EPOLL_CTL_ADD, efd, &ee) == 0, 0);
        /* not ready: a 0-timeout wait returns 0 */
        CHECKV(sc6(LX_epoll_pwait, ep, out, 4, 0, 0, 0) == 0, 0);
        /* write it -> reported with its data; drain -> not ready again */
        CHECKV(sc3(LX_write, efd, &one, 8) == 8, 0);
        long got = sc6(LX_epoll_pwait, ep, out, 4, 1000, 0, 0);
        CHECKV(got == 1 && out[0].data == 0xCAFE && (out[0].events & LX_EPOLLIN), (long)got);
        CHECKV(sc3(LX_read, efd, &sink, 8) == 8, 0);
        CHECKV(sc6(LX_epoll_pwait, ep, out, 4, 0, 0, 0) == 0, 0);

        /* multi: add a timerfd too; arm only it; the bounded wait returns the
         * timerfd entry */
        long tfd = sc2(LX_timerfd_create, LX_CLOCK_MONOTONIC, LX_TFD_NONBLOCK);
        CHECKV(tfd >= 3, tfd);
        struct lx_epoll_event te = { .events = LX_EPOLLIN, .data = 0x7 };
        CHECKV(sc4(LX_epoll_ctl, ep, LX_EPOLL_CTL_ADD, tfd, &te) == 0, 0);
        struct lx_itimerspec its;
        __builtin_memset(&its, 0, sizeof(its));
        its.it_value.tv_nsec = 40000000;          /* 40 ms */
        CHECKV(sc4(LX_timerfd_settime, tfd, 0, &its, 0) == 0, 0);
        got = sc6(LX_epoll_pwait, ep, out, 4, 2000, 0, 0);
        CHECKV(got == 1 && out[0].data == 0x7, (long)got);
        uint64_t exp;
        CHECKV(sc3(LX_read, tfd, &exp, 8) == 8, 0);   /* drain the timerfd */
        CHECKV(sc4(LX_epoll_ctl, ep, LX_EPOLL_CTL_DEL, tfd, 0) == 0, 0);
        CHECKV(sc1(LX_close, tfd) == 0, 0);

        /* timeout: a registered-but-not-ready member, finite wait returns 0 */
        CHECKV(sc6(LX_epoll_pwait, ep, out, 4, 50, 0, 0) == 0, 0);

        /* MOD to events 0: not reported even once written; MOD back restores it */
        struct lx_epoll_event z = { .events = 0, .data = 0xCAFE };
        CHECKV(sc4(LX_epoll_ctl, ep, LX_EPOLL_CTL_MOD, efd, &z) == 0, 0);
        CHECKV(sc3(LX_write, efd, &one, 8) == 8, 0);
        CHECKV(sc6(LX_epoll_pwait, ep, out, 4, 0, 0, 0) == 0, 0);
        CHECKV(sc4(LX_epoll_ctl, ep, LX_EPOLL_CTL_MOD, efd, &ee) == 0, 0);
        CHECKV(sc6(LX_epoll_pwait, ep, out, 4, 1000, 0, 0) == 1, 0);
        CHECKV(sc3(LX_read, efd, &sink, 8) == 8, 0);

        /* oneshot: reported once, then not until MOD re-arms */
        struct lx_epoll_event os = { .events = LX_EPOLLIN | LX_EPOLLONESHOT, .data = 0x9 };
        CHECKV(sc4(LX_epoll_ctl, ep, LX_EPOLL_CTL_MOD, efd, &os) == 0, 0);
        CHECKV(sc3(LX_write, efd, &one, 8) == 8, 0);
        CHECKV(sc6(LX_epoll_pwait, ep, out, 4, 1000, 0, 0) == 1 && out[0].data == 0x9, 0);
        CHECKV(sc6(LX_epoll_pwait, ep, out, 4, 0, 0, 0) == 0, 0);   /* still readable, but disabled */
        CHECKV(sc4(LX_epoll_ctl, ep, LX_EPOLL_CTL_MOD, efd, &ee) == 0, 0);
        CHECKV(sc6(LX_epoll_pwait, ep, out, 4, 0, 0, 0) == 1, 0);   /* re-armed, still readable */
        CHECKV(sc3(LX_read, efd, &sink, 8) == 8, 0);

        /* edge-triggered (EPOLLET): reported only on a transition into
         * readiness, not on every wait while it stays ready. efd (above) is
         * drained and not ready, so it does not interfere. Deterministic
         * timeout==0 polls plus one finite-deadline wait -- no timing. */
        long eet = sc2(LX_eventfd2, 0, 0);
        CHECKV(eet >= 3, eet);
        struct lx_epoll_event ete = { .events = LX_EPOLLIN | LX_EPOLLET, .data = 0xED };
        CHECKV(sc4(LX_epoll_ctl, ep, LX_EPOLL_CTL_ADD, eet, &ete) == 0, 0);   /* accepted; was -EINVAL */
        CHECKV(sc3(LX_write, eet, &one, 8) == 8, 0);
        got = sc6(LX_epoll_pwait, ep, out, 4, 0, 0, 0);
        CHECKV(got == 1 && out[0].data == 0xED, (long)got);                  /* one edge */
        CHECKV(sc6(LX_epoll_pwait, ep, out, 4, 0, 0, 0) == 0, 0);            /* still ready, no new edge */
        CHECKV(sc3(LX_read, eet, &sink, 8) == 8, 0);                         /* drain */
        CHECKV(sc6(LX_epoll_pwait, ep, out, 4, 0, 0, 0) == 0, 0);            /* drained: not ready */
        CHECKV(sc3(LX_write, eet, &one, 8) == 8, 0);
        CHECKV(sc6(LX_epoll_pwait, ep, out, 4, 1000, 0, 0) == 1, 0);         /* fresh edge, finite deadline */
        CHECKV(sc6(LX_epoll_pwait, ep, out, 4, 0, 0, 0) == 0, 0);            /* suppressed again */
        /* level contrast: MOD the same fd to level; still ready (not drained)
         * -> reported on every poll, unlike the edge member above */
        struct lx_epoll_event etl = { .events = LX_EPOLLIN, .data = 0xED };
        CHECKV(sc4(LX_epoll_ctl, ep, LX_EPOLL_CTL_MOD, eet, &etl) == 0, 0);
        CHECKV(sc6(LX_epoll_pwait, ep, out, 4, 0, 0, 0) == 1, 0);            /* level: ready -> reported */
        CHECKV(sc6(LX_epoll_pwait, ep, out, 4, 0, 0, 0) == 1, 0);            /* level: still reported */
        CHECKV(sc3(LX_read, eet, &sink, 8) == 8, 0);
        CHECKV(sc4(LX_epoll_ctl, ep, LX_EPOLL_CTL_DEL, eet, 0) == 0, 0);
        CHECKV(sc1(LX_close, eet) == 0, 0);
        lx_puts("LXEPOLLET: edge-triggered reported once, re-arms on drain+refill; level repeats\n");

        /* errors */
        CHECKV(sc4(LX_epoll_ctl, ep, LX_EPOLL_CTL_ADD, efd, &ee) == -17, 0);   /* EEXIST */
        long efd2 = sc2(LX_eventfd2, 0, 0);
        CHECKV(sc4(LX_epoll_ctl, ep, LX_EPOLL_CTL_MOD, efd2, &ee) == -2, 0);   /* ENOENT */
        CHECKV(sc4(LX_epoll_ctl, ep, LX_EPOLL_CTL_DEL, efd2, 0) == -2, 0);     /* ENOENT */
        CHECKV(sc4(LX_epoll_ctl, ep, LX_EPOLL_CTL_ADD, ep, &ee) == -22, 0);    /* nesting -> EINVAL */
        CHECKV(sc6(LX_epoll_pwait, ep, out, 0, 0, 0, 0) == -22, 0);            /* maxevents 0 -> EINVAL */
        CHECKV(sc1(LX_close, efd2) == 0, 0);

        /* DEL: no longer reported */
        CHECKV(sc4(LX_epoll_ctl, ep, LX_EPOLL_CTL_DEL, efd, 0) == 0, 0);
        CHECKV(sc3(LX_write, efd, &one, 8) == 8, 0);
        CHECKV(sc6(LX_epoll_pwait, ep, out, 4, 0, 0, 0) == 0, 0);
        CHECKV(sc1(LX_close, efd) == 0, 0);
        CHECKV(sc1(LX_close, ep) == 0, 0);
#ifdef LX_epoll_create
        /* x86-64 legacy epoll_create + epoll_wait */
        long lep = sc1(LX_epoll_create, 1);
        CHECKV(lep >= 3, lep);
        long lfd = sc2(LX_eventfd2, 0, 0);
        struct lx_epoll_event le = { .events = LX_EPOLLIN, .data = 0x5 };
        CHECKV(sc4(LX_epoll_ctl, lep, LX_EPOLL_CTL_ADD, lfd, &le) == 0, 0);
        CHECKV(sc3(LX_write, lfd, &one, 8) == 8, 0);
        CHECKV(sc4(LX_epoll_wait, lep, out, 4, 1000) == 1 && out[0].data == 0x5, 0);
        CHECKV(sc1(LX_epoll_create, 0) == -22, 0);   /* size <= 0 -> EINVAL */
        CHECKV(sc1(LX_close, lfd) == 0 && sc1(LX_close, lep) == 0, 0);
#endif
    }
    /* sysinfo (docs/audit/next-subsystem-sysinfo.md) */
    {
        struct lx_sysinfo si;
        __builtin_memset(&si, 0xAB, sizeof(si));   /* poison, so a non-write is caught */
        CHECKV(sc1(LX_sysinfo, &si) == 0, 0);
        CHECKV(sizeof(struct lx_sysinfo) == 112, (long)sizeof(struct lx_sysinfo));   /* LP64 layout */
        CHECKV(si.mem_unit == 1, (long)si.mem_unit);
        CHECKV(si.uptime >= 0 && si.uptime < 86400, (long)si.uptime);   /* plausible seconds, not poison */
        CHECKV(si.totalram > 0 && si.freeram < si.totalram, (long)(si.totalram >> 20));   /* some RAM is always in use */
        CHECKV(si.procs >= 1, (long)si.procs);
        /* every field with no backing reads zero (the struct was poisoned) */
        CHECKV(si.loads[0] == 0 && si.loads[1] == 0 && si.loads[2] == 0, 0);
        CHECKV(si.sharedram == 0 && si.bufferram == 0, 0);
        CHECKV(si.totalswap == 0 && si.freeswap == 0, 0);
        CHECKV(si.totalhigh == 0 && si.freehigh == 0 && si.pad == 0, 0);
    }
    /* mremap (docs/audit/next-subsystem-mremap.md): in-place anon resize.
     * FIXED placement keeps the gaps deterministic. */
    {
        /* grow in place: the sentinel survives and the new page is usable */
        long gp = sc6(LX_mmap, 0x32000000000UL, 4096, LX_PROT_READ | LX_PROT_WRITE,
                      LX_MAP_PRIVATE | LX_MAP_ANONYMOUS | LX_MAP_FIXED, -1, 0);
        CHECKV(gp == 0x32000000000L, gp);
        volatile unsigned int *gw = (unsigned int *)gp;
        gw[0] = 0xA5A5;
        CHECKV(sc5(LX_mremap, gp, 4096, 8192, 0, 0) == gp, 0);   /* same address */
        CHECK(gw[0] == 0xA5A5);                                  /* sentinel survived */
        volatile unsigned int *gw2 = (unsigned int *)(gp + 4096);
        gw2[0] = 0x1234;
        CHECK(gw2[0] == 0x1234);                                 /* new page usable */
        /* shrink: the freed page is really unmapped (mprotect -> -ENOMEM) */
        CHECKV(sc5(LX_mremap, gp, 8192, 4096, 0, 0) == gp, 0);
        CHECK(gw[0] == 0xA5A5);                                  /* first page intact */
        CHECKV(sc3(LX_mprotect, gp + 4096, 4096, LX_PROT_READ) == -12, 0);   /* -ENOMEM: unmapped */
        CHECKV(sc2(LX_munmap, gp, 4096) == 0, 0);
        /* blocked grow: a different-prot mapping right after -> -ENOMEM, even MAYMOVE */
        long ba = sc6(LX_mmap, 0x31000000000UL, 4096, LX_PROT_READ | LX_PROT_WRITE,
                      LX_MAP_PRIVATE | LX_MAP_ANONYMOUS | LX_MAP_FIXED, -1, 0);
        CHECKV(ba == 0x31000000000L, ba);
        long bb = sc6(LX_mmap, ba + 4096, 4096, LX_PROT_READ,
                      LX_MAP_PRIVATE | LX_MAP_ANONYMOUS | LX_MAP_FIXED, -1, 0);
        CHECKV(bb == ba + 4096, bb);
        CHECKV(sc5(LX_mremap, ba, 4096, 8192, 0, 0) == -12, 0);                    /* -ENOMEM */
        CHECKV(sc5(LX_mremap, ba, 4096, 8192, LX_MREMAP_MAYMOVE, 0) == -12, 0);    /* never moves */
        CHECKV(sc2(LX_munmap, ba, 4096) == 0 && sc2(LX_munmap, ba + 4096, 4096) == 0, 0);
        /* errors: FIXED, a non-page-aligned old_addr, and an unmapped address */
        CHECKV(sc5(LX_mremap, 0x31000000000UL, 4096, 8192, LX_MREMAP_FIXED, 0x33000000000UL) == -22, 0);
        CHECKV(sc5(LX_mremap, 0x32000000001UL, 4096, 8192, 0, 0) == -22, 0);   /* unaligned */
        CHECKV(sc5(LX_mremap, 0x40000000000UL, 4096, 8192, 0, 0) == -14, 0);   /* unmapped -> -EFAULT */
        /* an equal-size request still validates the mapping: unmapped is
         * -EFAULT, not a bogus success */
        CHECKV(sc5(LX_mremap, 0x40000000000UL, 4096, 4096, 0, 0) == -14, 0);   /* equal size, unmapped */
        long eq = sc6(LX_mmap, 0x34000000000UL, 4096, LX_PROT_READ | LX_PROT_WRITE,
                      LX_MAP_PRIVATE | LX_MAP_ANONYMOUS | LX_MAP_FIXED, -1, 0);
        CHECKV(eq == 0x34000000000L, eq);
        CHECKV(sc5(LX_mremap, eq, 4096, 4096, 0, 0) == eq, 0);   /* equal size, live -> same address */
        CHECKV(sc2(LX_munmap, eq, 4096) == 0, 0);
    }
    /* System V shared memory (docs/audit/next-subsystem-shm.md): shmget makes
     * an anonymous segment, shmat maps it MAP_SHARED, shmdt/IPC_RMID tear it
     * down with the "freed on the last detach" lifecycle. */
    {
        int sid = (int)sc3(LX_shmget, LX_IPC_PRIVATE, 8192, LX_IPC_CREAT | 0600);
        CHECKV(sid >= 0, sid);
        /* attach; a write is visible through a SECOND attach of the same id */
        long s1 = sc3(LX_shmat, sid, 0, 0);
        CHECKV(s1 > 0, s1);
        volatile unsigned int *q1 = (unsigned int *)s1;
        q1[0] = 0x5151;
        long s2 = sc3(LX_shmat, sid, 0, 0);
        CHECKV(s2 > 0, s2);
        CHECK(s2 != s1);                         /* two distinct mappings */
        volatile unsigned int *q2 = (unsigned int *)s2;
        CHECK(q2[0] == 0x5151);                  /* shared: the second sees it */
        q2[1] = 0x6262;
        CHECK(q1[1] == 0x6262);                  /* and the write goes both ways */
        /* IPC_STAT: the size and the live attach count (2) */
        struct lx_shmid_ds ds;
        CHECKV(sc3(LX_shmctl, sid, LX_IPC_STAT, &ds) == 0, 0);
        CHECK(ds.shm_segsz == 8192);
        CHECK(ds.shm_nattch == 2);
        /* attach at an exact address: lands there, reads the sentinel */
        long sf = sc3(LX_shmat, sid, 0x35000000000UL, 0);
        CHECKV(sf == 0x35000000000L, sf);
        CHECK(((volatile unsigned int *)sf)[0] == 0x5151);
        CHECKV(sc3(LX_shmat, sid, 0x35000000000UL, 0) == -12, 0);   /* occupied -> -ENOMEM */
        CHECKV(sc3(LX_shmat, sid, 0x35000000001UL, 0) == -22, 0);   /* unaligned -> -EINVAL */
        CHECKV(sc1(LX_shmdt, sf) == 0, 0);
        /* shmdt really unmaps; a second detach of the same address is -EINVAL */
        CHECKV(sc1(LX_shmdt, s2) == 0, 0);
        CHECKV(sc3(LX_mprotect, s2, 4096, LX_PROT_READ) == -12, 0);   /* -ENOMEM: unmapped */
        CHECKV(sc1(LX_shmdt, s2) == -22, 0);
        /* IPC_RMID with s1 still attached: the live mapping still reads its
         * sentinel (not freed at removal), but the id is gone for new ops */
        CHECKV(sc3(LX_shmctl, sid, LX_IPC_RMID, 0) == 0, 0);
        CHECK(q1[0] == 0x5151);
        CHECKV(sc3(LX_shmat, sid, 0, 0) == -22, 0);               /* no new attach */
        CHECKV(sc3(LX_shmctl, sid, LX_IPC_STAT, &ds) == -22, 0);  /* id gone */
        CHECKV(sc1(LX_shmdt, s1) == 0, 0);                        /* last detach frees it */
        CHECKV(sc3(LX_shmat, sid, 0, 0) == -22, 0);               /* still gone */
        /* errors: a bad id, and IPC_CREAT|IPC_EXCL of an existing key */
        CHECKV(sc3(LX_shmat, 999999, 0, 0) == -22, 0);
        int kid = (int)sc3(LX_shmget, 0x5109, 4096, LX_IPC_CREAT | 0600);
        CHECKV(kid >= 0, kid);
        CHECKV(sc3(LX_shmget, 0x5109, 4096, LX_IPC_CREAT | LX_IPC_EXCL | 0600) == -17, 0);   /* -EEXIST */
        CHECKV(sc3(LX_shmctl, kid, LX_IPC_RMID, 0) == 0, 0);
        /* SHM_RDONLY: a read-only attach's ceiling bars mprotect from adding write */
        int rid = (int)sc3(LX_shmget, LX_IPC_PRIVATE, 4096, LX_IPC_CREAT | 0600);
        CHECKV(rid >= 0, rid);
        long ro = sc3(LX_shmat, rid, 0, LX_SHM_RDONLY);
        CHECKV(ro > 0, ro);
        CHECKV(sc3(LX_mprotect, ro, 4096, LX_PROT_READ | LX_PROT_WRITE) == -13, 0);   /* -EACCES */
        CHECKV(sc1(LX_shmdt, ro) == 0, 0);
        CHECKV(sc3(LX_shmctl, rid, LX_IPC_RMID, 0) == 0, 0);
        /* IPC_STAT reports the REQUESTED size, not the page-rounded one */
        int bid = (int)sc3(LX_shmget, LX_IPC_PRIVATE, 100, LX_IPC_CREAT | 0600);
        CHECKV(bid >= 0, bid);
        CHECKV(sc3(LX_shmctl, bid, LX_IPC_STAT, &ds) == 0, 0);
        CHECK(ds.shm_segsz == 100);
        CHECKV(sc3(LX_shmctl, bid, LX_IPC_RMID, 0) == 0, 0);
        /* an unsupported flag (SHM_HUGETLB) is rejected, not silently honoured */
        CHECKV(sc3(LX_shmget, LX_IPC_PRIVATE, 4096, LX_IPC_CREAT | 04000 | 0600) == -22, 0);
        /* shmdt detaches only the segment's own pages: a mapping the program
         * put over a detached attach is left alone */
        int xid = (int)sc3(LX_shmget, LX_IPC_PRIVATE, 4096, LX_IPC_CREAT | 0600);
        CHECKV(xid >= 0, xid);
        long xa = sc3(LX_shmat, xid, 0, 0);
        CHECKV(xa > 0, xa);
        CHECKV(sc2(LX_munmap, xa, 4096) == 0, 0);   /* drop the attach's mapping by hand */
        long rep = sc6(LX_mmap, xa, 4096, LX_PROT_READ | LX_PROT_WRITE,
                       LX_MAP_PRIVATE | LX_MAP_ANONYMOUS | LX_MAP_FIXED, -1, 0);
        CHECKV(rep == xa, rep);                     /* foreign anon at the same address */
        ((volatile unsigned int *)rep)[0] = 0x7777;
        CHECKV(sc1(LX_shmdt, xa) == 0, 0);          /* must NOT tear the foreign mapping down */
        CHECK(((volatile unsigned int *)rep)[0] == 0x7777);
        CHECKV(sc2(LX_munmap, rep, 4096) == 0, 0);
        CHECKV(sc3(LX_shmctl, xid, LX_IPC_RMID, 0) == 0, 0);
        /* a partial self-unmap of the MIDDLE page still lets shmdt free the
         * attach's remaining pieces (both the first and third page) */
        int pid2 = (int)sc3(LX_shmget, LX_IPC_PRIVATE, 12288, LX_IPC_CREAT | 0600);
        CHECKV(pid2 >= 0, pid2);
        long pa = sc3(LX_shmat, pid2, 0, 0);
        CHECKV(pa > 0, pa);
        CHECKV(sc2(LX_munmap, pa + 4096, 4096) == 0, 0);   /* drop the middle page by hand */
        CHECKV(sc1(LX_shmdt, pa) == 0, 0);                 /* detaches the first AND third pages */
        CHECKV(sc3(LX_mprotect, pa, 4096, LX_PROT_READ) == -12, 0);          /* page 1 unmapped */
        CHECKV(sc3(LX_mprotect, pa + 8192, 4096, LX_PROT_READ) == -12, 0);   /* page 3 unmapped */
        CHECKV(sc3(LX_shmctl, pid2, LX_IPC_RMID, 0) == 0, 0);
        lx_puts("LXSHM: shmget/at/dt/ctl: shared, fixed, stat, rmid, perms, rdonly, partial-dt\n");
    }
    /* signalfd (docs/audit/next-subsystem-signalfd.md): block signals, read
     * them as a file. */
    {
        /* block SIGUSR1 (10) and SIGCHLD (17) so they queue for the fd;
         * save the prior mask to restore before the later signal tests run */
        unsigned long blk = (1UL << (10 - 1)) | (1UL << (17 - 1));
        unsigned long saved_mask = 0;
        CHECKV(sc4(LX_rt_sigprocmask, LX_SIG_BLOCK, &blk, &saved_mask, 8) == 0, 0);
        /* a signalfd for SIGUSR1; a raised SIGUSR1 reads back */
        unsigned long m1 = 1UL << (10 - 1);
        long sfd = sc4(LX_signalfd4, -1, &m1, 8, 0);
        CHECKV(sfd >= 0, sfd);
        CHECKV(sc2(LX_kill, sc0(LX_getpid), 10) == 0, 0);
        struct lx_signalfd_siginfo ssi;
        CHECKV(sc3(LX_read, sfd, &ssi, sizeof(ssi)) == (long)sizeof(ssi), 0);
        CHECK(ssi.ssi_signo == 10);
        /* nonblock: nothing pending now -> -EAGAIN */
        long sfn = sc4(LX_signalfd4, -1, &m1, 8, LX_SFD_NONBLOCK);
        CHECKV(sfn >= 0, sfn);
        CHECKV(sc3(LX_read, sfn, &ssi, sizeof(ssi)) == -11, 0);   /* -EAGAIN */
        /* blocked SIGCHLD (default-ignore) still reaches the fd; nonblock so a
         * regression that discards it fails cleanly (the signal is pending the
         * moment kill returns) rather than blocking */
        unsigned long mc = 1UL << (17 - 1);
        long sfc = sc4(LX_signalfd4, -1, &mc, 8, LX_SFD_NONBLOCK);
        CHECKV(sfc >= 0, sfc);
        CHECKV(sc2(LX_kill, sc0(LX_getpid), 17) == 0, 0);
        CHECKV(sc3(LX_read, sfc, &ssi, sizeof(ssi)) == (long)sizeof(ssi), 0);
        CHECK(ssi.ssi_signo == 17);
        /* a signal NOT in the fd's mask is not reported (sfc watches only
         * SIGCHLD): raise SIGUSR1, the SIGCHLD fd stays empty (-EAGAIN) */
        CHECKV(sc2(LX_kill, sc0(LX_getpid), 10) == 0, 0);
        long sfc2 = sc4(LX_signalfd4, -1, &mc, 8, LX_SFD_NONBLOCK);
        CHECKV(sfc2 >= 0, sfc2);
        CHECKV(sc3(LX_read, sfc2, &ssi, sizeof(ssi)) == -11, 0);   /* -EAGAIN: SIGUSR1 not in mask */
        /* drain the pending SIGUSR1 via its own fd so it does not leak */
        CHECKV(sc3(LX_read, sfd, &ssi, sizeof(ssi)) == (long)sizeof(ssi), 0);
        CHECK(ssi.ssi_signo == 10);
        /* mask update: a pending signal newly added to the mask becomes
         * readable (sfu watches only SIGCHLD, then is widened to SIGUSR1) */
        long sfu = sc4(LX_signalfd4, -1, &mc, 8, LX_SFD_NONBLOCK);
        CHECKV(sfu >= 0, sfu);
        CHECKV(sc2(LX_kill, sc0(LX_getpid), 10) == 0, 0);          /* SIGUSR1: not in sfu's mask yet */
        CHECKV(sc3(LX_read, sfu, &ssi, sizeof(ssi)) == -11, 0);    /* -EAGAIN */
        unsigned long both = (1UL << (10 - 1)) | (1UL << (17 - 1));
        CHECKV(sc4(LX_signalfd4, sfu, &both, 8, 0) == sfu, 0);     /* widen the mask */
        CHECKV(sc3(LX_read, sfu, &ssi, sizeof(ssi)) == (long)sizeof(ssi), 0);
        CHECK(ssi.ssi_signo == 10);                                /* now readable */
        /* a read whose destination faults must not lose the signal: the fd
         * drains it, then the copy to user faults and read_undo puts it back.
         * An in-range page made PROT_NONE passes user_range_ok but faults the
         * copy; a raised SIGUSR1 survives the -EFAULT read and the next read
         * still returns it. */
        long pg = sc6(LX_mmap, 0, 4096, LX_PROT_READ | LX_PROT_WRITE, LX_MAP_PRIVATE | LX_MAP_ANONYMOUS, -1, 0);
        CHECKV(pg > 0 && (pg & 0xfff) == 0, pg);
        CHECKV(sc3(LX_mprotect, pg, 4096, 0) == 0, 0);             /* PROT_NONE */
        long sff = sc4(LX_signalfd4, -1, &m1, 8, LX_SFD_NONBLOCK);
        CHECKV(sff >= 0, sff);
        CHECKV(sc2(LX_kill, sc0(LX_getpid), 10) == 0, 0);          /* SIGUSR1 pending */
        CHECKV(sc3(LX_read, sff, pg, sizeof(ssi)) == -14, 0);      /* -EFAULT: copy faults */
        CHECKV(sc3(LX_read, sff, &ssi, sizeof(ssi)) == (long)sizeof(ssi), 0);   /* not lost */
        CHECK(ssi.ssi_signo == 10);
        CHECKV(sc2(LX_munmap, pg, 4096) == 0, 0);
        CHECKV(sc1(LX_close, sff) == 0, 0);
        /* errors */
        CHECKV(sc4(LX_signalfd4, -1, &m1, 4, 0) == -22, 0);         /* bad sizemask */
        CHECKV(sc4(LX_signalfd4, -1, &m1, 8, 0x9999) == -22, 0);    /* bad flags */
        CHECKV(sc4(LX_signalfd4, -2, &m1, 8, 0) == -9, 0);          /* only -1 creates: -EBADF */
        /* close the fds and restore the signal mask so the later tests see a
         * clean descriptor table and signal state */
        CHECKV(sc1(LX_close, sfd) == 0, 0);
        CHECKV(sc1(LX_close, sfn) == 0, 0);
        CHECKV(sc1(LX_close, sfc) == 0, 0);
        CHECKV(sc1(LX_close, sfc2) == 0, 0);
        CHECKV(sc1(LX_close, sfu) == 0, 0);
        CHECKV(sc4(LX_rt_sigprocmask, LX_SIG_SETMASK, &saved_mask, 0, 8) == 0, 0);
        lx_puts("LXSIGFD: signalfd read/nonblock/mask-scope/blocked-SIGCHLD/fault-keeps\n");
    }
#ifdef LX_stat
    CHECKV(sc2(LX_stat, "/tmp/nope", &st) == -2, 0);           /* ENOENT */
    CHECKV(sc2(LX_stat, "/tmp", &st) == 0 && (st.st_mode & LX_S_IFMT) == LX_S_IFDIR, 0);
#endif
    /* Symbolic links through the Linux calls: readlink does not
     * terminate, lstat is the link and stat is the target, and
     * AT_SYMLINK_NOFOLLOW is honoured rather than read and dropped. */
    {
        char lb[32];
        for (unsigned i = 0; i < sizeof(lb); i++)
            lb[i] = 'Z';
#ifdef LX_symlink
        CHECKV(sc2(LX_symlink, "/tmp/lxtest.txt", "/tmp/lxlink") == 0, 0);
#else
        CHECKV(sc3(LX_symlinkat, "/tmp/lxtest.txt", LX_AT_FDCWD, "/tmp/lxlink") == 0, 0);
#endif
        CHECKV(sc4(LX_readlinkat, LX_AT_FDCWD, "/tmp/lxlink", lb, 32) == 15, 0);
        CHECKV(memeq(lb, "/tmp/lxtest.txt", 15) && lb[15] == 'Z', 0);
        struct lx_stat ls;
        CHECKV(sc4(LX_newfstatat, LX_AT_FDCWD, "/tmp/lxlink", &ls, 0) == 0, 0);
        CHECKV((ls.st_mode & LX_S_IFMT) == LX_S_IFREG && ls.st_size == 22, ls.st_mode);
        CHECKV(sc4(LX_newfstatat, LX_AT_FDCWD, "/tmp/lxlink", &ls, LX_AT_SYMLINK_NOFOLLOW) == 0, 0);
        CHECKV((ls.st_mode & LX_S_IFMT) == LX_S_IFLNK && ls.st_size == 15, ls.st_mode);
        /* statx follows the link by default and stats the link itself with
         * AT_SYMLINK_NOFOLLOW, like newfstatat. */
        struct lx_statx lsx;
        CHECKV(sc5(LX_statx, LX_AT_FDCWD, "/tmp/lxlink", 0, LX_STATX_BASIC_STATS, &lsx) == 0
               && (lsx.stx_mode & LX_S_IFMT) == LX_S_IFREG && lsx.stx_size == 22, (long)lsx.stx_mode);
        CHECKV(sc5(LX_statx, LX_AT_FDCWD, "/tmp/lxlink", LX_AT_SYMLINK_NOFOLLOW, LX_STATX_BASIC_STATS, &lsx) == 0
               && (lsx.stx_mode & LX_S_IFMT) == LX_S_IFLNK && lsx.stx_size == 15, (long)lsx.stx_mode);
#ifdef LX_lstat
        CHECKV(sc2(LX_lstat, "/tmp/lxlink", &ls) == 0, 0);
        CHECKV((ls.st_mode & LX_S_IFMT) == LX_S_IFLNK, ls.st_mode);
#endif
#ifdef LX_readlink
        for (unsigned i = 0; i < sizeof(lb); i++)
            lb[i] = 'Z';
        CHECKV(sc3(LX_readlink, "/tmp/lxlink", lb, 32) == 15 && lb[15] == 'Z', 0);
        CHECKV(sc3(LX_readlink, "/tmp/lxtest.txt", lb, 32) == -22, 0);   /* EINVAL: not a link */
#endif
        CHECKV(sc3(LX_unlinkat, LX_AT_FDCWD, "/tmp/lxlink", 0) == 0, 0);
    }
    CHECKV(sc4(LX_newfstatat, LX_AT_FDCWD, "/tmp/nope", &st, 0) == -2, 0);
    CHECKV(sc4(LX_newfstatat, LX_AT_FDCWD, "/tmp", &st, 0) == 0 && (st.st_mode & LX_S_IFMT) == LX_S_IFDIR, 0);
    CHECKV(sc2(LX_fstat, 0, &st) == 0 && (st.st_mode & LX_S_IFMT) == LX_S_IFCHR, st.st_mode);
    /*
     * TCGETS on the console: it answered -ENOTTY until the terminal-modes
     * unit, and a libc told that fully buffers its output. It now
     * succeeds and reports a canonical terminal with echo, which is what
     * a libc reads to decide to line-buffer. `fd` is a plain file here
     * and is still not a terminal.
     */
    {
        struct {
            uint32_t iflag, oflag, cflag, lflag;
            uint8_t line, cc[19];
        } tio;
        CHECKV(sc3(LX_ioctl, 1, 0x5401, &tio) == 0, 0);        /* TCGETS on the console */
        CHECKV((tio.lflag & 0000002) != 0, tio.lflag);         /* ICANON */
        CHECKV((tio.lflag & 0000010) != 0, tio.lflag);         /* ECHO */
        struct {
            uint16_t row, col, xp, yp;
        } ws;
        CHECKV(sc3(LX_ioctl, 1, 0x5413, &ws) == 0, 0);         /* TIOCGWINSZ */
        int tmp = (int)sc3(LX_openat, LX_AT_FDCWD, "/tmp/lxtest.txt", 0);
        CHECKV(tmp >= 0, tmp);
        CHECKV(sc3(LX_ioctl, tmp, 0x5401, &tio) == -25, 0);    /* a file is not a terminal */
        sc1(LX_close, tmp);
        /* And a terminal opened by name, not inherited as a handle: the
         * two ABIs resolve a handle to a terminal through one function,
         * so this answers exactly as handle 1 did. */
        int con = (int)sc3(LX_openat, LX_AT_FDCWD, "/dev/console", 0);
        CHECKV(con >= 0, con);
        CHECKV(sc3(LX_ioctl, con, 0x5401, &tio) == 0, 0);
        sc1(LX_close, con);
    }
#ifdef LX_access
    CHECKV(sc2(LX_access, "/tmp/lxtest.txt", 0) == 0, 0);
#endif
    CHECKV(sc3(LX_faccessat, LX_AT_FDCWD, "/tmp/lxtest.txt", 0) == 0, 0);
    CHECKV(sc3(LX_mkdirat, LX_AT_FDCWD, "/tmp/lxdir", 0755) == 0, 0);
#ifdef LX_rename
    CHECKV(sc2(LX_rename, "/tmp/lxtest.txt", "/tmp/lxdir/moved") == 0, 0);
#else
    CHECKV(sc4(LX_renameat, LX_AT_FDCWD, "/tmp/lxtest.txt", LX_AT_FDCWD, "/tmp/lxdir/moved") == 0, 0);
#endif
    /* getdents64 on the directory: ".", "..", "moved" */
    long dfd = sc4(LX_openat, LX_AT_FDCWD, "/tmp/lxdir", LX_O_RDONLY | LX_O_DIRECTORY, 0);
    CHECKV(dfd >= 3, dfd);
    static char dents[1024];
    long n = sc3(LX_getdents64, dfd, dents, sizeof(dents));
    CHECKV(n > 0, n);
    int seen = 0;
    for (long off = 0; off < n;) {
        struct lx_dirent64 *d = (struct lx_dirent64 *)(dents + off);
        if (streq(d->d_name, "moved") && d->d_type == LX_DT_REG)
            seen |= 1;
        if (streq(d->d_name, "."))
            seen |= 2;
        if (streq(d->d_name, ".."))
            seen |= 4;
        CHECKV(d->d_reclen >= 24 && (d->d_reclen & 7) == 0, d->d_reclen);
        off += d->d_reclen;
    }
    CHECKV(seen == 7, seen);
    CHECKV(sc3(LX_getdents64, dfd, dents, sizeof(dents)) == 0, 0);
    /*
     * A directory descriptor names a directory (P31,
     * docs/audit/next-subsystem-dirfd.md). Every *at call resolves a
     * relative path from `dfd`, and every effect is cross-checked by
     * ABSOLUTE path: an *at call that quietly resolved from the working
     * directory ("/") instead would still succeed, and only the absolute
     * check says which directory it acted in.
     */
    {
        static char sb[256], lb[64];
        long o = sc4(LX_openat, dfd, "moved", LX_O_RDONLY, 0);
        CHECKV(o >= 3, o);
        if (o >= 0)
            sc1(LX_close, o);
        CHECKV(sc4(LX_newfstatat, dfd, "moved", sb, 0) == 0, 0);
        CHECKV(sc3(LX_faccessat, dfd, "moved", 0) == 0, 0);
        CHECKV(sc3(LX_mkdirat, dfd, "sub", 0755) == 0, 0);
        CHECKV(sc4(LX_newfstatat, LX_AT_FDCWD, "/tmp/lxdir/sub", sb, 0) == 0, 0);
        CHECKV(sc3(LX_symlinkat, "moved", dfd, "lnk") == 0, 0);
        long rl = sc4(LX_readlinkat, dfd, "lnk", lb, sizeof(lb));
        CHECKV(rl == 5 && lb[0] == 'm' && lb[4] == 'd', rl);
        CHECKV(sc4(LX_readlinkat, LX_AT_FDCWD, "/tmp/lxdir/lnk", lb, sizeof(lb)) == 5, 0);
        CHECKV(sc4(LX_readlinkat, dfd, "absent", lb, sizeof(lb)) == -2, 0);   /* ENOENT, not ENOSYS */
        CHECKV(sc4(LX_mknodat, dfd, "fifo", LX_S_IFIFO | 0644, 0) == 0, 0);
        CHECKV(sc4(LX_newfstatat, LX_AT_FDCWD, "/tmp/lxdir/fifo", sb, 0) == 0, 0);
        CHECKV(sc3(LX_unlinkat, dfd, "fifo", 0) == 0, 0);
        CHECKV(sc3(LX_unlinkat, dfd, "lnk", 0) == 0, 0);
        CHECKV(sc4(LX_newfstatat, LX_AT_FDCWD, "/tmp/lxdir/lnk", sb, LX_AT_SYMLINK_NOFOLLOW) == -2, 0);
        /* renameat across two descriptors: moved -> sub/m2 -> back */
        long dfd2 = sc4(LX_openat, dfd, "sub", LX_O_RDONLY | LX_O_DIRECTORY, 0);
        CHECKV(dfd2 >= 3, dfd2);
        CHECKV(sc4(LX_renameat, dfd, "moved", dfd2, "m2") == 0, 0);
        CHECKV(sc4(LX_newfstatat, LX_AT_FDCWD, "/tmp/lxdir/sub/m2", sb, 0) == 0, 0);
        CHECKV(sc4(LX_renameat, dfd2, "m2", dfd, "moved") == 0, 0);
        CHECKV(sc4(LX_newfstatat, LX_AT_FDCWD, "/tmp/lxdir/moved", sb, 0) == 0, 0);
        if (dfd2 >= 0)
            sc1(LX_close, dfd2);
        CHECKV(sc3(LX_unlinkat, dfd, "sub", LX_AT_REMOVEDIR) == 0, 0);
        CHECKV(sc4(LX_newfstatat, LX_AT_FDCWD, "/tmp/lxdir/sub", sb, 0) == -2, 0);
        /* Refusals: a regular file is not a directory, a closed one is nothing. */
        long rf = sc4(LX_openat, dfd, "moved", LX_O_RDONLY, 0);
        CHECKV(sc4(LX_openat, rf, "x", LX_O_RDONLY, 0) == -20, 0);             /* ENOTDIR */
        sc1(LX_close, rf);
        CHECKV(sc4(LX_openat, rf, "x", LX_O_RDONLY, 0) == -9, 0);              /* EBADF */
        /* fchdir: the name comes with the directory, and the next relative
         * chdir normalises against it. */
        CHECKV(sc1(LX_fchdir, dfd) == 0, 0);
        static char cw[64];
        long gl = sc2(LX_getcwd, cw, sizeof(cw));
        CHECKV(gl == 11 && streq(cw, "/tmp/lxdir"), gl);
        long rel = sc4(LX_openat, LX_AT_FDCWD, "moved", LX_O_RDONLY, 0);
        CHECKV(rel >= 3, rel);
        if (rel >= 0)
            sc1(LX_close, rel);
        CHECKV(sc1(LX_chdir, "..") == 0, 0);
        CHECKV(sc2(LX_getcwd, cw, sizeof(cw)) == 5 && streq(cw, "/tmp"), 0);
        CHECKV(sc1(LX_chdir, "/") == 0, 0);
        long rf2 = sc4(LX_openat, LX_AT_FDCWD, "/tmp/lxdir/moved", LX_O_RDONLY, 0);
        CHECKV(sc1(LX_fchdir, rf2) == -20, 0);                                  /* not a directory */
        sc1(LX_close, rf2);
        /* A directory reached through a symbolic link: its name is the one
         * the walk took (P32), so fchdir publishes the directory's own
         * path, not the link's spelling. The dirfd unit refused this
         * (-ENOENT, no coherent name); the cwd-name unit gave it one. */
        CHECKV(sc3(LX_symlinkat, "/tmp/lxdir", LX_AT_FDCWD, "/tmp/lxdlink") == 0, 0);
        long dl = sc4(LX_openat, LX_AT_FDCWD, "/tmp/lxdlink", LX_O_RDONLY | LX_O_DIRECTORY, 0);
        CHECKV(dl >= 3, dl);
        CHECKV(sc4(LX_newfstatat, dl, "moved", sb, 0) == 0, 0);
        CHECKV(sc1(LX_fchdir, dl) == 0, 0);
        CHECKV(sc2(LX_getcwd, cw, sizeof(cw)) == 11 && streq(cw, "/tmp/lxdir"), 0);   /* the directory, not the link */
        CHECKV(sc1(LX_chdir, "/") == 0, 0);
        if (dl >= 0)
            sc1(LX_close, dl);
        CHECKV(sc3(LX_unlinkat, LX_AT_FDCWD, "/tmp/lxdlink", 0) == 0, 0);
        /* The same through a relative link: an absolute target restarts
         * the name at `/`, which would hide a link's spelling left in it;
         * a relative one continues from the name as it stands. */
        CHECKV(sc3(LX_symlinkat, "lxdir", LX_AT_FDCWD, "/tmp/lxdrel") == 0, 0);
        long dr = sc4(LX_openat, LX_AT_FDCWD, "/tmp/lxdrel", LX_O_RDONLY | LX_O_DIRECTORY, 0);
        CHECKV(dr >= 3, dr);
        CHECKV(sc1(LX_fchdir, dr) == 0, 0);
        CHECKV(sc2(LX_getcwd, cw, sizeof(cw)) == 11 && streq(cw, "/tmp/lxdir"), 0);
        CHECKV(sc1(LX_chdir, "/") == 0, 0);
        if (dr >= 0)
            sc1(LX_close, dr);
        CHECKV(sc3(LX_unlinkat, LX_AT_FDCWD, "/tmp/lxdrel", 0) == 0, 0);
        /* `..` after a link: /tmp/lxdeep -> /tmp/lxdir/deep, so opening
         * "/tmp/lxdeep/.." reaches /tmp/lxdir, and that is its name; a
         * lexical normalisation said "/tmp", a different directory. */
        CHECKV(sc3(LX_mkdirat, dfd, "deep", 0755) == 0, 0);
        CHECKV(sc3(LX_symlinkat, "/tmp/lxdir/deep", LX_AT_FDCWD, "/tmp/lxdeep") == 0, 0);
        long dd = sc4(LX_openat, LX_AT_FDCWD, "/tmp/lxdeep/..", LX_O_RDONLY | LX_O_DIRECTORY, 0);
        CHECKV(dd >= 3, dd);
        CHECKV(sc4(LX_newfstatat, dd, "moved", sb, 0) == 0, 0);             /* it IS /tmp/lxdir */
        CHECKV(sc1(LX_fchdir, dd) == 0, 0);
        CHECKV(sc2(LX_getcwd, cw, sizeof(cw)) == 11 && streq(cw, "/tmp/lxdir"), 0);   /* not "/tmp" */
        CHECKV(sc1(LX_chdir, "/") == 0, 0);
        if (dd >= 0)
            sc1(LX_close, dd);
        CHECKV(sc3(LX_unlinkat, LX_AT_FDCWD, "/tmp/lxdeep", 0) == 0, 0);
        CHECKV(sc3(LX_unlinkat, dfd, "deep", LX_AT_REMOVEDIR) == 0, 0);
    }
    CHECKV(sc1(LX_close, dfd) == 0, 0);
    CHECKV(sc3(LX_unlinkat, LX_AT_FDCWD, "/tmp/lxdir/moved", 0) == 0, 0);
    CHECKV(sc3(LX_unlinkat, LX_AT_FDCWD, "/tmp/lxdir", LX_AT_REMOVEDIR) == 0, 0);

    /* --- named pipes (the named-pipes unit): mknodat makes a FIFO and
     * nothing else; the open rules; bytes; fstat; O_NONBLOCK by fcntl --- */
    CHECKV(sc4(LX_mknodat, LX_AT_FDCWD, "/tmp/lxfifo", LX_S_IFIFO | 0644, 0) == 0, 0);
    CHECKV(sc4(LX_mknodat, LX_AT_FDCWD, "/tmp/lxchr", LX_S_IFCHR | 0644, 0) == -1, 0);       /* EPERM */
    CHECKV(sc4(LX_mknodat, LX_AT_FDCWD, "/tmp/lxsockn", LX_S_IFSOCK | 0644, 0) == -22, 0);   /* EINVAL: bind makes those */
#ifdef LX_mknod
    CHECKV(sc3(LX_mknod, "/tmp/lxfifo", LX_S_IFIFO | 0644, 0) == -17, 0);   /* the legacy form, x86-64: EEXIST */
#endif
    CHECKV(sc4(LX_openat, LX_AT_FDCWD, "/tmp/lxfifo", LX_O_WRONLY | LX_O_NONBLOCK, 0) == -6, 0);   /* ENXIO: no reader */
    long fr = sc4(LX_openat, LX_AT_FDCWD, "/tmp/lxfifo", LX_O_RDONLY | LX_O_NONBLOCK, 0);
    CHECKV(fr >= 3, fr);
    long fw = sc4(LX_openat, LX_AT_FDCWD, "/tmp/lxfifo", LX_O_WRONLY | LX_O_NONBLOCK, 0);
    CHECKV(fw >= 3, fw);
    char fbuf[16];
    CHECKV(sc3(LX_read, fr, fbuf, 4) == -11, 0);   /* EAGAIN: empty, a writer present */
    CHECKV(sc3(LX_write, fw, "fifo", 4) == 4, 0);
    CHECKV(sc3(LX_read, fr, fbuf, 16) == 4 && memeq(fbuf, "fifo", 4), 0);
    CHECKV(sc2(LX_fstat, fr, &st) == 0 && (st.st_mode & LX_S_IFMT) == LX_S_IFIFO, st.st_mode);
    CHECKV(sc3(LX_fcntl, fr, LX_F_GETFL, 0) & LX_O_NONBLOCK, 0);
    CHECKV(sc3(LX_fcntl, fr, LX_F_SETFL, 0) == 0 && !(sc3(LX_fcntl, fr, LX_F_GETFL, 0) & LX_O_NONBLOCK), 0);
    CHECKV(sc3(LX_fcntl, fr, LX_F_SETFL, LX_O_NONBLOCK) == 0 && sc3(LX_read, fr, fbuf, 4) == -11, 0);
    sc1(LX_close, fw);
    CHECKV(sc3(LX_read, fr, fbuf, 4) == 0, 0);   /* the last writer closed: end of file */
    sc1(LX_close, fr);
    CHECKV(sc3(LX_unlinkat, LX_AT_FDCWD, "/tmp/lxfifo", 0) == 0, 0);
    CHECKV(sc1(LX_chdir, "/tmp") == 0, 0);
    CHECKV(sc2(LX_getcwd, buf, sizeof(buf)) == 5 && streq(buf, "/tmp"), 0);   /* length includes the NUL */
    /* The name chdir publishes is the path the walk took (P32,
     * docs/audit/next-subsystem-cwd-name.md): through a link it is the
     * directory's own path, and `..` after it is that directory's parent.
     * A lexical name said /tmp/lxcnl, then /tmp -- while the process
     * stood in /tmp/lxcn. Each name is checked against where the process
     * is, by inode. */
    {
        static struct lx_stat here, named;
        static char cn[64];
        CHECKV(sc3(LX_mkdirat, LX_AT_FDCWD, "/tmp/lxcn", 0755) == 0, 0);
        CHECKV(sc3(LX_mkdirat, LX_AT_FDCWD, "/tmp/lxcn/deep", 0755) == 0, 0);
        CHECKV(sc3(LX_symlinkat, "/tmp/lxcn/deep", LX_AT_FDCWD, "/tmp/lxcnl") == 0, 0);   /* absolute target */
        CHECKV(sc3(LX_symlinkat, "lxcn/deep", LX_AT_FDCWD, "/tmp/lxcnr") == 0, 0);        /* relative target */
        static const struct {
            const char *to;
            const char *name;
        } steps[] = {
            { "/tmp/lxcnl", "/tmp/lxcn/deep" },   /* through an absolute link */
            { "..", "/tmp/lxcn" },                /* the directory's parent, not the link's */
            { "/tmp", "/tmp" },
            { "lxcnr", "/tmp/lxcn/deep" },        /* a relative link, from a relative chdir */
            { "/tmp/lxcnl/..", "/tmp/lxcn" },     /* `..` after a link in one path */
            { "deep/../../lxcnl", "/tmp/lxcn/deep" },
        };
        for (unsigned i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
            CHECKV(sc1(LX_chdir, steps[i].to) == 0, i);
            CHECKV(sc2(LX_getcwd, cn, sizeof(cn)) > 0 && streq(cn, steps[i].name), i);
            CHECKV(sc4(LX_newfstatat, LX_AT_FDCWD, ".", &here, 0) == 0 &&
                       sc4(LX_newfstatat, LX_AT_FDCWD, cn, &named, 0) == 0 && here.st_ino == named.st_ino,
                   i);
        }
        CHECKV(sc1(LX_chdir, "/") == 0, 0);
        CHECKV(sc3(LX_unlinkat, LX_AT_FDCWD, "/tmp/lxcnr", 0) == 0, 0);
        CHECKV(sc3(LX_unlinkat, LX_AT_FDCWD, "/tmp/lxcnl", 0) == 0, 0);
        CHECKV(sc3(LX_unlinkat, LX_AT_FDCWD, "/tmp/lxcn/deep", LX_AT_REMOVEDIR) == 0, 0);
        CHECKV(sc3(LX_unlinkat, LX_AT_FDCWD, "/tmp/lxcn", LX_AT_REMOVEDIR) == 0, 0);
    }
    CHECKV(sc1(LX_chdir, "/") == 0, 0);
    CHECKV(sc0(LX_umask) == 022, 0);

    /* --- file-backed mmap (milestone 10): a private snapshot --- */
    long mfd = sc4(LX_openat, LX_AT_FDCWD, "/tmp/lxmap", LX_O_RDWR | LX_O_CREAT | LX_O_TRUNC, 0644);
    CHECKV(mfd >= 3, mfd);
    static unsigned char pattern[6000];
    for (int i = 0; i < 6000; i++)
        pattern[i] = (unsigned char)(i * 7 + 3);
    CHECKV(sc3(LX_write, mfd, pattern, 6000) == 6000, 0);
    long fm = sc6(LX_mmap, 0, 8192, LX_PROT_READ, LX_MAP_PRIVATE, mfd, 0);
    CHECKV(fm > 0 && (fm & 0xfff) == 0, fm);
    if (fm > 0) {
        const unsigned char *fb = (const unsigned char *)fm;
        CHECK(memeq(fb, pattern, 6000));
        int tail_zero = 1;
        for (int i = 6000; i < 8192; i++)
            if (fb[i])
                tail_zero = 0;
        CHECK(tail_zero);                                                     /* past the end: zero */
        CHECKV(sc3(LX_write, 1, fm + 8192 - 8, 0) == 0, 0);
        CHECKV(sc2(LX_munmap, fm, 8192) == 0, 0);
    }
    long fm2 = sc6(LX_mmap, 0, 4096, LX_PROT_READ | LX_PROT_WRITE, LX_MAP_PRIVATE, mfd, 4096);   /* offset one page */
    CHECKV(fm2 > 0, fm2);
    if (fm2 > 0) {
        unsigned char *fb = (unsigned char *)fm2;
        CHECK(memeq(fb, pattern + 4096, 6000 - 4096));
        fb[0] = 0xEE;                                                         /* private: the file is untouched */
        unsigned char one;
        CHECKV(sc4(LX_pread64, mfd, &one, 1, 4096) == 1 && one == pattern[4096], one);
        CHECKV(sc2(LX_munmap, fm2, 4096) == 0, 0);
    }
    /* MAP_SHARED|PROT_WRITE: the file's own pages (the file-regions unit;
     * this used to be -EOPNOTSUPP). A write through the mapping is in the
     * file at once, a write() is in the mapping at once: one frame. */
    long fsm = sc6(LX_mmap, 0, 4096, LX_PROT_READ | LX_PROT_WRITE, LX_MAP_SHARED, mfd, 0);
    CHECKV(fsm > 0, fsm);
    if (fsm > 0) {
        unsigned char *sb = (unsigned char *)fsm;
        CHECK(memeq(sb, pattern, 4096));
        sb[10] = 0xAB;
        unsigned char one = 0;
        CHECKV(sc4(LX_pread64, mfd, &one, 1, 10) == 1 && one == 0xAB, one);
        unsigned char wc = 0xCD;
        CHECKV(sc4(LX_pwrite64, mfd, &wc, 1, 20) == 1, 0);
        CHECKV(sb[20] == 0xCD, sb[20]);
        CHECKV(sc3(LX_msync, fsm, 4096, LX_MS_SYNC) == 0, 0);
        CHECKV(sc3(LX_msync, fsm, 4096, LX_MS_SYNC | LX_MS_ASYNC) == -22, 0);   /* one or the other */
        CHECKV(sc3(LX_msync, fsm, 4096, 8) == -22, 0);                          /* an undefined bit */
        CHECKV(sc2(LX_munmap, fsm, 4096) == 0, 0);
        CHECKV(sc4(LX_pwrite64, mfd, pattern + 10, 1, 10) == 1, 0);           /* the pattern back, for the checks below */
        CHECKV(sc4(LX_pwrite64, mfd, pattern + 20, 1, 20) == 1, 0);
    }
    long fm3 = sc6(LX_mmap, 0, 4096, LX_PROT_READ, LX_MAP_SHARED, mfd, 0);   /* read-only shared: coherent, not a snapshot */
    CHECKV(fm3 > 0 && memeq((const void *)fm3, pattern, 4096), fm3);
    if (fm3 > 0) {
        unsigned char wz = 0x5A;
        CHECKV(sc4(LX_pwrite64, mfd, &wz, 1, 30) == 1, 0);
        CHECKV(((const unsigned char *)fm3)[30] == 0x5A, 0);                  /* a later write() is seen */
        CHECKV(sc4(LX_pwrite64, mfd, pattern + 30, 1, 30) == 1, 0);
        sc2(LX_munmap, fm3, 4096);
    }
    /* The mapping type: none, or a value past MAP_SHARED_VALIDATE, is
     * EINVAL, anonymous or not; 3 (SHARED_VALIDATE) is shared. */
    CHECKV(sc6(LX_mmap, 0, 4096, LX_PROT_READ | LX_PROT_WRITE, LX_MAP_ANONYMOUS, -1, 0) == -22, 0);
    CHECKV(sc6(LX_mmap, 0, 4096, LX_PROT_READ, 0, mfd, 0) == -22, 0);
    CHECKV(sc6(LX_mmap, 0, 4096, LX_PROT_READ, 4, mfd, 0) == -22, 0);
    long sv = sc6(LX_mmap, 0, 4096, LX_PROT_READ | LX_PROT_WRITE, LX_MAP_SHARED | LX_MAP_PRIVATE, mfd, 0);
    CHECKV(sv > 0, sv);
    if (sv > 0) {
        ((unsigned char *)sv)[40] = 0x77;
        unsigned char got = 0;
        CHECKV(sc4(LX_pread64, mfd, &got, 1, 40) == 1 && got == 0x77, got);   /* shared, as VALIDATE means */
        CHECKV(sc4(LX_pwrite64, mfd, pattern + 40, 1, 40) == 1, 0);
        sc2(LX_munmap, sv, 4096);
    }
    /* The futex flag on a shared page (the shared-futex unit): with
     * FUTEX_PRIVATE_FLAG the key is this process's, without it the key is
     * the file's -- the flag used to be masked out. A waiter with the flag
     * is not woken by a wake without it and is by one with it; the reverse
     * pair for a waiter without. The sleeper count through a self
     * CMP_REQUEUE says when each is asleep. */
    long fsm2 = sc6(LX_mmap, 0, 4096, LX_PROT_READ | LX_PROT_WRITE, LX_MAP_SHARED, mfd, 0);
    CHECKV(fsm2 > 0, fsm2);
    if (fsm2 > 0) {
        g_flag_word = (volatile unsigned *)(fsm2 + 256);
        *g_flag_word = 0;
        for (int with_flag = 1; with_flag >= 0; with_flag--) {
            unsigned f = with_flag ? LX_FUTEX_PRIVATE_FLAG : 0, other = with_flag ? 0 : LX_FUTEX_PRIVATE_FLAG;
            int32_t ptid = 0;
            g_tidword[2] = 1;
            g_flag_rc[with_flag] = 99;
            long ct = lx_clone(t_flag_waiter, g_stacks[2] + sizeof(g_stacks[2]), (void *)(uintptr_t)with_flag,
                               THREAD_FLAGS, &ptid, &g_tidword[2], g_tcb);
            CHECKV(ct > 0, ct);
            long asleep = 0;
            for (int i = 0; i < 4000 && asleep != 1; i++) {
                asleep = sc6(LX_futex, g_flag_word, LX_FUTEX_CMP_REQUEUE | f, 0, 1000, g_flag_word, 0);
                if (asleep != 1)
                    sc0(LX_sched_yield);
            }
            CHECKV(asleep == 1, asleep);
            CHECKV(sc6(LX_futex, g_flag_word, LX_FUTEX_WAKE | other, 1, 0, 0, 0) == 0, with_flag);   /* the other key */
            CHECKV(sc6(LX_futex, g_flag_word, LX_FUTEX_WAKE | f, 1, 0, 0, 0) == 1, with_flag);       /* its own */
            CHECKV(lx_join(&g_tidword[2]) == 0, with_flag);
            CHECKV(g_flag_rc[with_flag] == 0, g_flag_rc[with_flag]);
        }
        sc2(LX_munmap, fsm2, 4096);
    }
    /* A shared writable mapping of a file opened read-only: EACCES. */
    long rofd = sc4(LX_openat, LX_AT_FDCWD, "/tmp/lxmap", LX_O_RDONLY, 0);
    CHECKV(rofd >= 3, rofd);
    CHECKV(sc6(LX_mmap, 0, 4096, LX_PROT_READ | LX_PROT_WRITE, LX_MAP_SHARED, rofd, 0) == -13, 0);
    long ros = sc6(LX_mmap, 0, 4096, LX_PROT_READ, LX_MAP_SHARED, rofd, 0);
    CHECKV(ros > 0, ros);
    if (ros > 0) {
        CHECKV(sc3(LX_mprotect, ros, 4096, LX_PROT_READ | LX_PROT_WRITE) == -13, 0);   /* maxprot */
        sc2(LX_munmap, ros, 4096);
    }
    CHECKV(sc1(LX_close, rofd) == 0, 0);
    CHECKV(sc6(LX_mmap, 0, 4096, LX_PROT_READ, LX_MAP_PRIVATE, mfd, 100) == -22, 0);               /* unaligned offset */
    CHECKV(sc1(LX_close, mfd) == 0, 0);
    CHECKV(sc3(LX_unlinkat, LX_AT_FDCWD, "/tmp/lxmap", 0) == 0, 0);

    /* --- pipes, dup, fcntl --- */
    int32_t p[2];
    CHECKV(sc2(LX_pipe2, p, LX_O_CLOEXEC) == 0, 0);
    CHECKV(sc3(LX_write, p[1], "xyz", 3) == 3, 0);
    CHECKV(sc3(LX_read, p[0], buf, 8) == 3 && memeq(buf, "xyz", 3), 0);
    long d = sc1(LX_dup, p[1]);
    CHECKV(d >= 3 && d != p[1], d);
    CHECKV(sc3(LX_dup3, p[0], 40, 0) == 40, 0);
    CHECKV(sc3(LX_dup3, p[0], p[0], 0) == -22, 0);
    CHECKV(sc3(LX_fcntl, p[1], LX_F_GETFL, 0) == LX_O_WRONLY, 0);
    CHECKV(sc3(LX_fcntl, p[0], LX_F_DUPFD, 50) >= 50, 0);
    CHECKV(sc1(LX_close, p[1]) == 0 && sc1(LX_close, d) == 0, 0);
    CHECKV(sc3(LX_read, 40, buf, 8) == 0, 0);                  /* every writer closed: EOF */
    CHECKV(sc2(LX_fstat, 40, &st) == 0 && (st.st_mode & LX_S_IFMT) == LX_S_IFIFO, 0);
    sc1(LX_close, 40);
    sc1(LX_close, p[0]);

    /* --- non-blocking pipes: pipe2(O_NONBLOCK), fcntl(F_SETFL) --- */
    CHECKV(sc2(LX_pipe2, p, LX_O_NONBLOCK) == 0, 0);
    CHECKV(sc3(LX_read, p[0], buf, 8) == -11, 0);                              /* EAGAIN: empty, writer alive */
    CHECKV(sc3(LX_fcntl, p[0], LX_F_GETFL, 0) == (LX_O_RDONLY | LX_O_NONBLOCK), 0);
    CHECKV(sc3(LX_fcntl, p[0], LX_F_SETFL, 0) == 0, 0);                        /* clear it */
    CHECKV(sc3(LX_fcntl, p[0], LX_F_GETFL, 0) == LX_O_RDONLY, 0);
    CHECKV(sc3(LX_fcntl, p[1], LX_F_SETFL, LX_O_NONBLOCK) == 0, 0);
    static char page[4096];
    long filled = 0;
    for (int i = 0; i < 64; i++) {
        long wn = sc3(LX_write, p[1], page, sizeof(page));
        if (wn < 0) {
            CHECKV(wn == -11, wn);   /* EAGAIN once the ring is full */
            break;
        }
        filled += wn;
    }
    CHECKV(filled == 16384, filled);                                           /* the ring, then EAGAIN */
    CHECKV(sc3(LX_read, p[0], buf, 8) == 8, 0);
    sc1(LX_close, p[0]);
    sc1(LX_close, p[1]);

    /* --- signals (milestone 10): handlers, masks, siginfo, the alternate
     * stack, faults, the FPU image; wait, kill --- */
    struct lx_sigaction act = { .handler = 0x400000, .flags = LX_SA_RESTORER, .restorer = 0x400000 }, old;
    CHECKV(sc4(LX_rt_sigaction, 2, &act, 0, 8) == 0, 0);
    CHECKV(sc4(LX_rt_sigaction, 2, 0, &old, 8) == 0 && old.handler == 0x400000, 0);
    CHECKV(sc4(LX_rt_sigaction, 9, &act, 0, 8) == -22, 0);    /* SIGKILL */
    CHECKV(sc4(LX_rt_sigaction, 0, &act, 0, 8) == -22 && sc4(LX_rt_sigaction, 64, &act, 0, 8) == -22, 0);
    CHECKV(sc4(LX_rt_sigaction, 2, &act, 0, 4) == -22, 0);    /* sigsetsize */
    uint64_t set = 1ull << 1, oset = 0;
    CHECKV(sc4(LX_rt_sigprocmask, LX_SIG_BLOCK, &set, &oset, 8) == 0 && oset == 0, 0);
    CHECKV(sc4(LX_rt_sigprocmask, LX_SIG_BLOCK, 0, &oset, 8) == 0 && oset == set, 0);
    int32_t status;
    CHECKV(sc4(LX_wait4, -1, &status, 0, 0) == -10, 0);       /* ECHILD: no children */
    CHECKV(sc2(LX_kill, 999999, 15) == -3, 0);                 /* ESRCH */
    CHECKV(sc2(LX_kill, pid, 0) == 0, 0);                       /* existence probe */
    CHECKV(sc2(LX_kill, pid, 65) == -22, 0);
    CHECKV(sc2(LX_kill, pid, 17) == 0, 0);                      /* SIGCHLD: the default ignores it */
    CHECKV(sc1(LX_execve, "/bin/true") == -38, 0);             /* ENOSYS */
#ifdef LX_fork
    CHECKV(sc0(LX_fork) == -38, 0);
#endif
    CHECKV(sc0(LX_sched_yield) == 0, 0);

    /* A handler through kill: siginfo names the sender, the signal is
     * blocked while it runs, the mask is back afterwards. */
    sig_install(10, 0);
    g_sig.count = 0;
    CHECKV(sc2(LX_kill, pid, 10) == 0, 0);
    CHECKV(g_sig.count == 1 && g_sig.sig == 10 && g_sig.code == LX_SI_USER && g_sig.pid == pid, g_sig.code);
    CHECKV((g_sig.blocked & (1ull << 9)) != 0 && (g_sig.blocked & (1ull << 1)) != 0, g_sig.blocked);
    CHECKV(sc4(LX_rt_sigprocmask, LX_SIG_BLOCK, 0, &oset, 8) == 0 && oset == set, oset);
    CHECKV(g_sig.fpstate_ok, 0);
    CHECKV(g_sig.ss_flags == LX_SS_DISABLE && g_sig.ss_now == LX_SS_DISABLE, g_sig.ss_flags);   /* no alternate stack */
    /* tgkill: SI_TKILL. */
    sig_install(12, 0);
    CHECKV(sc3(LX_tgkill, pid, pid, 12) == 0, 0);
    CHECKV(g_sig.count == 2 && g_sig.sig == 12 && g_sig.code == LX_SI_TKILL, g_sig.code);
    CHECKV(sc3(LX_tgkill, pid, 424242, 12) == -3, 0);          /* no such thread */
    CHECKV(sc2(LX_tkill, pid, 0) == 0, 0);
    /* Blocked: pending, not delivered; delivered by the unblock. */
    uint64_t usr1 = 1ull << 9, pend = 0;
    CHECKV(sc4(LX_rt_sigprocmask, LX_SIG_BLOCK, &usr1, 0, 8) == 0, 0);
    CHECKV(sc2(LX_kill, pid, 10) == 0 && g_sig.count == 2, g_sig.count);
    CHECKV(sc2(LX_rt_sigpending, &pend, 8) == 0 && (pend & usr1) != 0, pend);
    CHECKV(sc4(LX_rt_sigprocmask, LX_SIG_UNBLOCK, &usr1, 0, 8) == 0, 0);
    CHECKV(g_sig.count == 3 && g_sig.sig == 10, g_sig.count);
    CHECKV(sc2(LX_rt_sigpending, &pend, 8) == 0 && (pend & usr1) == 0, pend);
    /* rt_sigsuspend: the temporary mask lets the pending signal in, the
     * handler runs, the call is EINTR, the old mask is back. */
    CHECKV(sc4(LX_rt_sigprocmask, LX_SIG_BLOCK, &usr1, 0, 8) == 0, 0);
    CHECKV(sc2(LX_kill, pid, 10) == 0 && g_sig.count == 3, g_sig.count);
    uint64_t during = 1ull << 1;
    CHECKV(sc2(LX_rt_sigsuspend, &during, 8) == -4, 0);
    CHECKV(g_sig.count == 4 && (g_sig.blocked & usr1) != 0, g_sig.count);
    CHECKV(sc4(LX_rt_sigprocmask, LX_SIG_BLOCK, 0, &oset, 8) == 0 && oset == (set | usr1), oset);
    CHECKV(sc4(LX_rt_sigprocmask, LX_SIG_UNBLOCK, &usr1, 0, 8) == 0, 0);
    /* SA_RESETHAND: one shot. */
    sig_install(10, LX_SA_RESETHAND);
    CHECKV(sc2(LX_kill, pid, 10) == 0 && g_sig.count == 5, g_sig.count);
    CHECKV(sc4(LX_rt_sigaction, 10, 0, &old, 8) == 0 && old.handler == 0, old.handler);
    /* The alternate stack. */
    static char altstk[16384];
    struct lx_stack_t ss = { .ss_sp = (uint64_t)(uintptr_t)altstk, .ss_size = sizeof(altstk) }, oss;
    CHECKV(sc2(LX_sigaltstack, &ss, 0) == 0, 0);
    struct lx_stack_t tiny = { .ss_sp = (uint64_t)(uintptr_t)altstk, .ss_size = 100 };
    CHECKV(sc2(LX_sigaltstack, &tiny, 0) == -12, 0);          /* ENOMEM: below MINSIGSTKSZ */
    sig_install(10, LX_SA_ONSTACK);
    CHECKV(sc2(LX_kill, pid, 10) == 0 && g_sig.count == 6, g_sig.count);
    CHECKV(g_sig.sp >= (unsigned long)(uintptr_t)altstk && g_sig.sp < (unsigned long)(uintptr_t)altstk + sizeof(altstk), g_sig.sp);
    CHECKV(g_sig.ss_flags == 0 && g_sig.ss_now == LX_SS_ONSTACK, g_sig.ss_now);   /* interrupted off it, running on it */
    CHECKV(sc2(LX_sigaltstack, 0, &oss) == 0 && oss.ss_flags == 0 && oss.ss_size == sizeof(altstk), oss.ss_flags);
    struct lx_stack_t dis = { .ss_flags = LX_SS_DISABLE };
    CHECKV(sc2(LX_sigaltstack, &dis, &oss) == 0 && oss.ss_sp == (uint64_t)(uintptr_t)altstk, 0);
    CHECKV(sc2(LX_sigaltstack, 0, &oss) == 0 && oss.ss_flags == LX_SS_DISABLE, oss.ss_flags);
    /* Faults: SEGV_MAPERR on an unmapped page, SEGV_ACCERR on the read-only
     * trampoline page; the handler steps over the store. */
    sig_install(11, 0);
    sig_fault_store(0x7000);
    CHECKV(g_sig.count == 7 && g_sig.sig == 11 && g_sig.code == LX_SEGV_MAPERR && g_sig.addr == 0x7000, g_sig.code);
#if defined(__x86_64__)
    static const unsigned char tramp[] = { 0xb8, 0x0f, 0x00, 0x00, 0x00, 0x0f, 0x05 };   /* mov $15,%eax; syscall */
#else
    static const unsigned char tramp[] = { 0x68, 0x11, 0x80, 0xd2, 0x01, 0x00, 0x00, 0xd4 };   /* mov x8,#139; svc #0 */
#endif
    CHECK(memeq((const void *)(uintptr_t)LX_SIGTRAMP, tramp, sizeof(tramp)));
    sig_fault_store(LX_SIGTRAMP);
    CHECKV(g_sig.count == 8 && g_sig.code == LX_SEGV_ACCERR && g_sig.addr == LX_SIGTRAMP, g_sig.code);
    /* The vector registers survive a handler (the frame's FXSAVE image). */
    sig_install(10, 0);
#if defined(__x86_64__)
    CHECKV(sig_xmm_roundtrip(pid) == 0x0123456789abcdefull, 0);
#else
    CHECKV(sig_vreg_roundtrip(pid) == 0x0123456789abcdefull, 0);
#endif
    CHECKV(g_sig.count == 9, g_sig.count);
    /* Back to defaults for the rest. */
    struct lx_sigaction dfl = { 0 };
    CHECKV(sc4(LX_rt_sigaction, 10, &dfl, 0, 8) == 0 && sc4(LX_rt_sigaction, 11, &dfl, 0, 8) == 0, 0);
    CHECKV(sc4(LX_rt_sigaction, 12, &dfl, 0, 8) == 0, 0);

    /* --- threads: clone(CLONE_THREAD), tid words, TLS, join through CHILD_CLEARTID --- */
    int32_t ptid = 0;
    long ctid = lx_clone(t_basic, g_stacks[0] + sizeof(g_stacks[0]), 0, THREAD_FLAGS, &ptid, &g_tidword[0], g_tcb);
    CHECKV(ctid >= 0x10000 && ctid != pid && ptid == ctid, ctid);
    CHECKV(lx_join(&g_tidword[0]) == 0, g_tidword[0]);
    CHECKV(g_child_tid == ctid && g_child_fs_ok, g_child_tid);
    CHECK(tls_is(tcb));                                                   /* the parent's TLS untouched */
    CHECKV(sc0(LX_gettid) == pid, 0);
    /*
     * "Gone" is EVENTUAL, not immediate, and asserting it once was
     * wrong. The kernel wakes the joiner from `thread_clear_tid`,
     * which `process_thread_exit` calls immediately before
     * `thread_exit` -- deliberately, so a caller whose join returned
     * may create another thread at once (kernel/process/process.c).
     * Between that wake and `thread_exit` completing, the exiting
     * thread still resolves by tid, so `tgkill` can legitimately
     * return 0. Linux promises nothing stronger: "my join returned,
     * therefore that tid is unresolvable" was never true.
     *
     * The single-shot version failed twice on CI -- once on x86-64
     * and once on aarch64, both on commits that could not have
     * caused it -- and took eleven markers with it each time,
     * because `/etc/rc.linux` is `lxtest || exit 1`
     * (docs/testing/flakes.md). Wait for the condition the check
     * actually means instead of inferring it from the join.
     */
    long gone = 0;
    for (unsigned i = 0; i < 2000; i++) {
        gone = sc3(LX_tgkill, pid, ctid, 0);
        if (gone == -3)
            break;
        sc0(LX_sched_yield);   /* the exiting thread needs the CPU this loop is on */
    }
    CHECKV(gone == -3, gone);                                             /* gone */
    CHECKV(lx_clone(t_basic, g_stacks[0] + sizeof(g_stacks[0]), 0, LX_CLONE_VM | LX_CLONE_THREAD, 0, 0, 0) == -22, 0);   /* no SIGHAND: EINVAL */
    CHECKV(lx_clone(t_basic, g_stacks[0] + sizeof(g_stacks[0]), 0, THREAD_FLAGS, 0, &g_tidword[0], g_tcb) == -14, 0);   /* PARENT_SETTID to NULL */
    CHECKV(sc6(LX_clone, 0x11, 0, 0, 0, 0, 0) == -38, 0);                /* a fork: ENOSYS */
    CHECKV(sc6(LX_clone, THREAD_FLAGS | 0x2000, 0, 0, 0, 0, 0) == -22, 0); /* CLONE_PTRACE: EINVAL */
    /* A signal to one thread interrupts its read (EINTR); with SA_RESTART
     * the read restarts and completes with the data written afterwards. */
    CHECKV(sc2(LX_pipe2, g_pipe, 0) == 0, 0);
    struct lx_sigaction ta = { .handler = (uint64_t)(uintptr_t)thread_handler, .flags = LX_SA_SIGINFO | LX_SA_RESTORER,
                               .restorer = (uint64_t)(uintptr_t)lx_restorer };
    CHECKV(sc4(LX_rt_sigaction, 10, &ta, 0, 8) == 0, 0);
    g_read_rc[0] = g_read_rc[1] = 99;
    long tr0 = lx_clone(t_reader, g_stacks[1] + sizeof(g_stacks[1]), (void *)0, THREAD_FLAGS, &ptid, &g_tidword[1], g_tcb);
    CHECKV(tr0 > 0, tr0);
    nap_ms(30);
    CHECKV(sc3(LX_tgkill, pid, tr0, 10) == 0, 0);
    CHECKV(lx_join(&g_tidword[1]) == 0, 0);
    CHECKV(g_read_rc[0] == -4 && g_handler_tid == tr0, g_read_rc[0]);   /* EINTR, in that thread */
    ta.flags |= LX_SA_RESTART;
    CHECKV(sc4(LX_rt_sigaction, 10, &ta, 0, 8) == 0, 0);
    g_handler_tid = 0;
    long tr1 = lx_clone(t_reader, g_stacks[1] + sizeof(g_stacks[1]), (void *)1, THREAD_FLAGS, &ptid, &g_tidword[1], g_tcb);
    CHECKV(tr1 > 0, tr1);
    nap_ms(30);
    CHECKV(sc3(LX_tgkill, pid, tr1, 10) == 0, 0);
    nap_ms(30);
    CHECKV(g_read_rc[1] == 99 && g_handler_tid == tr1, g_read_rc[1]);   /* still reading, handler ran */
    CHECKV(sc3(LX_write, g_pipe[1], "abcd", 4) == 4, 0);
    CHECKV(lx_join(&g_tidword[1]) == 0, 0);
    CHECKV(g_read_rc[1] == 4, g_read_rc[1]);                              /* restarted, completed */
    sc1(LX_close, g_pipe[0]);
    sc1(LX_close, g_pipe[1]);
    /* Placement is one operation (M46): two threads mmap(NULL) at once and
     * none is told EEXIST, which Linux never answers and which a find then
     * a map under two holds of the space lock answered about half the
     * time (docs/audit/next-subsystem-mmap-place.md). */
    {
        long p0 = lx_clone(t_place, g_stacks[2] + sizeof(g_stacks[2]), (void *)0, THREAD_FLAGS, &ptid, &g_tidword[2], g_tcb);
        long p1 = lx_clone(t_place, g_stacks[3] + sizeof(g_stacks[3]), (void *)1, THREAD_FLAGS, &ptid, &g_tidword[3], g_tcb);
        CHECKV(p0 > 0 && p1 > 0, p0 > 0 ? p1 : p0);
        if (p0 > 0)
            CHECKV(lx_join(&g_tidword[2]) == 0, 0);
        if (p1 > 0)
            CHECKV(lx_join(&g_tidword[3]) == 0, 0);
        long eexist = 0, other = 0;
        for (int t = 0; t < 2; t++)
            for (int i = 0; i < LX_PLACE_ROUNDS; i++) {
                long v = g_place[t][i];
                if (v == -17)
                    eexist++;
                else if (v < 0 && v > -4096)
                    other++;
                else
                    sc2(LX_munmap, v, 4096);
            }
        CHECKV(eexist == 0, eexist);
        CHECKV(other == 0, other);
    }
    /* Requeue: two waiters on A (WAIT_BITSET, absolute realtime deadline)
     * move to B; only a wake on B releases them. */
    g_wait_rc[0] = g_wait_rc[1] = 99;
    g_waiting = 0;
    long tw0 = lx_clone(t_waiter, g_stacks[2] + sizeof(g_stacks[2]), (void *)0, THREAD_FLAGS, &ptid, &g_tidword[2], g_tcb);
    long tw1 = lx_clone(t_waiter, g_stacks[3] + sizeof(g_stacks[3]), (void *)1, THREAD_FLAGS, &ptid, &g_tidword[3], g_tcb);
    CHECKV(tw0 > 0 && tw1 > 0 && tw0 != tw1, tw1);
    for (int i = 0; i < 200 && g_waiting < 2; i++)
        nap_ms(5);
    nap_ms(30);                                                           /* both parked in the kernel */
    CHECKV(sc6(LX_futex, &g_futex_a, LX_FUTEX_CMP_REQUEUE, 0, 2, &g_futex_b, 1) == -11, 0);   /* value mismatch */
    long rq = sc6(LX_futex, &g_futex_a, LX_FUTEX_CMP_REQUEUE, 0, 2, &g_futex_b, 0);
    CHECKV(rq == 2, rq);
    CHECKV(sc6(LX_futex, &g_futex_a, LX_FUTEX_WAKE, 2, 0, 0, 0) == 0, 0);   /* nobody left on A */
    nap_ms(20);
    CHECKV(g_wait_rc[0] == 99 && g_wait_rc[1] == 99, g_wait_rc[0]);      /* still waiting, now on B */
    CHECKV(sc6(LX_futex, &g_futex_b, LX_FUTEX_WAKE_BITSET, 2, 0, 0, LX_FUTEX_BITSET_MATCH_ANY) == 2, 0);
    CHECKV(lx_join(&g_tidword[2]) == 0 && lx_join(&g_tidword[3]) == 0, 0);
    CHECKV(g_wait_rc[0] == 0 && g_wait_rc[1] == 0, g_wait_rc[1]);
    /* An absolute deadline already past: ETIMEDOUT at once. */
    struct lx_timespec past = { 1, 0 };
    CHECKV(sc6(LX_futex, &g_futex_a, LX_FUTEX_WAIT_BITSET | LX_FUTEX_CLOCK_REALTIME, 0, &past, 0, LX_FUTEX_BITSET_MATCH_ANY) == -110, 0);
    CHECKV(sc6(LX_futex, &g_futex_a, LX_FUTEX_WAIT_BITSET, 0, &past, 0, 0x1) == -38, 0);   /* a real bitset: ENOSYS */
    /* sched_getaffinity: the online CPUs, 8 bytes. */
    uint64_t cpus = 0;
    CHECKV(sc3(LX_sched_getaffinity, 0, 8, &cpus) == 8 && cpus != 0 && (cpus & 1), cpus);
    CHECKV(sc3(LX_sched_getaffinity, 0, 4, &cpus) == -22, 0);
    CHECKV(sc3(LX_sched_getaffinity, 999999, 8, &cpus) == -3, 0);
    CHECKV(sc3(LX_sched_setaffinity, 0, 8, &cpus) == 0, 0);                /* accepted, ignored */
    CHECKV(sc3(LX_sched_setaffinity, 0, 4, &cpus) == -22, 0);
    CHECKV(sc4(LX_rt_sigaction, 10, &dfl, 0, 8) == 0, 0);

    /* --- poll and ppoll (milestone 10) --- */
    CHECKV(sc2(LX_pipe2, g_pipe, 0) == 0, 0);
    struct lx_pollfd pf[3] = { { g_pipe[0], LX_POLLIN, 0 }, { g_pipe[1], LX_POLLOUT, 0 }, { 77, LX_POLLIN, 0 } };
    CHECKV(lx_poll_ms(pf, 2, 0) == 1 && pf[0].revents == 0 && pf[1].revents == LX_POLLOUT, pf[1].revents);
    CHECKV(lx_poll_ms(pf, 3, 0) == 2 && pf[2].revents == LX_POLLNVAL, pf[2].revents);   /* a bad fd: POLLNVAL, at once */
    pf[2].fd = -1;
    CHECKV(lx_poll_ms(pf, 3, 0) == 1 && pf[2].revents == 0, pf[2].revents);              /* negative: ignored */
    struct lx_timespec pt0, pt1;
    sc2(LX_clock_gettime, LX_CLOCK_MONOTONIC, &pt0);
    CHECKV(lx_poll_ms(pf, 1, 20) == 0, 0);                                                /* 20 ms, nothing */
    sc2(LX_clock_gettime, LX_CLOCK_MONOTONIC, &pt1);
    long pdt = (pt1.tv_sec - pt0.tv_sec) * 1000000000L + (pt1.tv_nsec - pt0.tv_nsec);
    CHECKV(pdt >= 15000000 && pdt < 1000000000, pdt);
    long tlw = lx_clone(t_late_writer, g_stacks[0] + sizeof(g_stacks[0]), 0, THREAD_FLAGS, &ptid, &g_tidword[0], g_tcb);
    CHECKV(tlw > 0, tlw);
    CHECKV(lx_poll_ms(pf, 1, -1) == 1 && pf[0].revents == LX_POLLIN, pf[0].revents);    /* woken by the write */
    CHECKV(lx_join(&g_tidword[0]) == 0, 0);
    char pc;
    CHECKV(sc3(LX_read, g_pipe[0], &pc, 1) == 1 && pc == 'z', pc);
    struct lx_timespec pzero = { 0, 0 };
    CHECKV(sc4(LX_ppoll, pf, 1, &pzero, 0) == 0, 0);
    CHECKV(lx_poll_ms(pf, 2000, 0) == -22, 0);                                            /* nfds > 1024 */
    /* ppoll with a mask: the pending, unblocked-for-the-wait SIGUSR1 runs
     * its handler, the call is EINTR, the old mask is back. */
    sig_install(10, 0);
    CHECKV(sc4(LX_rt_sigprocmask, LX_SIG_BLOCK, &usr1, 0, 8) == 0, 0);
    int before = g_sig.count;
    CHECKV(sc2(LX_kill, pid, 10) == 0 && g_sig.count == before, g_sig.count);
    uint64_t none = 0;
    CHECKV(sc6(LX_ppoll, pf, 1, 0, &none, 8, 0) == -4, 0);
    CHECKV(g_sig.count == before + 1, g_sig.count);
    CHECKV(sc4(LX_rt_sigprocmask, LX_SIG_BLOCK, 0, &oset, 8) == 0 && (oset & usr1) != 0, oset);
    CHECKV(sc4(LX_rt_sigprocmask, LX_SIG_UNBLOCK, &usr1, 0, 8) == 0, 0);
    CHECKV(sc4(LX_rt_sigaction, 10, &dfl, 0, 8) == 0, 0);
    CHECKV(sc1(LX_close, g_pipe[1]) == 0, 0);
    CHECKV(lx_poll_ms(pf, 1, 0) == 1 && (pf[0].revents & LX_POLLHUP) && (pf[0].revents & LX_POLLIN), pf[0].revents);   /* writer gone */
    sc1(LX_close, g_pipe[0]);

    /* --- select and pselect6 (the device-readiness unit): fd_sets over io_poll --- */
    {
        CHECKV(sc2(LX_pipe2, g_pipe, 0) == 0, 0);
        uint64_t rset[16], wset[16], xset[16];
        /* the write end ready, the read end not, with a zero timeout */
        lx_fdzero(rset); lx_fdzero(wset); lx_fdzero(xset);
        lx_fdset(rset, g_pipe[0]); lx_fdset(wset, g_pipe[1]); lx_fdset(xset, g_pipe[0]);
        struct lx_timespec szero = { 0, 0 };
        long sn = sc6(LX_pselect6, g_pipe[1] + 1, rset, wset, xset, &szero, 0);
        CHECKV(sn == 1 && !lx_fdisset(rset, g_pipe[0]) && lx_fdisset(wset, g_pipe[1]) && !lx_fdisset(xset, g_pipe[0]), sn);
        CHECKV(sc6(LX_pselect6, 1025, rset, 0, 0, &szero, 0) == -22, 0);           /* nfds > FD_SETSIZE */
        /* the fault paths: an unreadable set, an unwritable one, an unreadable
         * sigmask pair, a sigmask of the wrong size */
        CHECKV(sc6(LX_pselect6, 8, 1, 0, 0, &szero, 0) == -14, 0);
        CHECKV(sc6(LX_pselect6, 8, 0, 0, 0, &szero, 1) == -14, 0);
        {
            struct { uint64_t ss; uint64_t ss_len; } sbadlen = { (uint64_t)(uintptr_t)&szero, 4 };
            CHECKV(sc6(LX_pselect6, g_pipe[1] + 1, 0, wset, 0, &szero, &sbadlen) == -22, 0);
            struct { uint64_t ss; uint64_t ss_len; } sbadptr = { 1, 8 };
            CHECKV(sc6(LX_pselect6, g_pipe[1] + 1, 0, wset, 0, &szero, &sbadptr) == -14, 0);
        }
        lx_fdzero(rset); lx_fdset(rset, 60);                                        /* a closed fd's bit */
        CHECKV(sc6(LX_pselect6, 61, rset, 0, 0, &szero, 0) == -9, 0);               /* EBADF */
        CHECKV(sc6(LX_pselect6, 5, rset, 0, 0, &szero, 0) == 0, 0);                 /* the same bit above nfds: not looked at */
        /* an error condition is not an exceptional one: a pipe's write end
         * with its reader gone is WRITABLE|ERROR, and select's except set
         * (POLLPRI) never carries it */
        {
            int ep[2];
            CHECKV(sc2(LX_pipe2, ep, 0) == 0, 0);
            sc1(LX_close, ep[0]);
            lx_fdzero(xset); lx_fdset(xset, ep[1]);
            CHECKV(sc6(LX_pselect6, ep[1] + 1, 0, 0, xset, &szero, 0) == 0 && !lx_fdisset(xset, ep[1]), 0);
            /* and an except-only fd with an error condition does not end the
             * wait early: a 20 ms timeout still takes at least 15 ms */
            struct lx_timespec x20 = { 0, 20000000 }, xt0, xt1;
            lx_fdzero(xset); lx_fdset(xset, ep[1]);
            sc2(LX_clock_gettime, LX_CLOCK_MONOTONIC, &xt0);
            CHECKV(sc6(LX_pselect6, ep[1] + 1, 0, 0, xset, &x20, 0) == 0, 0);
            sc2(LX_clock_gettime, LX_CLOCK_MONOTONIC, &xt1);
            long xdt = (xt1.tv_sec - xt0.tv_sec) * 1000000000L + (xt1.tv_nsec - xt0.tv_nsec);
            CHECKV(xdt >= 15000000 && xdt < 1000000000, xdt);
            lx_fdzero(wset); lx_fdset(wset, ep[1]);
            CHECKV(sc6(LX_pselect6, ep[1] + 1, 0, wset, 0, &szero, 0) == 1 && lx_fdisset(wset, ep[1]), 0);   /* writable (POLLERR is in the writable set) */
            sc1(LX_close, ep[1]);
        }
        /* a 20 ms timeout with nothing ready returns 0 after at least 15 ms */
        lx_fdzero(rset); lx_fdset(rset, g_pipe[0]);
        struct lx_timespec s20 = { 0, 20000000 }, st0, st1;
        sc2(LX_clock_gettime, LX_CLOCK_MONOTONIC, &st0);
        CHECKV(sc6(LX_pselect6, g_pipe[0] + 1, rset, 0, 0, &s20, 0) == 0, 0);
        sc2(LX_clock_gettime, LX_CLOCK_MONOTONIC, &st1);
        long sdt = (st1.tv_sec - st0.tv_sec) * 1000000000L + (st1.tv_nsec - st0.tv_nsec);
        CHECKV(sdt >= 15000000 && sdt < 1000000000, sdt);
        /* a byte written: readable */
        CHECKV(sc3(LX_write, g_pipe[1], "s", 1) == 1, 0);
        lx_fdzero(rset); lx_fdset(rset, g_pipe[0]);
        CHECKV(sc6(LX_pselect6, g_pipe[0] + 1, rset, 0, 0, 0, 0) == 1 && lx_fdisset(rset, g_pipe[0]), 0);
        char sc;
        CHECKV(sc3(LX_read, g_pipe[0], &sc, 1) == 1 && sc == 's', sc);
        /* a mask admitting a pending SIGUSR1: the handler runs, -EINTR, the old mask back */
        sig_install(10, 0);
        CHECKV(sc4(LX_rt_sigprocmask, LX_SIG_BLOCK, &usr1, 0, 8) == 0, 0);
        int sbefore = g_sig.count;
        CHECKV(sc2(LX_kill, pid, 10) == 0 && g_sig.count == sbefore, g_sig.count);
        uint64_t snone = 0;
        struct { uint64_t ss; uint64_t ss_len; } sarg = { (uint64_t)(uintptr_t)&snone, 8 };
        lx_fdzero(rset); lx_fdset(rset, g_pipe[0]);
        CHECKV(sc6(LX_pselect6, g_pipe[0] + 1, rset, 0, 0, 0, &sarg) == -4, 0);
        CHECKV(g_sig.count == sbefore + 1, g_sig.count);
        CHECKV(sc4(LX_rt_sigprocmask, LX_SIG_BLOCK, 0, &oset, 8) == 0 && (oset & usr1) != 0, oset);
        CHECKV(sc4(LX_rt_sigprocmask, LX_SIG_UNBLOCK, &usr1, 0, 8) == 0, 0);
        CHECKV(sc4(LX_rt_sigaction, 10, &dfl, 0, 8) == 0, 0);
#ifdef LX_select
        /* the legacy select, x86-64: a timeval */
        struct lx_timeval stv = { 0, 0 };
        lx_fdzero(rset); lx_fdzero(wset);
        lx_fdset(rset, g_pipe[0]); lx_fdset(wset, g_pipe[1]);
        CHECKV(sc6(LX_select, g_pipe[1] + 1, rset, wset, 0, &stv, 0) == 1 && lx_fdisset(wset, g_pipe[1]), 0);
        struct lx_timeval sbad = { 0, 2000000 };
        CHECKV(sc6(LX_select, g_pipe[1] + 1, rset, 0, 0, &sbad, 0) == -22, 0);
        /* a timeval whose seconds times 1e9 wraps 64 bits to 290 ms must not
         * become a 290 ms wait: a writer 500 ms out still ends it with 1 */
        {
            struct lx_timeval shuge = { 18446744074LL, 0 };
            long tlw2 = lx_clone(t_later_writer, g_stacks[0] + sizeof(g_stacks[0]), 0, THREAD_FLAGS, &ptid, &g_tidword[0], g_tcb);
            CHECKV(tlw2 > 0, tlw2);
            lx_fdzero(rset); lx_fdset(rset, g_pipe[0]);
            CHECKV(sc6(LX_select, g_pipe[0] + 1, rset, 0, 0, &shuge, 0) == 1 && lx_fdisset(rset, g_pipe[0]), 0);
            CHECKV(lx_join(&g_tidword[0]) == 0, 0);
            char wc;
            CHECKV(sc3(LX_read, g_pipe[0], &wc, 1) == 1 && wc == 'w', wc);
        }
#endif
        /* the writer closed: readable (Linux's readable set includes POLLHUP) */
        sc1(LX_close, g_pipe[1]);
        lx_fdzero(rset); lx_fdset(rset, g_pipe[0]);
        CHECKV(sc6(LX_pselect6, g_pipe[0] + 1, rset, 0, 0, &szero, 0) == 1 && lx_fdisset(rset, g_pipe[0]), 0);
        sc1(LX_close, g_pipe[0]);
        /* a select over the tap and a socket: neither ready; an ARP request
         * written to the tap, and the tap's read bit comes back set alone */
        long tapfd = sc4(LX_openat, LX_AT_FDCWD, "/dev/net/tap", LX_O_RDWR | LX_O_NONBLOCK, 0);
        CHECKV(tapfd >= 3, tapfd);
        long us = sc3(LX_socket, LX_AF_INET, LX_SOCK_DGRAM, 0);
        CHECKV(us >= 3, us);
        lx_fdzero(rset); lx_fdset(rset, tapfd); lx_fdset(rset, us);
        long mx = tapfd > us ? tapfd : us;
        CHECKV(sc6(LX_pselect6, mx + 1, rset, 0, 0, &szero, 0) == 0, 0);
        for (unsigned k = 0; k < 8; k++) {
            uint8_t req[42];
            lx_arp_request(req, k);
            CHECKV(sc3(LX_write, tapfd, req, 42) == 42, k);
        }
        struct lx_timespec s2 = { 2, 0 };
        lx_fdzero(rset); lx_fdset(rset, tapfd); lx_fdset(rset, us);
        long tn = sc6(LX_pselect6, mx + 1, rset, 0, 0, &s2, 0);
        CHECKV(tn == 1 && lx_fdisset(rset, tapfd) && !lx_fdisset(rset, us), tn);
        uint8_t tfr[2048];
        long tl = sc3(LX_read, tapfd, tfr, sizeof(tfr));
        CHECKV(tl >= 42 && tfr[12] == 0x08 && tfr[13] == 0x06 && tfr[21] == 2, tl);
        /* the bench: pselect6 against ppoll, one descriptor and sixty-four bits' worth, per call */
        {
            struct lx_pollfd bp[1] = { { us, LX_POLLIN, 0 } };
            struct lx_timespec bt0, bt1;
            sc2(LX_clock_gettime, LX_CLOCK_MONOTONIC, &bt0);
            for (int i = 0; i < 2000; i++)
                sc4(LX_ppoll, bp, 1, &szero, 0);
            sc2(LX_clock_gettime, LX_CLOCK_MONOTONIC, &bt1);
            long ppoll_ns = ((bt1.tv_sec - bt0.tv_sec) * 1000000000L + (bt1.tv_nsec - bt0.tv_nsec)) / 2000;
            lx_fdzero(rset); lx_fdset(rset, us);
            sc2(LX_clock_gettime, LX_CLOCK_MONOTONIC, &bt0);
            for (int i = 0; i < 2000; i++)
                sc6(LX_pselect6, us + 1, rset, 0, 0, &szero, 0);
            sc2(LX_clock_gettime, LX_CLOCK_MONOTONIC, &bt1);
            long psel_ns = ((bt1.tv_sec - bt0.tv_sec) * 1000000000L + (bt1.tv_nsec - bt0.tv_nsec)) / 2000;
            lx_puts("LINUXBENCH: pselect6 ");
            put_num(psel_ns);
            lx_puts(" ns per call on one descriptor, ppoll ");
            put_num(ppoll_ns);
            lx_puts("\n");
        }
        sc1(LX_close, us);
        sc1(LX_close, tapfd);
    }

    /* --- rlimits (milestone 6): one value, reported as cur == max --- */
    struct lx_rlimit rl;
    CHECKV(sc2(LX_getrlimit, LX_RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur == 64 && rl.rlim_max == 64, rl.rlim_cur);
    CHECKV(sc4(LX_prlimit64, 0, LX_RLIMIT_AS, 0, &rl) == 0 && rl.rlim_cur == (2ull << 30), rl.rlim_cur);
    CHECKV(sc2(LX_getrlimit, LX_RLIMIT_STACK, &rl) == 0 && rl.rlim_cur == LX_RLIM_INFINITY, 0);   /* not bounded here */
    struct lx_rlimit few = { 8, 8 };
    CHECKV(sc2(LX_setrlimit, LX_RLIMIT_NOFILE, &few) == 0, 0);
    long opened[8];
    int nopen = 0, emfile = 0;
    for (int i = 0; i < 8; i++) {
        long ofd = sc4(LX_openat, LX_AT_FDCWD, "/etc/rc", LX_O_RDONLY, 0);
        if (ofd >= 0)
            opened[nopen++] = ofd;
        else if (ofd == -24)
            emfile++;
    }
    CHECKV(nopen >= 1 && emfile >= 1, nopen);   /* EMFILE at the eighth handle */
    for (int i = 0; i < nopen; i++)
        sc1(LX_close, opened[i]);
    struct lx_rlimit many = { 64, 64 };
    CHECKV(sc2(LX_setrlimit, LX_RLIMIT_NOFILE, &many) == 0, 0);   /* root raises it back */
    struct lx_rlimit bad = { 10, 5 };
    CHECKV(sc2(LX_setrlimit, LX_RLIMIT_NOFILE, &bad) == -22, 0);
    CHECKV(sc4(LX_prlimit64, 99999, LX_RLIMIT_NOFILE, 0, &rl) == -1, 0);   /* EPERM: not ours */
    CHECKV(sc2(LX_setrlimit, LX_RLIMIT_STACK, &few) == 0, 0);   /* accepted and ignored */

    /* --- futex --- */
    static uint32_t word = 5;
    CHECKV(sc6(LX_futex, &word, LX_FUTEX_WAIT | LX_FUTEX_PRIVATE_FLAG, 6, 0, 0, 0) == -11, 0);   /* EAGAIN */
    struct lx_timespec to = { 0, 20000000 };
    CHECKV(sc6(LX_futex, &word, LX_FUTEX_WAIT, 5, &to, 0, 0) == -110, 0);   /* ETIMEDOUT */
    CHECKV(sc6(LX_futex, &word, LX_FUTEX_WAKE, 1, 0, 0, 0) == 0, 0);
    CHECKV(sc6(LX_futex, &word, 99, 0, 0, 0, 0) == -38, 0);
    /* A misaligned word is EINVAL on every path, the past-deadline
     * WAIT_BITSET one included (it used to read the word and answer
     * EAGAIN or ETIMEDOUT there; the shared-futex unit's review). */
    {
        struct lx_timespec gone_by;
        sc2(LX_clock_gettime, LX_CLOCK_MONOTONIC, &gone_by);
        if (gone_by.tv_sec > 0)
            gone_by.tv_sec -= 1;
        CHECKV(sc6(LX_futex, (char *)&word + 1, LX_FUTEX_WAIT_BITSET, 5, &gone_by, 0, LX_FUTEX_BITSET_MATCH_ANY) == -22, 0);
        CHECKV(sc6(LX_futex, (char *)&word + 1, LX_FUTEX_WAKE, 1, 0, 0, 0) == -22, 0);
    }

    /* --- random, sockets --- */
    unsigned char rnd[32] = { 0 };
    CHECKV(sc3(LX_getrandom, rnd, 32, 0) == 32, 0);
    int zero = 1;
    for (int i = 0; i < 32; i++)
        if (rnd[i])
            zero = 0;
    CHECK(!zero);
    long s = sc3(LX_socket, LX_AF_INET, LX_SOCK_DGRAM | LX_SOCK_CLOEXEC, 0);
    CHECKV(s >= 3, s);
    struct lx_sockaddr_in me = { .sin_family = LX_AF_INET, .sin_port = (uint16_t)((40100 >> 8) | (40100 << 8)),
                                 .sin_addr = 0x0100007f };
    CHECKV(sc3(LX_bind, s, &me, sizeof(me)) == 0, 0);
    CHECKV(sc6(LX_sendto, s, "ping", 4, 0, &me, sizeof(me)) == 4, 0);
    struct lx_sockaddr_in from;
    int32_t flen = sizeof(from);
    CHECKV(sc6(LX_recvfrom, s, buf, 16, 0, &from, &flen) == 4 && memeq(buf, "ping", 4), 0);
    CHECKV(flen == 16 && from.sin_family == LX_AF_INET && from.sin_port == me.sin_port && from.sin_addr == 0x0100007f, flen);
    int32_t short_len = 4;
    struct { struct lx_sockaddr_in a; uint32_t canary; } box = { .canary = 0xdeadbeef };
    CHECKV(sc3(LX_getsockname, s, &box.a, &short_len) == 0 && short_len == 16 && box.canary == 0xdeadbeef, short_len);
    CHECKV(sc2(LX_fstat, s, &st) == 0 && (st.st_mode & LX_S_IFMT) == LX_S_IFSOCK, 0);
    int one = 1;
    /* setsockopt used to return 0 for every SOL_SOCKET option while doing
     * nothing, and this line asserted it. A program that set SO_RCVTIMEO
     * was told it worked and then blocked forever
     * (docs/audit/next-subsystem-socket-verdict.md, Design 4). Nothing is
     * settable here yet, so every option is refused the way Linux refuses
     * one a protocol does not implement, and a program's existing error
     * path sees it. */
    CHECKV(sc6(LX_setsockopt, s, LX_SOL_SOCKET, 2, &one, 4, 0) == -92, 0);   /* SO_REUSEADDR: ENOPROTOOPT */
    CHECKV(sc6(LX_setsockopt, s, 6 /* SOL_TCP */, 1, &one, 4, 0) == -92, 0);
    /* getsockopt answers one question, through the same kernel path the
     * native SYS_getsockopt takes: the pending error, positive as POSIX
     * asks, and cleared by the read. This socket has none. */
    int err = 0x5a5a5a;
    int32_t elen = sizeof(err);
    CHECKV(sc6(LX_getsockopt, s, LX_SOL_SOCKET, LX_SO_ERROR, &err, &elen, 0) == 0, 0);
    CHECKV(err == 0 && elen == 4, err);
    CHECKV(sc6(LX_getsockopt, s, LX_SOL_SOCKET, 2, &err, &elen, 0) == -92, 0);   /* every other option */
    int32_t narrow = 2;
    CHECKV(sc6(LX_getsockopt, s, LX_SOL_SOCKET, LX_SO_ERROR, &err, &narrow, 0) == -22, 0);   /* EINVAL, not a truncated verdict */
    CHECKV(sc1(LX_close, s) == 0, 0);

    /* A connect to a closed loopback port fails, and the socket can be
     * asked which way afterwards rather than only that it did. */
    long ce = sc3(LX_socket, LX_AF_INET, LX_SOCK_STREAM, 0);
    CHECKV(ce >= 3, ce);
    struct lx_sockaddr_in shut = { .sin_family = LX_AF_INET,
                                   .sin_port = (uint16_t)((40199 >> 8) | (40199 << 8)),
                                   .sin_addr = 0x0100007f };
    CHECKV(sc3(LX_connect, ce, &shut, sizeof(shut)) == -111, 0);   /* ECONNREFUSED */
    err = 0x5a5a5a;
    elen = sizeof(err);
    CHECKV(sc6(LX_getsockopt, ce, LX_SOL_SOCKET, LX_SO_ERROR, &err, &elen, 0) == 0, 0);
    CHECKV(err == 111, err);                                        /* positive, as POSIX asks */
    CHECKV(sc1(LX_close, ce) == 0, 0);
    /* --- unix domain sockets: a name, a pair, a handle in a message --- */
    CHECKV(sc3(LX_socket, LX_AF_UNIX, LX_SOCK_SEQPACKET, 0) == -94, 0);   /* ESOCKTNOSUPPORT */
    (void)sc3(LX_unlinkat, LX_AT_FDCWD, "/tmp/lx-ux", 0);
    long uls = sc3(LX_socket, LX_AF_UNIX, LX_SOCK_STREAM | LX_SOCK_CLOEXEC, 0);
    CHECKV(uls >= 3, uls);
    struct lx_sockaddr_un uname;
    __builtin_memset(&uname, 0, sizeof(uname));
    uname.sun_family = LX_AF_UNIX;
    __builtin_memcpy(uname.sun_path, "/tmp/lx-ux", 11);
    CHECKV(sc3(LX_bind, uls, &uname, 2 + 11) == 0, 0);
    CHECKV(sc2(LX_listen, uls, 2) == 0, 0);
    CHECKV(sc4(LX_openat, LX_AT_FDCWD, "/tmp/lx-ux", LX_O_RDONLY, 0) == -6, 0);   /* ENXIO */
    long ucs = sc3(LX_socket, LX_AF_UNIX, LX_SOCK_STREAM, 0);
    CHECKV(ucs >= 3, ucs);
    CHECKV(sc3(LX_connect, ucs, &uname, 2 + 10) == 0, 0);   /* unterminated: as long as the length says */
    CHECKV(sc3(LX_write, ucs, "hi", 2) == 2, 0);            /* before anyone accepts */
    struct lx_sockaddr_un upeer;
    int32_t uplen = sizeof(upeer);
    long uas = sc4(LX_accept4, uls, &upeer, &uplen, 0);
    CHECKV(uas >= 3 && uplen == 2, uplen);                   /* the client has no name */
    CHECKV(sc3(LX_read, uas, buf, 16) == 2 && memeq(buf, "hi", 2), 0);
    uplen = sizeof(upeer);
    CHECKV(sc3(LX_getpeername, ucs, &upeer, &uplen) == 0 && uplen == 2 + 11 && upeer.sun_family == LX_AF_UNIX &&
               memeq(upeer.sun_path, "/tmp/lx-ux", 11), uplen);
    struct lx_ucred ucr = { 0 };
    int32_t ucl = sizeof(ucr);
    CHECKV(sc6(LX_getsockopt, uas, LX_SOL_SOCKET, LX_SO_PEERCRED, &ucr, &ucl, 0) == 0 && ucr.pid == sc0(LX_getpid), ucr.pid);
    /* A pipe's write end rides in a message; what arrives writes into the pipe. */
    int32_t upipe[2];
    CHECKV(sc2(LX_pipe2, upipe, 0) == 0, 0);
    struct { struct lx_cmsghdr h; int32_t fd; uint8_t pad[4]; } ctl = {
        .h = { .cmsg_len = 20, .cmsg_level = LX_SOL_SOCKET, .cmsg_type = LX_SCM_RIGHTS }, .fd = upipe[1] };
    struct lx_iovec uiov = { .iov_base = (uint64_t)(uintptr_t)"m", .iov_len = 1 };
    struct lx_msghdr umsg = { .msg_iov = (uint64_t)(uintptr_t)&uiov, .msg_iovlen = 1,
                              .msg_control = (uint64_t)(uintptr_t)&ctl, .msg_controllen = sizeof(ctl) };
    long smrc = sc3(LX_sendmsg, ucs, &umsg, 0);
    CHECKV(smrc == 1, smrc);
    struct { struct lx_cmsghdr h; int32_t fd; uint8_t pad[4]; } rctl;
    __builtin_memset(&rctl, 0, sizeof(rctl));
    struct lx_iovec uriov = { .iov_base = (uint64_t)(uintptr_t)buf, .iov_len = 16 };
    struct lx_msghdr rmsg = { .msg_iov = (uint64_t)(uintptr_t)&uriov, .msg_iovlen = 1,
                              .msg_control = (uint64_t)(uintptr_t)&rctl, .msg_controllen = sizeof(rctl) };
    long rm1 = sc3(LX_recvmsg, uas, &rmsg, LX_MSG_DONTWAIT);   /* the message is there, or the test says why not */
    CHECKV(rm1 == 1 && buf[0] == 'm', rm1);
    CHECKV(rmsg.msg_controllen == 24 && rctl.h.cmsg_type == LX_SCM_RIGHTS && rctl.h.cmsg_len == 20 && rctl.fd >= 3, rctl.fd);
    CHECKV(rmsg.msg_flags == 0, rmsg.msg_flags);
    CHECKV(sc3(LX_write, rctl.fd, "z", 1) == 1 && sc3(LX_read, upipe[0], buf, 4) == 1 && buf[0] == 'z', 0);
    CHECKV(sc1(LX_close, rctl.fd) == 0, 0);
    /* Too little control room: the bytes arrive, the handle is closed, MSG_CTRUNC says so. */
    ctl.fd = upipe[1];
    smrc = sc3(LX_sendmsg, ucs, &umsg, 0);
    CHECKV(smrc == 1, smrc);
    rmsg.msg_controllen = 8;
    long rmrc = sc3(LX_recvmsg, uas, &rmsg, LX_MSG_DONTWAIT);
    CHECKV(rmrc == 1, rmrc);
    CHECKV((rmsg.msg_flags & LX_MSG_CTRUNC) && rmsg.msg_controllen == 0, rmsg.msg_flags);
    CHECKV(sc1(LX_close, upipe[0]) == 0 && sc1(LX_close, upipe[1]) == 0, 0);
    CHECKV(sc1(LX_close, ucs) == 0, 0);
    CHECKV(sc3(LX_read, uas, buf, 16) == 0, 0);             /* end of stream */
    CHECKV(sc1(LX_close, uas) == 0 && sc1(LX_close, uls) == 0, 0);
    CHECKV(sc3(LX_unlinkat, LX_AT_FDCWD, "/tmp/lx-ux", 0) == 0, 0);
    /* An abstract name, and a pair. */
    long als = sc3(LX_socket, LX_AF_UNIX, LX_SOCK_STREAM, 0), acs = sc3(LX_socket, LX_AF_UNIX, LX_SOCK_STREAM, 0);
    CHECKV(als >= 3 && acs >= 3, als);
    __builtin_memset(&uname, 0, sizeof(uname));
    uname.sun_family = LX_AF_UNIX;
    __builtin_memcpy(uname.sun_path + 1, "lxabs", 5);
    CHECKV(sc3(LX_bind, als, &uname, 2 + 1 + 5) == 0 && sc2(LX_listen, als, 1) == 0, 0);
    CHECKV(sc3(LX_connect, acs, &uname, 2 + 1 + 5) == 0, 0);
    uplen = sizeof(upeer);
    long aas = sc4(LX_accept4, als, &upeer, &uplen, LX_SOCK_NONBLOCK);
    CHECKV(aas >= 3, aas);
    uplen = sizeof(upeer);
    CHECKV(sc3(LX_getpeername, acs, &upeer, &uplen) == 0 && uplen == 2 + 1 + 5 && upeer.sun_path[0] == 0 &&
               memeq(upeer.sun_path + 1, "lxabs", 5), uplen);
    CHECKV(sc3(LX_read, aas, buf, 4) == -11, 0);            /* EAGAIN: the accepted end is non-blocking */
    CHECKV(sc1(LX_close, aas) == 0 && sc1(LX_close, acs) == 0 && sc1(LX_close, als) == 0, 0);
    int32_t usv[2] = { -1, -1 };
    CHECKV(sc4(LX_socketpair, LX_AF_UNIX, LX_SOCK_STREAM, 0, usv) == 0 && usv[0] >= 3 && usv[1] >= 3, usv[0]);
    CHECKV(sc3(LX_write, usv[0], "pair", 4) == 4 && sc3(LX_read, usv[1], buf, 16) == 4 && memeq(buf, "pair", 4), 0);
    CHECKV(sc1(LX_close, usv[0]) == 0 && sc1(LX_close, usv[1]) == 0, 0);
    /* A datagram by name. */
    (void)sc3(LX_unlinkat, LX_AT_FDCWD, "/tmp/lx-dg", 0);
    long ud1 = sc3(LX_socket, LX_AF_UNIX, LX_SOCK_DGRAM, 0), ud2 = sc3(LX_socket, LX_AF_UNIX, LX_SOCK_DGRAM, 0);
    __builtin_memset(&uname, 0, sizeof(uname));
    uname.sun_family = LX_AF_UNIX;
    __builtin_memcpy(uname.sun_path, "/tmp/lx-dg", 10);
    CHECKV(ud1 >= 3 && ud2 >= 3 && sc3(LX_bind, ud1, &uname, 2 + 11) == 0, 0);
    CHECKV(sc6(LX_sendto, ud2, "dg", 2, 0, &uname, 2 + 11) == 2, 0);
    uplen = sizeof(upeer);
    CHECKV(sc6(LX_recvfrom, ud1, buf, 16, 0, &upeer, &uplen) == 2 && uplen == 2 && memeq(buf, "dg", 2), uplen);
    CHECKV(sc1(LX_close, ud1) == 0 && sc1(LX_close, ud2) == 0, 0);
    CHECKV(sc3(LX_unlinkat, LX_AT_FDCWD, "/tmp/lx-dg", 0) == 0, 0);

    /* --- non-blocking sockets: SOCK_NONBLOCK, accept4, EINPROGRESS --- */
    long nb = sc3(LX_socket, LX_AF_INET, LX_SOCK_DGRAM | LX_SOCK_NONBLOCK, 0);
    CHECKV(nb >= 3, nb);
    me.sin_port = (uint16_t)((40101 >> 8) | (40101 << 8));
    CHECKV(sc3(LX_bind, nb, &me, sizeof(me)) == 0, 0);
    CHECKV(sc6(LX_recvfrom, nb, buf, 16, 0, 0, 0) == -11, 0);                 /* EAGAIN */
    CHECKV((sc3(LX_fcntl, nb, LX_F_GETFL, 0) & LX_O_NONBLOCK) != 0, 0);
    CHECKV(sc1(LX_close, nb) == 0, 0);
    long ls = sc3(LX_socket, LX_AF_INET, LX_SOCK_STREAM, 0);
    me.sin_port = (uint16_t)((40102 >> 8) | (40102 << 8));
    CHECKV(ls >= 3 && sc3(LX_bind, ls, &me, sizeof(me)) == 0 && sc2(LX_listen, ls, 2) == 0, 0);
    CHECKV(sc3(LX_fcntl, ls, LX_F_SETFL, LX_O_NONBLOCK) == 0, 0);
    CHECKV(sc4(LX_accept4, ls, 0, 0, LX_SOCK_NONBLOCK) == -11, 0);            /* EAGAIN: nobody yet */
    long cs = sc3(LX_socket, LX_AF_INET, LX_SOCK_STREAM | LX_SOCK_NONBLOCK, 0);
    long crc = sc3(LX_connect, cs, &me, sizeof(me));
    CHECKV(crc == 0 || crc == -115, crc);                                      /* EINPROGRESS */
    long as = -11;
    for (int i = 0; i < 200 && as == -11; i++) {
        as = sc4(LX_accept4, ls, 0, 0, LX_SOCK_NONBLOCK);
        if (as == -11)
            sc2(LX_nanosleep, &nap, 0);
    }
    CHECKV(as >= 3, as);
    CHECKV(sc3(LX_connect, cs, &me, sizeof(me)) == -106, 0);                   /* EISCONN */
    CHECKV(sc6(LX_recvfrom, as, buf, 16, 0, 0, 0) == -11, 0);                  /* the accepted end inherited NONBLOCK */
    CHECKV(sc6(LX_sendto, cs, "hey", 3, 0, 0, 0) == 3, 0);
    long got = -11;
    for (int i = 0; i < 200 && got == -11; i++) {
        got = sc6(LX_recvfrom, as, buf, 16, 0, 0, 0);
        if (got == -11)
            sc2(LX_nanosleep, &nap, 0);
    }
    CHECKV(got == 3 && memeq(buf, "hey", 3), got);
    sc1(LX_close, as);
    sc1(LX_close, cs);
    sc1(LX_close, ls);

    /* --- unknown numbers --- */
    CHECKV(sc0(510) == -38, 0);
    CHECKV(sc0(9999) == -38, 0);

    if (g_failures == 0) {
        lx_puts("LINUXTEST: PASS\n");
        return 0;
    }
    lx_puts("LINUXTEST: FAIL (");
    put_num(g_failures);
    lx_puts(" checks)\n");
    return 1;
}
