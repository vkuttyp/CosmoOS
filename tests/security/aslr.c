/*
 * aslr.c - roadmap M5's layout probe, a static musl program
 * (docs/kernel/process/testing.md, "test-security").
 *
 * Built twice (tests/security/security.mk): aslr-static, an ET_EXEC image
 * that keeps its link address, and aslr-pie, a static PIE the kernel
 * places at a random bias. One line says where this exec put things:
 *
 *   aslr: kind=pie stack=0x... sp=0x... mmap=0x... brk=0x... load=0x...
 *
 * stack is argv (the initial stack, which the random sub-page offset
 * moves), sp a local, mmap an anonymous page placed by the kernel, brk the
 * initial break, load the image's ELF header. make test-security runs it
 * ten times in one boot and requires every value but a static load to be
 * distinct each time.
 *
 * Then W^X (decision 4): code copied to the stack does not run (SIGSEGV),
 * and neither mmap nor mprotect will make memory writable and executable:
 *
 *   aslr: wx stack-exec=SIGSEGV mmap-wx=EINVAL mprotect-wx=EINVAL
 */

#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef ASLR_KIND
#error "ASLR_KIND must name the build (static or pie)"
#endif
#define STR2(x) #x
#define STR(x) STR2(x)

extern char __ehdr_start[] __attribute__((visibility("hidden")));

static sigjmp_buf g_jmp;
static volatile sig_atomic_t g_sig;

static void on_fault(int sig)
{
    g_sig = sig;
    siglongjmp(g_jmp, 1);
}

static const char *errname(int e)
{
    return e == EINVAL ? "EINVAL" : e == EACCES ? "EACCES" : e == EPERM ? "EPERM" : "other";
}

int main(int argc, char **argv)
{
    (void)argc;
    volatile int local = 0;
    void *m = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    void *brk0 = sbrk(0);
    printf("aslr: kind=%s stack=%p sp=%p mmap=%p brk=%p load=%p\n", STR(ASLR_KIND), (void *)argv,
           (void *)(uintptr_t)&local, m, brk0, (void *)__ehdr_start);

    /* A return instruction on the stack, called. */
#if defined(__x86_64__)
    static const unsigned char ret[] = { 0xc3 };
#else
    static const unsigned char ret[] = { 0xc0, 0x03, 0x5f, 0xd6 };   /* ret */
#endif
    unsigned char code[16] __attribute__((aligned(16)));
    memcpy(code, ret, sizeof(ret));
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_fault;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    const char *exec_result = "ran";
    if (sigsetjmp(g_jmp, 1) == 0) {
        void (*fn)(void) = (void (*)(void))(uintptr_t)code;
        __asm__ volatile("" ::: "memory");
        fn();
    } else {
        exec_result = g_sig == SIGSEGV ? "SIGSEGV" : "SIGBUS";
    }

    void *wx = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    const char *mmap_result = wx == MAP_FAILED ? errname(errno) : "mapped";
    uintptr_t page = (uintptr_t)code & ~(uintptr_t)4095;
    int rc = mprotect((void *)page, 4096, PROT_READ | PROT_WRITE | PROT_EXEC);
    const char *mprotect_result = rc < 0 ? errname(errno) : "changed";
    printf("aslr: wx stack-exec=%s mmap-wx=%s mprotect-wx=%s\n", exec_result, mmap_result, mprotect_result);
    return 0;
}
