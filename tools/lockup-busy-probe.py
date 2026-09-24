#!/usr/bin/env python3
"""
lockup-busy-probe.py -- what does lockup-sample-busy's time bound measure?

`lockup-sample-busy` (kernel/core/lockuptest.c) has, in its three-CPU
part, two target CPUs spinning with interrupts masked; it samples every
CPU once and requires

    el < LOCKUP_SAMPLE_TIMEOUT_NS + 2 ms          (5 ms + 2 ms)

to show "the bound is total, not per target": two CPUs that cannot
answer cost one 5 ms timeout together, where a per-target wait would
cost ten. It has failed with el = 88, 101 and 241 ms
(`docs/testing/flakes.md`, lockup-sample) -- far past the ten a
per-target wait would take -- on trees that do not touch the sampler.

This probe asks where those milliseconds go. It adds, for the probe
only, a record in `lockup_sample_all`'s wait loop of the largest gap
between two consecutive clock reads (with preemption off, a large gap is
time the CPU itself did not run -- a host descheduling the virtual CPU),
and replaces the three-CPU part's single sample with repeated samples of
the same shape, printing each one that exceeds the bound and a summary:

    LBPROBE over: el E us, largest gap G us, el minus gap R us (arch)
    LBPROBE summary: S samples, over the bound O, el min/median/max A/B/C us,
      largest gap max G us, targets answered by nmi N (arch)

`el minus gap` is the time the sampler spent when it was running: about
5000 us where the targets cannot answer (they time out), about 0 where
they can (x86-64 answers through NMI even with interrupts masked). The
test's own checks are left out for the probe; the boot runs on.

Run it quiet and under host load (the adversary: more busy processes on
the host than it has cores, which steals time from the guest's vCPUs):

    python3 tools/lockup-busy-probe.py apply
    gmake ARCH=aarch64 test; grep LBPROBE out/aarch64-debug/boot-test.log
    python3 tools/lockup-busy-probe.py load 12 -- gmake ARCH=aarch64 test
    python3 tools/lockup-busy-probe.py revert

`apply` refuses a file with uncommitted changes and edits nothing if an
anchor is missing, restoring every file on a failure part-way; `revert`
checks every file's hash first (accepting one an earlier attempt already
restored), restores all from copies, and removes the backups only once
every file is restored. `load N -- CMD` runs CMD with N host busy loops
running beside it and kills them when CMD exits, however it exits.
"""

import hashlib
import os
import shutil
import signal
import subprocess
import sys

SAMPLER = 'kernel/core/lockup.c'
TEST = 'kernel/core/lockuptest.c'
BACKUP = '.lockup-busy-probe.orig'
STAMP = '.lockup-busy-probe.applied'

# The sampler: remember the largest gap between clock reads in the wait.
S_ANCHOR_DECL = 'bool lockup_sample_cpu(unsigned cpu, uint64_t timeout_ns, struct cpu_sample *out)\n'
S_ANCHOR_LOOP = '''    uint64_t deadline = clock_now_ns() + timeout_ns;
    cpumask_t got = 0;
    for (;;) {
        for (unsigned c = 0; c < cpu_count(); c++) {
            if ((targets & CPUMASK_OF(c)) && !(got & CPUMASK_OF(c)) &&
                __atomic_load_n(&percpu_get(c)->sample.seq, __ATOMIC_ACQUIRE) == seq)
                got |= CPUMASK_OF(c);
        }
        if (got == targets || clock_now_ns() >= deadline)
            break;
        arch_cpu_relax();
    }
'''
S_PROBE_LOOP = '''    uint64_t deadline = clock_now_ns() + timeout_ns;
    cpumask_t got = 0;
    uint64_t lbp_prev = clock_now_ns(), lbp_gap = 0;   /* LBPROBE */
    for (;;) {
        for (unsigned c = 0; c < cpu_count(); c++) {
            if ((targets & CPUMASK_OF(c)) && !(got & CPUMASK_OF(c)) &&
                __atomic_load_n(&percpu_get(c)->sample.seq, __ATOMIC_ACQUIRE) == seq)
                got |= CPUMASK_OF(c);
        }
        uint64_t lbp_now = clock_now_ns();
        if (lbp_now - lbp_prev > lbp_gap)
            lbp_gap = lbp_now - lbp_prev;
        lbp_prev = lbp_now;
        if (got == targets || lbp_now >= deadline)
            break;
        arch_cpu_relax();
    }
    __atomic_store_n(&g_lbprobe_gap_ns, lbp_gap, __ATOMIC_RELAXED);
'''
S_PROBE_DECL = '''uint64_t g_lbprobe_gap_ns;   /* LBPROBE (tools/lockup-busy-probe.py; not for merge) */

''' + S_ANCHOR_DECL

