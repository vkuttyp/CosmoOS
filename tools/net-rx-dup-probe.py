#!/usr/bin/env python3
"""net-rx-dup-probe.py -- the network receive path has no duplicate-frame
fault injection.

`enum fi_kind` (kernel/core/faultinject.c) has kmalloc/blk/demand/usb/ahci/
hv/file kinds and nothing for the network, so `netif_rx` -> `rx_common`, the
choke point every interface funnels through, cannot be made to deliver the
same frame twice. The stack's behaviour under a duplicated frame -- TCP
delivering a duplicated segment once, ARP staying idempotent -- is therefore
unverified. The one adjacent hook, the loopback loss filter, can drop a frame
but is called before `lo_transmit` completes the checksum, so a copy made
there lacks M_CSUM_OK and is dropped as a bad checksum; it is also
loopback-only. See docs/audit/next-subsystem-net-rx-dup.md.

This probe adds a self-test, `net-rx-dup-gap`, that subjects `rx_common` to a
duplicated frame the only way possible today -- a hand-patch of the receive
path, gated on a probe-only flag and armed only around one loopback TCP
transfer -- and reports that there is no fault-injection kind for it.

    python3 tools/net-rx-dup-probe.py apply
    gmake ARCH=x86_64 test > run.txt 2>&1
    grep NETRXDUP out/x86_64-debug/boot-test.log
    python3 tools/net-rx-dup-probe.py revert

`apply` and `revert` are those of tools/device-reset-probe.py.
"""

import hashlib
import os
import stat
import subprocess
import sys

NETIF = 'kernel-services/network/netif.c'
NETTEST = 'kernel-services/network/nettest.c'
DECL = 'kernel/include/kernel/selftest.h'
REG = 'kernel/core/selftest.c'
BACKUP = '.net-rx-dup-probe.orig'
STAMP = '.net-rx-dup-probe.applied'

# --- netif.c: the probe flag + a duplication in rx_common -------------------

OLD_HOOK = "static void *g_rx_hook_arg;\n"
NEW_HOOK = OLD_HOOK + """
/* NETRXDUP probe (tools/net-rx-dup-probe.py): no faultinject kind can
 * duplicate a received frame, so this flag -- armed only around the
 * net-rx-dup-gap self-test -- makes rx_common deliver a second copy of each
 * frame, to demonstrate the gap FI_NET_RX_DUP fills. m_copypacket keeps
 * m->pkt and M_CSUM_OK (a copy made in the loopback filter, before
 * lo_transmit finishes the csum, would not), which is why the duplication
 * belongs here rather than there. */
static volatile bool g_netrxdup_probe;
static volatile uint64_t g_netrxdup_count;
void netrxdup_probe_set(bool on);
uint64_t netrxdup_probe_count(void);
void netrxdup_probe_set(bool on) { __atomic_store_n(&g_netrxdup_probe, on, __ATOMIC_RELEASE); }
uint64_t netrxdup_probe_count(void) { return __atomic_load_n(&g_netrxdup_count, __ATOMIC_ACQUIRE); }
"""

OLD_RX = """    __atomic_fetch_add(&c->stats.rx_queued, 1, __ATOMIC_RELAXED);
    quiesce_read_unlock();
    waitqueue_wake_one(&c->wq);
"""
NEW_RX = """    __atomic_fetch_add(&c->stats.rx_queued, 1, __ATOMIC_RELAXED);
    if (__atomic_load_n(&g_netrxdup_probe, __ATOMIC_ACQUIRE)) {
        struct mbuf *d = m_copypacket(m);   /* keeps m->pkt and M_CSUM_OK */
        if (d != NULL) {
            if (mbufq_enqueue(&c->rxq, d)) {
                __atomic_fetch_add(&c->stats.rx_queued, 1, __ATOMIC_RELAXED);
                __atomic_fetch_add(&g_netrxdup_count, 1, __ATOMIC_RELAXED);
            } else {
                m_freem(d);
            }
        }
    }
    quiesce_read_unlock();
    waitqueue_wake_one(&c->wq);
"""

# --- nettest.c: the gap self-test -------------------------------------------

OLD_TCP = """bool selftest_net_lo_tcp(const char **reason)
{
"""
NEW_TCP = r"""/* NETRXDUP probe (tools/net-rx-dup-probe.py): faultinject has no network RX
 * kind, so this exercises duplicate-frame delivery with the hand-patched
 * duplication in rx_common, armed only for the duration of one loopback TCP
 * transfer, and reports the gap. */
extern void netrxdup_probe_set(bool on);
extern uint64_t netrxdup_probe_count(void);

bool selftest_net_rx_dup_gap(const char **reason)
{
    uint64_t before = netrxdup_probe_count();
    netrxdup_probe_set(true);
    bool ok = tcp_transfer(reason, v4addr(INADDR_LOOPBACK_N, 6050), 256u * 1024u, 0);
    netrxdup_probe_set(false);
    uint64_t dups = netrxdup_probe_count() - before;
    if (!ok)
        return false;   /* tcp_transfer set *reason: the stream was not delivered once */
    if (dups == 0) {
        *reason = "check failed: no frames were duplicated (the probe injected nothing)";
        return false;
    }
    kprintf("NETRXDUP: no faultinject kind duplicates a received frame; rx_common hand-patched to "
            "deliver %llu duplicates (loopback filter runs pre-csum, its copy dropped); TCP still "
            "delivered 262144 bytes once\n", (unsigned long long)dups);
    return true;
}

bool selftest_net_lo_tcp(const char **reason)
{
"""

