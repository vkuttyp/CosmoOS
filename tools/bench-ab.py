#!/usr/bin/env python3
"""Compare the boot benchmarks of two revisions, booted alternately.

Builds a debug image of each revision in its own clone (never the working
tree: commit first), then boots them in turn -- after, before, after,
before ... -- so a host that drifts between boots drifts under both. Every
boot is a full self-test boot through tests/boot/run_boot_test.py at the
priority this script runs at (start it in the foreground or as a tool's
background job, never with zsh's `&`, which runs QEMU at nice 5).

From each boot it reads the reports-only benchmark lines:
  net-bench       tcp 1 flow / 2 flows MiB/s, udp sends/s, per steering round
  net-nicbench    arp round trips/s and udp sends/s, per interface
  blk-bench       MiB/s or req/s per disk, direction and bio size
and prints, per metric, every boot's value and each side's median.

  python3 tools/bench-ab.py --arch x86_64 --before origin/main --pairs 4
  python3 tools/bench-ab.py --arch aarch64 --before origin/main --after HEAD --only net
  python3 tools/bench-ab.py --arch x86_64 --before HEAD~1 --patch-after budget-off.patch

--patch-before / --patch-after apply a patch to that side's clone (a probe
that removes something, measured against the tree that has it).
LOCKDEP follows the debug default (1) unless --lockdep 0; the summary says
which, since lockdep halves every debug benchmark (docs/testing/flakes.md).
Logs under out/bench-ab-<arch>/.
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import statistics
import subprocess
import sys
import tempfile

PATTERNS = {
    'net': [
        (re.compile(r'net-bench: steer=(\d+) cpus=\d+: tcp 1 flow (\d+) MiB/s, 2 flows (\d+) MiB/s total, udp (\d+) sends/s'),
         lambda m: {'net-bench steer=%s tcp1 MiB/s' % m[1]: int(m[2]),
                    'net-bench steer=%s tcp2 MiB/s' % m[1]: int(m[3]),
                    'net-bench steer=%s udp sends/s' % m[1]: int(m[4])}),
        (re.compile(r'net-nicbench: (\S+) \(caps 0x[0-9a-f]+\): arp (\d+) rt/s .*?; udp (\d+) sends/s'),
         lambda m: {'nicbench %s arp rt/s' % m[1]: int(m[2]), 'nicbench %s udp sends/s' % m[1]: int(m[3])}),
    ],
    'blk': [
        (re.compile(r'blk-bench: (\S+): (read|write) (\d+) KiB bios: \d+ requests in \d+ ms = (\d+) MiB/s'),
         lambda m: {'blk %s %s %sK MiB/s' % (m[1], m[2], m[3]): int(m[4])}),
        (re.compile(r'blk-bench: (\S+): (\d+) threads reading 4 KiB bios at once: \d+ requests in \d+ ms = (\d+) req/s'),
         lambda m: {'blk %s %s-thread 4K req/s' % (m[1], m[2]): int(m[3])}),
    ],
}


def resolve(root, rev):
    """A revision as a commit id, named in the source repository: a clone's
    `origin/main` is the source's local main, not its upstream."""
    return subprocess.run(['git', 'rev-parse', '--verify', rev + '^{commit}'], cwd=root, check=True,
                          capture_output=True, text=True).stdout.strip()


def build(root, sha, patch, out, arch, lockdep):
    """Clone, check out `sha`, apply `patch`, build; the clone's path. A
    failure removes the clone before it propagates."""
    tmp = tempfile.mkdtemp(prefix='cosmo-bench-ab-')
    try:
        subprocess.run(['git', 'clone', '--quiet', '--shared', str(root), tmp], check=True)
        subprocess.run(['git', 'checkout', '--quiet', sha], cwd=tmp, check=True)
        if patch:
            subprocess.run(['git', 'apply', str(Path(patch).resolve())], cwd=tmp, check=True)
        out.mkdir(parents=True, exist_ok=True)
        make = ['gmake', '-j8', 'ARCH=' + arch, 'BUILD=debug', 'HAVE_MUSL=0', 'LOCKDEP=%d' % lockdep,
                'OUT=' + str(out)]
        with (out / 'build.log').open('w') as log:
            subprocess.run(make + ['image'], cwd=tmp, stdout=log, stderr=subprocess.STDOUT, check=True)
    except BaseException:
        shutil.rmtree(tmp, ignore_errors=True)
        raise
    return Path(tmp)


