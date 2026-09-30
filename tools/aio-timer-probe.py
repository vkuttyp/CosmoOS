#!/usr/bin/env python3
"""aio-timer-probe.py -- the async I/O ring cannot submit a timer.

The ring (kernel/io/aio.c) drives any object with a readiness operation:
COSMO_AIO_READ/WRITE/PREAD/PWRITE/FSYNC/POLL, on a handle to a file, socket
or device. Its only timer is aio_wait's whole-call timeout (the aio_alarm),
which returns a bare 0 -- not a completion carrying user_data. There is no
timer object and no timer op, so Prompt #2 §23's "async I/O must work for
... timers" has nothing to submit (inventory row, docs/audit/2026-09-
deferred-work-inventory.md: "a timer as a submittable object ... unverified").

This probe adds a block to init's async-I/O self-test showing that a parked
poll plus a 20 ms wait yields zero completions -- the timeout is bare, no
timer completion arrives -- and that the op space tops out at COSMO_AIO_POLL.

    python3 tools/aio-timer-probe.py apply
    gmake ARCH=x86_64 test > run.txt 2>&1
    grep AIOTIMER out/x86_64-debug/boot-test.log
    python3 tools/aio-timer-probe.py revert

`apply` and `revert` are those of tools/lockup-interrupted-probe.py.
"""

import hashlib
import os
import stat
import subprocess
import sys

INIT = 'userland/init/init.c'
BACKUP = '.aio-timer-probe.orig'
STAMP = '.aio-timer-probe.applied'

OLD = """        CHECK(got == 1 && cq[0].user_data == 1 && cq[0].result == 8 && memcmp(rbuf, "ringdata", 8) == 0);
        /* POLL: not ready parks, then completes with the bit when data arrives; a write completes at once. */
"""
NEW = """        CHECK(got == 1 && cq[0].user_data == 1 && cq[0].result == 8 && memcmp(rbuf, "ringdata", 8) == 0);
        /* AIOTIMER probe: a timer is not a submittable object. The ring's only
         * timer is aio_wait's whole-call timeout, which returns a bare 0 -- not
         * a completion carrying user_data -- so "wake me in 20 ms" cannot be
         * submitted alongside I/O and told apart from "nothing was ready". */
        {
            struct cosmo_sqe tsq = { .op = COSMO_AIO_POLL, .handle = p[0],
                                     .events = COSMO_IO_READABLE, .user_data = 77 };
            CHECK(cosmo_aio_submit(ring, &tsq, 1) == 1);   /* parks: p[0] is empty */
            long tgot = cosmo_aio_wait(ring, cq, 8, 1, 20000000);   /* 20 ms */
            fprintf(stderr, "AIOTIMER: op max COSMO_AIO_POLL(%d), no timer op; parked poll + 20ms wait -> %ld completions (bare timeout, no user_data); no timer object to submit\\n",
                    COSMO_AIO_POLL, tgot);
            CHECK(tgot == 0);   /* the timeout produced no completion */
            CHECK(write(p[1], "z", 1) == 1);
            CHECK(cosmo_aio_wait(ring, cq, 8, 1, COSMO_AIO_WAIT_FOREVER) == 1 && cq[0].user_data == 77);
            CHECK(read(p[0], rbuf, 1) == 1);   /* drain, leaving p[0] empty as before */
        }
        /* POLL: not ready parks, then completes with the bit when data arrives; a write completes at once. */
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
                     f'edits and remove the AIOTIMER lines by hand, then delete {path + BACKUP} '
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
    apply_files([(INIT, [(OLD, NEW)])])
    print('applied: init async-I/O self-test shows a timer is not a submittable object')


if __name__ == '__main__':
    {'apply': apply, 'revert': revert}.get(sys.argv[1] if len(sys.argv) > 1 else '',
                                           lambda: sys.exit(__doc__))()
