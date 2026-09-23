#!/usr/bin/env python3
"""
cwd-race-probe.py -- can the working-directory race test see the bug it is for?

The cwd-ref unit (`docs/audit/next-subsystem-cwd-ref.md`) fixed a
use-after-free: every relative-path system call read
`process_current()->cwd` with no reference, so a `chdir` on another
thread could drop the last reference to a directory a walk was inside.
The fix is `process_cwd_get()`, a reference taken under the process lock.
Its test, `userland/tests/cwdtest.c` step 4, moves the process in and out
of a directory another thread keeps removing while a third walks a path
inside it -- and that unit's own record says every reverted-fix run
passed. The window is a few instructions on one side and a whole path
walk on the other. With the poison below, the test lands in it in some
boots and not others (one of five x86-64 boots that reached it, none of
three AArch64, on 2026-09-23) -- a rate, which is what this probe
measures and what a proof replaces.

This measures that. It puts the bug back at ONE site -- the native
`open`, which is what the walker in step 4 calls -- and poisons every
freed vnode, so a walk that resumes inside one meets 0x5a5a... instead of
memory that still looks like a directory. Then boot, as many times as
patience allows, and count:

    boot-test: PASS ...                       the test did not see the bug
    cwdtest: NNN walks in the victim          how often the window was open

Both outcomes are the measurement: a PASS with the bug present is a boot
the test could not see it in, a `#GP` in `kobject_get` with
`RAX=5a5a5a5a5a5a5a5a` is a boot it did. Neither is a claim that the fix
is wrong; together they are the evidence that the test is a regression
test that catches the bug intermittently and not a proof, which is what
the held-walk unit is for.

Usage:

    python3 tools/cwd-race-probe.py apply
    gmake test; grep -E 'cwdtest|boot-test: (PASS|FAIL)' out/x86_64-debug/boot-test.log
    ...repeat...
    python3 tools/cwd-race-probe.py revert

`apply` refuses a file with uncommitted changes. `revert` restores its own
snapshot, never runs `git checkout`, refuses outright if an instrumented
file has changed since the apply (it records what it wrote and compares),
and **touches what it restores** -- a restored file older than the object
compiled from the mutated one is skipped by make, and the mutated kernel
then keeps booting after the revert (found the hard way, one unit ago).
"""

import hashlib
import os
import shutil
import subprocess
import sys

NATIVE = 'kernel/syscall/native.c'
VFS = 'kernel-services/vfs/vfs.c'
FILES = [NATIVE, VFS]
BACKUP = '.cwd-race-probe.orig'
STAMP = '.cwd-race-probe.applied'


def _digest(text):
    return hashlib.sha256(text.encode()).hexdigest()


EDITS = {
    # The bug, at the one site the walker exercises: the raw field, no
    # reference, no put. This is what every relative-path system call did
    # before the cwd-ref unit.
    NATIVE: [(
        "    struct vnode *cwd = process_cwd_get();\n"
        "    rc = vfs_open(cwd, path, flags, mode, &f);\n"
        "    vnode_put(cwd);\n",
        "    struct vnode *cwd = process_current()->cwd_locked;   /* CWD RACE PROBE: the unowned read (not for merge) */\n"
        "    rc = vfs_open(cwd, path, flags, mode, &f);\n",
    )],
    # A freed vnode that still looks like a directory hides the bug; one
    # full of 0x5a does not. The pmm poisons freed frames for the same
    # reason (kernel/memory/pmm.c); the slab does not poison objects.
    VFS: [(
        "        vn->ops->evict(vn);\n"
        "    kfree(vn);\n",
        "        vn->ops->evict(vn);\n"
        "    memset(vn, 0x5a, sizeof(*vn));   /* CWD RACE PROBE: poison the freed vnode (not for merge) */\n"
        "    kfree(vn);\n",
    )],
}


def apply_():
    for f in FILES:
        if os.path.exists(f + BACKUP):
            sys.exit("%s%s exists: a previous run was not reverted." % (f, BACKUP))
    dirty = subprocess.run(['git', 'status', '--porcelain', '--'] + FILES,
                           capture_output=True, text=True, check=True).stdout.strip()
    if dirty:
        sys.exit("uncommitted changes in: %s" % dirty)
    staged = {}
    for path, pairs in EDITS.items():
        text = open(path).read()
        for old, new in pairs:
            if text.count(old) != 1:
                sys.exit("%s: anchor appears %d times: %r" % (path, text.count(old), old[:60]))
            text = text.replace(old, new, 1)
        staged[path] = text
    for path, text in staged.items():
        shutil.copyfile(path, path + BACKUP)
        open(path, 'w').write(text)
        open(path + STAMP, 'w').write(_digest(text))
    print("applied: the native open reads the cwd unowned and freed vnodes are poisoned.")
    print("Build and boot; a PASS is the measurement. Then: %s revert" % sys.argv[0])


def revert():
    missing = [f for f in FILES if not os.path.exists(f + BACKUP)]
    if len(missing) == len(FILES):
        sys.exit("no snapshots found: nothing to revert")
    edited = []
    for f in FILES:
        if not os.path.exists(f + BACKUP):
            continue
        want = open(f + STAMP).read().strip() if os.path.exists(f + STAMP) else None
        if want is None or _digest(open(f).read()) != want:
            edited.append(f)
    if edited:
        sys.exit("changed since apply, so this would discard work that is not mine:\n  " +
                 "\n  ".join(edited) +
                 "\nThe originals are beside them as *%s. Merge or delete them by hand." % BACKUP)
    for f in FILES:
        if os.path.exists(f + BACKUP):
            shutil.move(f + BACKUP, f)
            os.utime(f, None)   # newer than the mutated object, so make rebuilds it
        if os.path.exists(f + STAMP):
            os.remove(f + STAMP)
    print("reverted (and touched, so the next build is of the restored source)")


if __name__ == '__main__':
    if len(sys.argv) != 2 or sys.argv[1] not in ('apply', 'revert'):
        sys.exit(__doc__)
    (apply_ if sys.argv[1] == 'apply' else revert)()
