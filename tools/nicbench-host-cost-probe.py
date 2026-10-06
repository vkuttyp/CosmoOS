#!/usr/bin/env python3
"""Show what sets net-nicbench's UDP rate, and that the test's time no longer depends on it.

The benchmark's UDP phase sends 1 KiB datagrams to the gateway. Under
QEMU's user-mode network every one of them is a `sendto` on an
unconnected host socket; an ARP round trip is answered inside QEMU with
no system call. So the UDP rate is bounded by the host's per-datagram
cost -- about 115 us idle on a macOS host, more when the host is busy --
while the ARP rate is not (docs/testing/flakes.md, "`net-nicbench`'s UDP
rate"). The old phase sent a fixed 10,000 datagrams and so took 10,000
times that cost; the fixed phase sends for 500 ms and reports the rate.

This probe builds a temporary clone whose runner panics right after
`net-nicbench` with the test's verdict and duration, optionally with the
old count-bound phase restored, and optionally under a host-side load
that makes the same `sendto` path busier (N processes flooding the same
closed loopback port):
  python3 tools/nicbench-host-cost-probe.py --arch x86_64 --old --load 4   # old structure, loaded host: must take > 8 s
  python3 tools/nicbench-host-cost-probe.py --arch x86_64 --load 4         # fixed, loaded host: must stay under 8 s
  python3 tools/nicbench-host-cost-probe.py --arch x86_64 --old            # controls: no extra load
  python3 tools/nicbench-host-cost-probe.py --arch x86_64
Each run prints both interfaces' ARP and UDP rates, the per-send
histogram, the driver's share of the window and the test's duration.
Four CPUs; never edits the working tree; logs under out/nicbench-host-cost-*/.
"""
import argparse
import importlib.util
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

FLOOD = r'''
import socket, sys
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
p = b"\0" * 1024
while True:
    s.sendto(p, ("127.0.0.1", 33434))
'''


def replace_once(source, before, after):
    if source.count(before) != 1:
        raise RuntimeError('probe anchor must occur exactly once: ' + before[:80])
    return source.replace(before, after)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--arch', choices=['x86_64', 'aarch64'], default='x86_64')
    ap.add_argument('--old', action='store_true', help='the count-bound phase: 10,000 datagrams however long they take')
    ap.add_argument('--load', type=int, default=0, metavar='N', help='N host processes flooding the same loopback port')
    args = ap.parse_args()
    root = Path(__file__).resolve().parents[1]
    tag = 'nicbench-host-cost-' + args.arch + ('-old' if args.old else '-fixed') + ('-load%d' % args.load if args.load else '')
    out = root / 'out' / tag
    out.mkdir(parents=True, exist_ok=True)

    nettest = (root / 'kernel-services/network/nettest.c').read_text()
    if args.old:
        nettest = replace_once(nettest, '''        uint64_t a = clock_now_ns();
        if (a >= t_end)
            break;
        int64_t rc = ksock_sendto(tx, payload, sizeof(payload), &to);''', '''        uint64_t a = clock_now_ns();
        if (st->attempts >= 10000u)   /* NICPROBE --old: the count-bound phase */
            break;
        int64_t rc = ksock_sendto(tx, payload, sizeof(payload), &to);''')
    selftest = (root / 'kernel/core/selftest.c').read_text()
    selftest = replace_once(selftest, '''        uint64_t dt = clock_since_ns(t0);
        total_ns += dt;
''', '''        uint64_t dt = clock_since_ns(t0);
        total_ns += dt;
        if (strcmp(tests[i].name, "net-nicbench") == 0)
            panic("NICPROBE: ok=%u ms=%llu", ok, (unsigned long long)(dt / 1000000));
''')

    flood = []
    try:
        with tempfile.TemporaryDirectory(prefix='cosmo-nicbench-host-cost-') as tmp:
            subprocess.run(['git', 'clone', '--quiet', '--shared', str(root), tmp], check=True)
            work = Path(tmp)
            (work / 'kernel-services/network/nettest.c').write_text(nettest)
            (work / 'kernel/core/selftest.c').write_text(selftest)
            with (out / 'build.log').open('w') as log:
                subprocess.run(['gmake', '-j4', 'ARCH=' + args.arch, 'BUILD=debug', 'HAVE_MUSL=0',
                                'OUT=' + str(out), 'image'], cwd=work, stdout=log, stderr=subprocess.STDOUT, check=True)
            for _ in range(args.load):
                flood.append(subprocess.Popen([sys.executable, '-c', FLOOD], stdout=subprocess.DEVNULL,
                                              stderr=subprocess.DEVNULL))
            spec = importlib.util.spec_from_file_location('nicbench_boot', root / 'tests/boot/run_boot_test.py')
            harness = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(harness)
            harness.PANIC_REQUIRED_MARKERS = [r'^KERNEL PANIC: NICPROBE: ok=1 ms=\d+$',
                                              r'^\[ INFO\] selftest: net-nicbench: eth1: udp us per send:']
            harness.PANIC_FORBIDDEN_MARKERS = [r'^KERNEL PANIC: NICPROBE: ok=0', r'KERNEL PANIC \(recursive\)',
                                               r'^SELFTEST: net-nicbench .*FAIL']
            os.environ.update(COSMO_ARCH=args.arch, QEMU_ARCH=args.arch, QEMU_SMP='4')
            sys.argv = ['nicbench-host-cost-probe', '--expect-panic', 'fault', '--timeout', '170',
                        '--image', str(out / 'cosmoos.img'), '--log', str(out / 'boot.log')]
            rc = harness.main()
    finally:
        for p in flood:
            p.kill()
    log = (out / 'boot.log').read_text(errors='replace')
    for line in log.splitlines():
        if 'selftest: net-nicbench: eth' in line and ('udp' in line or 'arp ' in line):
            print(line.split('selftest: ', 1)[1])
    m = re.search(r'NICPROBE: ok=1 ms=(\d+)', log)
    if rc != 0 or m is None:
        print('probe: the boot did not deliver the verdict (harness exit %s)' % rc)
        return 1
    ms = int(m.group(1))
    print('probe: net-nicbench took %d ms (%s phase, %s)' % (ms, 'count-bound' if args.old else '500 ms window',
                                                            'host load %d' % args.load if args.load else 'no added load'))
    if args.old and args.load:
        if ms <= 8000:
            print('probe: FAIL -- expected the count-bound phase to exceed the former 8 s budget under load')
            return 1
    elif not args.old:
        if ms >= 8000:
            print('probe: FAIL -- the windowed phase must keep the test under 8 s whatever the host does')
            return 1
    print('probe: PASS')
    return 0


if __name__ == '__main__':
    sys.exit(main())
