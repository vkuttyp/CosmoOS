#!/usr/bin/env python3
"""Boot the write-write-read measurement with the delayed ACK as it is, or as it was before b76e4536.

PR #322 (b76e4536, 2026-10-07) made the delayed acknowledgement fire for the
first time: an odd in-order segment is acknowledged by the next segment, by
the application's read, or by the 40 ms timer, where before it was
acknowledged at once on the receive path's own output. A peer with Nagle on
holds its second small write until the first is acknowledged, so the question
is whether the change costs such a peer up to 40 ms per request
(docs/audit/2026-10-07-delack-nagle-report.md).

This probe builds a temporary clone -- as committed, or with `--old`: the
commit's whole change to tcp.c reverse-applied (`git show b76e4536 -- tcp.c |
git apply -R`), which is exactly the rule before it; a partial revert is
not, since the handshake's and the every-second-segment acknowledgements
moved from `delack_pending` to `ack_now` in the same commit and a tree with
half of each never sends them -- and runs the standard boot test on it, whose network harness now measures the exchange
both ways (tests/boot/nettest.py, WWR_*) and whose `net-tcp-nagle-peer` builds
the Nagle peer the harness cannot (QEMU's user-mode backend has no Nagle).
It then prints the measurement lines from the serial log:
  python3 tools/delack-nagle-probe.py --arch x86_64         # as committed: must PASS
  python3 tools/delack-nagle-probe.py --arch x86_64 --old   # before: `net-tcp-delack` FAILs (the timer never fires), the latencies print
AArch64 is the same with --arch aarch64. Four CPUs; never edits the working
tree (the clone is of HEAD: commit first); logs under
out/delack-nagle-<arch>-{old,fixed}/.
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


DELACK_COMMIT = 'b76e4536'   # tcp: the delayed acknowledgement fires (PR #322)

LINES = [r'^NETTEST: wwr ', r'selftest: net-tcp-nagle-peer', r'^SELFTEST: net-tcp-(delack|nagle-peer) ',
         r'^network harness: write-write-read', r'^boot-test: (PASS|FAIL)']


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--arch', choices=['x86_64', 'aarch64'], default='x86_64')
    ap.add_argument('--old', action='store_true', help='the delayed ACK as before b76e4536: acknowledged on any output')
    args = ap.parse_args()
    root = Path(__file__).resolve().parents[1]
    tag = 'delack-nagle-' + args.arch + ('-old' if args.old else '-fixed')
    out = root / 'out' / tag
    shutil.rmtree(out, ignore_errors=True)   # a previous run's dependency files name a temporary clone that is gone
    out.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix='cosmo-delack-nagle-') as tmp:
        subprocess.run(['git', 'clone', '--quiet', '--shared', str(root), tmp], check=True)
        work = Path(tmp)
        if args.old:
            # The whole of the commit's tcp.c change, reversed: `git apply -R`
            # refuses (and this probe stops) if tcp.c has moved on since, so
            # the old rule is never approximated.
            patch = subprocess.run(['git', 'show', DELACK_COMMIT, '--', 'kernel-services/network/tcp.c'],
                                   cwd=work, check=True, capture_output=True).stdout
            subprocess.run(['git', 'apply', '-R'], cwd=work, input=patch, check=True)
        with (out / 'build.log').open('w') as log:
            subprocess.run(['gmake', '-j4', 'ARCH=' + args.arch, 'BUILD=debug', 'HAVE_MUSL=0',
                            'OUT=' + str(out), 'image'], cwd=work, stdout=log, stderr=subprocess.STDOUT, check=True)
        # Before the import: the harness reads COSMO_ARCH at import time, and
        # this probe runs it in its normal mode, where the architecture's own
        # markers are required (the panic-mode probes skip them).
        os.environ.update(COSMO_ARCH=args.arch, QEMU_ARCH=args.arch, QEMU_SMP='4')
        spec = importlib.util.spec_from_file_location('delack_boot', root / 'tests/boot/run_boot_test.py')
        harness = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(harness)
        timeout = '240' if args.arch == 'aarch64' else '180'
        log_path = out / 'boot.log'
        sys.argv = ['delack-nagle-probe', '--timeout', timeout, '--image', str(out / 'cosmoos.img'),
                    '--log', str(log_path), '--kernel', str(out / 'kernel/kernel.elf')]
        rc = harness.main()

    print('--- %s: the measurement lines (%s) ---' % (tag, log_path))
    try:
        text = log_path.read_text(errors='replace')
    except OSError:
        text = ''
    for line in text.splitlines():
        if any(re.search(p, line) for p in LINES):
            print(line.rstrip())
    if args.old:
        print('(--old: `net-tcp-delack` must FAIL -- its timer cannot fire under the old rule -- and nothing else)')
    return rc


if __name__ == '__main__':
    sys.exit(main())
