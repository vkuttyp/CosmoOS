/*
 * svc - start, stop and supervise services (docs/userland/design.md,
 * "Services"; constitution section 55; invariants U8, U9, U10).
 *
 *   svc boot                 start every service, in dependency order
 *   svc start|stop <name>    one service
 *   svc restart <name>       stop then start
 *   svc status [name]        what is running
 *   svc --supervise <name>   the supervisor itself; not for hand use
 *
 * There is no daemon. One supervisor process per service and the state
 * in the filesystem: /run/svc/<name>.pid while it runs, and
 * /var/log/svc/<name> for its output. A central manager would need a
 * control channel, and this kernel has neither named pipes nor unix
 * sockets -- building one in order to build a service manager is
 * backwards. This way `svc` holds no privileged position either: what
 * it knows, `cat` can read.
 */

#include <cosmo/syscall.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define SVC_DIR "/etc/svc"
#define RUN_DIR "/run/svc"
#define LOG_DIR "/var/log/svc"

#define MAX_SERVICES 32
#define MAX_DEPS 8
#define MAX_ARGS 16
#define NAME_MAX_LEN 32
#define LINE_MAX_LEN 256

enum restart_policy { R_NEVER, R_ON_FAILURE, R_ALWAYS };

struct service {
    char name[NAME_MAX_LEN];
    char exec[LINE_MAX_LEN];
    char deps[MAX_DEPS][NAME_MAX_LEN];
    int nr_deps;
    enum restart_policy restart;
    int retries;
    unsigned backoff_ms;
    /* Confinement, straight from the file to the spawn flags. */
    int uid, gid;              /* -1: the caller's */
    char root[LINE_MAX_LEN];   /* empty: none */
    int mountns, utsns, domain;
    struct {
        unsigned resource;
        unsigned long long value;
    } limits[COSMO_RLIMIT_COUNT];
    int nr_limits;
};

static const struct {
    const char *name;
    unsigned resource;
} g_limit_names[] = {
    { "as", COSMO_RLIMIT_AS },       { "mem", COSMO_RLIMIT_MEM },     { "nofile", COSMO_RLIMIT_NOFILE },
    { "nproc", COSMO_RLIMIT_NPROC }, { "vmem", COSMO_RLIMIT_VMEM },
};

static void trim(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == ' ' || s[n - 1] == '\t'))
        s[--n] = '\0';
}

/* Split on spaces in place. Local rather than strtok: this libc has no
 * strtok, and a splitter with no static state is the better thing to
 * add anyway. Returns NULL when there is nothing left. */
static char *next_word(char **p)
{
    char *s = *p;
    while (*s == ' ')
        s++;
    if (*s == '\0') {
        *p = s;
        return NULL;
    }
    char *start = s;
    while (*s != '\0' && *s != ' ')
        s++;
    if (*s == ' ')
        *s++ = '\0';
    *p = s;
    return start;
}

static int yes_no(const char *v, int *out)
{
    if (strcmp(v, "yes") == 0 || strcmp(v, "1") == 0) {
        *out = 1;
        return 0;
    }
    if (strcmp(v, "no") == 0 || strcmp(v, "0") == 0) {
        *out = 0;
        return 0;
    }
    return -1;
}

/*
 * Read one definition. An unknown key is an error rather than a warning:
 * a typo in `root` or `user` would otherwise leave a service running
 * with more authority than its author wrote down, and the file is the
 * only place that authority is stated (U9).
 */
