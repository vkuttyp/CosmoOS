/*
 * fsck.cosmofs - check and repair a cosmofs pool nothing has mounted
 * (roadmap 1.2 B2; docs/kernel-services/filesystem/cosmofs/api.md,
 * "fsck.cosmofs(8)").
 *
 *   fsck.cosmofs [--check | --repair] DEVICE
 *   fsck.cosmofs --boot check|repair DEVICE
 *
 * The check is the kernel's own (`/dev/fsctl` CHECK_DEV): the pool is
 * mounted where no namespace sees it -- read-only for --check, which
 * writes nothing to the device -- checked, and unmounted. There is no
 * second implementation of the checker here.
 *
 * --repair repairs what has one right answer (leaked blocks, totals,
 * orphans, link counts, a stale rollback record or hold, snapshot
 * identity, and -- offline only -- a reachable block the bitmap calls
 * free) and reports the rest. A writable check mounts the pool as any
 * writable mount does, so a pending rollback is applied by it -- and kept
 * only when the check repaired something: a check that repaired nothing
 * writes nothing.
 *
 * --boot is init's, before the root is mounted (design.md, "fsck at
 * boot"): check read-only; if every finding is one a boot repairs and
 * the mode is repair, repair those; print one line either way,
 *
 *   cosmofs: root checked, N findings, M repaired
 *
 * or, when anything else was found, refuse:
 *
 *   cosmofs: root refused: CLASS[, CLASS...]; DEVICE is not mounted ...
 *
 * Output: the `class` and `finding` lines of fsreport.h. Exit 0 nothing
 * found, 1 all repaired, 4 findings left (--boot: the pool is refused),
 * 8 the check could not run, 2 usage.
 */
#include <errno.h>
#include <stdbool.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <uapi/cosmo/fsctl.h>

#include "fsreport.h"

static int devcheck(const char *dev, unsigned flags, struct cosmo_fsctl_devcheck *out)
{
    int fd = open("/dev/fsctl", O_RDWR);
    if (fd < 0)
        return -errno;
    struct cosmo_fsctl cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.version = COSMO_FSCTL_VERSION;
    cmd.op = COSMO_FSCTL_CHECK_DEV;
    cmd.flags = flags;
    if (strlen(dev) >= sizeof(cmd.name)) {
        close(fd);
        return -ENAMETOOLONG;
    }
    strcpy(cmd.name, dev);
    static char buf[sizeof(struct cosmo_fsctl_result) + sizeof(struct cosmo_fsctl_devcheck)];
    int rc = 0;
    if (write(fd, &cmd, sizeof(cmd)) != (long)sizeof(cmd))
        rc = -errno;
    else if (read(fd, buf, sizeof(buf)) != (long)sizeof(buf))
        rc = -EIO;
    close(fd);
    if (rc == 0)
        memcpy(out, buf + sizeof(struct cosmo_fsctl_result), sizeof(*out));
    return rc;
}

static void header(const char *dev, const struct cosmo_fsctl_devcheck *r, bool writable)
{
    const struct cosmo_fsctl_dev *d = &r->dev;
    if (!(d->flags & COSMO_FSCTL_DEV_F_CHECKED))
        return;
    const struct cosmo_fsctl_check *c = &r->check;
    printf("fsck.cosmofs: %s: format %u, generation %llu, %llu blocks seen, %llu free, %llu inodes, "
           "%llu directories, %llu snapshots (%llu us)\n",
           dev, d->format, (unsigned long long)d->generation, (unsigned long long)c->blocks_seen,
           (unsigned long long)c->counted_free, (unsigned long long)c->inodes_seen,
           (unsigned long long)c->dirs_seen, (unsigned long long)c->snapshots_seen,
           (unsigned long long)(c->elapsed_ns / 1000));
    if (d->flags & COSMO_FSCTL_DEV_F_FELL_BACK)
        printf("fsck.cosmofs: %s: the newer root did not load; generation %llu, the older, was checked\n", dev,
               (unsigned long long)d->generation);
    if (d->replayed_frees || d->replayed_orphans)
        printf("fsck.cosmofs: %s: the mount's replay released %llu block(s) and %u orphan inode(s)%s\n", dev,
               (unsigned long long)d->replayed_frees, d->replayed_orphans,
               writable ? "" : " in memory only (a read-only check writes nothing)");
    if (d->flags & COSMO_FSCTL_DEV_F_ROLLBACK_APPLIED)
        printf("fsck.cosmofs: %s: the pending rollback to %s was applied by this check's mount\n", dev,
               d->rollback_target[0] ? d->rollback_target : "?");
    else if (d->flags & COSMO_FSCTL_DEV_F_ROLLBACK_REFUSED)
        printf("fsck.cosmofs: %s: a rollback to %s is pending and could not be applied: %s\n", dev,
               d->rollback_target[0] ? d->rollback_target : "a snapshot that is gone",
               strerror(-d->rollback_error));
    else if (d->flags & COSMO_FSCTL_DEV_F_ROLLBACK_PENDING)
        printf("fsck.cosmofs: %s: a rollback to %s is pending; the next writable mount applies it\n", dev,
               d->rollback_target[0] ? d->rollback_target : "a snapshot that is gone");
}

/* One pass: print what it found, add it to `t`. False when the check did
 * not run (the device record says why, and it is in `t`). */
