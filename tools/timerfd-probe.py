#!/usr/bin/env python3
"""timerfd-probe.py -- the Linux personality has no timerfd.

timerfd_create (x86-64 283, AArch64 85) and its settime/gettime companions are
unlisted in linux_table, so the dispatcher's lx_unknown returns -ENOSYS. A
Linux program that wants a waitable timer fd for its poll/epoll loop dies at
creation, though the kernel already has the timer-as-readiness-kobject the
timerfd needs (the aio-timer unit's timerobj, "a timer as a submittable I/O
object (a timerfd)"). See docs/audit/next-subsystem-timerfd.md.

This probe adds LX_timerfd_create to both syscall-number headers and a check to
the Linux raw-ABI test tests/linux/lxtest.c (which runs in the standard boot)
that timerfd_create(CLOCK_MONOTONIC, 0) returns -ENOSYS.

    python3 tools/timerfd-probe.py apply
    gmake ARCH=x86_64 test > run.txt 2>&1
    grep LXTIMERFD out/x86_64-debug/boot-test.log
    python3 tools/timerfd-probe.py revert

`apply` and `revert` are those of tools/eventfd-probe.py.
"""

import hashlib
import os
import stat
import subprocess
import sys

NR_X86 = 'compat/linux/nr_x86_64.h'
NR_ARM = 'compat/linux/nr_aarch64.h'
LXTEST = 'tests/linux/lxtest.c'
BACKUP = '.timerfd-probe.orig'
STAMP = '.timerfd-probe.applied'

OLD_X86 = "#define LX_eventfd2 290\n"
NEW_X86 = OLD_X86 + "#define LX_timerfd_create 283\n"

OLD_ARM = "#define LX_eventfd2 19\n"
NEW_ARM = OLD_ARM + "#define LX_timerfd_create 85\n"

OLD_TEST = '    CHECKV(sc4(LX_newfstatat, LX_AT_FDCWD, "/tmp/lxtest.txt", &st, 0) == 0 && st.st_size == 22, 0);\n'
NEW_TEST = OLD_TEST + """    /* LXTIMERFD probe (tools/timerfd-probe.py): timerfd_create is not
     * implemented, so the dispatcher's lx_unknown answers -ENOSYS. The marker
     * prints only when the syscall really returned -ENOSYS, so grepping it
     * cannot show a false result after a failed check. */
    {
        long tfdrc = sc2(LX_timerfd_create, LX_CLOCK_MONOTONIC, 0);
        CHECKV(tfdrc == -38, tfdrc);   /* -ENOSYS */
        if (tfdrc == -38)
            lx_puts("LXTIMERFD: timerfd_create unimplemented -> -ENOSYS; no waitable timer\\n");
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
                     f'edits and remove the LXTIMERFD lines by hand, then delete {path + BACKUP} '
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
        (LXTEST, [(OLD_TEST, NEW_TEST)]),
    ])
    print('applied: lxtest shows timerfd_create returns -ENOSYS')


if __name__ == '__main__':
    {'apply': apply, 'revert': revert}.get(sys.argv[1] if len(sys.argv) > 1 else '',
                                           lambda: sys.exit(__doc__))()
