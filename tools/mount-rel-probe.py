#!/usr/bin/env python3
"""
mount-rel-probe.py -- where does a relative mount target land?

Every native path call resolves a relative path from the caller's
working directory (P27): open, stat, mkdir, unlink, rename, symlink,
readlink, chdir, spawn's program, cwd and root. `sys_mount` and
`sys_umount` hand the path to `vfs_mount` and `vfs_umount2`, which look
it up with a NULL start -- the caller's root. So a relative target is
resolved from the root, whatever directory the caller stands in.

This measures it where a program meets it: it injects into `init`'s
self-test, after its own mount checks, a process in `/tmp` with both
`/tmp/mrel` and `/mrel` present that mounts a ramfs on `mrel` and then
unmounts `mrel`. Which directory the mount covered is read from the
inode each name reports before and after (a mount root is a different
inode from the directory it covers):

    MRPROBE mount rc=0 covers_tmp_mrel=0 covers_root_mrel=1
    MRPROBE umount rc=0 tmp_mrel_mounted=0 root_mrel_mounted=0
    MRPROBE absent mount rc=-2

The first line is the defect: the caller asked for the `mrel` beside it
and got the one under `/`. The last repeats the mount with `/mrel`
removed: `ENOENT`, with `/tmp/mrel` present in the caller's own
directory. It undoes only what it did: a directory is removed only if
this run created it, a mount unmounted only if this run made it, and the
absent case is skipped (and says so) if `/mrel` existed before the probe,
since it would have to remove it. The caller returns to `/`.

Usage:

    python3 tools/mount-rel-probe.py apply
    gmake test && grep MRPROBE out/x86_64-debug/boot-test.log
    python3 tools/mount-rel-probe.py revert

`apply` refuses a file with uncommitted changes and edits nothing if the
anchor is missing; `revert` restores its snapshot, refuses if the file
changed since the apply, and touches it so make rebuilds.
"""

import hashlib
import os
import shutil
import subprocess
import sys

TARGET = 'userland/init/init.c'
BACKUP = '.mount-rel-probe.orig'
STAMP = '.mount-rel-probe.applied'

ANCHOR = '    CHECK(cosmo_umount2("/tmp/flagm", 0) == 0);\n    CHECK(cosmo_rmdir("/tmp/flagm") == 0);\n'
PROBE = r'''    /* --- MRPROBE (tools/mount-rel-probe.py; not for merge) --- */
    {
        /* Undo only what this run did: a directory is removed only if this
         * run created it, a mount unmounted only if this run made it. */
        struct cosmo_stat t0, r0, t1, r1;
        int made_t = cosmo_mkdir("/tmp/mrel", 0755) == 0;
        int made_r = cosmo_mkdir("/mrel", 0755) == 0;
        int ok = cosmo_stat("/tmp/mrel", &t0) == 0 && cosmo_stat("/mrel", &r0) == 0;
        if (ok && chdir("/tmp") == 0) {
            long m = cosmo_mount("none", "mrel", "ramfs", 0);
            (void)cosmo_stat("/tmp/mrel", &t1);
            (void)cosmo_stat("/mrel", &r1);
            int on_t = m == 0 && t1.ino != t0.ino, on_r = m == 0 && r1.ino != r0.ino;
            fprintf(stderr, "MRPROBE mount rc=%ld covers_tmp_mrel=%d covers_root_mrel=%d\n", m, on_t, on_r);
            long u = m == 0 ? cosmo_umount("mrel") : -1;
            (void)cosmo_stat("/tmp/mrel", &t1);
            (void)cosmo_stat("/mrel", &r1);
            fprintf(stderr, "MRPROBE umount rc=%ld tmp_mrel_mounted=%d root_mrel_mounted=%d\n", u,
                    t1.ino != t0.ino, r1.ino != r0.ino);
            if (t1.ino != t0.ino)
                (void)cosmo_umount("/tmp/mrel");   /* this run's mount, still there */
            if (r1.ino != r0.ino)
                (void)cosmo_umount("/mrel");
            if (made_r) {
                /* The absent case needs /mrel gone, which only this run may do. */
                (void)cosmo_rmdir("/mrel");
                made_r = 0;
                long a = cosmo_mount("none", "mrel", "ramfs", 0);
                fprintf(stderr, "MRPROBE absent mount rc=%ld\n", a);
                if (a == 0)
                    (void)cosmo_umount("/tmp/mrel");   /* the only mrel left for it to cover */
            } else {
                fprintf(stderr, "MRPROBE absent skipped: /mrel existed before the probe\n");
            }
            (void)chdir("/");
        } else {
            fprintf(stderr, "MRPROBE setup failed\n");
        }
        if (made_t)
            (void)cosmo_rmdir("/tmp/mrel");
        if (made_r)
            (void)cosmo_rmdir("/mrel");
    }
'''


def sha(p):
    return hashlib.sha256(open(p, 'rb').read()).hexdigest()


def apply():
    if os.path.exists(STAMP):
        sys.exit('already applied')
    if subprocess.run(['git', 'status', '--porcelain', '--', TARGET], capture_output=True, text=True).stdout.strip():
        sys.exit(f'{TARGET} has uncommitted changes')
    s = open(TARGET).read()
    if s.count(ANCHOR) != 1:
        sys.exit(f'{TARGET}: anchor not found exactly once')
    shutil.copyfile(TARGET, TARGET + BACKUP)
    try:
        open(TARGET, 'w').write(s.replace(ANCHOR, ANCHOR + PROBE))
        open(STAMP, 'w').write(sha(TARGET) + '\n')
    except BaseException:
        shutil.move(TARGET + BACKUP, TARGET)
        os.utime(TARGET, None)
        if os.path.exists(STAMP):
            os.remove(STAMP)
        raise
    print('applied')


def revert():
    if not os.path.exists(STAMP):
        sys.exit('not applied')
    if sha(TARGET) != open(STAMP).read().strip():
        sys.exit(f'{TARGET} changed since apply; restore by hand from {TARGET + BACKUP}')
    shutil.move(TARGET + BACKUP, TARGET)
    os.utime(TARGET, None)
    os.remove(STAMP)
    print('reverted')


if __name__ == '__main__':
    {'apply': apply, 'revert': revert}.get(sys.argv[1] if len(sys.argv) > 1 else '', lambda: sys.exit(__doc__))()