static int load(const char *name, struct service *s)
{
    char path[128];
    snprintf(path, sizeof(path), SVC_DIR "/%s", name);
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        fprintf(stderr, "svc: %s: %s\n", path, strerror(errno));
        return -1;
    }
    memset(s, 0, sizeof(*s));
    snprintf(s->name, sizeof(s->name), "%s", name);
    s->restart = R_NEVER;
    s->retries = 5;
    s->backoff_ms = 100;
    s->uid = s->gid = -1;

    char line[LINE_MAX_LEN];
    int lineno = 0, bad = 0;
    while (fgets(line, sizeof(line), f) != NULL) {
        lineno++;
        trim(line);
        if (line[0] == '\0' || line[0] == '#')
            continue;
        char *sp = strchr(line, ' ');
        if (sp == NULL) {
            fprintf(stderr, "svc: %s:%d: no value for '%s'\n", name, lineno, line);
            bad = 1;
            continue;
        }
        *sp = '\0';
        const char *key = line, *val = sp + 1;
        while (*val == ' ')
            val++;

        if (strcmp(key, "exec") == 0) {
            snprintf(s->exec, sizeof(s->exec), "%s", val);
        } else if (strcmp(key, "after") == 0) {
            char buf[LINE_MAX_LEN];
            snprintf(buf, sizeof(buf), "%s", val);
            char *cur = buf;
            for (char *tok = next_word(&cur); tok != NULL; tok = next_word(&cur)) {
                if (s->nr_deps == MAX_DEPS) {
                    fprintf(stderr, "svc: %s: too many dependencies\n", name);
                    bad = 1;
                    break;
                }
                snprintf(s->deps[s->nr_deps++], NAME_MAX_LEN, "%s", tok);
            }
        } else if (strcmp(key, "restart") == 0) {
            if (strcmp(val, "never") == 0)
                s->restart = R_NEVER;
            else if (strcmp(val, "on-failure") == 0)
                s->restart = R_ON_FAILURE;
            else if (strcmp(val, "always") == 0)
                s->restart = R_ALWAYS;
            else {
                fprintf(stderr, "svc: %s:%d: restart '%s' is not never, on-failure or always\n", name, lineno,
                        val);
                bad = 1;
            }
        } else if (strcmp(key, "retries") == 0) {
            s->retries = atoi(val);
        } else if (strcmp(key, "backoff-ms") == 0) {
            s->backoff_ms = (unsigned)atoi(val);
        } else if (strcmp(key, "user") == 0) {
            s->uid = s->gid = atoi(val);
        } else if (strcmp(key, "root") == 0) {
            snprintf(s->root, sizeof(s->root), "%s", val);
        } else if (strcmp(key, "mountns") == 0 || strcmp(key, "utsns") == 0 || strcmp(key, "domain") == 0) {
            int *field = key[0] == 'm' ? &s->mountns : key[0] == 'u' ? &s->utsns : &s->domain;
            if (yes_no(val, field) != 0) {
                fprintf(stderr, "svc: %s:%d: %s '%s' is not yes or no\n", name, lineno, key, val);
                bad = 1;
            }
        } else if (strncmp(key, "limit-", 6) == 0) {
            const char *which = key + 6;
            int found = 0;
            for (size_t i = 0; i < sizeof(g_limit_names) / sizeof(g_limit_names[0]); i++) {
                if (strcmp(which, g_limit_names[i].name) != 0)
                    continue;
                if (s->nr_limits == COSMO_RLIMIT_COUNT) {
                    bad = 1;
                    break;
                }
                s->limits[s->nr_limits].resource = g_limit_names[i].resource;
                s->limits[s->nr_limits].value = strtoull(val, NULL, 0);
                s->nr_limits++;
                found = 1;
                break;
            }
            if (!found) {
                fprintf(stderr, "svc: %s:%d: no limit called '%s'\n", name, lineno, which);
                bad = 1;
            }
        } else {
            fprintf(stderr, "svc: %s:%d: unknown key '%s'\n", name, lineno, key);
            bad = 1;
        }
    }
    fclose(f);
    if (s->exec[0] == '\0') {
        fprintf(stderr, "svc: %s: no exec line\n", name);
        bad = 1;
    }
    return bad ? -1 : 0;
}

static void mkdirs(void)
{
    mkdir("/run", 0755);
    mkdir(RUN_DIR, 0755);
    mkdir("/var", 0755);
    mkdir("/var/log", 0755);
    mkdir(LOG_DIR, 0755);
}

static void pid_path(const char *name, char *out, size_t n)
{
    snprintf(out, n, RUN_DIR "/%s.pid", name);
}

/* The service's own pid, written by the supervisor after each spawn.
 * `svc stop` needs it because the supervisor cannot clean up after
 * itself: a native process here has no signal handlers, so being told
 * to stop kills it on the spot. */
static void child_path(const char *name, char *out, size_t n)
{
    snprintf(out, n, RUN_DIR "/%s.child", name);
}

