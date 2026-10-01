#!/usr/bin/env python3
"""statx-probe.py -- the Linux personality has no statx(2).

compat/linux implements fstat/newfstatat/stat/lstat but not statx (x86-64 332,
AArch64 291): the number is unlisted in linux_table, so the dispatcher routes
it to lx_unknown and it returns -ENOSYS. A program that prefers statx (modern
glibc) or needs a statx-only field gets -ENOSYS where fstat would succeed.
See docs/audit/next-subsystem-statx.md.

This probe adds LX_statx to both syscall-number headers, a 5-argument syscall
wrapper (sc5) to the Linux raw-ABI test header, and a check to tests/linux/
lxtest.c (which runs in the standard boot) that statx on the test file returns
-ENOSYS -- beside the fstat/newfstatat calls on the same file that succeed.

    python3 tools/statx-probe.py apply
    gmake ARCH=x86_64 test > run.txt 2>&1
    grep LXSTATX out/x86_64-debug/boot-test.log
    python3 tools/statx-probe.py revert

`apply` and `revert` are those of tools/net-rx-dup-probe.py.
"""

import hashlib
import os
import stat
import subprocess
import sys

NR_X86 = 'compat/linux/nr_x86_64.h'
NR_ARM = 'compat/linux/nr_aarch64.h'
LXABI = 'tests/linux/lxabi.h'
LXTEST = 'tests/linux/lxtest.c'
BACKUP = '.statx-probe.orig'
STAMP = '.statx-probe.applied'

OLD_X86 = "#define LX_newfstatat 262\n"
NEW_X86 = OLD_X86 + "#define LX_statx 332\n"

OLD_ARM = "#define LX_newfstatat 79\n"
NEW_ARM = OLD_ARM + "#define LX_statx 291\n"

OLD_SC = "#define sc4(n, a, b, c, d) lx_syscall6((n), (long)(a), (long)(b), (long)(c), (long)(d), 0, 0)\n"
NEW_SC = OLD_SC + "#define sc5(n, a, b, c, d, e) lx_syscall6((n), (long)(a), (long)(b), (long)(c), (long)(d), (long)(e), 0)\n"

OLD_TEST = '    CHECKV(sc4(LX_newfstatat, LX_AT_FDCWD, "/tmp/lxtest.txt", &st, 0) == 0 && st.st_size == 22, 0);\n'
NEW_TEST = OLD_TEST + """    /* LXSTATX probe (tools/statx-probe.py): statx is not implemented, so the
     * dispatcher's lx_unknown answers -ENOSYS where fstat on the same file
     * just succeeded. */
    {
        char sxbuf[256];
        long sxrc = sc5(LX_statx, LX_AT_FDCWD, "/tmp/lxtest.txt", 0, 0x7ffL, sxbuf);
        CHECKV(sxrc == -38, sxrc);   /* -ENOSYS */
        lx_puts("LXSTATX: statx unimplemented -> -ENOSYS; no struct statx marshaller\\n");
    }
"""


def sha(p):
    return hashlib.sha256(open(p, 'rb').read()).hexdigest()


def write_atomic(path, data):
    tmp = path + '.probe-tmp'
    with open(tmp, 'wb') as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())
    if os.path.exists(path):
        os.chmod(tmp, stat.S_IMODE(os.stat(path).st_mode))
    os.replace(tmp, path)


def git_clean(path):
    r = subprocess.run(['git', 'status', '--porcelain', '--', path], capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f'git status failed for {path}: {r.stderr.strip() or r.returncode}')
    return not r.stdout.strip()


def apply_files(fl):
    if os.path.exists(STAMP):
        sys.exit('already applied (or an apply was interrupted): run revert first')
    if os.path.exists(STAMP + '.probe-tmp'):
        os.remove(STAMP + '.probe-tmp')
    plan = []
    for path, edits in fl:
        if os.path.exists(path + BACKUP):
            sys.exit(f'{path + BACKUP} exists from an earlier run; restore or remove it by hand first')
        if not os.path.isfile(path):
            sys.exit(f'{path} not found: run from the top of the tree')
        if not git_clean(path):
            sys.exit(f'{path} has uncommitted changes')
        orig = open(path, 'rb').read()
        s = orig.decode()
        for a, b in edits:
            if s.count(a) != 1:
                sys.exit(f'{path}: anchor not found exactly once: {a[:50]!r}')
            s = s.replace(a, b)
        plan.append((path, orig, s.encode()))
    write_atomic(STAMP, ''.join(f'{p} {hashlib.sha256(n).hexdigest()} {hashlib.sha256(o).hexdigest()}\n'
                                for p, o, n in plan).encode())
    for path, orig, new in plan:
        write_atomic(path + BACKUP, orig)
        write_atomic(path, new)


def revert():
    if not os.path.exists(STAMP):
        if os.path.exists(STAMP + '.probe-tmp'):
            os.remove(STAMP + '.probe-tmp')
            sys.exit('not applied (removed a partial stamp an interrupted apply left)')
        sys.exit('not applied')
    entries = [line.split() for line in open(STAMP).read().split('\n') if line]
    for path, patched, orig in entries:
        cur = sha(path)
        if cur == orig:
            continue
        if cur != patched:
            sys.exit(f'{path} changed since apply. {path + BACKUP} is the pre-probe '
                     f'original, so copying it back would erase those changes. Keep your '
                     f'edits and remove the LXSTATX lines by hand, then delete {path + BACKUP} '
                     f'and {STAMP}.')
        if not os.path.exists(path + BACKUP) or sha(path + BACKUP) != orig:
            sys.exit(f'{path} is still patched and {path + BACKUP} is missing or not its original; restore by hand')
    for path, patched, orig in entries:
        if sha(path) == patched:
            write_atomic(path, open(path + BACKUP, 'rb').read())
    for path, _, _ in entries:
        for leftover in (path + BACKUP, path + '.probe-tmp', path + BACKUP + '.probe-tmp'):
            if os.path.exists(leftover):
                os.remove(leftover)
    if os.path.exists(STAMP + '.probe-tmp'):
        os.remove(STAMP + '.probe-tmp')
    os.remove(STAMP)
    print('reverted')


def apply():
    apply_files([
        (NR_X86, [(OLD_X86, NEW_X86)]),
        (NR_ARM, [(OLD_ARM, NEW_ARM)]),
        (LXABI, [(OLD_SC, NEW_SC)]),
        (LXTEST, [(OLD_TEST, NEW_TEST)]),
    ])
    print('applied: lxtest shows statx returns -ENOSYS')


if __name__ == '__main__':
    {'apply': apply, 'revert': revert}.get(sys.argv[1] if len(sys.argv) > 1 else '',
                                           lambda: sys.exit(__doc__))()
