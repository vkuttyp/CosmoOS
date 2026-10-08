#!/usr/bin/env python3
"""Reproduce net-accept-race's census failure on demand, on any tree.

main's CI failed on the #326 merge (run 37679564267, x86-64, chaos
migrator) with every check of net-accept-race passing:

  selftest: net-accept-race left the network changed: ... sockets 0 -> 1

A network worker that wakes a socket holds a reference from under the
pcb lock until after the wake. A test that has put every socket of its
can return inside that window, and the runner's census counts the
socket the worker still holds.

--adversary makes the window wide and certain instead of rare: a worker
that has woken net-accept-race's listener sleeps 30 ms before its put
(thread_sleep_ms in tcp.c's sock_wake_after, for that one socket, until
the runner's census after the test). The test's last accept is woken by
that wake, so the test returns while the worker holds the listener.
--old reverts the fix commit (found by its subject, applied whole with
`git show | git apply -R`) on this tree, so net-census-wake-ref runs
against a census that does not wait.

  python3 tools/census-wake-ref-probe.py --tree before --adversary   # e33dd5a2, the #326 merge's first parent: no wake_one change
  python3 tools/census-wake-ref-probe.py --tree main --adversary     # 9917c700, main when this was written
  python3 tools/census-wake-ref-probe.py --tree this --adversary      # HEAD: the census waits
  python3 tools/census-wake-ref-probe.py --tree this --old           # HEAD without the fix: net-census-wake-ref fails
--arch aarch64 for the other architecture. Never edits the working
tree (the clone is of HEAD: commit first); logs under
out/census-wake-ref-<arch>-<tree>[-adversary][-old]/.
"""
import argparse
import importlib.util
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

TREES = {'before': 'e33dd5a2', 'main': '9917c700', 'this': None}
FIX_SUBJECT = 'selftest: the census waits out wake references'
LINES = [r'left the network changed', r'^SELFTEST: net-accept-race ', r'net-census-wake-ref',
         r'^SELFTEST: (PASS|FAIL)', r'^boot-test: (PASS|FAIL)']


def replace_once(path, before, after):
    source = path.read_text()
    if source.count(before) != 1:
        raise RuntimeError('probe anchor must occur exactly once in %s: %s' % (path, before[:80]))
    path.write_text(source.replace(before, after))


def add_adversary(work):
    decl = 'extern struct socket *volatile g_cwr_listener;'
    replace_once(work / 'kernel-services/network/tcp.c',
                 'static void sock_wake_after(struct socket *s)\n{\n    if (s) {\n        sock_wake(s);\n',
                 decl + '\nstruct socket *volatile g_cwr_listener;   /* PROBE */\n'
                 'static void sock_wake_after(struct socket *s)\n{\n    if (s) {\n        sock_wake(s);\n'
                 '        if (s == g_cwr_listener)\n            thread_sleep_ms(30);   /* PROBE: the put, late */\n')
    replace_once(work / 'kernel-services/network/nettest.c',
                 '    CHECK(ksock_bind(ls, &any) == 0 && ksock_listen(ls, 8) == 0);\n',
                 '    CHECK(ksock_bind(ls, &any) == 0 && ksock_listen(ls, 8) == 0);\n'
                 '    { ' + decl + ' g_cwr_listener = ls; }   /* PROBE */\n')
    replace_once(work / 'kernel/core/selftest.c',
                 '        nettest_census(&net_after);\n',
                 '        nettest_census(&net_after);\n'
                 '        { ' + decl + ' g_cwr_listener = NULL; }   /* PROBE */\n')


