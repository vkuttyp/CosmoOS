#!/usr/bin/env python3
"""
percpu-probe: which per-CPU answers are read while the thread could move?

The measuring tool behind docs/audit/next-subsystem-percpu-migration.md.
It patches the two per-CPU accessors (`arch_percpu_get`, `arch_cpu_id`)
so that a call made from thread context with preemption enabled,
interrupts enabled, in a thread whose affinity admits more than one CPU,
on a machine with more than one CPU, is reported once per call site:

    PERCPU-PROBE this_cpu ip=0x... thread=<name>

Reads that are per-thread by construction are made raw first
(`thread_current`, `preempt_disable`, `preempt_enable`), so the list is
the sites that keep a genuinely per-CPU answer.

    tools/percpu-probe.py apply --arch x86_64      # patch the tree (working copy only)
    gmake test ARCH=x86_64                          # one debug boot; the log has the sites
    tools/percpu-probe.py symbolize out/x86_64-debug/boot-test.log \
                                    out/x86_64-debug/kernel/kernel.elf
    git checkout -- kernel                          # take the probe out again

The patch is not meant to be committed: the unit it measures for ships
the same check as a panic in debug builds, at which point this script is
history. `symbolize` needs llvm-symbolizer on PATH (the swiftly toolchain
has one) and prints each site with its inline chain, then a per-file
tally.
"""
import argparse
import collections
import re
import subprocess
import sys

ROOT = __import__("os").path.dirname(__import__("os").path.dirname(__import__("os").path.abspath(__file__)))

PROBE = r'''
#include <kernel/thread.h>
#include <kernel/log.h>
#include <arch/irq.h>

struct percpu *arch_percpu_get_raw(void) { return probe_raw_get(); }

static uintptr_t g_probe_sites[512];
static unsigned g_probe_nsites;
static bool g_probe_lock;   /* the lookup and the insert are one step: two CPUs meeting one site report it once */

/* Report a per-CPU read made where a migration could invalidate it,
 * once per call site. Every read here is raw: the probe must not probe,
 * and the lock below is a bare test-and-set for the same reason (a
 * spinlock's own accessor reads would recurse into this function). It
 * is taken with preemption disabled and only from a context that the
 * checks above have already shown to be preemptible thread context, so
 * it cannot be taken twice on one CPU. */
static void percpu_probe(uintptr_t ip, const char *what)
{
    struct percpu *pc = probe_raw_get();
    if (pc->preempt_count != 0 || pc->irq_depth != 0 || !arch_irq_enabled())
        return;
    struct thread *cur = pc->current;
    if (cur == NULL || cpu_count() < 2)
        return;
    if ((cur->flags & THREAD_FLAG_IDLE) != 0)
        return;
    if (__builtin_popcountll(cur->affinity) <= 1)
        return;
    pc->preempt_count++;
    while (__atomic_test_and_set(&g_probe_lock, __ATOMIC_ACQUIRE))
        arch_cpu_relax();
    bool seen = false;
    for (unsigned i = 0; i < g_probe_nsites && i < 512; i++)
        if (g_probe_sites[i] == ip)
            seen = true;
    if (!seen && g_probe_nsites < 512)
        g_probe_sites[g_probe_nsites++] = ip;
    __atomic_clear(&g_probe_lock, __ATOMIC_RELEASE);
    pc->preempt_count--;
    if (!seen)
        kwarn("PERCPU-PROBE %s ip=%p thread=%s", what, (void *)ip, cur->name);
}

struct percpu *arch_percpu_get(void)
{
    percpu_probe((uintptr_t)__builtin_return_address(0), "this_cpu");
    return probe_raw_get();
}

unsigned arch_cpu_id(void)
{
    percpu_probe((uintptr_t)__builtin_return_address(0), "cpu_id");
    return probe_raw_get()->cpu_id;
}
'''

ARCH = {
    "x86_64": (
        "kernel/arch/x86_64/percpu.c",
        '''struct percpu *arch_percpu_get(void)
{
    struct percpu *pc;
    __asm__ volatile("mov %%gs:0, %0" : "=r"(pc));
    return pc;
}

unsigned arch_cpu_id(void)
{
    unsigned id;
    __asm__ volatile("mov %%gs:%c1, %0" : "=r"(id) : "i"(__builtin_offsetof(struct percpu, cpu_id)));
    return id;
}
''',
        '''static struct percpu *probe_raw_get(void)
{
    struct percpu *pc;
    __asm__ volatile("mov %%gs:0, %0" : "=r"(pc));
    return pc;
}
''' + PROBE,
    ),
    "aarch64": (
        "kernel/arch/aarch64/percpu.c",
        '''struct percpu *arch_percpu_get(void)
{
    return (struct percpu *)(uintptr_t)READ_SYSREG(tpidr_el1);
}

unsigned arch_cpu_id(void)
{
    return arch_percpu_get()->cpu_id;
}
''',
        '''static struct percpu *probe_raw_get(void)
{
    return (struct percpu *)(uintptr_t)READ_SYSREG(tpidr_el1);
}
''' + PROBE,
    ),
}

