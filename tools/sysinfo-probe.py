#!/usr/bin/env python3
"""sysinfo-probe.py -- the Linux personality stubs sysinfo.

sysinfo (x86-64 99, AArch64 179) is listed in linux_table but wired to the
lx_nosys stub, so it returns -ENOSYS. A Linux program reading uptime or a
coarse memory snapshot through it gets nothing, though the kernel already keeps
the page counts (pmm_get_stats), the boot clock (clock_now_ns) and the process
count (process_count) it would report. See docs/audit/next-subsystem-sysinfo.md.

LX_sysinfo is already defined, so this probe patches only the Linux raw-ABI
test tests/linux/lxtest.c (which runs in the standard boot) to assert sysinfo
returns -ENOSYS.

    python3 tools/sysinfo-probe.py apply
    gmake ARCH=x86_64 test > run.txt 2>&1
    grep LXSYSINFO out/x86_64-debug/boot-test.log
    python3 tools/sysinfo-probe.py revert

`apply` and `revert` are those of tools/epoll-probe.py.
"""

import hashlib
import os
import stat
import subprocess
import sys

LXTEST = 'tests/linux/lxtest.c'
BACKUP = '.sysinfo-probe.orig'
STAMP = '.sysinfo-probe.applied'

OLD_TEST = '    CHECKV(sc4(LX_newfstatat, LX_AT_FDCWD, "/tmp/lxtest.txt", &st, 0) == 0 && st.st_size == 22, 0);\n'
NEW_TEST = OLD_TEST + """    /* LXSYSINFO probe (tools/sysinfo-probe.py): sysinfo is stubbed to lx_nosys,
     * so it returns -ENOSYS. The marker prints only when the syscall really
     * returned -ENOSYS, so grepping it cannot show a false result after a
     * failed check. */
    {
        char sibuf[128];
        long sirc = sc1(LX_sysinfo, sibuf);
        CHECKV(sirc == -38, sirc);   /* -ENOSYS */
        if (sirc == -38)
            lx_puts("LXSYSINFO: sysinfo stubbed -> -ENOSYS; no uptime/memory snapshot\\n");
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
                     f'edits and remove the LXSYSINFO lines by hand, then delete {path + BACKUP} '
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
    apply_files([(LXTEST, [(OLD_TEST, NEW_TEST)])])
    print('applied: lxtest shows sysinfo returns -ENOSYS')


if __name__ == '__main__':
    {'apply': apply, 'revert': revert}.get(sys.argv[1] if len(sys.argv) > 1 else '',
                                           lambda: sys.exit(__doc__))()
