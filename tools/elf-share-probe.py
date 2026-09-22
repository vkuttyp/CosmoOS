#!/usr/bin/env python3
"""
elf-share-probe.py -- what does a second copy of the same program cost?

`elf_load_into` maps every PT_LOAD segment as anonymous, POPULATED
memory and memcpy's the file's bytes into it
(`kernel/process/elf.c`), so two processes running the same binary hold
two private copies of its text. The file-regions unit built
`VM_REGION_FILE` over the page cache -- shared mappings coherent by
construction, private ones copy-on-write, demand-paged -- and named the
loader's segments as deferred.

This measures the gap. It spawns N copies of one program, one at a time,
and reports the free-frame count before each, so the cost of a copy is a
subtraction rather than an estimate:

    ELFSHARE copies 1 free 129384 delta 0
    ELFSHARE copies 2 free 129301 delta 83
    ELFSHARE copies 3 free 129218 delta 83
    ELFSHARE image text 200704 data 8192 bss 4096

The delta is what a file-backed text mapping would mostly remove.

Usage:

    python3 tools/elf-share-probe.py apply
    gmake test && grep ELFSHARE out/x86_64-debug/boot-test.log
    python3 tools/elf-share-probe.py revert

`apply` refuses a file with uncommitted changes; `revert` restores its
own snapshot and never runs `git checkout`, so it can only undo what it
did.

**The boot it runs in is a measuring boot, not a passing one.** The
probe spawns four copies of `init --block`, which hold the console, and
kills them; `process-spawn` and `hid-keyboard` fail as a result. That is
the probe perturbing what it shares the machine with, and the numbers
above are still the numbers -- but do not read the boot's verdict as a
regression.
"""

import os
import shutil
import subprocess
import sys

TEST = 'kernel/process/proctest.c'
SELFTEST_C = 'kernel/core/selftest.c'
SELFTEST_H = 'kernel/include/kernel/selftest.h'
FILES = [TEST, SELFTEST_C, SELFTEST_H]
BACKUP = '.elf-share-probe.orig'

PROBE = r'''
/* --- ELF SHARE PROBE (tools/elf-share-probe.py; not for merge) --- */
bool selftest_elf_share_probe(const char **reason);
bool selftest_elf_share_probe(const char **reason)
{
    const void *image;
    size_t image_size;
    if (!bootarchive_find("init", &image, &image_size)) {
        kinfo("selftest: elf-share-probe: no init image; skipping");
        return true;
    }
    /* What the image asks for, so the delta below can be read against
     * it: the sum of every PT_LOAD's file bytes and its zero tail. */
    struct elf_info info;
    const char *why = NULL;
    if (elf_validate(image, image_size, 0x1000, 0x7fffffffffffull, &info, &why) == 0) {
        uint64_t filesz = 0, memsz = 0;
        for (unsigned i = 0; i < info.nr_segments; i++) {
            filesz += info.segments[i].filesz;
            memsz += info.segments[i].memsz;
        }
        kprintf("ELFSHARE image segments %u filesz %llu memsz %llu (%llu pages)\n",
                info.nr_segments, (unsigned long long)filesz, (unsigned long long)memsz,
                (unsigned long long)((memsz + 4095) / 4096));
    }

    enum { COPIES = 4 };
    struct process *p[COPIES] = { NULL };
    static const char *const argv[] = { "init", "--block", NULL };
    struct pmm_stats st;
    uint64_t prev = 0;
    unsigned made = 0;
    for (unsigned i = 0; i < COPIES; i++) {
        pmm_get_stats(&st);
        uint64_t before = st.free_pages;
        if (process_create_from_elf(image, image_size, "init", argv, NULL, NULL, &p[i]) != 0)
            break;
        made++;
        /* Let it reach user mode: the cost is paid at load, but the
         * stack and the first faults are the process's too. */
        thread_sleep_ms(80);
        pmm_get_stats(&st);
        kprintf("ELFSHARE copy %u free_before %llu free_after %llu cost %lld pages%s\n",
                i + 1, (unsigned long long)before, (unsigned long long)st.free_pages,
                (long long)(before - st.free_pages),
                i == 0 ? " (the first: process, space, stack and image)" : "");
        prev = st.free_pages;
    }
    (void)prev;
    for (unsigned i = 0; i < made; i++) {
        process_kill(p[i], COSMO_SIGKILL);
        process_wait_exit(p[i]);
        process_put(p[i]);
    }
    (void)reason;
    return true;
}
/* --- end elf share probe --- */

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
        TEST: [("bool selftest_process_spawn(const char **reason)",
                PROBE + "bool selftest_process_spawn(const char **reason)"),
               ("#include <kernel/process.h>",
                "#include <kernel/pmm.h>\n#include <kernel/process.h>")],
        SELFTEST_H: [("bool selftest_process_spawn(const char **reason);",
                      "bool selftest_elf_share_probe(const char **reason);\n"
                      "bool selftest_process_spawn(const char **reason);")],
        SELFTEST_C: [('{ "process-spawn",',
                      '{ "elf-share-probe", selftest_elf_share_probe },\n    { "process-spawn",')],
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
    print("applied: build and boot, then grep ELFSHARE in the boot log")


def revert():
    missing = [f for f in FILES if not os.path.exists(f + BACKUP)]
    if len(missing) == len(FILES):
        sys.exit("no snapshots found: nothing to revert")
    for f in FILES:
        if os.path.exists(f + BACKUP):
            shutil.move(f + BACKUP, f)
    print("reverted")


if __name__ == '__main__':
    if len(sys.argv) != 2 or sys.argv[1] not in ('apply', 'revert'):
        sys.exit(__doc__)
    (apply_ if sys.argv[1] == 'apply' else revert)()