static bool report(const char *dev, const struct cosmo_fsctl_devcheck *r, bool writable, struct fsr_tally *t)
{
    header(dev, r, writable);
    fsreport_dev(stdout, &r->dev, t);
    if (!(r->dev.flags & COSMO_FSCTL_DEV_F_CHECKED))
        return false;
    fsreport_check(stdout, &r->check, t);
    if (r->check.flags & COSMO_FSCTL_R_PARTIAL)
        printf("fsck.cosmofs: %s: the answer is incomplete: something could not be read\n", dev);
    return true;
}

static int failed(const char *dev, int rc)
{
    if (rc == -EINVAL)
        fprintf(stderr, "fsck.cosmofs: %s: this kernel has no check of an unmounted device (CHECK_DEV)\n", dev);
    else if (rc == -EBUSY)
        fprintf(stderr, "fsck.cosmofs: %s is mounted: check it with fsctl check ID (fsctl list)\n", dev);
    else
        fprintf(stderr, "fsck.cosmofs: %s: %s\n", dev, strerror(-rc));
    return FSR_FAILED;
}

static int plain(const char *dev, bool repair)
{
    static struct cosmo_fsctl_devcheck r;
    int rc = devcheck(dev, repair ? COSMO_FSCTL_F_REPAIR : 0, &r);
    if (rc)
        return failed(dev, rc);
    struct fsr_tally t = { 0 };
    report(dev, &r, repair, &t);
    if (repair && (r.check.flags & COSMO_FSCTL_R_REPAIR_REFUSED))
        printf("fsck.cosmofs: %s: repair refused: the walk was not sure enough to act on\n", dev);
    int code = fsreport_exit(&t);
    printf("fsck.cosmofs: %s: %llu finding(s), %llu repaired, %s\n", dev, t.findings, t.repaired,
           code == FSR_CLEAN ? "clean" : code == FSR_REPAIRED ? "all repaired" : "findings left");
    return code;
}

/* The boot's one line, and the refusal's. */
static int refuse(const char *dev, const struct fsr_tally *t)
{
    printf("cosmofs: root refused: %s; %s is not mounted. From this shell: fsck.cosmofs --repair %s\n",
           t->refused, dev, dev);
    return FSR_LEFT;
}

static int boot(const char *dev, bool repair)
{
    static struct cosmo_fsctl_devcheck r;
    int rc = devcheck(dev, 0, &r);
    if (rc) {
        failed(dev, rc);
        printf("cosmofs: root not checked: %s\n", strerror(-rc));
        return FSR_FAILED;
    }
    struct fsr_tally t = { 0 };
    bool ran = report(dev, &r, false, &t);
    if (!ran || t.refuse)
        return refuse(dev, &t);
    if (t.findings == 0) {
        printf("cosmofs: root checked, 0 findings, 0 repaired\n");
        return FSR_CLEAN;
    }
    if (!repair) {
        printf("cosmofs: root checked, %llu findings, 0 repaired (fsck=check)\n", t.findings);
        return FSR_CLEAN;   /* reported; the pool mounts */
    }
    /* Every finding is one a boot repairs: repair them. The kernel
     * applies the same rule again (F_BOOT), against what it finds now. */
    static struct cosmo_fsctl_devcheck f;
    rc = devcheck(dev, COSMO_FSCTL_F_REPAIR | COSMO_FSCTL_F_BOOT, &f);
    if (rc) {
        failed(dev, rc);
        printf("cosmofs: root checked, %llu findings, 0 repaired (the repair could not run: %s)\n", t.findings,
               strerror(-rc));
        return FSR_FAILED;
    }
    struct fsr_tally u = { 0 };
    if (!report(dev, &f, true, &u) || u.refuse || (f.dev.flags & COSMO_FSCTL_DEV_F_BOOT_REFUSED))
        return refuse(dev, &u);
    /* The logged classes, by inode: what the repair touched. */
    for (unsigned i = 0; i < COSMO_FSCTL_CLASSES; i++) {
        const struct cosmo_fsctl_class *k = &f.check.class[i];
        if (fsreport_classes[i].policy != FSR_LOGGED || k->repaired == 0)
            continue;
        for (unsigned n = 0; n < k->named && n < COSMO_FSCTL_NAMES; n++)
            printf("cosmofs: root: %s inode %llu repaired\n", fsreport_classes[i].name,
                   (unsigned long long)k->name[n]);
    }
    printf("cosmofs: root checked, %llu findings, %llu repaired\n", u.findings, u.repaired);
    return fsreport_exit(&u);
}

static void usage(void)
{
    fprintf(stderr, "usage: fsck.cosmofs [--check | --repair] DEVICE\n"
                    "       fsck.cosmofs --boot check|repair DEVICE\n");
}

int main(int argc, char **argv)
{
    const char *dev;
    if (argc == 4 && strcmp(argv[1], "--boot") == 0 &&
        (strcmp(argv[2], "check") == 0 || strcmp(argv[2], "repair") == 0)) {
        dev = argv[3];
        if (strncmp(dev, "/dev/", 5) == 0)
            dev += 5;
        return boot(dev, strcmp(argv[2], "repair") == 0);
    }
    bool repair = false;
    if (argc == 3 && (strcmp(argv[1], "--check") == 0 || strcmp(argv[1], "--repair") == 0)) {
        repair = strcmp(argv[1], "--repair") == 0;
        dev = argv[2];
    } else if (argc == 2 && argv[1][0] != '-') {
        dev = argv[1];
    } else {
        usage();
        return 2;
    }
    if (strncmp(dev, "/dev/", 5) == 0)
        dev += 5;
    return plain(dev, repair);
}