def boot(clone, out, log_path, timeout, arch):
    """One boot through the side's own harness, in its own interpreter: its
    markers and its helper modules are that tree's (a module ABI bump on one
    side fails the other's boots by a marker otherwise, and helpers imported
    by name would be the first side's in a shared interpreter). The harness
    is told what the image was built with (HAVE_MUSL=0)."""
    env = dict(os.environ, COSMO_ARCH=arch, QEMU_ARCH=arch, HAVE_MUSL='0')
    cmd = [sys.executable, str(clone / 'tests/boot/run_boot_test.py'), '--timeout', str(timeout), '--image',
           str(out / 'cosmoos.img'), '--log', str(log_path), '--kernel', str(out / 'kernel/kernel.elf')]
    return subprocess.run(cmd, env=env).returncode


def metrics(text, groups):
    got = {}
    for g in groups:
        for rx, fn in PATTERNS[g]:
            for m in rx.finditer(text):
                got.update(fn(m))
    return got


def positive_int(v):
    n = int(v)
    if n < 1:
        raise argparse.ArgumentTypeError('must be 1 or more')
    return n


def positive_seconds(v):
    x = float(v)
    if not (x > 0 and x < float('inf')):
        raise argparse.ArgumentTypeError('must be a positive number of seconds')
    return x


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--arch', choices=['x86_64', 'aarch64'], default='x86_64')
    ap.add_argument('--before', required=True, help='revision measured as "before"')
    ap.add_argument('--after', default='HEAD', help='revision measured as "after" (default HEAD)')
    ap.add_argument('--patch-before')
    ap.add_argument('--patch-after')
    ap.add_argument('--pairs', type=positive_int, default=3, help='boots of each side, alternated')
    ap.add_argument('--only', choices=sorted(PATTERNS), action='append', help='metric groups (default all)')
    ap.add_argument('--lockdep', type=int, choices=[0, 1], default=1)
    ap.add_argument('--timeout', type=positive_seconds, default=300.0)
    args = ap.parse_args()
    groups = args.only or sorted(PATTERNS)
    root = Path(__file__).resolve().parents[1]
    base = root / 'out' / ('bench-ab-' + args.arch)
    shutil.rmtree(base, ignore_errors=True)
    sides = {}
    try:
        for name, rev, patch in (('before', args.before, args.patch_before), ('after', args.after, args.patch_after)):
            sha = resolve(root, rev)
            out = base / name
            clone = build(root, sha, patch, out, args.arch, args.lockdep)
            sides[name] = {'out': out, 'clone': clone, 'sha': sha[:8] + (' +patch' if patch else ''), 'runs': [],
                           'verdicts': []}
            print('bench-ab: built %s = %s (%s)' % (name, sides[name]['sha'], rev), flush=True)
        return run(args, groups, sides)
    finally:
        for s in sides.values():
            shutil.rmtree(s['clone'], ignore_errors=True)


def run(args, groups, sides):
    for i in range(args.pairs):
        for name in ('after', 'before'):
            s = sides[name]
            log_path = s['out'] / ('boot-%d.log' % i)
            rc = boot(s['clone'], s['out'], log_path, args.timeout, args.arch)
            try:
                text = log_path.read_text(errors='replace')
            except OSError:
                text = ''
            s['runs'].append(metrics(text, groups))
            s['verdicts'].append(rc)
            print('bench-ab: %s boot %d: harness rc %s, %d metrics' % (name, i, rc, len(s['runs'][-1])), flush=True)
    keys = sorted(set().union(*[r.keys() for s in sides.values() for r in s['runs']]))
    print('\nbench-ab: %s, debug, LOCKDEP=%d, before %s, after %s, %d boots each, alternated' % (
        args.arch, args.lockdep, sides['before']['sha'], sides['after']['sha'], args.pairs))
    print('%-40s %10s %10s %7s   before runs | after runs' % ('metric', 'before', 'after', 'change'))
    missing = []
    for k in keys:
        b = [r[k] for r in sides['before']['runs'] if k in r]
        a = [r[k] for r in sides['after']['runs'] if k in r]
        if len(b) != args.pairs or len(a) != args.pairs:
            missing.append('%s (before %d of %d boots, after %d)' % (k, len(b), args.pairs, len(a)))
        mb = statistics.median(b) if b else None
        ma = statistics.median(a) if a else None
        ch = '%+.1f%%' % ((ma - mb) * 100.0 / mb) if mb and ma is not None else '-'
        print('%-40s %10s %10s %7s   %s | %s' % (k, mb, ma, ch, ' '.join(map(str, b)), ' '.join(map(str, a))))
    print('harness verdicts: before %s, after %s' % (sides['before']['verdicts'], sides['after']['verdicts']))
    # A comparison that could not be made is a failure, not a short table:
    # no metric at all, or a metric some boot did not report.
    if not keys:
        print('bench-ab: no benchmark line matched in any boot (groups %s)' % ', '.join(groups))
        return 1
    for m in missing:
        print('bench-ab: missing samples: ' + m)
    ok = not missing and all(v == 0 for s in sides.values() for v in s['verdicts'])
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
