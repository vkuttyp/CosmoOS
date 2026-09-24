#!/usr/bin/env python3
"""
chdir-link-probe.py -- what name does chdir publish through a symbolic link?

`chdir` normalises the path it was given against the old working
directory's name (`path_normalize`) and publishes that string with the
vnode the walk reached (`chdir_inner`, kernel/process/process.c). The
walk follows symbolic links; the normalisation does not know they exist.
So through a link the two can name different directories, and every
later relative `chdir("..")` normalises against the wrong one.

This measures it where a program meets it, at both doors, with the same
scenario: `/tmp/clp/deep` is a directory, `/tmp/clink` is a link to it.

    1. chdir("/tmp/clink")      -- through the link
    2. chdir("..")              -- lexically /tmp, physically /tmp/clp

After each step it prints the published name (`getcwd`) and whether that
name, looked up now, is the directory the process actually stands in
(`stat(".")` and `stat(name)` agree on the inode). `same=0` is a name
that names some other directory:

    CLPROBE native link cwd=/tmp/clink same=1 physical=0
    CLPROBE native dotdot cwd=/tmp same=0
    CLPROBE linux link cwd=/tmp/clink same=1 physical=0
    CLPROBE linux dotdot cwd=/tmp same=0

`physical=0` on the first line says the name reaches the right directory
only by following the link again (`lstat` of it is a link, not a
directory). It changes nothing it does not undo: it returns to `/` and
removes the link and both directories.

Usage:

    python3 tools/chdir-link-probe.py apply
    gmake test && grep CLPROBE out/x86_64-debug/boot-test.log
    python3 tools/chdir-link-probe.py revert

`apply` refuses a file with uncommitted changes; `revert` restores its
snapshot, refuses if the file changed since the apply, and touches it so
make rebuilds.
"""

import hashlib
import os
import shutil
import subprocess
import sys

NATIVE = 'userland/init/init.c'
LINUX = 'tests/linux/lxtest.c'
BACKUP = '.chdir-link-probe.orig'
STAMP = '.chdir-link-probe.applied'

NATIVE_ANCHOR = '    CHECK(rmdir("cwdtest") == 0);\n    CHECK(chdir("/") == 0);\n'
NATIVE_PROBE = r'''    /* --- CLPROBE (tools/chdir-link-probe.py; not for merge) --- */
    {
        struct stat a, b;
        (void)mkdir("/tmp/clp", 0755);
        (void)mkdir("/tmp/clp/deep", 0755);
        (void)symlink("/tmp/clp/deep", "/tmp/clink");
        if (chdir("/tmp/clink") == 0 && getcwd(buf, sizeof(buf))) {
            int same = stat(".", &a) == 0 && stat(buf, &b) == 0 && a.st_ino == b.st_ino;
            int phys = lstat(buf, &b) == 0 && S_ISDIR(b.st_type);
            fprintf(stderr, "CLPROBE native link cwd=%s same=%d physical=%d\n", buf, same, phys);
        } else {
            fprintf(stderr, "CLPROBE native link chdir failed errno=%d\n", errno);
        }
        if (chdir("..") == 0 && getcwd(buf, sizeof(buf))) {
            int same = stat(".", &a) == 0 && stat(buf, &b) == 0 && a.st_ino == b.st_ino;
            fprintf(stderr, "CLPROBE native dotdot cwd=%s same=%d\n", buf, same);
        }
        (void)chdir("/");
        (void)unlink("/tmp/clink");
        (void)rmdir("/tmp/clp/deep");
        (void)rmdir("/tmp/clp");
    }
'''

