#!/usr/bin/env python3
"""
dirfd-probe.py -- what does the Linux door do with a real directory descriptor?

`compat/linux/syscalls.c` resolves a path relative to a directory only
when that directory is the process's current one: `check_dirfd()` passes
`AT_FDCWD` and absolute paths and answers every real `dirfd` with
`-ENOSYS`. Nine `*at` calls go through it, and `fchdir` is not in the
table at all. This measures it where a program meets it: it injects into
`tests/linux/lxtest.c`, at the point where `lxtest` already holds an open
directory descriptor for `/tmp/lxdir` (containing `moved`), one call of
each kind against that descriptor with a relative path, and prints what
each answered:

    DIRFD openat -38
    DIRFD newfstatat -38
    ...

`-38` is `ENOSYS`. Each call is chosen so that a door that resolves the
descriptor answers something else -- a new descriptor, 0, or `-ENOENT`
for the one that names something absent on purpose -- so the column is a
direct reading of which calls can use a directory descriptor at all. It
changes nothing it does not undo: the rename is renamed back, the
directory and node it makes are removed, and `fchdir` is followed by a
`chdir` back to `/`.

Usage:

    python3 tools/dirfd-probe.py apply
    gmake test && grep DIRFD out/x86_64-debug/boot-test.log
    python3 tools/dirfd-probe.py revert

`apply` refuses a file with uncommitted changes; `revert` restores its
snapshot, refuses if the file changed since the apply, and touches it so
make rebuilds.
"""

import hashlib
import os
import shutil
import subprocess
import sys

TEST = 'tests/linux/lxtest.c'
FILES = [TEST]
BACKUP = '.dirfd-probe.orig'
STAMP = '.dirfd-probe.applied'

ANCHOR = "    CHECKV(sc3(LX_getdents64, dfd, dents, sizeof(dents)) == 0, 0);\n    CHECKV(sc1(LX_close, dfd) == 0, 0);\n"

PROBE = r'''    /* --- DIRFD PROBE (tools/dirfd-probe.py; not for merge) --- */
    {
        static char pb[64];
        static char lb[64];
        static char sb[256];
#define DIRFD_SAY(name, v)                                                   \
        do {                                                                 \
            long _v = (v);                                                   \
            int _i = 0, _n = 0;                                              \
            char _d[24];                                                     \
            const char *_s = "DIRFD " name " ";                              \
            while (_s[_n])                                                   \
                pb[_i++] = _s[_n++];                                         \
            if (_v < 0) {                                                    \
                pb[_i++] = '-';                                              \
                _v = -_v;                                                    \
            }                                                                \
            _n = 0;                                                          \
            do {                                                             \
                _d[_n++] = (char)('0' + _v % 10);                            \
                _v /= 10;                                                    \
            } while (_v);                                                    \
            while (_n)                                                       \
                pb[_i++] = _d[--_n];                                         \
            pb[_i++] = '\n';                                                 \
            pb[_i] = 0;                                                      \
            lx_puts(pb);                                                     \
        } while (0)
        long o = sc4(LX_openat, dfd, "moved", LX_O_RDONLY, 0);
        DIRFD_SAY("openat", o);
        if (o >= 0)
            sc1(LX_close, o);
        DIRFD_SAY("newfstatat", sc4(LX_newfstatat, dfd, "moved", sb, 0));
        DIRFD_SAY("faccessat", sc3(LX_faccessat, dfd, "moved", 0));
        DIRFD_SAY("readlinkat", sc4(LX_readlinkat, dfd, "absent", lb, sizeof(lb)));
        long sl = sc3(LX_symlinkat, "moved", dfd, "lnk");
        DIRFD_SAY("symlinkat", sl);
        if (sl == 0)
            sc3(LX_unlinkat, LX_AT_FDCWD, "/tmp/lxdir/lnk", 0);
        long md = sc3(LX_mkdirat, dfd, "sub", 0755);
        DIRFD_SAY("mkdirat", md);
        long nd = sc4(LX_mknodat, dfd, "fifo", LX_S_IFIFO | 0644, 0);
        DIRFD_SAY("mknodat", nd);
        if (nd == 0)
            sc3(LX_unlinkat, LX_AT_FDCWD, "/tmp/lxdir/fifo", 0);
        long ud = sc3(LX_unlinkat, dfd, "sub", LX_AT_REMOVEDIR);
        DIRFD_SAY("unlinkat", ud);
        if (md == 0 && ud != 0)
            sc3(LX_unlinkat, LX_AT_FDCWD, "/tmp/lxdir/sub", LX_AT_REMOVEDIR);
#ifdef LX_renameat
        long rn = sc4(LX_renameat, dfd, "moved", dfd, "moved2");
        DIRFD_SAY("renameat", rn);
        if (rn == 0)
            sc4(LX_renameat, LX_AT_FDCWD, "/tmp/lxdir/moved2", LX_AT_FDCWD, "/tmp/lxdir/moved");
#endif
#if defined(__x86_64__)
        long fc = sc1(81, dfd);    /* fchdir: not numbered in nr_x86_64.h */
#else
        long fc = sc1(50, dfd);    /* fchdir: not numbered in nr_aarch64.h */
#endif
        DIRFD_SAY("fchdir", fc);
        if (fc == 0)
            sc1(LX_chdir, "/");
#undef DIRFD_SAY
    }
    /* --- end dirfd probe --- */
'''


def _digest(text):
    return hashlib.sha256(text.encode()).hexdigest()


def apply_():
    if os.path.exists(TEST + BACKUP):
        sys.exit("%s%s exists: a previous run was not reverted." % (TEST, BACKUP))
    if subprocess.run(['git', 'status', '--porcelain', '--', TEST], capture_output=True, text=True).stdout.strip():
        sys.exit("uncommitted changes in %s" % TEST)
    text = open(TEST).read()
    if text.count(ANCHOR) != 1:
        sys.exit("anchor appears %d times" % text.count(ANCHOR))
    first, second = ANCHOR.split('\n', 1)
    text = text.replace(ANCHOR, first + '\n' + PROBE + second, 1)
    shutil.copyfile(TEST, TEST + BACKUP)
    open(TEST, 'w').write(text)
    open(TEST + STAMP, 'w').write(_digest(text))
    print("applied: build and boot, then grep DIRFD in the boot log")


def revert():
    if not os.path.exists(TEST + BACKUP):
        sys.exit("no snapshot found: nothing to revert")
    want = open(TEST + STAMP).read().strip() if os.path.exists(TEST + STAMP) else None
    if want is None or _digest(open(TEST).read()) != want:
        sys.exit("%s changed since apply; the original is beside it as *%s" % (TEST, BACKUP))
    shutil.move(TEST + BACKUP, TEST)
    os.utime(TEST, None)
    if os.path.exists(TEST + STAMP):
        os.remove(TEST + STAMP)
    print("reverted (and touched)")


if __name__ == '__main__':
    if len(sys.argv) != 2 or sys.argv[1] not in ('apply', 'revert'):
        sys.exit(__doc__)
    (apply_ if sys.argv[1] == 'apply' else revert)()
