#!/usr/bin/env python3
"""
mmap-place-probe.py -- how often do two placements collide?

`sys_mmap`'s non-fixed path, at both doors, is two calls under two holds
of the space lock: `vm_user_find_free()` picks a hole and releases the
lock, then `vm_user_map_anon()` takes it again and `space_insert`s. Two
threads asking for a placement at once can be handed the same hole, and
the loser's insert is `-EEXIST` -- which reached userland as
`thrtest: FAIL env_reader start at line 1169: rc -17` on 2026-09-23
(docs/testing/flakes.md, "`thrtest` cannot start a thread", the fourth
sighting), because a thread start reserves its stack with `mmap(NULL)`
while a sibling mallocs.

This measures the race at the kernel's own boundary: N threads on one
scratch user space, each doing exactly the syscall's two calls in a
loop, and it prints how many placements collided:

    MMAPPLACE threads 2 rounds 2000 each: ok 3xxx eexist yyy other 0

Every `eexist` is a placement the kernel promised and then refused. The
held-walk seam's argument applies: a rate is a measurement of the race,
and the repair is to make the two calls one critical section, after
which the number is zero by construction and not by luck.

Usage:

    python3 tools/mmap-place-probe.py apply
    gmake test && grep MMAPPLACE out/x86_64-debug/boot-test.log
    python3 tools/mmap-place-probe.py revert

`apply` refuses a file with uncommitted changes. `revert` restores its
own snapshot, refuses if an instrumented file changed since the apply,
and touches what it restores so make rebuilds it.
"""

import hashlib
import os
import shutil
import subprocess
import sys

TEST = 'kernel/memory/memtest.c'
SELFTEST_C = 'kernel/core/selftest.c'
SELFTEST_H = 'kernel/include/kernel/selftest.h'
FILES = [TEST, SELFTEST_C, SELFTEST_H]
BACKUP = '.mmap-place-probe.orig'
STAMP = '.mmap-place-probe.applied'


def _digest(text):
    return hashlib.sha256(text.encode()).hexdigest()


PROBE = r'''
/* --- MMAP PLACE PROBE (tools/mmap-place-probe.py; not for merge) --- */
#include <kernel/process.h>
struct place_racer {
    struct vm_space *sp;
    unsigned rounds, ok, eexist, other, nofree;
};

/* Exactly sys_mmap's non-fixed path: find under one hold, map under another. */
static void place_racer_main(void *arg)
{
    struct place_racer *r = arg;
    for (unsigned i = 0; i < r->rounds; i++) {
        uint64_t base = vm_user_find_free(r->sp, USER_MMAP_BASE, PAGE_SIZE);
        if (base == 0) {
            r->nofree++;
            continue;
        }
        int rc = vm_user_map_anon(r->sp, base, PAGE_SIZE, VM_PROT_RW, 0, "place");
        if (rc == 0)
            r->ok++;
        else if (rc == -EEXIST)
            r->eexist++;
        else
            r->other++;
    }
}

bool selftest_mmap_place_probe(const char **reason);
bool selftest_mmap_place_probe(const char **reason)
{
    (void)reason;
    static const unsigned counts[] = { 2, 4 };
    for (unsigned c = 0; c < 2; c++) {
        unsigned n = counts[c];
        struct vm_space *sp = NULL;
        if (vm_space_create_user(&sp) != 0)
            return true;
        struct place_racer rr[4];
        struct thread *th[4];
        unsigned made = 0;
        for (unsigned i = 0; i < n; i++) {
            rr[i] = (struct place_racer){ .sp = sp, .rounds = 2000 };
            th[i] = thread_create(place_racer_main, &rr[i], "place", SCHED_PRIO_DEFAULT);
            if (th[i])
                made++;
        }
        unsigned ok = 0, ee = 0, other = 0, nofree = 0;
        for (unsigned i = 0; i < n; i++) {   /* every slot: a failed create leaves a NULL, not a gap */
            if (th[i]) {
                thread_join(th[i]);
                ok += rr[i].ok;
                ee += rr[i].eexist;
                other += rr[i].other;
                nofree += rr[i].nofree;
            }
        }
        kprintf("MMAPPLACE threads %u rounds 2000 each: ok %u eexist %u other %u nofree %u\n", made, ok, ee, other,
                nofree);
        vm_space_destroy(sp);
    }
    return true;
}
/* --- end mmap place probe --- */

'''


def apply_():
    for f in FILES:
        if os.path.exists(f + BACKUP):
            sys.exit("%s%s exists: a previous run was not reverted." % (f, BACKUP))
    dirty = subprocess.run(['git', 'status', '--porcelain', '--'] + FILES,
                           capture_output=True, text=True, check=True).stdout.strip()
    if dirty:
        sys.exit("uncommitted changes in: %s" % dirty)
    edits = {
        TEST: [("bool selftest_vm_replace_race(const char **reason)",
                PROBE + "bool selftest_vm_replace_race(const char **reason)")],
        SELFTEST_H: [("bool selftest_vm_replace_race(const char **reason);",
                      "bool selftest_mmap_place_probe(const char **reason);\n"
                      "bool selftest_vm_replace_race(const char **reason);")],
        SELFTEST_C: [('    { "vm-replace-race", selftest_vm_replace_race },',
                      '    { "mmap-place-probe", selftest_mmap_place_probe },\n'
                      '    { "vm-replace-race", selftest_vm_replace_race },')],
    }
    staged = {}
    for path, pairs in edits.items():
        text = open(path).read()
        for old, new in pairs:
            if text.count(old) != 1:
                sys.exit("%s: anchor appears %d times: %r" % (path, text.count(old), old[:50]))
            text = text.replace(old, new, 1)
        staged[path] = text
    for path, text in staged.items():
        shutil.copyfile(path, path + BACKUP)
        open(path, 'w').write(text)
        open(path + STAMP, 'w').write(_digest(text))
    print("applied: build and boot, then grep MMAPPLACE in the boot log")


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
