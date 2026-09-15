/*
 * fsctl - run a filesystem's maintenance passes
 * (docs/audit/next-subsystem-fsctl.md).
 *
 *   fsctl list
 *   fsctl check <id> [--repair]
 *   fsctl scrub <id>
 *
 * A mount is named by the id `list` prints, not by its path: the same
 * mount is at different paths in different mount namespaces, and a path
 * names different mounts over time.
 *
 * Exit status is about whether the *command* worked, not about what it
 * found. A check that reports leaked blocks succeeded and exits 0; only
 * a refusal, a repair the kernel would not make, or a pass that failed
 * exits non-zero. Anything else and a shell script could not tell "this
 * filesystem has a problem" from "I could not ask".
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include <uapi/cosmo/fsctl.h>

static const char *const class_name[COSMO_FSCTL_CLASSES] = {
    "leaked blocks",          /* alloc_not_seen */
    "reachable but free",     /* seen_not_alloc */
    "cross-linked blocks",    /* dup */
    "wrong link counts",      /* nlink_wrong */
    "orphaned inodes",        /* orphan */
    "dangling entries",       /* dangling_entry */
    "malformed entries",      /* dir_bad */
    "wrong superblock totals",/* counter_wrong */
    "cyclic chains",          /* chain_cycle */
    "unreadable blocks",      /* unreadable */
};

/*
 * A mount id, or -1. `strtoull` alone would take "12junk" for 12 and run
 * against a filesystem the operator did not name -- and this command
 * repairs filesystems, so a misread argument is not a cosmetic bug.
 */
static int parse_id(const char *s, unsigned long long *out)
{
    if (s == NULL || *s < '0' || *s > '9')
        return -1;   /* digits only: strtoull would skip space and take a sign */
    errno = 0;
    char *end = NULL;
    unsigned long long v = strtoull(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v == 0)
        return -1;   /* zero names no mount, so it is as wrong as a letter */
    *out = v;
    return 0;
}

static int fsctl_open(void)
{
    int fd = open("/dev/fsctl", O_RDWR);
    if (fd < 0)
        perror("fsctl: /dev/fsctl");
    return fd;
}

static int run(int fd, unsigned op, unsigned long long id, unsigned flags, void *buf, size_t cap)
{
    struct cosmo_fsctl cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.version = COSMO_FSCTL_VERSION;
    cmd.op = (unsigned short)op;
    cmd.flags = flags;
    cmd.mount_id = id;
    if (write(fd, &cmd, sizeof(cmd)) != (long)sizeof(cmd)) {
        perror("fsctl");
        return -1;
    }
    long n = read(fd, buf, cap);
    if (n < 0) {
        if (errno != ERANGE)   /* the caller retries a short buffer */
            perror("fsctl: read");
        return -1;
    }
    return (int)n;
}

static int cmd_list(int fd)
{
    /*
     * The kernel returns the listing whole or refuses a buffer too small
     * with -ERANGE, so a fixed size is a limit on how many mounts a
     * machine may have before its operator cannot find any of them.
     * Grow until it fits.
     */
    size_t room = 16;
    char *buf = NULL;
    int n = -1;
    for (unsigned tries = 0; tries < 12; tries++) {
        size_t cap = sizeof(struct cosmo_fsctl_result) + room * sizeof(struct cosmo_fsctl_mount);
        char *bigger = realloc(buf, cap);
        if (bigger == NULL) {
            free(buf);
            fprintf(stderr, "fsctl: out of memory for %zu mounts\n", room);
            return 1;
        }
        buf = bigger;
        n = run(fd, COSMO_FSCTL_LIST, 0, 0, buf, cap);
        if (n >= 0)
            break;
        if (errno != ERANGE) {
            free(buf);
            return 1;
        }
        room *= 2;
    }
    if (n < (int)sizeof(struct cosmo_fsctl_result)) {
        free(buf);
        return 1;
    }
    struct cosmo_fsctl_result *h = (struct cosmo_fsctl_result *)buf;
    struct cosmo_fsctl_mount *m = (struct cosmo_fsctl_mount *)(buf + sizeof(*h));
    printf("%6s  %-10s %-6s %s\n", "ID", "TYPE", "PASSES", "PATH");
    for (unsigned i = 0; i < h->count; i++) {
        char passes[8];
        passes[0] = '\0';
        if (m[i].caps & COSMO_FSCTL_CAP_CHECK)
            strcat(passes, "c");
        if (m[i].caps & COSMO_FSCTL_CAP_SCRUB)
            strcat(passes, "s");
        printf("%6llu  %-10s %-6s %s\n", (unsigned long long)m[i].id, m[i].fstype,
               passes[0] ? passes : "-", m[i].path);
    }
    if (h->count < h->total)
        printf("(%u of %u; the list changed while it was read)\n", h->count, h->total);
    free(buf);
    return 0;
}

