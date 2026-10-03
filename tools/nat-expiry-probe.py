#!/usr/bin/env python3
"""Hold net-nat's final four packets before translation in a temporary clone.

The old wait mistakes four earlier translations for flood completion and
ages before the tail. The fixed wait must let the tail finish first. The
stalled control never releases it and must fail the completion assertion.
Every mode requires its exact outcome and panic exit, not a generic hang.
"""
import argparse
import importlib.util
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


def once(source, before, after):
    if source.count(before) != 1:
        raise RuntimeError('probe anchor must occur exactly once: ' + before[:80])
    return source.replace(before, after)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--arch', choices=['x86_64', 'aarch64'], default='x86_64')
    parser.add_argument('--mode', choices=['fixed', 'old', 'stalled'], default='fixed')
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    parent = root / 'out' / ('nat-expiry-' + args.arch + '-' + args.mode)
    parent.mkdir(parents=True, exist_ok=True)
    out = Path(tempfile.mkdtemp(prefix='run-', dir=parent))
    print('nat-expiry-probe: artifacts: ' + str(out), flush=True)

    nat = (root / 'kernel-services/network/nat.c').read_text()
    nat = '#include <kernel/panic.h>\n#include <kernel/thread.h>\n' + nat
    anchor = 'int nat_out(struct netif *in, struct netif *out, struct mbuf *m,'
    nat = once(nat, anchor, '''static unsigned probe_released, probe_parked;
void nat_expiry_probe_release(void);
unsigned nat_expiry_probe_parked(void);
void nat_expiry_probe_release(void) { __atomic_store_n(&probe_released, 1u, __ATOMIC_RELEASE); }
unsigned nat_expiry_probe_parked(void) { return __atomic_load_n(&probe_parked, __ATOMIC_ACQUIRE); }

''' + anchor)
    nat = once(nat, '    *new_src = iph->src;', '''    /* No NAT lock or receive-hook read section is held here. The real
     * worker retains its packet/interface references across this pause. */
    if (iph->src == IPV4_ADDR(10, 77, 3, 15) && iph->proto == IPPROTO_UDP) {
        uint16_t port = ntohs(get16(m->data + ihl));
        if (port >= 10260 && port < 10264) {
            __atomic_fetch_add(&probe_parked, 1u, __ATOMIC_RELEASE);
            uint64_t start = clock_now_ns();
            while (!__atomic_load_n(&probe_released, __ATOMIC_ACQUIRE)) {
                if (clock_since_ns(start) > 10000000000ULL)
                    panic("NATEXPIRY: tail gate timeout");
                thread_sleep_ms(1);
            }
        }
    }
    *new_src = iph->src;''')
    tests = (root / 'kernel-services/network/nettest.c').read_text()
    tests = '#include <kernel/panic.h>\n' + tests
    start = tests.index('bool selftest_net_nat(const char **reason)')
    end = tests.index('/* --- tap input filter', start)
    test = tests[start:end]
    barrier = '(ns1.out_new - ns0.out_new) + (ns1.out_drop_share - ns0.out_drop_share)'
    if args.mode == 'old':
        test = once(test, barrier + ' >= injected', 'ns1.out_new + ns1.out_drop_share >= NAT_TABLE_SIZE + 8')
        test = once(test, '    CHECK(' + barrier + ' == injected);\n', '')
    anchor = '    for (unsigned i = 0; i < 200; i++) {'
    test = once(test, anchor, '''    struct nat_stats probe_base = ns0;
    uint64_t probe_start = clock_now_ns();
    for (;;) {
        nat_get_stats(&ns1);
        uint64_t done = (ns1.out_new - ns0.out_new) + (ns1.out_drop_share - ns0.out_drop_share);
        if (done == 260 && nat_expiry_probe_parked())
            break;
        if (clock_since_ns(probe_start) > 2000000000ULL)
            panic("NATEXPIRY: prefix guard expired");
        thread_sleep_ms(1);
    }
    CHECK(ns0.out_new + ns0.out_drop_share == 4);
    kinfo("NATEXPIRY: baseline=4 completed=260 pending=4");
''' + anchor)
    if args.mode != 'stalled':
        # This runs only if the actual wait predicate refused to finish.
        test = once(test, '        thread_sleep_ms(10);',
                    '        nat_expiry_probe_release();\n        thread_sleep_ms(10);')
    anchor = '    nat_age(clock_now_ns() + 2ull * NAT_TIMEOUT_UDP_NS);'
    test = once(test, anchor, anchor + '''
    /* An early sweep lets delayed packets create entries after it. */
    nat_expiry_probe_release();
    probe_start = clock_now_ns();
    for (;;) {
        nat_get_stats(&ns1);
        if ((ns1.out_new - probe_base.out_new) + (ns1.out_drop_share - probe_base.out_drop_share) == injected)
            break;
        if (clock_since_ns(probe_start) > 2000000000ULL)
            panic("NATEXPIRY: tail completion guard expired");
        thread_sleep_ms(1);
    }
    kinfo("NATEXPIRY: after-age entries=%u", ns1.entries);
''')
    test = once(test, 'bool selftest_net_nat(', 'static bool nat_expiry_inner(')
    test = ('extern void nat_expiry_probe_release(void);\n'
            'extern unsigned nat_expiry_probe_parked(void);\n' + test + '''
bool selftest_net_nat(const char **reason)
{
    bool ok = nat_expiry_inner(reason);
    panic("NATEXPIRY: result=%u reason=%s", ok, ok ? "none" : *reason);
}

''')
    tests = tests[:start] + test + tests[end:]
    runner = (root / 'kernel/core/selftest.c').read_text()
    entry = '    { "net-nat",         selftest_net_nat },\n'
    runner = once(runner, entry, '')
    runner = once(runner, 'static const struct selftest tests[] = {\n',
                  'static const struct selftest tests[] = {\n' + entry)
    with tempfile.TemporaryDirectory(prefix='cosmo-nat-expiry-') as tmp:
        subprocess.run(['git', 'clone', '--quiet', '--shared', str(root), tmp], check=True)
        work = Path(tmp)
        for name, contents in [('kernel-services/network/nat.c', nat),
                               ('kernel-services/network/nettest.c', tests),
                               ('kernel/core/selftest.c', runner)]:
            (work / name).write_text(contents)
        with (out / 'build.log').open('w') as log:
            subprocess.run(['gmake', '-j4', 'ARCH=' + args.arch, 'BUILD=debug', 'HAVE_MUSL=0',
                            'OUT=' + str(out), 'image'], cwd=work,
                           stdout=log, stderr=subprocess.STDOUT, check=True)
        sys.path.insert(0, str(root / 'tests/boot'))
        spec = importlib.util.spec_from_file_location('nat_boot', root / 'tests/boot/run_boot_test.py')
        harness = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(harness)
        reason = ('ns1.entries == 0 && ns1.expired > ns0.expired' if args.mode == 'old'
                  else barrier + ' == injected')
        outcome = ('result=1 reason=none' if args.mode == 'fixed' else
                   'result=0 reason=check failed: ' + reason)
        suffix = '$' if args.mode == 'fixed' else r' at line [0-9]+$'
        harness.PANIC_REQUIRED_MARKERS = [r'NATEXPIRY: baseline=4 completed=260 pending=4$',
                                          r'^KERNEL PANIC: NATEXPIRY: ' + re.escape(outcome) + suffix, r'^halting\.$']
        if args.mode != 'stalled':
            harness.PANIC_REQUIRED_MARKERS.append('NATEXPIRY: after-age entries=' + ('4' if args.mode == 'old' else '0') + '$')
        harness.PANIC_FORBIDDEN_MARKERS = [r'^KERNEL PANIC: NATEXPIRY: .*guard expired',
                                           r'^KERNEL PANIC: NATEXPIRY: tail gate timeout']
        os.environ.update(COSMO_ARCH=args.arch, QEMU_ARCH=args.arch, QEMU_SMP='4')
        sys.argv = ['nat-expiry-probe', '--expect-panic', 'fault', '--timeout', '30',
                    '--image', str(out / 'cosmoos.img'), '--log', str(out / 'boot.log')]
        return harness.main()


if __name__ == '__main__':
    sys.exit(main())
