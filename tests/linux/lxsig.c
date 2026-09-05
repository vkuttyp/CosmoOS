/*
 * lxsig.c - a Linux program that dies the way its argument says
 * (docs/compat/linux/testing.md). rc.linux runs each mode and prints the
 * status the shell collected; the boot harness expects Linux's numbers:
 *   term      kill(getpid(), SIGTERM), the default action        143
 *   segv      a store through a null pointer, no handler         139
 *   ill       an undefined instruction                           132
 *   badret    rt_sigreturn to a non-canonical rip (the guard)    139
 *   badstack  rt_sigreturn to a non-canonical rsp, then a push   139
 *   group     a second thread calls exit_group(7)                  7
 *   lastthread the main thread exits; the other calls exit_group(5) 5
 */

#include "lxabi.h"

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

extern void lx_pusher(void);
#if defined(__x86_64__)
__asm__(".text\n"
        ".globl lx_pusher\n"
        "lx_pusher:\n"
        "    pushq $0\n"   /* the first stack access on a non-canonical rsp: #SS */
        "    ud2\n");
#define ILLEGAL "ud2"
#else
__asm__(".text\n"
        ".globl lx_pusher\n"
        "lx_pusher:\n"
        "    str x0, [sp, #-16]!\n"   /* the first stack access on a non-canonical sp: a data abort */
        "    udf #0\n");
#define ILLEGAL "udf #0"
#endif

/* rt_sigreturn with a ucontext we wrote, laid out where the restorer
 * leaves the stack: x86-64 rsp at `uc` (RET popped pretcode); AArch64 sp at
 * the frame record below the rt_sigframe. */
static void sigreturn_to(unsigned long pc, unsigned long sp)
{
#if defined(__x86_64__)
    static struct lx_ucontext_x86 uc;
    uc.uc_mcontext.rip = pc;
    uc.uc_mcontext.rsp = sp;
    uc.uc_mcontext.eflags = 0x202;
    uc.uc_sigmask = 0;
    __asm__ volatile("movq %0, %%rsp\n\t"
                     "movl $15, %%eax\n\t"
                     "syscall\n\t"
                     "ud2"
                     : : "r"(&uc) : "memory");
#else
    static struct {
        uint64_t record[2];
        struct lx_rt_sigframe_a64 f;
    } blob __attribute__((aligned(16)));
    blob.f.uc.uc_mcontext.pc = pc;
    blob.f.uc.uc_mcontext.sp = sp;
    blob.f.uc.uc_mcontext.pstate = 0;
    blob.f.uc.uc_sigmask = 0;
    __asm__ volatile("mov sp, %0\n\t"
                     "mov x8, #139\n\t"
                     "svc #0\n\t"
                     "udf #0"
                     : : "r"(&blob.record) : "memory");
#endif
}

static char stack[16384] __attribute__((aligned(16)));

static int t_group(void *arg)
{
    (void)arg;
    struct lx_timespec ts = { 0, 20000000 };
    sc2(LX_nanosleep, &ts, 0);
    sc1(LX_exit_group, 7);
    return 0;
}

static int t_last(void *arg)
{
    (void)arg;
    struct lx_timespec ts = { 0, 40000000 };
    sc2(LX_nanosleep, &ts, 0);   /* the main thread is gone by now */
    sc1(LX_exit_group, 5);
    return 0;
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "";
    if (streq(mode, "term")) {
        sc2(LX_kill, sc0(LX_getpid), 15);
        for (;;)
            sc0(LX_sched_yield);
    }
    if (streq(mode, "segv"))
        *(volatile int *)0 = 1;
    if (streq(mode, "ill"))
        __asm__ volatile(ILLEGAL);
    if (streq(mode, "badret"))
        sigreturn_to(0x8000000000000000ull, (unsigned long)__builtin_frame_address(0));
    if (streq(mode, "badstack"))
        sigreturn_to((unsigned long)lx_pusher, 0x8000000000000000ull);
    unsigned long flags = LX_CLONE_VM | LX_CLONE_FS | LX_CLONE_FILES | LX_CLONE_SIGHAND | LX_CLONE_THREAD;
    if (streq(mode, "group")) {
        lx_clone(t_group, stack + sizeof(stack), 0, flags, 0, 0, 0);
        for (;;)
            sc0(LX_sched_yield);   /* ended by the other thread's exit_group */
    }
    if (streq(mode, "lastthread")) {
        lx_clone(t_last, stack + sizeof(stack), 0, flags, 0, 0, 0);
        sc1(LX_exit, 3);   /* this thread only; the process lives on */
    }
    lx_puts("lxsig: unknown mode\n");
    return 2;
}
