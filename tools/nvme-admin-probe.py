#!/usr/bin/env python3
"""
nvme-admin-probe.py -- does the NVMe admin path free its completion while
complete() is still inside it?

One CI sighting on main (run 36082559265, aarch64 debug, 2026-09-25): a
panic in spin_unlock -- "assertion failed: lock->locked != 0" -- from the
nvme module's interrupt handler on CPU 1 during probe, with lockdep saying
CPU 1 held the spin lock named 'nvme-admin'. That name is the lock inside
the struct completion admin_cmd() keeps on its stack.

The suspected mechanism: admin_cmd() polls completion_done() and, once it
is true, returns without wait_for_completion() -- the handshake
kernel/include/kernel/completion.h requires before the memory goes. So
the frame can be reused (the next admin_cmd's completion_init at the same
address clears the lock word) while the interrupt handler is still inside
complete(), between setting `done` and releasing the completion's lock.

The probe builds the adversary from that mechanism: it widens exactly that
window -- a spin of SPIN_US inside complete(), after `done` is set and
before the wake and unlock, for completions whose lock is named
'nvme-admin' and only when called from interrupt context -- so the waiter,
which sleeps 1 ms between polls, sees `done` and leaves while complete()
is still running. `--fixed` also applies the handshake to admin_cmd (a
wait_for_completion once `done` is seen), the fix the report proposes, so
the same adversary can be run against it.

    python3 tools/nvme-admin-probe.py apply [--fixed] [SPIN_US]   # default 3000
    gmake ARCH=aarch64 test        # and ARCH=x86_64
    python3 tools/nvme-admin-probe.py revert

Every widened complete() prints nothing (it runs with interrupts masked);
the boot's own verdict and, on a panic, its message and the held-lock list
are the result. Commit before applying; commit nothing while applied.

`apply` refuses a file with uncommitted changes, refuses to overwrite a
backup an earlier run left, and edits nothing if an anchor is missing,
restoring every file on a failure part-way. The stamp records each file's
hash before and after the patch; `revert` accepts a file already back to
its original hash, restores the rest, removes the stamp, and only then
removes the backups, so an interrupted revert can be run again.
"""

import hashlib
import os
import shutil
import subprocess
import sys

COMPLETION = 'kernel/scheduler/completion.c'
NVME = 'drivers/nvme/nvme.c'
BACKUP = '.nvme-admin-probe.orig'
STAMP = '.nvme-admin-probe.applied'

C_ANCHOR_INC = '#include <kernel/sched.h>\n'
C_PROBE_INC = C_ANCHOR_INC + '#include <arch/cpu.h>        /* NAPROBE */\n#include <kernel/string.h>   /* NAPROBE */\n#include <kernel/timer.h>    /* NAPROBE */\n'

C_ANCHOR = '''    arch_irq_state_t s = spin_lock_irqsave(&c->lock);
    c->done = true;
    waitqueue_wake_all(&c->wq);'''


def c_probe(spin_us):
    return f'''    arch_irq_state_t s = spin_lock_irqsave(&c->lock);
    c->done = true;
    /* NAPROBE (tools/nvme-admin-probe.py; not for merge): hold the window
     * between `done` and the unlock open, for the NVMe admin completion as
     * signalled from its interrupt handler only. */
    if (raw_this_cpu()->irq_depth != 0 && c->lock.name != NULL && strcmp(c->lock.name, "nvme-admin") == 0) {{
        uint64_t until = clock_now_ns() + {spin_us}ull * 1000ull;
        while (clock_now_ns() < until)
            arch_cpu_relax();
    }}
    waitqueue_wake_all(&c->wq);'''


N_ANCHOR = '''    int rc;
    bool timed_out = false;
    if (!completion_done(&w.done)) {'''
N_FIXED = '''    int rc;
    bool timed_out = false;
    if (completion_done(&w.done))
        wait_for_completion(&w.done);   /* NAPROBE --fixed: the handshake before the frame goes */
    if (!completion_done(&w.done)) {'''


def sha(p):
    return hashlib.sha256(open(p, 'rb').read()).hexdigest()


def files(spin_us, fixed):
    fl = [(COMPLETION, [(C_ANCHOR_INC, C_PROBE_INC), (C_ANCHOR, c_probe(spin_us))])]
    if fixed:
        fl.append((NVME, [(N_ANCHOR, N_FIXED)]))
    return fl


def apply():
    args = sys.argv[2:]
    fixed = '--fixed' in args
    rest = [a for a in args if a != '--fixed']
    spin_us = int(rest[0]) if rest else 3000
    if os.path.exists(STAMP):
        sys.exit('already applied')
    fl = files(spin_us, fixed)
    for path, edits in fl:
        if os.path.exists(path + BACKUP):
            sys.exit(f'{path + BACKUP} exists from an earlier run; restore or remove it by hand first')
        if not os.path.isfile(path):
            sys.exit(f'{path} not found: run from the top of the tree')
        if subprocess.run(['git', 'status', '--porcelain', '--', path], capture_output=True, text=True).stdout.strip():
            sys.exit(f'{path} has uncommitted changes')
        s = open(path).read()
        for a, _ in edits:
            if s.count(a) != 1:
                sys.exit(f'{path}: anchor not found exactly once: {a[:50]!r}')
    done, stamp = [], []
    try:
        for path, edits in fl:
            shutil.copyfile(path, path + BACKUP)
            done.append(path)
            orig = sha(path)
            s = open(path).read()
            for a, b in edits:
                s = s.replace(a, b)
            open(path, 'w').write(s)
            stamp.append(f'{path} {sha(path)} {orig}')
        open(STAMP, 'w').write('\n'.join(stamp) + '\n')
    except BaseException:
        for path in done:
            shutil.move(path + BACKUP, path)
            os.utime(path, None)
        if os.path.exists(STAMP):
            os.remove(STAMP)
        raise
    print(f'applied (spin {spin_us} us{", with the handshake fix" if fixed else ""})')


def revert():
    if not os.path.exists(STAMP):
        sys.exit('not applied')
    todo, paths = [], []
    for line in open(STAMP).read().split('\n'):
        if line:
            path, patched, orig = line.split()
            paths.append(path)
            if sha(path) == orig:
                continue                     # already restored by an earlier, interrupted revert
            if sha(path) != patched:
                sys.exit(f'{path} changed since apply; restore by hand from {path + BACKUP}')
            if not os.path.exists(path + BACKUP):
                sys.exit(f'{path} is still patched and {path + BACKUP} is gone; restore by hand')
            todo.append(path)
    for path in todo:
        shutil.copyfile(path + BACKUP, path)
        os.utime(path, None)
    os.remove(STAMP)                         # every file is original now
    for path in paths:
        if os.path.exists(path + BACKUP):
            os.remove(path + BACKUP)
    print('reverted')


if __name__ == '__main__':
    {'apply': apply, 'revert': revert}.get(sys.argv[1] if len(sys.argv) > 1 else '',
                                           lambda: sys.exit(__doc__))()
