/*
 * dltest.c - dlopen, dlsym and dlclose (roadmap 1.1 P4;
 * docs/compat/linux/testing.md, "Shared libraries"). Linked with a
 * RUNPATH of /boot/tests/dyn/lib, where the test library is.
 *
 *   dltest              the checks, then "dltest: ok":
 *     - libdltest.so found through RUNPATH: dltest_answer() is 42, its
 *       data symbol is reachable, dlclose succeeds;
 *     - a library that does not exist: dlopen is NULL, dlerror names it;
 *     - libtextrel.so (a text relocation): refused under W^X;
 *     - LD_LIBRARY_PATH naming a shadow libdltest.so (answer 7) is
 *       honoured, and ignored when AT_SECURE (effective uid not the real);
 *     - ASLR: two runs of `dynhello addr` and of `dltest addr` place
 *       libc.so, the executable and libdltest.so at different addresses.
 *   dltest answer       prints "dltest: answer N" (the library it found)
 *   dltest addr         prints "dltest: lib PTR"
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/wait.h>
#include <unistd.h>

#define DIR "/boot/tests/dyn"

static int g_fails;

#define CHECK(c)                                                                 \
    do {                                                                         \
        if (!(c)) {                                                              \
            printf("dltest: FAIL %s (line %d, errno %d)\n", #c, __LINE__, errno); \
            g_fails++;                                                           \
        }                                                                        \
    } while (0)

/* Run `argv` with `envp` (as uid `ruid`, euid 0, when ruid >= 0), its
 * standard output into `out`; its exit status, or -1. */
static int run(char *const argv[], char *const envp[], int ruid, char *out, size_t cap)
{
    int fd[2];
    if (pipe(fd) < 0)
        return -1;
    pid_t c = fork();
    if (c == 0) {
        close(fd[0]);
        dup2(fd[1], 1);
        if (ruid >= 0 && setresuid((uid_t)ruid, 0, 0) < 0)
            _exit(120);
        execve(argv[0], argv, envp);
        _exit(127);
    }
    close(fd[1]);
    size_t n = 0;
    ssize_t r;
    while (n + 1 < cap && (r = read(fd[0], out + n, cap - 1 - n)) > 0)
        n += (size_t)r;
    out[n] = '\0';
    close(fd[0]);
    int st;
    if (c < 0 || waitpid(c, &st, 0) != c || !WIFEXITED(st))
        return -1;
    return WEXITSTATUS(st);
}

static int answer(void)
{
    void *h = dlopen("libdltest.so", RTLD_NOW);
    if (h == NULL)
        return -1;
    int (*f)(void) = (int (*)(void))dlsym(h, "dltest_answer");
    int a = f ? f() : -2;
    dlclose(h);
    return a;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "answer") == 0) {
        printf("dltest: answer %d secure %lu\n", answer(), (unsigned long)getauxval(AT_SECURE));
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "addr") == 0) {
        void *h = dlopen("libdltest.so", RTLD_NOW);
        Dl_info di = { 0 };
        void *f = h ? dlsym(h, "dltest_answer") : NULL;
        printf("dltest: lib %#lx\n", f && dladdr(f, &di) ? (unsigned long)(uintptr_t)di.dli_fbase : 0UL);
        return h ? 0 : 1;
    }

    /* The shipped library, through RUNPATH. */
    void *h = dlopen("libdltest.so", RTLD_NOW);
    CHECK(h != NULL);
    if (h) {
        int (*f)(void) = (int (*)(void))dlsym(h, "dltest_answer");
        int *counter = (int *)dlsym(h, "dltest_counter");
        CHECK(f != NULL && counter != NULL);
        if (f && counter) {
            CHECK(f() == 42 && *counter == 2);
            Dl_info di;
            CHECK(dladdr((void *)f, &di) != 0 && strcmp(di.dli_fname, DIR "/lib/libdltest.so") == 0);
        }
        CHECK(dlsym(h, "no_such_symbol") == NULL && dlerror() != NULL);
        CHECK(dlclose(h) == 0);
    }

    /* A library that does not exist. */
    CHECK(dlopen("libcosmo-missing.so", RTLD_NOW) == NULL);
    const char *e = dlerror();
    CHECK(e != NULL && strstr(e, "libcosmo-missing.so") != NULL);

    /* A text relocation: refused (W^X), with the reason dlerror gives. */
    CHECK(dlopen(DIR "/libtextrel.so", RTLD_NOW) == NULL);
    e = dlerror();
    CHECK(e != NULL && strstr(e, "libtextrel.so") != NULL);
    if (e)
        printf("dltest: libtextrel.so refused: %s\n", e);

    /* LD_LIBRARY_PATH: honoured, then ignored when AT_SECURE. */
    char out[256];
    char *av_answer[] = { argv[0], "answer", NULL };
    char *env_shadow[] = { "LD_LIBRARY_PATH=" DIR "/shadow", NULL };
    CHECK(run(av_answer, env_shadow, -1, out, sizeof(out)) == 0 && strcmp(out, "dltest: answer 7 secure 0\n") == 0);
    CHECK(run(av_answer, env_shadow, 1000, out, sizeof(out)) == 0 && strcmp(out, "dltest: answer 42 secure 1\n") == 0);

    /* ASLR: libc.so, the executable and the library move between runs. */
    char a[2][128], b[2][128];
    char *av_dh[] = { DIR "/dynhello", "addr", NULL };
    char *av_addr[] = { argv[0], "addr", NULL };
    char *env0[] = { NULL };
    for (int i = 0; i < 2; i++) {
        CHECK(run(av_dh, env0, -1, a[i], sizeof(a[i])) == 0);
        CHECK(run(av_addr, env0, -1, b[i], sizeof(b[i])) == 0);
    }
    unsigned long l0 = 0, e0 = 0, l1 = 0, e1 = 0, d0 = 0, d1 = 0;
    CHECK(sscanf(a[0], "dynhello: libc %lx exe %lx", &l0, &e0) == 2 && sscanf(a[1], "dynhello: libc %lx exe %lx", &l1, &e1) == 2);
    CHECK(sscanf(b[0], "dltest: lib %lx", &d0) == 1 && sscanf(b[1], "dltest: lib %lx", &d1) == 1 && d0 && d1);
    CHECK(l0 != l1 && e0 != e1 && d0 != d1);
    printf("dltest: aslr: libc %#lx %#lx, exe %#lx %#lx, libdltest %#lx %#lx\n", l0, l1, e0, e1, d0, d1);

    printf(g_fails ? "dltest: FAIL (%d)\n" : "dltest: ok\n", g_fails);
    return g_fails ? 1 : 0;
}