static void write_pid(const char *path, pid_t pid)
{
    FILE *f = fopen(path, "w");
    if (f == NULL)
        return;
    fprintf(f, "%d\n", (int)pid);
    fclose(f);
}

static pid_t read_pid(const char *name)
{
    char path[128];
    pid_path(name, path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return -1;
    char buf[32] = { 0 };
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0)
        return -1;
    return (pid_t)atoi(buf);
}

/* Live means the pid is there to be signalled. Signal 0 asks without
 * sending, which is what tells a stale pid file from a running one. */
static int running(const char *name, pid_t *out)
{
    pid_t p = read_pid(name);
    if (p <= 0)
        return 0;
    if (kill(p, 0) != 0)
        return 0;
    if (out)
        *out = p;
    return 1;
}

static int log_open(const char *name)
{
    char path[128];
    snprintf(path, sizeof(path), LOG_DIR "/%s", name);
    return open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
}

static void log_line(int fd, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0)
        (void)write(fd, buf, (size_t)n);
}

/* --- the supervisor ---------------------------------------------------------
 *
 * Spawns the service, waits for it, and decides. The backoff and the
 * retry limit are both required: without the first a service that fails
 * instantly is a machine that does nothing else, and without the second
 * a dead service looks like a running one forever (U8).
 */
static int supervise(const char *name)
{
    struct service s;
    if (load(name, &s) != 0)
        return 2;

    mkdirs();
    int log = log_open(name);
    if (log < 0) {
        fprintf(stderr, "svc: %s: cannot open the log: %s\n", name, strerror(errno));
        return 2;
    }

    char pidfile[128], childfile[128];
    pid_path(name, pidfile, sizeof(pidfile));
    child_path(name, childfile, sizeof(childfile));
    write_pid(pidfile, getpid());

    /* Limits are set on this process and inherited by the service, so
     * the supervisor lives under them too -- which is honest: it is
     * part of what the service costs. */
    for (int i = 0; i < s.nr_limits; i++) {
        if (cosmo_setrlimit(s.limits[i].resource, s.limits[i].value) != 0)
            log_line(log, "svc: %s: limit %u could not be set\n", name, s.limits[i].resource);
    }

    char argv0[LINE_MAX_LEN];
    snprintf(argv0, sizeof(argv0), "%s", s.exec);
    const char *argv[MAX_ARGS];
    int argc = 0;
    char *cur = argv0;
    for (char *tok = next_word(&cur); tok != NULL && argc < MAX_ARGS - 1; tok = next_word(&cur))
        argv[argc++] = tok;
    argv[argc] = NULL;
    if (argc == 0) {
        log_line(log, "svc: %s: nothing to run\n", name);
        return 2;
    }

    unsigned flags = COSMO_SPAWN_HANDLE_RIGHTS;
    if (s.uid >= 0)
        flags |= COSMO_SPAWN_SETCRED;
    if (s.root[0] != '\0')
        flags |= COSMO_SPAWN_SETROOT;
    if (s.mountns)
        flags |= COSMO_SPAWN_NEWMOUNTNS;
    if (s.utsns)
        flags |= COSMO_SPAWN_NEWUTSNS;
    if (s.domain)
        flags |= COSMO_SPAWN_NEWDOMAIN;

    /* The service's output is its log, and so are the supervisor's own
     * notes: one file per service, which `cat` reads. */
    struct cosmo_spawn_handle map[] = {
        { .child = 0, .parent = 0, .rights = COSMO_RIGHTS_SAME },
        { .child = 1, .parent = log, .rights = COSMO_RIGHTS_SAME },
        { .child = 2, .parent = log, .rights = COSMO_RIGHTS_SAME },
    };

    unsigned wait_ms = s.backoff_ms;
    for (int attempt = 0;; attempt++) {
        struct cosmo_spawn req = {
            .path = argv[0],
            .argv = argv,
            .envp = NULL,
            .handles = map,
            .nr_handles = sizeof(map) / sizeof(map[0]),
            .cwd = NULL,
            .flags = flags,
            .uid = (uint32_t)(s.uid < 0 ? 0 : s.uid),
            .gid = (uint32_t)(s.gid < 0 ? 0 : s.gid),
            .root = s.root[0] ? s.root : NULL,
        };
        long pid = cosmo_spawn(&req);
        if (pid < 0) {
            log_line(log, "svc: %s: cannot start %s: %d\n", name, argv[0], (int)-pid);
            break;
        }
        write_pid(childfile, (pid_t)pid);
        log_line(log, "svc: %s: started, pid %d\n", name, (int)pid);

        int status = -1;
        if (cosmo_wait((int)pid, &status, 0) != pid) {
            log_line(log, "svc: %s: lost track of pid %d\n", name, (int)pid);
            break;
        }
        log_line(log, "svc: %s: exited with status %d\n", name, status);

        if (s.restart == R_NEVER)
            break;
        if (s.restart == R_ON_FAILURE && status == 0)
            break;
        /* `attempt` is how many restarts have happened, so `retries`
         * is a count of restarts and not of runs: retries 2 means the
         * service is started three times in all. */
        if (attempt >= s.retries) {
            log_line(log, "svc: %s: giving up after %d restarts\n", name, attempt);
            break;
        }
        log_line(log, "svc: %s: restarting in %u ms\n", name, wait_ms);
        cosmo_sleep_ns((uint64_t)wait_ms * 1000000ULL);
        wait_ms = wait_ms > 2500 ? 5000 : wait_ms * 2;   /* doubling, capped */
    }

    unlink(childfile);
    unlink(pidfile);
    close(log);
    return 0;
}

