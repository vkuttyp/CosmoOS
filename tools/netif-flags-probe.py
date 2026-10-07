#!/usr/bin/env python3
"""Restore the per-check reads of nif->flags and show net-netif-flags fail at its first check.

Invariant N26: ipv4_input reads the ingress interface's flag word once per
packet and ipv4_forward the egress's once, and every check of the packet
is made from those readings. Before 2026-10-07 each check read the word
again, so a toggle landing between the anti-spoof check and the masquerade
decision gave one packet half of each state.

This probe builds a temporary clone with the old reads restored (`--old`:
ipv4_forward's anti-spoof and masquerade decision, and nat_out's checks,
read nif->flags live again) or as committed, and boots to a panic right
after the test carrying its verdict:
  python3 tools/netif-flags-probe.py --arch x86_64 --old   # must FAIL at `src == other`: the parked datagram was masqueraded
  python3 tools/netif-flags-probe.py --arch x86_64         # must PASS
AArch64 is the same with --arch aarch64. Four CPUs; never edits the
working tree; logs under out/netif-flags-<arch>-{old,fixed}/.
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


def replace_once(source, before, after):
    if source.count(before) != 1:
        raise RuntimeError('probe anchor must occur exactly once: ' + before[:80])
    return source.replace(before, after)


def failing_check(tests):
    expr = 'src == other'
    for i, l in enumerate(tests.splitlines()):
        if 'CHECK(' + expr + ');' in l and 'two readings masquerade it' in l:
            return expr, i + 1
    raise RuntimeError('the check is not in nettest.c as expected')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--arch', choices=['x86_64', 'aarch64'], default='x86_64')
    ap.add_argument('--old', action='store_true', help='every check reads nif->flags again, as before')
    args = ap.parse_args()
    root = Path(__file__).resolve().parents[1]
    tag = 'netif-flags-' + args.arch + ('-old' if args.old else '-fixed')
    out = root / 'out' / tag
    shutil.rmtree(out, ignore_errors=True)   # a previous run's dependency files name a temporary clone that is gone
    out.mkdir(parents=True, exist_ok=True)

    ip = (root / 'kernel-services/network/ipv4.c').read_text()
    nat = (root / 'kernel-services/network/nat.c').read_text()
    if args.old:
        ip = replace_once(ip, '''        bool bad = (in_flags & NETIF_MASQUERADE)
                       ? iph->src != (net | htonl(TAPSVC_GUEST_HOST))''', '''        bool bad = (in->flags & NETIF_MASQUERADE)   /* FLAGSPROBE --old: a live read */
                       ? iph->src != (net | htonl(TAPSVC_GUEST_HOST))''')
        ip = replace_once(ip, '''    uint32_t new_src = src;
    if (in_flags & NETIF_MASQUERADE) {''', '''    uint32_t new_src = src;
    if (in->flags & NETIF_MASQUERADE) {   /* FLAGSPROBE --old: a live read */''')
        nat = replace_once(nat, '''    if (!(in_flags & NETIF_MASQUERADE) || on_egress || out->ip4.addr == 0 ||
        (out_flags & NETIF_FORWARD))''', '''    (void)in_flags;
    (void)out_flags;   /* FLAGSPROBE --old: the caller's readings ignored, nif->flags read live */
    if (!(in->flags & NETIF_MASQUERADE) || on_egress || out->ip4.addr == 0 ||
        (out->flags & NETIF_FORWARD))''')
    tests = (root / 'kernel-services/network/nettest.c').read_text()
    tests = replace_once(tests, '''bool selftest_net_netif_flags(const char **reason)
{
#if !CONFIG_DEBUG''', '''static bool flagsprobe_body(const char **reason);
bool selftest_net_netif_flags(const char **reason)
{
    bool r = flagsprobe_body(reason);
    panic("FLAGSPROBE: ok=%u reason=%s", r, *reason);
}
static bool flagsprobe_body(const char **reason)
{
#if !CONFIG_DEBUG''')
    if '#include <kernel/panic.h>' not in tests:
        tests = replace_once(tests, '#include <kernel/printf.h>', '#include <kernel/panic.h>\n#include <kernel/printf.h>')

    with tempfile.TemporaryDirectory(prefix='cosmo-netif-flags-') as tmp:
        subprocess.run(['git', 'clone', '--quiet', '--shared', str(root), tmp], check=True)
        work = Path(tmp)
        (work / 'kernel-services/network/ipv4.c').write_text(ip)
        (work / 'kernel-services/network/nat.c').write_text(nat)
        (work / 'kernel-services/network/nettest.c').write_text(tests)
        with (out / 'build.log').open('w') as log:
            subprocess.run(['gmake', '-j4', 'ARCH=' + args.arch, 'BUILD=debug', 'HAVE_MUSL=0',
                            'OUT=' + str(out), 'image'], cwd=work, stdout=log, stderr=subprocess.STDOUT, check=True)
        spec = importlib.util.spec_from_file_location('flags_boot', root / 'tests/boot/run_boot_test.py')
        harness = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(harness)
        if args.old:
            expr, line = failing_check(tests)
            outcome = r'ok=0 reason=check failed: ' + re.escape(expr) + ' at line %u' % line
        else:
            outcome = r'ok=1 reason='
        harness.PANIC_REQUIRED_MARKERS = [r'^KERNEL PANIC: FLAGSPROBE: ' + outcome + '$']
        harness.PANIC_FORBIDDEN_MARKERS = [r'^SELFTEST: net-netif-flags ', r'KERNEL PANIC \(recursive\)']
        os.environ.update(COSMO_ARCH=args.arch, QEMU_ARCH=args.arch, QEMU_SMP='4')
        sys.argv = ['netif-flags-probe', '--expect-panic', 'fault', '--timeout', '170',
                    '--image', str(out / 'cosmoos.img'), '--log', str(out / 'boot.log')]
        return harness.main()


if __name__ == '__main__':
    sys.exit(main())
