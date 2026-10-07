#!/usr/bin/env python3
"""Boot the suite with a network fix taken out, so its regression test is seen to fail.

  python3 tools/net-fuzz-probe.py --arch x86_64 --old fin-output
      the FIN branch of tcp_input builds a bare acknowledgement instead of
      running the output (the tree before the net-fuzz unit's fix):
      `net-fin-acks-last-data` must FAIL at its stream check, nothing else
  python3 tools/net-fuzz-probe.py --arch x86_64
      the clone as committed: everything must PASS

Clones HEAD (commit first: the working tree is never edited), applies the
mutation to the clone, builds the debug image into out/net-fuzz-<arch>-<mode>/
and boots it with the standard harness. The boot's verdict is the harness's:
with --old it is expected to say FAIL on the one forbidden marker, and the
probe's own verdict -- PASS when the expected test and only it failed -- is the
last line printed.
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

MODES = {
    'fin-output': ('kernel-services/network/tcp.c',
                   '''        pcb->delack_pending = false;
        timer_cancel(&pcb->delack);
        pcb->ack_now = true;
        tcp_output_locked(pcb, &b);
    } else if (pcb->state != TCP_CLOSED && pcb->state != TCP_TIME_WAIT) {''',
                   '''        pcb->delack_pending = false;
        build_segment(pcb, &b, TH_ACK, pcb->snd_nxt, 0, false);
        timer_cancel(&pcb->delack);
    } else if (pcb->state != TCP_CLOSED && pcb->state != TCP_TIME_WAIT) {''',
                   'net-fin-acks-last-data'),
}


def replace_once(source, before, after):
    if source.count(before) != 1:
        raise RuntimeError('probe anchor must occur exactly once: ' + before[:60])
    return source.replace(before, after)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--arch', choices=['x86_64', 'aarch64'], default='x86_64')
    ap.add_argument('--old', choices=sorted(MODES), default=None, help='the fix to take out')
    args = ap.parse_args()
    root = Path(__file__).resolve().parents[1]
    tag = 'net-fuzz-' + args.arch + '-' + (args.old or 'fixed')
    out = root / 'out' / tag
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='cosmo-net-fuzz-') as tmp:
        subprocess.run(['git', 'clone', '--quiet', '--shared', str(root), tmp], check=True)
        work = Path(tmp)
        expected_fail = None
        if args.old:
            path, before, after, expected_fail = MODES[args.old]
            f = work / path
            f.write_text(replace_once(f.read_text(), before, after))
        make = ['gmake', '-j4', 'ARCH=' + args.arch, 'BUILD=debug', 'HAVE_MUSL=0', 'OUT=' + str(out)]
        with (out / 'build.log').open('w') as log:
            subprocess.run(make + ['image'], cwd=work, stdout=log, stderr=subprocess.STDOUT, check=True)
        os.environ.update(COSMO_ARCH=args.arch, QEMU_ARCH=args.arch, QEMU_SMP='4')
        spec = importlib.util.spec_from_file_location('net_fuzz_boot', root / 'tests/boot/run_boot_test.py')
        harness = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(harness)
        timeout = '240' if args.arch == 'aarch64' else '180'
        log_path = out / 'boot.log'
        sys.argv = ['net-fuzz-probe', '--timeout', timeout, '--image', str(out / 'cosmoos.img'), '--log', str(log_path),
                    '--kernel', str(out / 'kernel/kernel.elf')]
        rc = harness.main()
    text = log_path.read_text(errors='replace') if log_path.exists() else ''
    failed = re.findall(r'^SELFTEST: (\S+) +\.\.\. FAIL', text, re.M)
    for line in text.splitlines():
        if re.search(r'^SELFTEST: \S+ +\.\.\. FAIL|^SELFTEST: (PASS|FAIL)|^boot-test: (PASS|FAIL)', line):
            print(line.rstrip())
    if expected_fail is None:
        ok = rc == 0 and not failed
    else:
        ok = failed == [expected_fail]
    print('--- %s: %s' % (tag, 'PASS' if ok else 'FAIL (harness rc %d, failed tests %s)' % (rc, failed or 'none')))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
