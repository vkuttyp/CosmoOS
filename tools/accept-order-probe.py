#!/usr/bin/env python3
"""
accept-order-probe.py -- why did net-lo-tcp hang with the test thread
blocked on a socket and every CPU idle?

One sighting (docs/testing/flakes.md, "`net-lo-tcp` hung, with the test
thread blocked on a socket"), local aarch64 debug, 2026-09-30.

The candidate is the test's listen-backlog step. Two clients, c1 then c2,
connect to a listener that has not accepted. The test accepts once, sends
"hi" on the accepted socket, and receives it on c1. That assumes accept
returns c1's connection. It returns the head of the listener's accept
queue, and a connection joins the queue when the listener processes that
client's final ACK. That happens on the network worker the ACK's flow
hashes to, and c1's and c2's usually hash to different workers. The
client's connect() returns on the SYN-ACK, before its ACK is processed. If
c1's worker has not run by the time c2's ACK is processed, c2 is queued
first. Then "hi" goes to c2, and recvfrom(c1) waits for data nobody will
send: a blocked test thread, idle CPUs, no timer armed.

    python3 tools/accept-order-probe.py apply [--force] [--fix]
    gmake ARCH=x86_64 test > run.txt 2>&1
    grep LTPROBE out/x86_64-debug/boot-test.log
    python3 tools/accept-order-probe.py revert

The instrumentation prints which client the accept returned
(`LTPROBE: accepted c1|c2`), by comparing the accepted socket's peer port
with c1's local port.

--force makes c2 first, deterministically. tcp_input holds the first bare
ACK to port 6002 (c1's final ACK: c1's handshake is the first to that
port) without processing it. After connect(c2) the test waits, up to
2 s, for that ACK to be held and for the listener to be readable (c2
queued). It then delivers the held ACK through tcp_input from the test
thread. A wait that times out fails the step rather than passing
untested. Without --fix, the step should hang exactly as the sighting did,
and the per-test watchdog prints its dump at 8 s.

--fix is the candidate: receive on whichever client the accept returned,
and run the reset check on the other.

`apply` and `revert` are those of tools/cond-phase-probe.py: stamp first,
every file replaced atomically, the stamp removed last, finished by
running revert (again).
"""

import hashlib
import os
import stat
import subprocess
import sys

NETTEST = 'kernel-services/network/nettest.c'
TCP = 'kernel-services/network/tcp.c'
BACKUP = '.accept-order-probe.orig'
STAMP = '.accept-order-probe.applied'

OLD_INPUT = """    m = m_pullup(m, hlen);
    if (m == NULL)
        return;
    th = (const struct tcp_hdr *)m->data;
"""
NEW_INPUT = OLD_INPUT + """    /* LTPROBE --force: hold the first bare ACK to port 6002 (c1's final ACK) */
    if (ip4 && __atomic_load_n(&g_ltprobe_arm, __ATOMIC_ACQUIRE) && ntohs(th->dport) == 6002 &&
        (th->flags & (TH_SYN | TH_ACK | TH_FIN | TH_RST)) == TH_ACK && len == hlen &&
        __atomic_exchange_n(&g_ltprobe_arm, 0, __ATOMIC_ACQ_REL)) {
        g_ltprobe_ip4 = *ip4;
        g_ltprobe_nif = nif;
        __atomic_store_n(&g_ltprobe_m, m, __ATOMIC_RELEASE);
        return;
    }
"""
OLD_INPUT_FN = """void tcp_input(struct netif *nif, struct mbuf *m, const struct ipv4_hdr *ip4, const struct ipv6_hdr *ip6)
{"""
NEW_INPUT_FN = """/* LTPROBE --force: the held ACK and what re-delivering it needs. */
int g_ltprobe_arm;
struct mbuf *g_ltprobe_m;
struct ipv4_hdr g_ltprobe_ip4;
struct netif *g_ltprobe_nif;

""" + OLD_INPUT_FN

OLD_DECL = """bool selftest_net_lo_tcp(const char **reason)
{"""
NEW_DECL = """extern int g_ltprobe_arm;             /* LTPROBE */
extern struct mbuf *g_ltprobe_m;
extern struct ipv4_hdr g_ltprobe_ip4;
extern struct netif *g_ltprobe_nif;

bool selftest_net_lo_tcp(const char **reason)
{"""

