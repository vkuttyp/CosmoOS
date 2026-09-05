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
    /* No FP/SIMD state at EL0: the reserved area starts with the esr_context. */
    struct lx_esr_context esr;
    __builtin_memcpy(&esr, uc->uc_mcontext.reserved, sizeof(esr));
    g_sig.fpstate_ok = esr.magic == LX_ESR_MAGIC && esr.size == 16 && uc->uc_mcontext.pc != 0;
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
#ifdef LX_stat
    CHECKV(sc2(LX_stat, "/tmp/nope", &st) == -2, 0);           /* ENOENT */
    CHECKV(sc2(LX_stat, "/tmp", &st) == 0 && (st.st_mode & LX_S_IFMT) == LX_S_IFDIR, 0);
#endif
    CHECKV(sc4(LX_newfstatat, LX_AT_FDCWD, "/tmp/nope", &st, 0) == -2, 0);
    CHECKV(sc4(LX_newfstatat, LX_AT_FDCWD, "/tmp", &st, 0) == 0 && (st.st_mode & LX_S_IFMT) == LX_S_IFDIR, 0);
    CHECKV(sc2(LX_fstat, 0, &st) == 0 && (st.st_mode & LX_S_IFMT) == LX_S_IFCHR, st.st_mode);
    CHECKV(sc3(LX_ioctl, 1, 0x5401, buf) == -25, 0);           /* TCGETS: ENOTTY */
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
    CHECKV(sc1(LX_close, dfd) == 0, 0);
    CHECKV(sc3(LX_unlinkat, LX_AT_FDCWD, "/tmp/lxdir/moved", 0) == 0, 0);
    CHECKV(sc3(LX_unlinkat, LX_AT_FDCWD, "/tmp/lxdir", LX_AT_REMOVEDIR) == 0, 0);
    CHECKV(sc1(LX_chdir, "/tmp") == 0, 0);
    CHECKV(sc2(LX_getcwd, buf, sizeof(buf)) == 5 && streq(buf, "/tmp"), 0);   /* length includes the NUL */
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
    CHECKV(sc6(LX_mmap, 0, 4096, LX_PROT_READ | LX_PROT_WRITE, LX_MAP_SHARED, mfd, 0) == -95, 0);   /* EOPNOTSUPP */
    long fm3 = sc6(LX_mmap, 0, 4096, LX_PROT_READ, LX_MAP_SHARED, mfd, 0);                          /* read-only shared: a snapshot */
    CHECKV(fm3 > 0 && memeq((const void *)fm3, pattern, 4096), fm3);
    if (fm3 > 0)
        sc2(LX_munmap, fm3, 4096);
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
    CHECKV(sc2(LX_kill, pid, 10) == 0, 0);   /* no FP/SIMD state at EL0 to carry */
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
    CHECKV(sc3(LX_tgkill, pid, ctid, 0) == -3, 0);                        /* gone */
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
    CHECKV(sc6(LX_setsockopt, s, LX_SOL_SOCKET, 2, &one, 4, 0) == 0, 0);   /* SO_REUSEADDR: accepted */
    CHECKV(sc1(LX_close, s) == 0, 0);
    CHECKV(sc3(LX_socket, 1, LX_SOCK_STREAM, 0) == -97, 0);   /* AF_UNIX: EAFNOSUPPORT */

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
