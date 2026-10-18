/* fetch.c - A URL into a file through /bin/cosmo-fetch (fetch.h). */

#include <errno.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#include "fetch.h"

int fetch_url(const char *url, const char *path, const struct fetch_opts *o)
{
    const char *argv[12];
    char max[24];
    int n = 0;
    argv[n++] = "cosmo-fetch";
    if (o->resume)
        argv[n++] = "-c";
    if (o->insecure)
        argv[n++] = "-i";
    if (o->pin && o->pin[0]) {
        argv[n++] = "-p";
        argv[n++] = o->pin;
    }
    if (o->max) {
        snprintf(max, sizeof(max), "%llu", (unsigned long long)o->max);
        argv[n++] = "-m";
        argv[n++] = max;
    }
    argv[n++] = url;
    argv[n++] = path;
    argv[n] = NULL;

    /* Standard input, output and error are the caller's; the environment
     * too, so SSL_CERT_FILE reaches it. */
    const struct spawn_handle map[3] = { { 0, 0, 0, 0 }, { 1, 1, 0, 0 }, { 2, 2, 0, 0 } };
    fflush(stdout);
    fflush(stderr);
    /* COSMO_FETCH: another copy, as a release's cosmo-upgrade.cpk carries
     * one for a system older than /bin/cosmo-fetch (docs/guide/upgrading.md). */
    const char *prog = getenv("COSMO_FETCH");
    pid_t pid = spawnve(prog && prog[0] ? prog : FETCH_PROGRAM, argv, (const char *const *)environ, map, 3);
    if (pid < 0)
        return -1;
    int status;
    while (waitpid(pid, &status, 0) != pid) {
        if (errno != EINTR)
            return -1;
    }
    if (!WIFEXITED(status))
        return FETCH_FAILED;   /* killed */
    return WEXITSTATUS(status);
}
