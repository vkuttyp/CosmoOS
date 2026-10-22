/*
 * fsctl - run a filesystem's maintenance passes
 * (docs/audit/next-subsystem-fsctl.md).
 *
 *   fsctl list
 *   fsctl check <id> [--repair]
 *   fsctl scrub <id> [--repair]
 *
 * A mount is named by the id `list` prints, not by its path: the same
 * mount is at different paths in different mount namespaces, and a path
 * names different mounts over time.
 *
 * Exit status follows fsck's convention since roadmap 1.2 B2 (fsreport.h):
 * 0 the pass found nothing, 1 everything it found was repaired, 4
 * findings are left (a repair refused among them), 8 the pass could not
 * be run (a refusal, a mount that is not there), 2 a usage error. A
 * shell script can still tell "this filesystem has a problem" (4) from
 * "I could not ask" (8). A check's findings are printed as fsreport's
 * `class` and `finding` lines, the scrub's as one line of counts.
 *
 * `scrub` without --repair reads and verifies and writes nothing (the
 * SCRUB_EX operation); a kernel older than it is told by -EINVAL, and
 * there `scrub --repair` falls back to the old SCRUB, which repairs.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include <uapi/cosmo/fsctl.h>

#include "fsreport.h"

/* The names a human reads in the summary; fsreport has the stable ones. */
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
    /* Version 2 (docs/audit/next-subsystem-fsck-unchecked.md). A
     * version-1 kernel sends nclasses = 10 and the loop stops there. */
    "unordered extents",      /* extent_order */
    "overlapping extents",    /* extent_overlap */
    "duplicate names",        /* dir_dup_name */
    /* Version 3: format version 12's snapshot identity and rollback
     * record (docs/kernel-services/filesystem/cosmofs/design.md). */
    "snapshot identities",    /* snap_identity */
    "rollback records",       /* rollback_record */
    "holds",                  /* hold */
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
    size_t room = 8;   /* most machines fit in one read; the rest grow */
    char *buf = NULL;
    int n = -1;
    for (unsigned tries = 0; tries < 12; tries++) {
        size_t cap = sizeof(struct cosmo_fsctl_result) + room * sizeof(struct cosmo_fsctl_mount);
        char *bigger = realloc(buf, cap);
        if (bigger == NULL) {
            free(buf);
            fprintf(stderr, "fsctl: out of memory for %zu mounts\n", room);
            return FSR_FAILED;
        }
        buf = bigger;
        n = run(fd, COSMO_FSCTL_LIST, 0, 0, buf, cap);
        if (n >= 0)
            break;
        if (errno != ERANGE) {
            free(buf);
            return FSR_FAILED;
        }
        room *= 2;
    }
    if (n < (int)sizeof(struct cosmo_fsctl_result)) {
        free(buf);
        return FSR_FAILED;
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
        if (m[i].caps & COSMO_FSCTL_CAP_SNAP)
            strcat(passes, "n");   /* snapshots (cosmo-snap) */
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
        return FSR_FAILED;
    struct cosmo_fsctl_check *c = (struct cosmo_fsctl_check *)(buf + sizeof(struct cosmo_fsctl_result));
    printf("mount %llu: %llu blocks seen, %llu free, %llu inodes, %llu directories, %llu snapshots (%llu us)\n",
           id, (unsigned long long)c->blocks_seen, (unsigned long long)c->counted_free,
           (unsigned long long)c->inodes_seen, (unsigned long long)c->dirs_seen,
           (unsigned long long)c->snapshots_seen, (unsigned long long)(c->elapsed_ns / 1000));
    struct fsr_tally t = { 0 };
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
        fsreport_check(stdout, c, &t);
    }
    if (c->flags & COSMO_FSCTL_R_PARTIAL)
        printf("the answer is incomplete: something could not be read\n");
    if (c->flags & COSMO_FSCTL_R_REPAIR_REFUSED) {
        /*
         * Repair was asked for and not done, so a script that assumed
         * the filesystem was fixed would be wrong: findings are left.
         */
        printf("repair refused: the walk was not sure enough to act on\n");
        return FSR_LEFT;
    }
    if ((c->flags & COSMO_FSCTL_R_CLEAN) == 0 && t.findings == 0)
        return FSR_LEFT;   /* not clean with no class counted: incomplete */
    return fsreport_exit(&t);
}

static int cmd_scrub(int fd, unsigned long long id, int repair)
{
    char buf[sizeof(struct cosmo_fsctl_result) + sizeof(struct cosmo_fsctl_scrub_ex)];
    int n = run(fd, COSMO_FSCTL_SCRUB_EX, id, repair ? COSMO_FSCTL_F_REPAIR : 0, buf, sizeof(buf));
    if (n < 0 && errno == EINVAL && repair) {
        /* A kernel before SCRUB_EX: its SCRUB repairs, and has no more to say. */
        char old[sizeof(struct cosmo_fsctl_result) + sizeof(struct cosmo_fsctl_scrub)];
        n = run(fd, COSMO_FSCTL_SCRUB, id, 0, old, sizeof(old));
        if (n < (int)sizeof(old))
            return FSR_FAILED;
        struct cosmo_fsctl_scrub *s = (struct cosmo_fsctl_scrub *)(old + sizeof(struct cosmo_fsctl_result));
        printf("mount %llu: %llu blocks read, %llu inodes, %llu repaired, %llu unrecoverable\n", id,
               (unsigned long long)s->blocks_read, (unsigned long long)s->inodes,
               (unsigned long long)s->repaired, (unsigned long long)s->unrecoverable);
        return s->unrecoverable ? FSR_LEFT : s->repaired ? FSR_REPAIRED : FSR_CLEAN;
    }
    if (n < 0 && errno == EINVAL) {
        fprintf(stderr, "fsctl: this kernel's scrub always repairs; ask for it with --repair\n");
        return FSR_FAILED;
    }
    if (n < (int)sizeof(buf))
        return FSR_FAILED;
    struct cosmo_fsctl_scrub_ex *s = (struct cosmo_fsctl_scrub_ex *)(buf + sizeof(struct cosmo_fsctl_result));
    printf("mount %llu: %llu blocks read, %llu inodes, %llu bad copies, %llu repaired, %llu unrecoverable, "
           "%llu skipped (%llu ms, bound %llu blocks/s)\n",
           id, (unsigned long long)s->blocks_read, (unsigned long long)s->inodes,
           (unsigned long long)s->bad_copies, (unsigned long long)s->repaired,
           (unsigned long long)s->unrecoverable, (unsigned long long)s->skipped,
           (unsigned long long)(s->elapsed_ns / 1000000), (unsigned long long)s->rate);
    if (s->bad_copies == 0 && s->unrecoverable == 0)
        printf("clean\n");
    if (s->unrecoverable || s->repaired < s->bad_copies)
        return FSR_LEFT;
    return s->bad_copies ? FSR_REPAIRED : FSR_CLEAN;
}

static void usage(void)
{
    fprintf(stderr, "usage: fsctl list\n"
                    "       fsctl check <id> [--repair]\n"
                    "       fsctl scrub <id> [--repair]\n");
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage();
        return 2;
    }
    int fd = fsctl_open();
    if (fd < 0)
        return FSR_FAILED;

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
    } else if (strcmp(argv[1], "scrub") == 0 && (argc == 3 || argc == 4)) {
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
        rc = cmd_scrub(fd, id, repair);
    } else {
        usage();
        rc = 2;
    }
    close(fd);
    return rc;
}