COMMON = [
    ("kernel/include/arch/percpu.h",
     "struct percpu *arch_percpu_get(void);",
     "struct percpu *arch_percpu_get(void);\nstruct percpu *arch_percpu_get_raw(void);"),
    ("kernel/include/kernel/thread.h",
     "static inline struct thread *thread_current(void)\n{\n    return this_cpu()->current;",
     "static inline struct thread *thread_current(void)\n{\n    return arch_percpu_get_raw()->current;"),
    ("kernel/include/kernel/percpu.h",
     "static inline void preempt_disable(void)\n{\n    this_cpu()->preempt_count++;",
     "static inline void preempt_disable(void)\n{\n    arch_percpu_get_raw()->preempt_count++;"),
    ("kernel/core/percpu.c",
     "void preempt_enable(void)\n{\n    struct percpu *pc = this_cpu();",
     "void preempt_enable(void)\n{\n    struct percpu *pc = arch_percpu_get_raw();"),
]


def edit(path, old, new):
    full = ROOT + "/" + path
    s = open(full).read()
    if s.count(old) != 1:
        sys.exit(f"{path}: anchor not found exactly once; the tree moved under the script")
    open(full, "w").write(s.replace(old, new))


def apply(arch):
    path, old, new = ARCH[arch]
    edit(path, old, new)
    for path, old, new in COMMON:
        edit(path, old, new)
    print(f"probe applied for {arch}; build and boot the debug suite, then `git checkout -- kernel`")


def symbolize(log, elf):
    rows = []
    for line in open(log, errors="replace"):
        m = re.search(r"PERCPU-PROBE (\w+) ip=(0x[0-9a-f]+) thread=(\S*)", line)
        if m:
            rows.append(m.groups())
    if not rows:
        sys.exit("no PERCPU-PROBE lines in the log: was the probe applied and the debug suite booted?")
    # The kernel reports each site once; a duplicate here means the log
    # holds two boots, and the tally must not count it twice.
    seen, unique = set(), []
    for row in rows:
        if row[1] not in seen:
            seen.add(row[1])
            unique.append(row)
    if len(unique) != len(rows):
        print(f"note: {len(rows) - len(unique)} duplicate site(s) in the log, counted once", file=sys.stderr)
    byfile = collections.Counter()
    unresolved = 0
    for what, ip, thread in unique:
        addr = hex(int(ip, 16) - 1)   # the call instruction, not the return address
        try:
            r = subprocess.run(["llvm-symbolizer", f"--obj={elf}", "-p", "-i", addr],
                               capture_output=True, text=True)
        except FileNotFoundError:
            sys.exit("llvm-symbolizer not on PATH (the swiftly toolchain has one)")
        if r.returncode != 0:
            sys.exit(f"llvm-symbolizer failed on {addr}: {r.stderr.strip()}")
        frames = [f.strip().replace("/cosmo/", "") for f in r.stdout.split("\n") if f.strip()]
        if not frames or frames[0].split(" at ")[-1].startswith("??"):
            # No file for it in the kernel's debug info (a symbol with no
            # line, a loaded module, a cold section): reported as such,
            # never as a file.
            unresolved += 1
            print(f"{what:9} UNRESOLVED {ip}  [{thread}]")
            continue
        inner = frames[0]
        outer = " <- ".join(f.split(" at ")[0].replace("(inlined by) ", "") for f in frames[1:])
        print(f"{what:9} {inner}" + (f"  (inlined into {outer})" if outer else "") + f"  [{thread}]")
        byfile[inner.split(" at ")[-1].split(":")[0]] += 1
    print(f"\n{len(unique)} sites ({unresolved} unresolved: not in the kernel's symbol table)")
    for f, n in byfile.most_common():
        print(f"{n:4} {f}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    a = sub.add_parser("apply", help="patch the working copy with the probe")
    a.add_argument("--arch", choices=sorted(ARCH), required=True)
    s = sub.add_parser("symbolize", help="turn a probed boot log into file:line sites")
    s.add_argument("log")
    s.add_argument("elf")
    args = ap.parse_args()
    if args.cmd == "apply":
        apply(args.arch)
    else:
        symbolize(args.log, args.elf)


if __name__ == "__main__":
    main()
