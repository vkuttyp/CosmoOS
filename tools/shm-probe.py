#!/usr/bin/env python3
"""shm-probe.py -- the Linux personality has no System V shared memory.

shmget/shmat/shmdt/shmctl (x86-64 29/30/67/31, AArch64 194/196/197/195) have no
syscall number defined in compat/linux/nr_*.h and no linux_table entry, so the
dispatcher's lx_unknown returns -ENOSYS. A Linux program that shares memory
through the System V API cannot run, though the backing it needs -- an anonymous
page-cache file mapped MAP_SHARED -- already exists (the memfd unit:
ramfs_anon_reg + vfs_ftruncate + vm_user_map_file). See
docs/audit/next-subsystem-shm.md.

The numbers are undefined, so this probe adds them to both nr_*.h headers (so a
raw call compiles on each arch) and one check to tests/linux/lxtest.c (which
runs in the standard boot) asserting shmget returns -ENOSYS.

    python3 tools/shm-probe.py apply
    gmake ARCH=x86_64 test > run.txt 2>&1
    grep LXSHM out/x86_64-debug/boot-test.log
    python3 tools/shm-probe.py revert

`apply` and `revert` are those of tools/mremap-probe.py.
"""

import hashlib
import os
import stat
import subprocess
import sys

LXTEST = 'tests/linux/lxtest.c'
NR_X86 = 'compat/linux/nr_x86_64.h'
NR_ARM = 'compat/linux/nr_aarch64.h'
BACKUP = '.shm-probe.orig'
STAMP = '.shm-probe.applied'

X86_ANCHOR = '#define LX_madvise 28\n'
X86_NEW = X86_ANCHOR + ('#define LX_shmget 29\n'
                        '#define LX_shmat 30\n'
                        '#define LX_shmctl 31\n'
                        '#define LX_shmdt 67\n')

ARM_ANCHOR = '#define LX_socket 198\n'
ARM_NEW = ARM_ANCHOR + ('#define LX_shmget 194\n'
                        '#define LX_shmctl 195\n'
                        '#define LX_shmat 196\n'
                        '#define LX_shmdt 197\n')

OLD_TEST = '    CHECKV(sc4(LX_newfstatat, LX_AT_FDCWD, "/tmp/lxtest.txt", &st, 0) == 0 && st.st_size == 22, 0);\n'
NEW_TEST = OLD_TEST + """    /* LXSHM probe (tools/shm-probe.py): the System V shm calls have no
     * number and no handler, so the dispatcher's lx_unknown answers -ENOSYS.
     * The marker prints only when the syscall really returned -ENOSYS, so
     * grepping it cannot show a false result after a failed check. */
    {
        long srp = sc3(LX_shmget, 0 /* IPC_PRIVATE */, 4096, 01000 | 0600 /* IPC_CREAT|0600 */);
        CHECKV(srp == -38, srp);   /* -ENOSYS */
        if (srp == -38)
            lx_puts("LXSHM: shmget unimplemented -> -ENOSYS; no System V shared memory\\n");
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
                     f'edits and remove the LXSHM lines by hand, then delete {path + BACKUP} '
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
    apply_files([(NR_X86, [(X86_ANCHOR, X86_NEW)]),
                 (NR_ARM, [(ARM_ANCHOR, ARM_NEW)]),
                 (LXTEST, [(OLD_TEST, NEW_TEST)])])
    print('applied: lxtest shows shmget returns -ENOSYS')


if __name__ == '__main__':
    {'apply': apply, 'revert': revert}.get(sys.argv[1] if len(sys.argv) > 1 else '',
                                           lambda: sys.exit(__doc__))()