OLD_DECL = "bool selftest_net_lo_tcp(const char **reason);\n"
NEW_DECL = OLD_DECL + "bool selftest_net_rx_dup_gap(const char **reason);\n"

OLD_REG = '    { "net-lo-tcp",      selftest_net_lo_tcp },\n'
NEW_REG = OLD_REG + '    { "net-rx-dup-gap", selftest_net_rx_dup_gap },\n'


def sha(p):
    return hashlib.sha256(open(p, 'rb').read()).hexdigest()


def write_atomic(path, data):
    tmp = path + '.probe-tmp'
    with open(tmp, 'wb') as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())
    if os.path.exists(path):
        os.chmod(tmp, stat.S_IMODE(os.stat(path).st_mode))
    os.replace(tmp, path)


def git_clean(path):
    r = subprocess.run(['git', 'status', '--porcelain', '--', path], capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f'git status failed for {path}: {r.stderr.strip() or r.returncode}')
    return not r.stdout.strip()


def apply_files(fl):
    if os.path.exists(STAMP):
        sys.exit('already applied (or an apply was interrupted): run revert first')
    if os.path.exists(STAMP + '.probe-tmp'):
        os.remove(STAMP + '.probe-tmp')
    plan = []
    for path, edits in fl:
        if os.path.exists(path + BACKUP):
            sys.exit(f'{path + BACKUP} exists from an earlier run; restore or remove it by hand first')
        if not os.path.isfile(path):
            sys.exit(f'{path} not found: run from the top of the tree')
        if not git_clean(path):
            sys.exit(f'{path} has uncommitted changes')
        orig = open(path, 'rb').read()
        s = orig.decode()
        for a, b in edits:
            if s.count(a) != 1:
                sys.exit(f'{path}: anchor not found exactly once: {a[:50]!r}')
            s = s.replace(a, b)
        plan.append((path, orig, s.encode()))
    write_atomic(STAMP, ''.join(f'{p} {hashlib.sha256(n).hexdigest()} {hashlib.sha256(o).hexdigest()}\n'
                                for p, o, n in plan).encode())
    for path, orig, new in plan:
        write_atomic(path + BACKUP, orig)
        write_atomic(path, new)


def revert():
    if not os.path.exists(STAMP):
        if os.path.exists(STAMP + '.probe-tmp'):
            os.remove(STAMP + '.probe-tmp')
            sys.exit('not applied (removed a partial stamp an interrupted apply left)')
        sys.exit('not applied')
    entries = [line.split() for line in open(STAMP).read().split('\n') if line]
    for path, patched, orig in entries:
        cur = sha(path)
        if cur == orig:
            continue
        if cur != patched:
            sys.exit(f'{path} changed since apply. {path + BACKUP} is the pre-probe '
                     f'original, so copying it back would erase those changes. Keep your '
                     f'edits and remove the NETRXDUP lines by hand, then delete {path + BACKUP} '
                     f'and {STAMP}.')
        if not os.path.exists(path + BACKUP) or sha(path + BACKUP) != orig:
            sys.exit(f'{path} is still patched and {path + BACKUP} is missing or not its original; restore by hand')
    for path, patched, orig in entries:
        if sha(path) == patched:
            write_atomic(path, open(path + BACKUP, 'rb').read())
    for path, _, _ in entries:
        for leftover in (path + BACKUP, path + '.probe-tmp', path + BACKUP + '.probe-tmp'):
            if os.path.exists(leftover):
                os.remove(leftover)
    if os.path.exists(STAMP + '.probe-tmp'):
        os.remove(STAMP + '.probe-tmp')
    os.remove(STAMP)
    print('reverted')


def apply():
    apply_files([
        (NETIF, [(OLD_HOOK, NEW_HOOK), (OLD_RX, NEW_RX)]),
        (NETTEST, [(OLD_TCP, NEW_TCP)]),
        (DECL, [(OLD_DECL, NEW_DECL)]),
        (REG, [(OLD_REG, NEW_REG)]),
    ])
    print('applied: net-rx-dup-gap shows the receive path has no duplicate-frame injection')


if __name__ == '__main__':
    {'apply': apply, 'revert': revert}.get(sys.argv[1] if len(sys.argv) > 1 else '',
                                           lambda: sys.exit(__doc__))()