def revert_fix(work):
    sha = subprocess.run(['git', 'log', '--format=%H', '--fixed-strings', '--grep', FIX_SUBJECT, '-1'],
                         cwd=work, check=True, capture_output=True, text=True).stdout.strip()
    if not sha:
        raise RuntimeError('no commit with subject: ' + FIX_SUBJECT)
    diff = subprocess.run(['git', 'show', '--format=', sha], cwd=work, check=True, capture_output=True).stdout
    subprocess.run(['git', 'apply', '-R'], cwd=work, input=diff, check=True)
    print('probe: reverted %s (%s) whole' % (sha[:12], FIX_SUBJECT))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--arch', choices=['x86_64', 'aarch64'], default='x86_64')
    ap.add_argument('--tree', choices=sorted(TREES), default='this')
    ap.add_argument('--adversary', action='store_true', help="hold net-accept-race's listener 30 ms past its wake")
    ap.add_argument('--old', action='store_true', help='revert the fix commit (--tree this only)')
    ap.add_argument('--chaos', action='store_true', help='build with SCHED_CHAOS=1 and boot with --chaos')
    args = ap.parse_args()
    if args.old and args.tree != 'this':
        ap.error('--old applies to --tree this')
    root = Path(__file__).resolve().parents[1]
    tag = 'census-wake-ref-%s-%s%s%s%s' % (args.arch, args.tree, '-adversary' if args.adversary else '',
                                           '-old' if args.old else '', '-chaos' if args.chaos else '')
    out = root / 'out' / tag
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='cosmo-census-wake-ref-') as tmp:
        subprocess.run(['git', 'clone', '--quiet', '--shared', str(root), tmp], check=True)
        work = Path(tmp)
        if TREES[args.tree] is not None:
            subprocess.run(['git', 'checkout', '--quiet', TREES[args.tree]], cwd=work, check=True)
        if args.old:
            revert_fix(work)
        if args.adversary:
            add_adversary(work)
        make = ['gmake', '-j4', 'ARCH=' + args.arch, 'BUILD=debug', 'HAVE_MUSL=0', 'OUT=' + str(out)]
        if args.chaos:
            make.append('SCHED_CHAOS=1')
        with (out / 'build.log').open('w') as log:
            subprocess.run(make + ['image'], cwd=work, stdout=log, stderr=subprocess.STDOUT, check=True)
        os.environ.update(COSMO_ARCH=args.arch, QEMU_ARCH=args.arch)
        spec = importlib.util.spec_from_file_location('census_boot', root / 'tests/boot/run_boot_test.py')
        harness = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(harness)
        log_path = out / 'boot.log'
        sys.argv = ['census-wake-ref-probe', '--timeout', '240', '--image', str(out / 'cosmoos.img'),
                    '--log', str(log_path), '--kernel', str(out / 'kernel/kernel.elf')]
        if args.chaos:
            sys.argv.append('--chaos')
        rc = harness.main()
    print('--- %s: the census lines (%s) ---' % (tag, log_path))
    try:
        text = log_path.read_text(errors='replace')
    except OSError:
        text = ''
    for line in text.splitlines():
        if any(re.search(p, line) for p in LINES):
            print(line.rstrip())
    # The probe's verdict is the census's, read from the self-test lines: an
    # older tree's boot can fail the harness on this tree's markers (the
    # module ABI one) without that being the probe's question.
    accept_left = re.search(r'net-accept-race left the network changed', text) is not None
    accept_ok = re.search(r'^SELFTEST: net-accept-race +\.\.\. ok', text, re.M) is not None
    census_fail = re.search(r'^SELFTEST: net-census-wake-ref +\.\.\. FAIL', text, re.M) is not None
    census_ok = re.search(r'^SELFTEST: net-census-wake-ref +\.\.\. ok', text, re.M) is not None
    if not (accept_left or accept_ok):
        print('probe: net-accept-race did not run to a verdict (the boot stopped earlier)')
        return 1
    print('probe: net-accept-race %s; net-census-wake-ref %s' % (
        'LEFT A SOCKET (the CI failure)' if accept_left else 'ok',
        'FAILED' if census_fail else 'ok' if census_ok else 'absent (an older tree)'))
    # Each mode's expected verdict, as the exit status:
    #   --old                     net-census-wake-ref fails (the test is not vacuous)
    #   --adversary, older tree   net-accept-race leaves a socket (the CI failure, reproduced)
    #   --adversary, this tree    both tests pass (the fix holds against the adversary)
    #   neither, this tree        the harness's own verdict
    if args.old:
        ok = census_fail
    elif args.adversary and args.tree != 'this':
        ok = accept_left
    elif args.adversary:
        ok = accept_ok and census_ok
    else:
        return rc
    print('probe: %s' % ('the expected verdict' if ok else 'NOT the expected verdict'))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
