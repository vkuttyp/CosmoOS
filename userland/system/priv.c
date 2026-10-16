/*
 * priv - see and drop privileges (roadmap M5; docs/userland/design.md,
 * "priv"; docs/kernel/security/design.md §1).
 *
 *   priv                                   this process's privileges
 *   priv [-d LIST | -k LIST] [-u UID [-g GID]] [--] COMMAND [ARG...]
 *
 * runs COMMAND without the privileges in LIST (-d), or with only those in
 * LIST (-k), optionally as UID and GID (which needs PRIV_CRED unless they
 * are the caller's own), waits for it and exits with its status. LIST is
 * names separated by commas -- mount, sys_admin, net, proc, cred, device
 * -- or "all". A child never holds a privilege its parent lacks, so -k
 * can only keep what priv itself holds, and nothing a child does gives a
 * privilege back. COMMAND without a slash is looked for in /bin, then
 * /sbin. Exit 125 when priv itself fails, 127 when COMMAND cannot start.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cosmo/syscall.h>

static const char *const g_names[COSMO_PRIV_COUNT] = { "mount", "sys_admin", "net", "proc", "cred", "device" };

static int parse_list(const char *s, uint32_t *out)
{
    if (strcmp(s, "all") == 0) {
        *out = COSMO_PRIV_ALL;
        return 0;
    }
    uint32_t m = 0;
    while (*s) {
        const char *e = strchr(s, ',');
        size_t n = e ? (size_t)(e - s) : strlen(s);
        unsigned i;
        for (i = 0; i < COSMO_PRIV_COUNT; i++)
            if (strlen(g_names[i]) == n && memcmp(g_names[i], s, n) == 0)
                break;
        if (i == COSMO_PRIV_COUNT) {
            fprintf(stderr, "priv: unknown privilege '%.*s'\n", (int)n, s);
            return -1;
        }
        m |= 1u << i;
        s += n;
        if (*s == ',')
            s++;
    }
    *out = m;
    return 0;
}

/* A whole decimal id, 0..UINT32_MAX: anything else -- empty, a sign, a
 * typo, an overflow -- is refused, so a mistyped -u can never become
 * uid 0 (PR #370 review). */
static int parse_id(const char *s, unsigned long *out)
{
    if (*s < '0' || *s > '9')
        return -1;
    char *end;
    errno = 0;
    unsigned long v = strtoul(s, &end, 10);
    if (errno != 0 || *end != '\0' || v > 0xffffffffUL)
        return -1;
    *out = v;
    return 0;
}

static int show(void)
{
    FILE *f = fopen("/proc/self/status", "r");
    if (f == NULL) {
        perror("priv: /proc/self/status");
        return 125;
    }
    char line[256];
    int rc = 125;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "Privs:\t", 7) == 0) {
            fputs(line + 7, stdout);
            rc = 0;
        }
    }
    fclose(f);
    return rc;
}

static int usage(void)
{
    fprintf(stderr, "usage: priv | priv [-d LIST | -k LIST] [-u UID [-g GID]] [--] COMMAND [ARG...]\n");
    return 125;
}

int main(int argc, char **argv)
{
    if (argc == 1)
        return show();
    uint32_t drop = 0;
    bool set_cred = false;
    unsigned long uid = 0, gid = 0;
    bool have_gid = false;
    int i = 1;
    while (i < argc && argv[i][0] == '-') {
        if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        }
        if (i + 1 >= argc)
            return usage();
        if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "-k") == 0) {
            uint32_t m;
            if (parse_list(argv[i + 1], &m) < 0)
                return 125;
            drop = argv[i][1] == 'd' ? m : (COSMO_PRIV_ALL & ~m);
        } else if (strcmp(argv[i], "-u") == 0) {
            if (parse_id(argv[i + 1], &uid) < 0) {
                fprintf(stderr, "priv: -u %s is not a uid\n", argv[i + 1]);
                return 125;
            }
            set_cred = true;
        } else if (strcmp(argv[i], "-g") == 0) {
            if (parse_id(argv[i + 1], &gid) < 0) {
                fprintf(stderr, "priv: -g %s is not a gid\n", argv[i + 1]);
                return 125;
            }
            have_gid = true;
        } else {
            return usage();
        }
        i += 2;
    }
    if (i >= argc)
        return usage();
    if (have_gid && !set_cred)
        return usage();
    if (set_cred && !have_gid)
        gid = uid;

    char path[256];
    const char *cmd = argv[i];
    if (strchr(cmd, '/') == NULL) {
        static const char *const dirs[] = { "/bin/", "/sbin/" };
        path[0] = '\0';
        for (unsigned d = 0; d < 2; d++) {
            snprintf(path, sizeof(path), "%s%s", dirs[d], cmd);
            if (access(path, X_OK) == 0)
                break;
            path[0] = '\0';
        }
        if (path[0] == '\0') {
            fprintf(stderr, "priv: %s: not found\n", cmd);
            return 127;
        }
        cmd = path;
    }

    struct cosmo_spawn req;
    memset(&req, 0, sizeof(req));
    req.path = cmd;
    req.argv = (const char *const *)&argv[i];
    req.envp = (const char *const *)environ;
    req.flags = COSMO_SPAWN_DROPPRIV | (set_cred ? COSMO_SPAWN_SETCRED : 0);
    req.priv_drop = drop;
    req.uid = (uint32_t)uid;
    req.gid = (uint32_t)gid;
    long pid = cosmo_spawn(&req);
    if (pid < 0) {
        fprintf(stderr, "priv: %s: %s\n", cmd, strerror((int)-pid));
        return 127;
    }
    int status = 0;
    if (cosmo_wait((int)pid, &status, 0) != pid) {
        fprintf(stderr, "priv: lost pid %ld\n", pid);
        return 125;
    }
    return WEXITSTATUS(status) & 0xff;
}