/* --- the commands ----------------------------------------------------------- */

static int cmd_start(const char *name)
{
    struct service s;
    if (load(name, &s) != 0)
        return 1;
    if (running(name, NULL)) {
        printf("svc: %s is already running\n", name);
        return 0;
    }
    mkdirs();
    const char *argv[] = { "svc", "--supervise", name, NULL };
    pid_t pid = spawnve("/sbin/svc", argv, NULL, NULL, 0);
    if (pid < 0) {
        fprintf(stderr, "svc: %s: cannot start a supervisor: %s\n", name, strerror(errno));
        return 1;
    }
    /* The supervisor writes its pid file before the first spawn; wait
     * for it so that `svc start x && svc status x` says what a person
     * would expect. */
    for (int i = 0; i < 200 && !running(name, NULL); i++)
        cosmo_sleep_ns(1000000ULL);
    printf("svc: %s started\n", name);
    return 0;
}

/*
 * Stopping is two kills and they are ordered. The supervisor goes
 * first, or it would see its service die and start another one --
 * killing the service alone is a restart, not a stop. Then the service
 * itself, because the supervisor cannot do it: a native process here
 * has no signal handlers, so being told to stop kills it where it
 * stands, with no chance to tidy up. `svc` therefore removes the pid
 * files too.
 */
static int cmd_stop(const char *name)
{
    char pidfile[128], childfile[128];
    pid_path(name, pidfile, sizeof(pidfile));
    child_path(name, childfile, sizeof(childfile));

    pid_t pid;
    int was_running = running(name, &pid);
    if (was_running && kill(pid, SIGTERM) != 0 && errno != ESRCH) {
        fprintf(stderr, "svc: %s: %s\n", name, strerror(errno));
        return 1;
    }
    for (int i = 0; i < 500 && was_running && running(name, NULL); i++)
        cosmo_sleep_ns(1000000ULL);

    FILE *cf = fopen(childfile, "r");
    if (cf != NULL) {
        char buf[32] = { 0 };
        size_t n = fread(buf, 1, sizeof(buf) - 1, cf);
        fclose(cf);
        pid_t child = n > 0 ? (pid_t)atoi(buf) : -1;
        if (child > 0 && kill(child, 0) == 0 && kill(child, SIGTERM) != 0 && errno != ESRCH)
            fprintf(stderr, "svc: %s: cannot stop pid %d: %s\n", name, (int)child, strerror(errno));
    }
    unlink(childfile);
    unlink(pidfile);
    printf("svc: %s %s\n", name, was_running ? "stopped" : "is not running");
    return 0;
}