OLD_STEP = """    struct socket *c1, *c2;
    CHECK(nt_ksock_create(COSMO_AF_INET, COSMO_SOCK_STREAM, 0, &c1) == 0 && ksock_connect(c1, &la) == 0);
    CHECK(nt_ksock_create(COSMO_AF_INET, COSMO_SOCK_STREAM, 0, &c2) == 0 && ksock_connect(c2, &la) == 0);
    struct socket *a1;
    struct netaddr peer;
    CHECK(nt_ksock_accept(ls, &a1, &peer) == 0 && peer.port >= NET_EPHEMERAL_LO);
    CHECK(ksock_sendto(a1, "hi", 2, NULL) == 2);
    char b[4];
    CHECK(ksock_recvfrom(c1, b, 4, NULL) == 2 && memcmp(b, "hi", 2) == 0);
    /* Closing the listener resets the still-queued connection. */
    nt_ksock_put(ls);
    thread_sleep_ms(20);
    CHECK(ksock_recvfrom(c2, b, 4, NULL) < 0 || ksock_sendto(c2, "x", 1, NULL) < 0);"""


def new_step(force, fix):
    s = """    struct socket *c1, *c2;
"""
    if force:
        s += """    __atomic_store_n(&g_ltprobe_arm, 1, __ATOMIC_RELEASE);   /* LTPROBE --force */
"""
    s += """    CHECK(nt_ksock_create(COSMO_AF_INET, COSMO_SOCK_STREAM, 0, &c1) == 0 && ksock_connect(c1, &la) == 0);
    CHECK(nt_ksock_create(COSMO_AF_INET, COSMO_SOCK_STREAM, 0, &c2) == 0 && ksock_connect(c2, &la) == 0);
"""
    if force:
        s += """    {   /* LTPROBE --force: c2 queued while c1's ACK is held, then c1's delivered */
        uint64_t lt_deadline = clock_now_ns() + 2000ull * 1000 * 1000;
        while ((__atomic_load_n(&g_ltprobe_m, __ATOMIC_ACQUIRE) == NULL ||
                !(ksock_ready(ls) & COSMO_IO_READABLE)) && clock_now_ns() < lt_deadline)
            thread_sleep_ms(1);
        struct mbuf *lt_m = __atomic_exchange_n(&g_ltprobe_m, NULL, __ATOMIC_ACQ_REL);
        bool lt_c2_first = lt_m != NULL && (ksock_ready(ls) & COSMO_IO_READABLE);
        kprintf("LTPROBE: forced: c1's ACK %s, c2 %s\\n", lt_m ? "held" : "NOT HELD",
                lt_c2_first ? "queued first" : "NOT QUEUED (timed out)");
        __atomic_store_n(&g_ltprobe_arm, 0, __ATOMIC_RELEASE);
        if (lt_m != NULL)   /* delivered either way: a failed wait must not strand c1 half-open */
            tcp_input(g_ltprobe_nif, lt_m, &g_ltprobe_ip4, NULL);
        CHECK(lt_c2_first);   /* a forced order that did not happen tested nothing */
    }
"""
    s += """    struct socket *a1;
    struct netaddr peer;
    CHECK(nt_ksock_accept(ls, &a1, &peer) == 0 && peer.port >= NET_EPHEMERAL_LO);
    struct netaddr lt_n1;
    CHECK(ksock_getsockname(c1, &lt_n1) == 0);
    bool lt_is_c1 = peer.port == lt_n1.port;
    kprintf("LTPROBE: accepted %s\\n", lt_is_c1 ? "c1" : "c2");
    CHECK(ksock_sendto(a1, "hi", 2, NULL) == 2);
    char b[4];
"""
    if fix:
        s += """    struct socket *lt_mine = lt_is_c1 ? c1 : c2, *lt_other = lt_is_c1 ? c2 : c1;   /* LTPROBE --fix */
    CHECK(ksock_recvfrom(lt_mine, b, 4, NULL) == 2 && memcmp(b, "hi", 2) == 0);
    /* Closing the listener resets the still-queued connection. */
    nt_ksock_put(ls);
    thread_sleep_ms(20);
    CHECK(ksock_recvfrom(lt_other, b, 4, NULL) < 0 || ksock_sendto(lt_other, "x", 1, NULL) < 0);"""
    else:
        s += """    CHECK(ksock_recvfrom(c1, b, 4, NULL) == 2 && memcmp(b, "hi", 2) == 0);
    /* Closing the listener resets the still-queued connection. */
    nt_ksock_put(ls);
    thread_sleep_ms(20);
    CHECK(ksock_recvfrom(c2, b, 4, NULL) < 0 || ksock_sendto(c2, "x", 1, NULL) < 0);"""
    return s