static int cmd_check(int fd, unsigned long long id, int repair)
{
    char buf[sizeof(struct cosmo_fsctl_result) + sizeof(struct cosmo_fsctl_check)];
    int n = run(fd, COSMO_FSCTL_CHECK, id, repair ? COSMO_FSCTL_F_REPAIR : 0, buf, sizeof(buf));
    if (n < (int)sizeof(buf))
        return 1;
    struct cosmo_fsctl_check *c = (struct cosmo_fsctl_check *)(buf + sizeof(struct cosmo_fsctl_result));
    printf("mount %llu: %llu blocks seen, %llu free, %llu inodes, %llu directories, %llu snapshots (%llu us)\n",
           id, (unsigned long long)c->blocks_seen, (unsigned long long)c->counted_free,
           (unsigned long long)c->inodes_seen, (unsigned long long)c->dirs_seen,
           (unsigned long long)c->snapshots_seen, (unsigned long long)(c->elapsed_ns / 1000));
    if (c->flags & COSMO_FSCTL_R_CLEAN) {
        printf("clean\n");
    } else {
        for (unsigned i = 0; i < c->nclasses && i < COSMO_FSCTL_CLASSES; i++) {
            if (c->class[i].count == 0)
                continue;
            printf("  %llu %s", (unsigned long long)c->class[i].count, class_name[i]);
            if (c->class[i].repaired)
                printf(" (%llu repaired)", (unsigned long long)c->class[i].repaired);
            for (unsigned k = 0; k < c->class[i].named; k++)
                printf("%s %llu", k ? "," : ":", (unsigned long long)c->class[i].name[k]);
            printf("\n");
        }
    }
    if (c->flags & COSMO_FSCTL_R_PARTIAL)
        printf("the answer is incomplete: something could not be read\n");
    if (c->flags & COSMO_FSCTL_R_REPAIR_REFUSED) {
        /*
         * The one case where a finding *is* an error for the caller:
         * repair was asked for and not done, so a script that assumed
         * the filesystem was fixed would be wrong.
         */
        printf("repair refused: the walk was not sure enough to act on\n");
        return 1;
    }
    return 0;
}

static int cmd_scrub(int fd, unsigned long long id)
{
    char buf[sizeof(struct cosmo_fsctl_result) + sizeof(struct cosmo_fsctl_scrub)];
    int n = run(fd, COSMO_FSCTL_SCRUB, id, 0, buf, sizeof(buf));
    if (n < (int)sizeof(buf))
        return 1;
    struct cosmo_fsctl_scrub *s = (struct cosmo_fsctl_scrub *)(buf + sizeof(struct cosmo_fsctl_result));
    printf("mount %llu: %llu blocks read, %llu inodes, %llu repaired, %llu unrecoverable\n",
           id, (unsigned long long)s->blocks_read, (unsigned long long)s->inodes,
           (unsigned long long)s->repaired, (unsigned long long)s->unrecoverable);
    return s->unrecoverable ? 1 : 0;
}

static void usage(void)
{
    fprintf(stderr, "usage: fsctl list\n"
                    "       fsctl check <id> [--repair]\n"
                    "       fsctl scrub <id>\n");
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage();
        return 2;
    }
    int fd = fsctl_open();
    if (fd < 0)
        return 1;

    int rc;
    if (strcmp(argv[1], "list") == 0 && argc == 2) {
        rc = cmd_list(fd);
    } else if (strcmp(argv[1], "check") == 0 && (argc == 3 || argc == 4)) {
        int repair = argc == 4 && strcmp(argv[3], "--repair") == 0;
        if (argc == 4 && !repair) {
            usage();
            close(fd);
            return 2;
        }
        unsigned long long id;
        if (parse_id(argv[2], &id) != 0) {
            fprintf(stderr, "fsctl: %s: not a mount id\n", argv[2]);
            close(fd);
            return 2;
        }
        rc = cmd_check(fd, id, repair);
    } else if (strcmp(argv[1], "scrub") == 0 && argc == 3) {
        unsigned long long id;
        if (parse_id(argv[2], &id) != 0) {
            fprintf(stderr, "fsctl: %s: not a mount id\n", argv[2]);
            close(fd);
            return 2;
        }
        rc = cmd_scrub(fd, id);
    } else {
        usage();
        rc = 2;
    }
    close(fd);
    return rc;
}
