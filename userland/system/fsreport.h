/*
 * fsreport.h - The checker's and the scrub's findings, printed one way
 * (roadmap 1.2 B2; docs/kernel-services/filesystem/cosmofs/api.md,
 * "fsck.cosmofs(8)"). fsck.cosmofs, fsctl and init share it, so a class
 * has one name, one boot policy and one line format wherever it is
 * printed -- the format test-fsck parses:
 *
 *   class NAME count N repaired M POLICY
 *   finding NAME KIND ID STATE
 *
 * NAME is a class of `fsreport_classes` or one of the device's
 * (superblock, root, free-record, orphan-record, mount); KIND is block
 * (printed member:block), inode, snapshot, count or object (a class that
 * names blocks and inodes both); STATE is repaired, left or partly (the
 * class was part repaired and the kernel does not say which). POLICY is
 * what a boot does with the class: unattended, logged or refuse.
 *
 * Exit status, fsck's convention: 0 nothing found, 1 everything found was
 * repaired, 4 findings are left, 8 the check could not be run.
 */
#ifndef FSREPORT_H
#define FSREPORT_H

#include <stdio.h>

#include <uapi/cosmo/fsctl.h>

#define FSR_CLEAN     0
#define FSR_REPAIRED  1
#define FSR_LEFT      4
#define FSR_FAILED    8

enum fsr_policy {
    FSR_UNATTENDED,   /* a boot repairs it without asking */
    FSR_LOGGED,       /* a boot repairs it and names what it touched */
    FSR_REFUSE,       /* a boot refuses the pool, by this name */
};

struct fsr_class {
    const char *name;
    const char *kind;
    enum fsr_policy policy;
};

/* By the check record's index (uapi/cosmo/fsctl.h's ABI order). */
extern const struct fsr_class fsreport_classes[COSMO_FSCTL_CLASSES];

const char *fsreport_policy(enum fsr_policy p);

struct fsr_tally {
    unsigned long long findings;   /* summed counts */
    unsigned long long repaired;
    unsigned refuse;               /* classes with findings whose policy is refuse */
    char refused[160];             /* their names, comma-separated */
};

/* The `class` and `finding` lines of one check record, added to `t`. */
void fsreport_check(FILE *out, const struct cosmo_fsctl_check *c, struct fsr_tally *t);
/* A device-level finding (superblock, root, ...): one class line and one
 * finding line, always refused. */
void fsreport_device(FILE *out, const char *name, const char *what, struct fsr_tally *t);
/* The device record's findings: what kept the check from running. */
void fsreport_dev(FILE *out, const struct cosmo_fsctl_dev *d, struct fsr_tally *t);
/* The exit status for what `t` holds. */
int fsreport_exit(const struct fsr_tally *t);
/* A stage of a failed mount, by name. */
const char *fsreport_stage(unsigned stage);

#endif
