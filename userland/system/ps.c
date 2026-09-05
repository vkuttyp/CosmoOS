/* ps - list processes. */
#include <cosmo/procinfo.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    size_t cap = 64;
    struct cosmo_procinfo *pi = NULL;
    int total;
    for (;;) {
        pi = realloc(pi, cap * sizeof(*pi));
        if (pi == NULL) {
            fprintf(stderr, "ps: out of memory\n");
            return 1;
        }
        total = procinfo(pi, cap);
        if (total < 0) {
            perror("ps");
            return 1;
        }
        if ((size_t)total <= cap)
            break;
        cap = (size_t)total + 16;
    }
    static const char *const states[] = { "R", "X", "Z" };
    printf("%5s %5s %4s %2s %3s %9s %8s %s\n", "PID", "PPID", "UID", "S", "THR", "SYSCALLS", "TIME(ms)", "NAME");
    for (int i = 0; i < total; i++) {
        printf("%5u %5u %4u %2s %3u %9llu %8llu %s\n", pi[i].pid, pi[i].ppid, pi[i].uid,
               pi[i].state < 3 ? states[pi[i].state] : "?", pi[i].nr_threads, (unsigned long long)pi[i].syscalls,
               (unsigned long long)(pi[i].run_ns / 1000000ULL), pi[i].name);
    }
    free(pi);
    return 0;
}
