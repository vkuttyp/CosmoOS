#!/usr/bin/env python3
"""The two per-boot modes of `irqrestore-bench` on x86-64 under TCG.

docs/testing/flakes.md, "`irqrestore-bench` and `fpu-bench` read two
per-boot modes on x86-64". Builds two commits in throwaway worktrees
(never the working tree), each with `irqrestore-bench` replaced by a
diagnostic version, and boots them alternately so host state is shared
as evenly as two trees allow. The diagnostic prints one BENCHPROBE line:

  pair          the benchmark as built, a million save/restore pairs
  chunk ...     the same, timed per thousand pairs: min, median, max
  masked pair   the million pairs with interrupts masked around them all,
                so no tick and no preemption point can land inside
  control       four million steps of a dependent integer recurrence,
                masked -- no interrupt code at all, the host's speed
  cpu/irqs/ticks/switches   the CPU it ran on and what interrupted it

A per-boot mode that the pair and the masked pair share while the control
does not is in the save/restore path's execution, not in the host's speed
or the tick. The fpu-bench line from each boot is printed beside it.

    tools/irqrestore-bench-modes-probe.py --base c9292e3d --fix 30c7a2f8 --pairs 6

Prints one line per boot and PROBE: PASS only when every boot passed and
printed its measurement; a failed or unmeasured boot makes the exit
status 1. Logs stay under out/irqrestore-bench-modes/<label>-<n>.serial.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

BODY = r'''static uint32_t probe_chunks[1000];

static uint64_t probe_median(void)
{
    for (unsigned i = 1; i < 1000; i++) {
        uint32_t v = probe_chunks[i];
        unsigned j = i;
        while (j > 0 && probe_chunks[j - 1] > v) { probe_chunks[j] = probe_chunks[j - 1]; j--; }
        probe_chunks[j] = v;
    }
    return probe_chunks[500];
}

static volatile uint64_t probe_sink;

bool selftest_irqrestore_bench(const char **reason)
{
    (void)reason;
    enum { N = 1000000, C = 1000 };
    struct percpu *pc = raw_this_cpu();
    unsigned cpu0 = pc->cpu_id;
    uint64_t irq0 = pc->irq_count, tk0 = pc->ticks, sw0 = thread_current()->switches;

    /* A: the benchmark as built, in chunks of 1000 pairs */
    uint64_t t0 = clock_now_ns();
    for (unsigned j = 0; j < N / C; j++) {
        uint64_t c0 = clock_now_ns();
        for (unsigned i = 0; i < C; i++) {
            arch_irq_state_t s = arch_irq_save();
            arch_irq_restore(s);
        }
        probe_chunks[j] = (uint32_t)clock_since_ns(c0);
    }
    uint64_t dt = clock_since_ns(t0);
    uint64_t med = probe_median();
    uint64_t cmin = probe_chunks[0], cmax = probe_chunks[999];
    unsigned cpu1 = raw_cpu_id();
    struct percpu *pc1 = raw_this_cpu();
    uint64_t irqs = pc1 == pc ? pc->irq_count - irq0 : ~0ull;
    uint64_t tks = pc1 == pc ? pc->ticks - tk0 : ~0ull;
    uint64_t sws = thread_current()->switches - sw0;

    /* B: the same pairs with interrupts masked around them all */
    arch_irq_state_t o = arch_irq_save();
    uint64_t t1 = clock_now_ns();
    for (unsigned i = 0; i < N; i++) {
        arch_irq_state_t s = arch_irq_save();
        arch_irq_restore(s);
    }
    uint64_t dt_off = clock_since_ns(t1);

    /* C: control, no interrupt code at all: a dependent recurrence, masked */
    uint64_t x = 1;
    uint64_t t2 = clock_now_ns();
    for (unsigned i = 0; i < 4 * N; i++)
        x = x * 6364136223846793005ull + 1442695040888963407ull;
    uint64_t dt_ctl = clock_since_ns(t2);
    probe_sink = x;
    arch_irq_restore(o);

    kinfo("BENCHPROBE: pair %llu ns; chunk median %llu min %llu max %llu ns/1000; masked pair %llu ns; control %llu ns/4M; cpu %u->%u irqs %llu ticks %llu switches %llu",
          (unsigned long long)(dt / N), (unsigned long long)med, (unsigned long long)cmin,
          (unsigned long long)cmax, (unsigned long long)(dt_off / N), (unsigned long long)dt_ctl,
          cpu0, cpu1, (unsigned long long)irqs, (unsigned long long)tks, (unsigned long long)sws);
    return true;
}
'''


def patch_bench(tree):
    p = os.path.join(tree, 'kernel/scheduler/schedtest.c')
    s = open(p).read()
    start = s.index('bool selftest_irqrestore_bench(const char **reason)\n{')
    end = s.index('    return true;\n}\n', start) + len('    return true;\n}\n')
    open(p, 'w').write(s[:start] + BODY + s[end:])


def is_worktree(path):
    out = subprocess.run(['git', '-C', ROOT, 'worktree', 'list', '--porcelain'],
                         capture_output=True, text=True).stdout
    return f'worktree {os.path.realpath(path)}\n' in out


def worktree(work, label, commit):
    tree = os.path.join(work, label)
    if os.path.exists(tree):
        if not is_worktree(tree):
            sys.exit(f'{tree} exists and is not a worktree of this probe; remove it yourself')
        subprocess.run(['git', '-C', ROOT, 'worktree', 'remove', '--force', tree], check=False)
        shutil.rmtree(tree, ignore_errors=True)
    subprocess.run(['git', '-C', ROOT, 'worktree', 'add', '--detach', tree, commit], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return tree


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--arch', default='x86_64', choices=['x86_64', 'aarch64'])
    ap.add_argument('--base', required=True, help='commit for the first tree')
    ap.add_argument('--fix', required=True, help='commit for the second tree')
    ap.add_argument('--pairs', type=int, default=6)
    ap.add_argument('--smp', default='4')
    args = ap.parse_args()

    work = os.path.join(ROOT, 'out', 'irqrestore-bench-modes')
    os.makedirs(work, exist_ok=True)
    trees = {}
    try:
        for label, commit in (('base', args.base), ('fix', args.fix)):
            trees[label] = worktree(work, label, commit)
            patch_bench(trees[label])
            with open(os.path.join(work, f'{label}.build.log'), 'w') as f:
                if subprocess.run(['gmake', '-C', trees[label], '-j6', 'ARCH=' + args.arch, 'image'],
                                  stdout=f, stderr=subprocess.STDOUT).returncode != 0:
                    print(f'PROBE: FAIL (build of {label} failed; {f.name})')
                    return 1
        failed = 0
        for i in range(1, args.pairs + 1):
            for label in ('base', 'fix'):
                log = os.path.join(work, f'{label}-{i}.serial')
                with open(os.path.join(work, f'{label}-{i}.result'), 'w') as f:
                    subprocess.run(['gmake', '-C', trees[label], 'ARCH=' + args.arch, 'QEMU_SMP=' + args.smp,
                                    'BOOT_LOG=' + log, 'test'], stdout=f, stderr=subprocess.STDOUT)
                    f.seek(0)
                res = open(f.name, errors='replace').read()
                verdict = re.findall(r'boot-test: (PASS|FAIL)', res)
                serial = open(log, errors='replace').read() if os.path.exists(log) else ''
                probe = next((l.split('BENCHPROBE: ', 1)[1] for l in serial.splitlines() if 'BENCHPROBE: ' in l), '(no BENCHPROBE line)')
                fpu = next((l.split('fpu-bench: ', 1)[1] for l in serial.splitlines() if 'selftest: fpu-bench: ' in l), '')
                ok = bool(verdict) and verdict[-1] == 'PASS' and probe.startswith('pair ')
                failed += not ok
                print(f'{label} {i} {verdict[-1] if verdict else "NO VERDICT"} | {probe} | {fpu}', flush=True)
        print(f'PROBE: {"PASS" if failed == 0 else "FAIL"} ({2 * args.pairs - failed} of {2 * args.pairs} boots passed with a measurement)')
        return 0 if failed == 0 else 1
    finally:
        for tree in trees.values():
            subprocess.run(['git', '-C', ROOT, 'worktree', 'remove', '--force', tree], check=False)


if __name__ == '__main__':
    sys.exit(main())
