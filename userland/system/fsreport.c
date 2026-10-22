/*
 * fsreport.c - The checker's findings, printed one way (fsreport.h).
 */
#include <string.h>

#include "fsreport.h"

/*
 * The boot policy is the design's (docs/kernel-services/filesystem/
 * cosmofs/design.md, "fsck at boot"), and the kernel enforces the same
 * set (COSMOFS_CHECK_BOOT): bookkeeping is repaired unattended, what a
 * crash leaves is repaired and named, anything that is an argument about
 * which of two things is real is refused to an operator.
 */
const struct fsr_class fsreport_classes[COSMO_FSCTL_CLASSES] = {
    { "leaked", "block", FSR_UNATTENDED },          /* alloc_not_seen */
    { "free-in-use", "block", FSR_REFUSE },         /* seen_not_alloc: repaired offline only */
    { "cross-linked", "block", FSR_REFUSE },        /* dup */
    { "nlink", "inode", FSR_LOGGED },               /* nlink_wrong */
    { "orphan", "inode", FSR_LOGGED },              /* orphan */
    { "dangling", "inode", FSR_REFUSE },            /* dangling_entry */
    { "bad-entry", "object", FSR_REFUSE },          /* dir_bad: a block or an inode */
    { "counter", "count", FSR_UNATTENDED },         /* counter_wrong: the count the walk made */
    { "cycle", "object", FSR_REFUSE },              /* chain_cycle */
    { "unreadable", "object", FSR_REFUSE },         /* unreadable */
    { "extent-order", "inode", FSR_REFUSE },
    { "extent-overlap", "inode", FSR_REFUSE },
    { "dup-name", "inode", FSR_REFUSE },            /* the directory */
    { "snap-identity", "snapshot", FSR_UNATTENDED },
    { "rollback-record", "block", FSR_UNATTENDED },
    { "hold", "block", FSR_UNATTENDED },
};

const char *fsreport_policy(enum fsr_policy p)
{
    return p == FSR_UNATTENDED ? "unattended" : p == FSR_LOGGED ? "logged" : "refuse";
}

static void refused(struct fsr_tally *t, const char *name)
{
    size_t n = strlen(t->refused);
    if (n + strlen(name) + 3 < sizeof(t->refused))
        snprintf(t->refused + n, sizeof(t->refused) - n, "%s%s", n ? ", " : "", name);
    t->refuse++;
}

void fsreport_check(FILE *out, const struct cosmo_fsctl_check *c, struct fsr_tally *t)
{
    unsigned long long before = t->findings;
    for (unsigned i = 0; i < c->nclasses && i < COSMO_FSCTL_CLASSES; i++) {
        const struct cosmo_fsctl_class *k = &c->class[i];
        if (k->count == 0)
            continue;
        const struct fsr_class *f = &fsreport_classes[i];
        fprintf(out, "class %s count %llu repaired %llu %s\n", f->name, (unsigned long long)k->count,
                (unsigned long long)k->repaired, fsreport_policy(f->policy));
        const char *state = k->repaired >= k->count ? "repaired" : k->repaired ? "partly" : "left";
        for (unsigned n = 0; n < k->named && n < COSMO_FSCTL_NAMES; n++) {
            unsigned long long v = k->name[n];
            if (strcmp(f->kind, "block") == 0)
                fprintf(out, "finding %s block %llu:%llu %s\n", f->name, v >> 56, v & ((1ull << 56) - 1), state);
            else
                fprintf(out, "finding %s %s %llu %s\n", f->name, f->kind, v, state);
        }
        if (k->count > k->named)
            fprintf(out, "finding %s %s +%llu more %s\n", f->name, f->kind,
                    (unsigned long long)(k->count - k->named), state);
        t->findings += k->count;
        t->repaired += k->repaired < k->count ? k->repaired : k->count;
        if (f->policy == FSR_REFUSE)
            refused(t, f->name);
    }
    /* A walk that stopped before the end (the duplicate-name
     * confirmations' cap) with nothing else to say has not found the pool
     * sound: a finding of its own, never "clean" (Codex on #434). */
    if ((c->flags & COSMO_FSCTL_R_PARTIAL) && t->findings == before)
        fsreport_device(out, "incomplete", "the-walk-stopped-before-the-end", t);
}

void fsreport_device(FILE *out, const char *name, const char *what, struct fsr_tally *t)
{
    fprintf(out, "class %s count 1 repaired 0 refuse\n", name);
    fprintf(out, "finding %s object %s left\n", name, what);
    t->findings++;
    refused(t, name);
}

const char *fsreport_stage(unsigned stage)
{
    static const char *const names[] = {
        [COSMO_FSCTL_STAGE_NONE] = "mount",
        [COSMO_FSCTL_STAGE_SUPER] = "superblock",
        [COSMO_FSCTL_STAGE_MEMBERS] = "members",
        [COSMO_FSCTL_STAGE_BITMAP] = "bitmap",
        [COSMO_FSCTL_STAGE_FREE_RECORD] = "free-record",
        [COSMO_FSCTL_STAGE_SNAPSHOTS] = "snapshots",
        [COSMO_FSCTL_STAGE_ROLLBACK] = "rollback-record",
        [COSMO_FSCTL_STAGE_ORPHAN_RECORD] = "orphan-record",
        [COSMO_FSCTL_STAGE_ROOT] = "root",
    };
    return stage < sizeof(names) / sizeof(names[0]) && names[stage] ? names[stage] : "mount";
}

void fsreport_dev(FILE *out, const struct cosmo_fsctl_dev *d, struct fsr_tally *t)
{
    char what[96];
    if (d->flags & COSMO_FSCTL_DEV_F_SUPER_NONE)
        fsreport_device(out, "superblock", "neither-slot-verifies", t);
    if (d->flags & COSMO_FSCTL_DEV_F_SUPER_DISAGREE) {
        snprintf(what, sizeof(what), "slots-name-different-pools(generations:%llu,%llu)",
                 (unsigned long long)d->slot_generation[0], (unsigned long long)d->slot_generation[1]);
        fsreport_device(out, "superblock", what, t);
    }
    if (d->flags & COSMO_FSCTL_DEV_F_MOUNT_FAILED) {
        snprintf(what, sizeof(what), "mount-failed(%s)", strerror(-d->mount_error));
        fsreport_device(out, fsreport_stage(d->mount_stage), what, t);
    }
}

int fsreport_exit(const struct fsr_tally *t)
{
    if (t->findings == 0)
        return FSR_CLEAN;
    return t->repaired >= t->findings ? FSR_REPAIRED : FSR_LEFT;
}