def sha(p):
    return hashlib.sha256(open(p, 'rb').read()).hexdigest()


def write_atomic(path, data):
    # Whole or not at all: a probe interrupted mid-write must leave every
    # file either as it was or as intended, never half of each.
    tmp = path + '.probe-tmp'
    with open(tmp, 'wb') as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())
    if os.path.exists(path):   # keep the file's mode: the umask must not change it
        os.chmod(tmp, stat.S_IMODE(os.stat(path).st_mode))
    os.replace(tmp, path)


def git_clean(path):
    r = subprocess.run(['git', 'status', '--porcelain', '--', path], capture_output=True, text=True)
    if r.returncode != 0:     # a failed check is not a clean tree
        sys.exit(f'git status failed for {path}: {r.stderr.strip() or r.returncode}')
    return not r.stdout.strip()


def apply_files(fl):
    """Patch every file in `fl`, or none. Every patch is built in memory
    first; then the stamp is written, recording each file's original and
    patched hash; then each backup and each file is replaced atomically.
    From the stamp on, every file is exactly its original or its patched
    bytes, so revert can finish whatever an interruption left."""
    if os.path.exists(STAMP):
        sys.exit('already applied (or an apply was interrupted): run revert first')
    if os.path.exists(STAMP + '.probe-tmp'):
        os.remove(STAMP + '.probe-tmp')  # the stamp is written first: without it nothing was patched
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
            os.remove(STAMP + '.probe-tmp')   # an apply interrupted before its stamp: nothing was patched
            sys.exit('not applied (removed a partial stamp an interrupted apply left)')
        sys.exit('not applied')
    entries = [line.split() for line in open(STAMP).read().split('\n') if line]
    for path, patched, orig in entries:
        cur = sha(path)
        if cur == orig:
            continue                     # never patched, or already restored
        if cur != patched:
            sys.exit(f'{path} changed since apply; restore by hand from {path + BACKUP}')
        if not os.path.exists(path + BACKUP) or sha(path + BACKUP) != orig:
            sys.exit(f'{path} is still patched and {path + BACKUP} is missing or not its original; restore by hand')
    for path, patched, orig in entries:
        if sha(path) == patched:
            write_atomic(path, open(path + BACKUP, 'rb').read())
    # Every file is original now. The backups and every temp write_atomic can
    # leave go first and the stamp last: while the stamp exists a revert can
    # be run again and finish, and once it is gone nothing is left behind.
    for path, _, _ in entries:
        for leftover in (path + BACKUP, path + '.probe-tmp', path + BACKUP + '.probe-tmp'):
            if os.path.exists(leftover):
                os.remove(leftover)
    if os.path.exists(STAMP + '.probe-tmp'):
        os.remove(STAMP + '.probe-tmp')
    os.remove(STAMP)
    print('reverted')


def apply():
    args = sys.argv[2:]
    force, fix = '--force' in args, '--fix' in args
    if [a for a in args if a not in ('--force', '--fix')]:
        sys.exit('usage: apply [--force] [--fix]')
    files = [(NETTEST, [(OLD_DECL, NEW_DECL), (OLD_STEP, new_step(force, fix))])]
    if force:
        files.append((TCP, [(OLD_INPUT_FN, NEW_INPUT_FN), (OLD_INPUT, NEW_INPUT)]))
    apply_files(files)
    print('applied: net-lo-tcp reports which client accept returned'
          + ('; c1\'s final ACK held until c2 is queued' if force else '')
          + ('; the step receives on the accepted client' if fix else ''))


if __name__ == '__main__':
    {'apply': apply, 'revert': revert}.get(sys.argv[1] if len(sys.argv) > 1 else '',
                                           lambda: sys.exit(__doc__))()
