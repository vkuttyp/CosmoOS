#!/usr/bin/env python3
"""Restore a before-the-lock NETIF_UP check and show net-neigh-down-race fail at that race's check.

Invariant N25's follow-up: arp_input and nd_input_* tested NETIF_UP
before taking the table lock, and arp_resolve/nd_resolve did not test it
at all, so an input or a resolve that lost the CPU between its decision
and the lock while netif_set_up(false) cleared the flag and flushed left
a neighbour entry on the down interface. All four now decide under the lock.

This probe builds a temporary clone with the old order restored in ONE
path (`--old <race>`) or as committed, and boots to a panic right after
the test carrying its verdict. Each race has its own mode and its own
failing check, so each is shown to need the fix, not only the first:

  --old arp-input    arp_input's check back before the lock: the test fails
                     at `!arp_lookup(&d.nif, r.ip4, mac)` (the asker learned
                     on the down interface)
  --old arp-resolve  no check in arp_resolve: fails at `r.rc == -ENETUNREACH`
                     (the resolve answered -EINPROGRESS, an incomplete entry
                     with a parked packet on a down interface)
  --old nd-input     nd_input_ns's check back before the lock: fails at
                     `nd_resolve(...) == -EINPROGRESS` (0: the asker learned)
  --old nd-resolve   no check in nd_resolve: fails at `r.rc == -ENETUNREACH`
                     in the ND resolve case

The advertisement path (nd_input_na) has no mode: it never allocates, so a
flush leaves it nothing to complete under either order; the test's case 4
pins the check's place and would pass with the old order too.

  python3 tools/neigh-down-race-probe.py --arch x86_64 --old arp-input   # must FAIL at that check
  python3 tools/neigh-down-race-probe.py --arch x86_64                   # must PASS
AArch64 is the same with --arch aarch64. Four CPUs; never edits the
working tree; logs under out/neigh-down-race-<arch>-{<race>,fixed}/.
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


RACES = ('arp-input', 'arp-resolve', 'nd-input', 'nd-resolve')

# Each race's own check in selftest_net_neigh_down_race, found by a fragment
# unique to its line (`after` names a line that must come first when the
# expression recurs), so the required failure carries the line number of
# the patched test source and no other case's identical check can stand in.
CHECKS = {
    'arp-input': ('!arp_lookup(&d.nif, r.ip4, mac)', '/* the old order leaves the asker reachable', None),
    'arp-resolve': ('r.rc == -ENETUNREACH', '/* the old order: -EINPROGRESS', None),
    'nd-input': ('nd_resolve(&d.nif, &peer6, mac, NULL) == -EINPROGRESS', '/* no entry: 0 would mean the asker', None),
    'nd-resolve': ('r.rc == -ENETUNREACH', None, 'down_race_run(&r, down_race_nd_resolve'),
}


def failing_check(tests, race):
    expr, fragment, after = CHECKS[race]
    lines = tests.splitlines()
    start = 0
    if after is not None:
        start = next(i for i, l in enumerate(lines) if after in l) + 1
    for i in range(start, len(lines)):
        l = lines[i]
        if 'CHECK(' + expr + ')' in l and (fragment is None or fragment in l):
            return expr, i + 1
    raise RuntimeError('the check for ' + race + ' is not in nettest.c as expected')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--arch', choices=['x86_64', 'aarch64'], default='x86_64')
    ap.add_argument('--old', choices=RACES, help='restore the old order in this one path (see above)')
    args = ap.parse_args()
    root = Path(__file__).resolve().parents[1]
    tag = 'neigh-down-race-' + args.arch + ('-' + args.old if args.old else '-fixed')
    out = root / 'out' / tag
    shutil.rmtree(out, ignore_errors=True)   # a previous run's dependency files name a temporary clone that is gone
    out.mkdir(parents=True, exist_ok=True)

    arp = (root / 'kernel-services/network/arp.c').read_text()
    nd = (root / 'kernel-services/network/ipv6.c').read_text()
    if args.old == 'arp-resolve':
        # arp_resolve: no check at all.
        arp = replace_once(arp, '''    if (!(__atomic_load_n(&nif->flags, __ATOMIC_ACQUIRE) & NETIF_UP)) {
        if (m)
            g_stats.pending_dropped++;   /* counted like the flush and the timeout */
        spin_unlock_irqrestore(&g_lock, s);
        m_freem(m);
        return -ENETUNREACH;
    }
    struct arp_entry *e = find(nif, ip);''', '''    struct arp_entry *e = find(nif, ip);   /* DOWNPROBE --old: no NETIF_UP check in resolve */''')
    if args.old == 'arp-input':
        # arp_input: the check before the park point and the lock.
        arp = replace_once(arp, '''    struct mbuf *pending = NULL;
    arp_test_park_before_lock();
    arch_irq_state_t s = spin_lock_irqsave(&g_lock);''', '''    struct mbuf *pending = NULL;
    if (!(__atomic_load_n(&nif->flags, __ATOMIC_ACQUIRE) & NETIF_UP))   /* DOWNPROBE --old: before the lock */
        return;
    arp_test_park_before_lock();
    arch_irq_state_t s = spin_lock_irqsave(&g_lock);''')
        arp = replace_once(arp, '''    if (!(__atomic_load_n(&nif->flags, __ATOMIC_ACQUIRE) & NETIF_UP)) {
        spin_unlock_irqrestore(&g_lock, s);
        return;
    }
    struct arp_entry *e = find(nif, a.spa);''', '''    struct arp_entry *e = find(nif, a.spa);''')
    if args.old == 'nd-resolve':
        # nd_resolve: no check.
        nd = replace_once(nd, '''    if (!(__atomic_load_n(&nif->flags, __ATOMIC_ACQUIRE) & NETIF_UP)) {   /* under the lock, as arp_resolve (N25) */
        if (m)
            STAT(nd_pending_dropped);
        spin_unlock_irqrestore(&g_nd_lock, s);
        m_freem(m);
        return -ENETUNREACH;
    }
    struct nd_entry *e = nd_find(nif, ip);''', '''    struct nd_entry *e = nd_find(nif, ip);   /* DOWNPROBE --old */''')
    if args.old == 'nd-input':
        # nd_input_ns: before the park point and the lock.
        nd = replace_once(nd, '''        nd_test_park_before_lock();
        arch_irq_state_t s = spin_lock_irqsave(&g_nd_lock);
        if (!(__atomic_load_n(&nif->flags, __ATOMIC_ACQUIRE) & NETIF_UP)) {
            spin_unlock_irqrestore(&g_nd_lock, s);   /* queued input on a down interface learns nothing (N25): read under the lock */
            return;
        }''', '''        if (!(__atomic_load_n(&nif->flags, __ATOMIC_ACQUIRE) & NETIF_UP))   /* DOWNPROBE --old: before the lock */
            return;
        nd_test_park_before_lock();
        arch_irq_state_t s = spin_lock_irqsave(&g_nd_lock);''')
    tests = (root / 'kernel-services/network/nettest.c').read_text()
    tests = replace_once(tests, '''bool selftest_net_neigh_down_race(const char **reason)
{
#if !CONFIG_DEBUG''', '''static bool downprobe_body(const char **reason);
bool selftest_net_neigh_down_race(const char **reason)
{
    bool r = downprobe_body(reason);
    panic("DOWNPROBE: ok=%u reason=%s", r, *reason);
}
static bool downprobe_body(const char **reason)
{
#if !CONFIG_DEBUG''')
    if '#include <kernel/panic.h>' not in tests:
        tests = replace_once(tests, '#include <kernel/printf.h>', '#include <kernel/panic.h>\n#include <kernel/printf.h>')

    with tempfile.TemporaryDirectory(prefix='cosmo-neigh-down-race-') as tmp:
        subprocess.run(['git', 'clone', '--quiet', '--shared', str(root), tmp], check=True)
        work = Path(tmp)
        (work / 'kernel-services/network/arp.c').write_text(arp)
        (work / 'kernel-services/network/ipv6.c').write_text(nd)
        (work / 'kernel-services/network/nettest.c').write_text(tests)
        with (out / 'build.log').open('w') as log:
            subprocess.run(['gmake', '-j4', 'ARCH=' + args.arch, 'BUILD=debug', 'HAVE_MUSL=0',
                            'OUT=' + str(out), 'image'], cwd=work, stdout=log, stderr=subprocess.STDOUT, check=True)
        spec = importlib.util.spec_from_file_location('down_boot', root / 'tests/boot/run_boot_test.py')
        harness = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(harness)
        if args.old:
            expr, line = failing_check(tests, args.old)
            outcome = r'ok=0 reason=check failed: ' + re.escape(expr) + ' at line %u' % line
        else:
            outcome = r'ok=1 reason='
        harness.PANIC_REQUIRED_MARKERS = [r'^KERNEL PANIC: DOWNPROBE: ' + outcome + '$']
        harness.PANIC_FORBIDDEN_MARKERS = [r'^SELFTEST: net-neigh-down-race ', r'KERNEL PANIC \(recursive\)']
        os.environ.update(COSMO_ARCH=args.arch, QEMU_ARCH=args.arch, QEMU_SMP='4')
        sys.argv = ['neigh-down-race-probe', '--expect-panic', 'fault', '--timeout', '170',
                    '--image', str(out / 'cosmoos.img'), '--log', str(out / 'boot.log')]
        return harness.main()


if __name__ == '__main__':
    sys.exit(main())