static int cmd_status(const char *name)
{
    pid_t pid;
    if (name != NULL) {
        int up = running(name, &pid);
        printf("%-16s %s", name, up ? "running" : "stopped");
        if (up)
            printf(" (supervisor %d)", pid);
        printf("\n");
        return up ? 0 : 1;
    }
    DIR *d = opendir(SVC_DIR);
    if (d == NULL) {
        fprintf(stderr, "svc: " SVC_DIR ": %s\n", strerror(errno));
        return 1;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.')
            continue;
        int up = running(e->d_name, &pid);
        printf("%-16s %s\n", e->d_name, up ? "running" : "stopped");
    }
    closedir(d);
    return 0;
}

/*
 * Start everything, in `after` order. A cycle is refused and named; a
 * service whose dependency did not start is not started and says which
 * one -- a dependency ignored when it fails is a dependency in name
 * only (U10).
 */
static int cmd_boot(void)
{
    struct service svcs[MAX_SERVICES];
    char names[MAX_SERVICES][NAME_MAX_LEN];
    int loaded[MAX_SERVICES] = { 0 };
    int started[MAX_SERVICES] = { 0 };
    int n = 0;

    DIR *d = opendir(SVC_DIR);
    if (d == NULL)
        return 0;   /* no services is not an error */
    struct dirent *e;
    while ((e = readdir(d)) != NULL && n < MAX_SERVICES) {
        if (e->d_name[0] == '.')
            continue;
        snprintf(names[n], NAME_MAX_LEN, "%s", e->d_name);
        loaded[n] = load(names[n], &svcs[n]) == 0;
        n++;
    }
    closedir(d);

    /* Repeatedly start whatever has all its dependencies started. What
     * is left when nothing moves is either blocked by a failure or in a
     * cycle, and the two are told apart by whether the dependency
     * loaded at all. */
    int remaining = n, rc = 0;
    while (remaining > 0) {
        int progress = 0;
        for (int i = 0; i < n; i++) {
            if (started[i])
                continue;
            if (!loaded[i]) {
                started[i] = -1;   /* its own definition is bad; already reported */
                remaining--;
                progress = 1;
                rc = 1;
                continue;
            }
            int ready = 1, failed_dep = -1;
            for (int k = 0; k < svcs[i].nr_deps && ready; k++) {
                int found = -1;
                for (int j = 0; j < n; j++)
                    if (strcmp(names[j], svcs[i].deps[k]) == 0)
                        found = j;
                if (found < 0) {
                    failed_dep = k;   /* named a service that does not exist */
                    ready = 0;
                } else if (started[found] < 0) {
                    failed_dep = k;
                    ready = 0;
                } else if (!started[found]) {
                    ready = 0;
                }
            }
            if (failed_dep >= 0) {
                fprintf(stderr, "svc: %s: not started: %s did not start\n", names[i],
                        svcs[i].deps[failed_dep]);
                started[i] = -1;
                remaining--;
                progress = 1;
                rc = 1;
                continue;
            }
            if (!ready)
                continue;
            started[i] = cmd_start(names[i]) == 0 ? 1 : -1;
            if (started[i] < 0)
                rc = 1;
            remaining--;
            progress = 1;
        }
        if (!progress) {
            fprintf(stderr, "svc: dependency cycle among:");
            for (int i = 0; i < n; i++)
                if (!started[i])
                    fprintf(stderr, " %s", names[i]);
            fprintf(stderr, "\n");
            return 1;
        }
    }
    return rc;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: svc boot | start|stop|restart|status [name]\n");
        return 2;
    }
    if (strcmp(argv[1], "--supervise") == 0 && argc == 3)
        return supervise(argv[2]);
    if (strcmp(argv[1], "boot") == 0 && argc == 2)
        return cmd_boot();
    if (strcmp(argv[1], "status") == 0)
        return cmd_status(argc >= 3 ? argv[2] : NULL);
    if (argc != 3) {
        fprintf(stderr, "usage: svc %s <name>\n", argv[1]);
        return 2;
    }
    if (strcmp(argv[1], "start") == 0)
        return cmd_start(argv[2]);
    if (strcmp(argv[1], "stop") == 0)
        return cmd_stop(argv[2]);
    if (strcmp(argv[1], "restart") == 0) {
        int rc = cmd_stop(argv[2]);
        return rc != 0 ? rc : cmd_start(argv[2]);
    }
    fprintf(stderr, "svc: no command '%s'\n", argv[1]);
    return 2;
}
