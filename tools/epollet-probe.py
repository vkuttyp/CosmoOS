#!/usr/bin/env python3
"""epollet-probe.py -- epoll is level-triggered only; EPOLLET is refused.

The epoll object (kernel/io/epoll.c, PR #291) is level-triggered. lx_epoll_ctl
rejects any registration whose events carry EPOLLET with -EINVAL
(compat/linux/syscalls.c: "edge-triggered is deferred"), so an edge-triggered
event loop -- the mode nginx, libev/libevent, Go's netpoller and tokio use --
cannot even register. Edge-triggered is the existing level-triggered machinery
plus a per-member "armed" flag and a re-arm rule, mirroring the one-shot
`disabled` flag already there. See docs/audit/next-subsystem-epollet.md.

EPOLLET (linux_abi.h) and the epoll_event ABI already exist, so this probe adds
no syscall number: it adds one check to tests/linux/lxtest.c (which runs in the
standard boot), inside the existing epoll block, asserting epoll_ctl with
EPOLLET returns -EINVAL.

    python3 tools/epollet-probe.py apply
    gmake ARCH=x86_64 test > run.txt 2>&1
    grep LXEPOLLET out/x86_64-debug/boot-test.log
    python3 tools/epollet-probe.py revert

`apply` and `revert` are those of tools/signalfd-probe.py.
"""

import hashlib
import os
import stat
import subprocess
import sys

LXTEST = 'tests/linux/lxtest.c'
BACKUP = '.epollet-probe.orig'
STAMP = '.epollet-probe.applied'

# The existing EPOLLET error-case check; the probe's marker block goes after it,
# reusing `ep` and the spare `efd2` already in scope in the epoll block.
OLD_TEST = '        CHECKV(sc4(LX_epoll_ctl, ep, LX_EPOLL_CTL_ADD, efd2, &et) == -22, 0);  /* EPOLLET -> EINVAL */\n'
NEW_TEST = OLD_TEST + """        /* LXEPOLLET probe (tools/epollet-probe.py): lx_epoll_ctl refuses EPOLLET
         * with -EINVAL today (edge-triggered deferred), so an edge-triggered
         * event loop cannot register. The marker prints only when the call
         * really returned -EINVAL, so grepping it cannot show a false result
         * after a failed check. */
        {
            struct lx_epoll_event etp = { .events = LX_EPOLLIN | LX_EPOLLET, .data = 0 };
            long etr = sc4(LX_epoll_ctl, ep, LX_EPOLL_CTL_ADD, efd2, &etp);
            CHECKV(etr == -22, etr);   /* -EINVAL */
            if (etr == -22)
                lx_puts("LXEPOLLET: edge-triggered epoll rejected -> -EINVAL; level-triggered only\\n");
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
                     f'edits and remove the LXEPOLLET lines by hand, then delete {path + BACKUP} '
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
    print('applied: lxtest shows epoll_ctl rejects EPOLLET with -EINVAL')


if __name__ == '__main__':
    {'apply': apply, 'revert': revert}.get(sys.argv[1] if len(sys.argv) > 1 else '',
                                           lambda: sys.exit(__doc__))()