LINUX_ANCHOR = '    CHECKV(sc1(LX_chdir, "/") == 0, 0);\n    CHECKV(sc0(LX_umask) == 022, 0);\n'
LINUX_PROBE = r'''    /* --- CLPROBE (tools/chdir-link-probe.py; not for merge) --- */
    {
        static struct lx_stat sa, sb;
        static char cw[128];
        sc3(LX_mkdirat, LX_AT_FDCWD, "/tmp/clp", 0755);
        sc3(LX_mkdirat, LX_AT_FDCWD, "/tmp/clp/deep", 0755);
        sc3(LX_symlinkat, "/tmp/clp/deep", LX_AT_FDCWD, "/tmp/clink");
        const char *step[2] = { "/tmp/clink", ".." };
        const char *tag[2] = { "CLPROBE linux link cwd=", "CLPROBE linux dotdot cwd=" };
        for (int i = 0; i < 2; i++) {
            if (sc1(LX_chdir, step[i]) != 0 || sc2(LX_getcwd, cw, sizeof(cw)) <= 0) {
                lx_puts("CLPROBE linux chdir failed\n");
                break;
            }
            int same = sc4(LX_newfstatat, LX_AT_FDCWD, ".", &sa, 0) == 0 &&
                       sc4(LX_newfstatat, LX_AT_FDCWD, cw, &sb, 0) == 0 && sa.st_ino == sb.st_ino;
            lx_puts(tag[i]);
            lx_puts(cw);
            lx_puts(same ? " same=1" : " same=0");
            if (i == 0) {
                int phys = sc4(LX_newfstatat, LX_AT_FDCWD, cw, &sb, LX_AT_SYMLINK_NOFOLLOW) == 0 &&
                           (sb.st_mode & 0170000) == 0040000;
                lx_puts(phys ? " physical=1" : " physical=0");
            }
            lx_puts("\n");
        }
        sc1(LX_chdir, "/");
        sc3(LX_unlinkat, LX_AT_FDCWD, "/tmp/clink", 0);
        sc3(LX_unlinkat, LX_AT_FDCWD, "/tmp/clp/deep", LX_AT_REMOVEDIR);
        sc3(LX_unlinkat, LX_AT_FDCWD, "/tmp/clp", LX_AT_REMOVEDIR);
    }
'''

# (file, anchor, probe, where): the native probe goes after init's own
# cwd checks, which end back at `/` -- placed before them, its closing
# chdir("/") broke their relative rmdir("cwdtest"). The Linux probe goes
# before lxtest's chdir("/"), which it leaves true.
EDITS = [(NATIVE, NATIVE_ANCHOR, NATIVE_PROBE, 'after'), (LINUX, LINUX_ANCHOR, LINUX_PROBE, 'before')]


def sha(p):
    return hashlib.sha256(open(p, 'rb').read()).hexdigest()


def apply():
    if os.path.exists(STAMP):
        sys.exit('already applied')
    for path, anchor, probe, _ in EDITS:
        if subprocess.run(['git', 'status', '--porcelain', '--', path], capture_output=True, text=True).stdout.strip():
            sys.exit(f'{path} has uncommitted changes')
        s = open(path).read()
        if s.count(anchor) != 1:
            sys.exit(f'{path}: anchor not found exactly once')
    stamp = []
    for path, anchor, probe, where in EDITS:
        shutil.copyfile(path, path + BACKUP)
        s = open(path).read()
        open(path, 'w').write(s.replace(anchor, anchor + probe if where == 'after' else probe + anchor))
        stamp.append(f'{path} {sha(path)}')
    open(STAMP, 'w').write('\n'.join(stamp) + '\n')
    print('applied')


def revert():
    if not os.path.exists(STAMP):
        sys.exit('not applied')
    for line in open(STAMP).read().split('\n'):
        if not line:
            continue
        path, digest = line.split()
        if sha(path) != digest:
            sys.exit(f'{path} changed since apply; restore by hand from {path + BACKUP}')
    for path, _, _, _ in EDITS:
        shutil.move(path + BACKUP, path)
        os.utime(path, None)
    os.remove(STAMP)
    print('reverted')


if __name__ == '__main__':
    {'apply': apply, 'revert': revert}.get(sys.argv[1] if len(sys.argv) > 1 else '', lambda: sys.exit(__doc__))()