# The test: repeated three-CPU samples instead of one, with no checks.
T_ANCHOR = '''        uint64_t t0 = clock_now_ns();
        cpumask_t m = 0;
        bool ok = lockup_sample_all(NULL, LOCKUP_SAMPLE_TIMEOUT_NS, &m);
        uint64_t el = clock_since_ns(t0);
        if (ok)
            lockup_print_samples(m);
'''
T_PROBE = '''        /* --- LBPROBE (tools/lockup-busy-probe.py; not for merge) --- */
#if defined(__aarch64__)
#define LBP_ARCH "aarch64"
#else
#define LBP_ARCH "x86_64"
#endif
        extern uint64_t g_lbprobe_gap_ns;
        enum { LBP_N = 200 };
        static uint64_t lbp_el[LBP_N];
        unsigned lbp_over = 0, lbp_nmi = 0, lbp_n = 0;
        uint64_t lbp_maxgap = 0, lbp_start = clock_now_ns();
        bool ok = true;
        uint64_t el = 0;
        while (lbp_n < LBP_N && clock_now_ns() - lbp_start < 3000000000ull) {
            uint64_t t0 = clock_now_ns();
            cpumask_t m = 0;
            ok = lockup_sample_all(NULL, LOCKUP_SAMPLE_TIMEOUT_NS, &m) && ok;
            el = clock_since_ns(t0);
            uint64_t gap = __atomic_load_n(&g_lbprobe_gap_ns, __ATOMIC_RELAXED);
            if ((m & CPUMASK_OF(a)) && (m & CPUMASK_OF(b)))
                lbp_nmi++;   /* both masked targets answered: through NMI */
            if (gap > lbp_maxgap)
                lbp_maxgap = gap;
            if (el >= LOCKUP_SAMPLE_TIMEOUT_NS + 2 * 1000 * 1000) {
                lbp_over++;
                kinfo("LBPROBE over: el %llu us, largest gap %llu us, el minus gap %lld us (%s)",
                      (unsigned long long)(el / 1000), (unsigned long long)(gap / 1000),
                      (long long)((int64_t)el - (int64_t)gap) / 1000, LBP_ARCH);
            }
            lbp_el[lbp_n++] = el;
        }
        for (unsigned i = 1; i < lbp_n; i++)   /* insertion sort: a few hundred */
            for (unsigned j = i; j > 0 && lbp_el[j - 1] > lbp_el[j]; j--) {
                uint64_t x = lbp_el[j];
                lbp_el[j] = lbp_el[j - 1];
                lbp_el[j - 1] = x;
            }
        kinfo("LBPROBE summary: %u samples, over the bound %u, el min/median/max %llu/%llu/%llu us, "
              "largest gap max %llu us, targets answered by nmi %u (%s)",
              lbp_n, lbp_over, (unsigned long long)(lbp_el[0] / 1000),
              (unsigned long long)(lbp_el[lbp_n / 2] / 1000), (unsigned long long)(lbp_el[lbp_n - 1] / 1000),
              (unsigned long long)(lbp_maxgap / 1000), lbp_nmi, LBP_ARCH);
        el = 0;   /* the probe reports; the test's check is not the question */
'''

FILES = [
    (SAMPLER, [(S_ANCHOR_DECL, S_PROBE_DECL), (S_ANCHOR_LOOP, S_PROBE_LOOP)]),
    (TEST, [(T_ANCHOR, T_PROBE)]),
]


def sha(p):
    return hashlib.sha256(open(p, 'rb').read()).hexdigest()


def apply():
    if os.path.exists(STAMP):
        sys.exit('already applied')
    for path, edits in FILES:
        if subprocess.run(['git', 'status', '--porcelain', '--', path], capture_output=True, text=True).stdout.strip():
            sys.exit(f'{path} has uncommitted changes')
        s = open(path).read()
        for a, _ in edits:
            if s.count(a) != 1:
                sys.exit(f'{path}: anchor not found exactly once: {a[:50]!r}')
    done, stamp = [], []
    try:
        for path, edits in FILES:
            shutil.copyfile(path, path + BACKUP)
            done.append(path)
            s = open(path).read()
            for a, b in edits:
                s = s.replace(a, b)
            open(path, 'w').write(s)
            stamp.append(f'{path} {sha(path)}')
        open(STAMP, 'w').write('\n'.join(stamp) + '\n')
    except BaseException:
        for path in done:
            shutil.move(path + BACKUP, path)
            os.utime(path, None)
        if os.path.exists(STAMP):
            os.remove(STAMP)
        raise
    print('applied')


def revert():
    if not os.path.exists(STAMP):
        sys.exit('not applied')
    for line in open(STAMP).read().split('\n'):
        if line:
            path, digest = line.split()
            restored = os.path.exists(path + BACKUP) and sha(path) == sha(path + BACKUP)
            if sha(path) != digest and not restored:
                sys.exit(f'{path} changed since apply; restore by hand from {path + BACKUP}')
    for path, _ in FILES:
        shutil.copyfile(path + BACKUP, path)
        os.utime(path, None)
    for path, _ in FILES:
        os.remove(path + BACKUP)
    os.remove(STAMP)
    print('reverted')


def load():
    # load N -- CMD...: N busy loops on the host while CMD runs.
    args = sys.argv[2:]
    if len(args) < 3 or args[1] != '--':
        sys.exit('usage: load N -- CMD...')
    n = int(args[0])
    busy = [subprocess.Popen(['sh', '-c', 'while :; do :; done'], start_new_session=True) for _ in range(n)]
    try:
        rc = subprocess.run(args[2:]).returncode
    finally:
        for p in busy:
            try:
                os.killpg(p.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            p.wait()
    sys.exit(rc)


if __name__ == '__main__':
    {'apply': apply, 'revert': revert, 'load': load}.get(sys.argv[1] if len(sys.argv) > 1 else '',
                                                           lambda: sys.exit(__doc__))()
