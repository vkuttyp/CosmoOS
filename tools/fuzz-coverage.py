#!/usr/bin/env python3
"""Line coverage of the network stack by a fuzz target, by file.

Builds the target with clang's source-based coverage into its own output
tree (FUZZ_EXTRA_CFLAGS, tests/fuzz/fuzz.mk), runs it as `make fuzz` does
(seeds, the checked-in corpus where the target has one, then N mutations)
with a profile per process, merges the profiles and reports the lines of
kernel-services/network/*.c the run executed.

  python3 tools/fuzz-coverage.py fuzz_net_frame                    # 20000 mutations, seed 1
  python3 tools/fuzz-coverage.py fuzz_net_frame --runs 100000 --seed 2
  python3 tools/fuzz-coverage.py fuzz_net_frame fuzz_net_config --runs 0   # seeds and corpus only

With several targets the profiles are merged: the union of what they
reach. Output: one line per file (lines covered, lines, percent) and the
total. The output tree is out/fuzz-coverage/.
"""
import argparse
import glob
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

CORPORA = {'fuzz_net_frame': 'tests/fuzz/corpus/net_frame'}
COV = ['-fprofile-instr-generate', '-fcoverage-mapping']


def tool(name):
    return subprocess.run(['xcrun', '--find', name], check=True, capture_output=True, text=True).stdout.strip() \
        if sys.platform == 'darwin' else name


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('targets', nargs='+')
    ap.add_argument('--runs', type=int, default=20000)
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--files', default='kernel-services/network/', help='report files whose path contains this')
    args = ap.parse_args()
    root = Path(__file__).resolve().parents[1]
    out = root / 'out' / 'fuzz-coverage'
    prof = out / 'profiles'
    shutil.rmtree(prof, ignore_errors=True)
    prof.mkdir(parents=True, exist_ok=True)
    make = ['gmake', '--no-print-directory', 'OUT=' + str(out), 'FUZZ_EXTRA_CFLAGS=' + ' '.join(COV),
            'FUZZ_EXTRA_LDFLAGS=' + COV[0]]
    bins = [str(out / 'fuzz' / t) for t in args.targets]
    subprocess.run(make + bins, cwd=root, check=True, stdout=subprocess.DEVNULL)
    env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0', LLVM_PROFILE_FILE=str(prof / '%p.profraw'))
    for t, b in zip(args.targets, bins):
        cmd = [b, '-runs', str(args.runs), '-seed', str(args.seed), '-out', str(out / 'fuzz')]
        if t in CORPORA:
            cmd.append(str(root / CORPORA[t]))
        r = subprocess.run(cmd, env=env, capture_output=True, text=True)
        print('%s: %s' % (t, (r.stdout + r.stderr).strip().splitlines()[-1] if (r.stdout + r.stderr).strip() else r.returncode))
        if r.returncode != 0:
            return r.returncode
    merged = out / 'merged.profdata'
    subprocess.run([tool('llvm-profdata'), 'merge', '-sparse', '-o', str(merged)] + glob.glob(str(prof / '*.profraw')),
                   check=True)
    objs = [bins[0]] + sum((['-object', b] for b in bins[1:]), [])
    rep = subprocess.run([tool('llvm-cov'), 'report', '-instr-profile', str(merged)] + objs,
                         check=True, capture_output=True, text=True).stdout
    # Columns: Filename, Regions, Missed, Cover, Functions, Missed, Executed, Lines, Missed, Cover, ...
    total_l = total_c = 0
    rows = []
    for line in rep.splitlines():
        cols = line.split()
        if len(cols) < 10 or args.files not in cols[0] or not cols[0].endswith('.c'):
            continue
        lines, missed = int(cols[7]), int(cols[8])
        rows.append((cols[0].split(args.files)[-1], lines - missed, lines))
        total_l += lines
        total_c += lines - missed
    for name, c, n in sorted(rows):
        print('  %-12s %5d / %5d  %5.1f %%' % (name, c, n, 100.0 * c / n if n else 0))
    print('  %-12s %5d / %5d  %5.1f %%' % ('total', total_c, total_l, 100.0 * total_c / total_l if total_l else 0))
    return 0


if __name__ == '__main__':
    sys.exit(main())
