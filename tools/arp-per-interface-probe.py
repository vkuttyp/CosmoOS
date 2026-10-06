#!/usr/bin/env python3
"""Restore the address-only neighbour keying and show the two per-interface tests fail at their own checks.

Invariant N25: an ARP or ND entry is (interface, address). Until
2026-10-07 `find`/`nd_find` matched the address alone, so the second of
two interfaces resolving the same neighbour address found the first's
entry: it sent no request of its own, parked its packet where the
first's reply would send it out of the wrong interface, and a reply on
either interface completed both.

This probe builds a temporary clone with that keying restored in both
tables (`--old`) or as committed, and boots to a panic right after the
two tests carrying both verdicts:
  python3 tools/arp-per-interface-probe.py --arch x86_64 --old   # both must FAIL: b.transmits == 1 (ARP), b.transmits == 1 (ND)
  python3 tools/arp-per-interface-probe.py --arch x86_64         # both must PASS
AArch64 is the same with --arch aarch64. Four CPUs; never edits the
working tree; logs under out/arp-per-interface-<arch>-{old,fixed}/.
"""
import argparse
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def replace_once(source, before, after):
    if source.count(before) != 1:
        raise RuntimeError('probe anchor must occur exactly once: ' + before[:80])
    return source.replace(before, after)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--arch', choices=['x86_64', 'aarch64'], default='x86_64')
    ap.add_argument('--old', action='store_true', help='key both tables by address alone, as before N25')
    args = ap.parse_args()
    root = Path(__file__).resolve().parents[1]
    tag = 'arp-per-interface-' + args.arch + ('-old' if args.old else '-fixed')
    out = root / 'out' / tag
    shutil.rmtree(out, ignore_errors=True)   # a previous run's dependency files name a temporary clone that is gone
    out.mkdir(parents=True, exist_ok=True)

    arp = (root / 'kernel-services/network/arp.c').read_text()
    nd = (root / 'kernel-services/network/ipv6.c').read_text()
    if args.old:
        arp = replace_once(arp, '        if (g_table[i].state != ARP_FREE && g_table[i].ip == ip && g_table[i].nif == nif)',
                           '        if (g_table[i].state != ARP_FREE && g_table[i].ip == ip)   /* ARPPROBE --old: address alone */')
        nd = replace_once(nd, '        if (g_nd[i].state != ND_FREE && g_nd[i].nif == nif && in6_equal(&g_nd[i].ip, ip))',
                          '        if (g_nd[i].state != ND_FREE && in6_equal(&g_nd[i].ip, ip))   /* ARPPROBE --old: address alone */')
        # `nif` is then unused in both finders; the build is -Werror.
        arp = replace_once(arp, 'static struct arp_entry *find(const struct netif *nif, uint32_t ip)\n{\n',
                           'static struct arp_entry *find(const struct netif *nif, uint32_t ip)\n{\n    (void)nif;\n')
        nd = replace_once(nd, 'static struct nd_entry *nd_find(const struct netif *nif, const struct in6_addr *ip)\n{\n',
                          'static struct nd_entry *nd_find(const struct netif *nif, const struct in6_addr *ip)\n{\n    (void)nif;\n')
    tests = (root / 'kernel-services/network/nettest.c').read_text()
    # Wrap both tests: the ARP test records its verdict, the ND test panics
    # with both, so the boot ends right after them.
    tests = replace_once(tests, 'bool selftest_net_arp_per_interface(const char **reason)\n{', '''static bool arpprobe_arp_body(const char **reason);
static bool arpprobe_nd_body(const char **reason);
static bool arpprobe_arp_ok, arpprobe_arp_ran;
static const char *arpprobe_arp_reason = "";
bool selftest_net_arp_per_interface(const char **reason)
{
    bool r = arpprobe_arp_body(reason);
    arpprobe_arp_ok = r;
    arpprobe_arp_reason = *reason;
    arpprobe_arp_ran = true;
    return r;
}
bool selftest_net_nd_per_interface(const char **reason)
{
    if (!arpprobe_arp_ran)
        panic("ARPPROBE: net-arp-per-interface did not run first");
    bool nd_ok = arpprobe_nd_body(reason);
    panic("ARPPROBE: arp=%u nd=%u arp-reason=%s nd-reason=%s", arpprobe_arp_ok, nd_ok, arpprobe_arp_reason, *reason);
}
static bool arpprobe_arp_body(const char **reason)
{''')
    tests = replace_once(tests, 'bool selftest_net_nd_per_interface(const char **reason)\n{\n    static struct dual_nif a, b;',
                         'static bool arpprobe_nd_body(const char **reason)\n{\n    static struct dual_nif a, b;')
    if '#include <kernel/panic.h>' not in tests:
        tests = replace_once(tests, '#include <kernel/printf.h>', '#include <kernel/panic.h>\n#include <kernel/printf.h>')

    with tempfile.TemporaryDirectory(prefix='cosmo-arp-per-interface-') as tmp:
        subprocess.run(['git', 'clone', '--quiet', '--shared', str(root), tmp], check=True)
        work = Path(tmp)
        (work / 'kernel-services/network/arp.c').write_text(arp)
        (work / 'kernel-services/network/ipv6.c').write_text(nd)
        (work / 'kernel-services/network/nettest.c').write_text(tests)
        with (out / 'build.log').open('w') as log:
            subprocess.run(['gmake', '-j4', 'ARCH=' + args.arch, 'BUILD=debug', 'HAVE_MUSL=0',
                            'OUT=' + str(out), 'image'], cwd=work, stdout=log, stderr=subprocess.STDOUT, check=True)
        spec = importlib.util.spec_from_file_location('arp_boot', root / 'tests/boot/run_boot_test.py')
        harness = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(harness)
        if args.old:
            outcome = (r'arp=0 nd=0 arp-reason=check failed: b\.transmits == 1 at line \d+ '
                       r'nd-reason=check failed: b\.transmits == 1 at line \d+')
        else:
            outcome = r'arp=1 nd=1 arp-reason= nd-reason='
        harness.PANIC_REQUIRED_MARKERS = [r'^KERNEL PANIC: ARPPROBE: ' + outcome + '$']
        harness.PANIC_FORBIDDEN_MARKERS = [r'^SELFTEST: net-nd-per-interface ', r'KERNEL PANIC \(recursive\)',
                                           r'ARPPROBE: net-arp-per-interface did not run first']
        os.environ.update(COSMO_ARCH=args.arch, QEMU_ARCH=args.arch, QEMU_SMP='4')
        sys.argv = ['arp-per-interface-probe', '--expect-panic', 'fault', '--timeout', '170',
                    '--image', str(out / 'cosmoos.img'), '--log', str(out / 'boot.log')]
        return harness.main()


if __name__ == '__main__':
    sys.exit(main())
