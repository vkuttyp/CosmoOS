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

`apply` refuses a file with uncommitted changes (or a `git status` that
fails), refuses to overwrite a backup an earlier run left, and edits
nothing if an anchor is missing. It builds every patch in memory, writes
the stamp -- each file's original and patched hash -- and then replaces
each backup and file atomically, so from the stamp on every file is
exactly one of the two. `revert` accepts a file already original,
restores the patched ones atomically, removes the stamp, and only then the
backups, so an apply or revert interrupted anywhere is finished by running
revert (again).
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
    /* NAPROBE --fixed: decide once. Two reads of `done` let the interrupt
     * land between them, skipping both the handshake and the timeout path. */
    if (completion_done(&w.done)) {
        wait_for_completion(&w.done);   /* the handshake before the frame goes */
    } else {'''


def sha(p):
    return hashlib.sha256(open(p, 'rb').read()).hexdigest()


def write_atomic(path, data):
    # Whole or not at all: a probe interrupted mid-write must leave every
    # file either as it was or as intended, never half of each.
    tmp = path + '.probe-tmp'
    with open(tmp, 'wb') as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)


def git_clean(path):
    r = subprocess.run(['git', 'status', '--porcelain', '--', path], capture_output=True, text=True)
    if r.returncode != 0:     # a failed check is not a clean tree
        sys.exit(f'git status failed for {path}: {r.stderr.strip() or r.returncode}')
    return not r.stdout.strip()


def apply_files(fl):
    """Patch every file in `fl`, or none. Every patch is built in memory
    first; then the stamp is written, recording each file's original and
    patched hash; then each backup and each file is replaced atomically.
    From the stamp on, every file is exactly its original or its patched
    bytes, so revert can finish whatever an interruption left."""
    if os.path.exists(STAMP):
        sys.exit('already applied (or an apply was interrupted): run revert first')
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
        sys.exit('not applied')
    entries = [line.split() for line in open(STAMP).read().split('\n') if line]
    for path, patched, orig in entries:
        cur = sha(path)
        if cur == orig:
            continue                     # never patched, or already restored
        if cur != patched:
            sys.exit(f'{path} changed since apply; restore by hand from {path + BACKUP}')
        if not os.path.exists(path + BACKUP) or sha(path + BACKUP) != orig:
            sys.exit(f'{path} is still patched and {path + BACKUP} is missing or not its original; restore by hand')
    for path, patched, orig in entries:
        if sha(path) == patched:
            write_atomic(path, open(path + BACKUP, 'rb').read())
    os.remove(STAMP)                     # every file is original now
    for path, _, _ in entries:
        for leftover in (path + BACKUP, path + '.probe-tmp'):
            if os.path.exists(leftover):
                os.remove(leftover)
    print('reverted')


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
    apply_files(files(spin_us, fixed))
    print(f'applied (spin {spin_us} us{", with the handshake fix" if fixed else ""})')


if __name__ == '__main__':
    {'apply': apply, 'revert': revert}.get(sys.argv[1] if len(sys.argv) > 1 else '',
                                           lambda: sys.exit(__doc__))()
