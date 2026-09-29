#!/usr/bin/env python3
"""
zero-window-probe.py -- why does net-hostinput's window update sometimes
not release the 50 bytes it expects?

Three sightings of one check (docs/testing/flakes.md, "net-hostinput: no
data after a window update"), all x86-64:

    CHECK(hin_recv(u, IPPROTO_TCP, 40001, &sg, HIN_TRIES) && sg.paylen == 50 && sg.seq == iss1 + 101)

The test closes the peer's window, sends 50 bytes, checks for 15 tries that
nothing of 50 bytes goes out, opens the window, and expects the first
segment to be all 50 bytes at iss1 + 101. But a zero window with data
waiting sends a one-byte probe when the retransmit timer fires
(tcp.c, tcp_output_locked), and the probe advances snd_nxt: after the
update the rest goes out at iss1 + 102. The "blocked" check passes a
one-byte probe (it is under 50).

The probe logs, at that step: how long the blocked phase took, the pcb's
RTO when it began, what (if anything) arrived during it, and the first
segment after the window update (ZWPROBE lines).

    python3 tools/zero-window-probe.py apply [--early MS] [--stretch MS] [--fix]
    gmake ARCH=x86_64 test
    grep ZWPROBE out/x86_64-debug/boot-test.log
    python3 tools/zero-window-probe.py revert

--stretch MS pauses MS before the window update, as a slow host does, so
the timer fires first. --early MS pauses MS after the send, before the
blocked check, so the probe goes out inside the blocked phase and that
check sees it. --fix is the candidate check: the 50 bytes arrive
from iss1 + 101 with no gap, a probe byte already seen during the blocked
phase counting as the first; a segment may repeat bytes already covered
(a second probe, or a retransmission from snd_una), never skip any.

It patches the check as it was before the zero-window unit (main
46dbd3b4 and earlier). On a tree with the fix (hin_recv_stream), its
anchor is not found, and apply exits without touching anything.

`apply` and `revert` are those of tools/nvme-admin-probe.py: stamp first,
every file replaced atomically, the stamp removed last, finished by
running revert (again).
"""

import hashlib
import os
import subprocess
import sys

NETTEST = 'kernel-services/network/nettest.c'
BACKUP = '.zero-window-probe.orig'
STAMP = '.zero-window-probe.applied'

OLD = """    CHECK(ksock_sendto(a1, data, 50, NULL) == 50);
    CHECK(!(hin_recv(u, IPPROTO_TCP, 40001, &sg, 15) && sg.paylen >= 50));   /* blocked by the zero window */
    l4len = hin_mk_tcp(l4, w[0], u_ip, 40001, 2222, 1003, iss1 + 101, TH_ACK, 64240, NULL, 0);   /* window update */
    CHECK(hin_send(u, umac, wmac[0], w[0], u_ip, IPPROTO_TCP, l4, l4len));
    CHECK(hin_recv(u, IPPROTO_TCP, 40001, &sg, HIN_TRIES) && sg.paylen == 50 && sg.seq == iss1 + 101);"""


def new_block(stretch, fix, early=0):
    s = """    uint64_t zw_rto = a1->tcp->rto_ns;   /* ZWPROBE */
    CHECK(ksock_sendto(a1, data, 50, NULL) == 50);
    uint64_t zw_t0 = clock_now_ns();
"""
    if early:
        s += f"""    thread_sleep_ms({early});   /* ZWPROBE --early: a slow host, before the blocked check */
"""
    s += """    bool zw_blocked_seen = hin_recv(u, IPPROTO_TCP, 40001, &sg, 15);
    uint32_t zw_bseq = zw_blocked_seen ? sg.seq - iss1 : 0, zw_blen = zw_blocked_seen ? sg.paylen : 0;
    uint64_t zw_blocked_ms = (clock_now_ns() - zw_t0) / 1000000;
    CHECK(!(zw_blocked_seen && sg.paylen >= 50));   /* blocked by the zero window */
"""
    if stretch:
        s += f"""    thread_sleep_ms({stretch});   /* ZWPROBE --stretch: a slow host */
"""
    s += """    l4len = hin_mk_tcp(l4, w[0], u_ip, 40001, 2222, 1003, iss1 + 101, TH_ACK, 64240, NULL, 0);   /* window update */
    CHECK(hin_send(u, umac, wmac[0], w[0], u_ip, IPPROTO_TCP, l4, l4len));
    bool zw_got = hin_recv(u, IPPROTO_TCP, 40001, &sg, HIN_TRIES);
    kinfo("ZWPROBE: rto %llu ms; blocked phase %llu ms, saw %s (seq +%u, %u bytes); after the update: %s seq +%u, %u bytes",
          (unsigned long long)(zw_rto / 1000000), (unsigned long long)zw_blocked_ms,
          zw_blocked_seen ? "a segment" : "nothing", zw_bseq, zw_blen, zw_got ? "a segment" : "nothing",
          zw_got ? sg.seq - iss1 : 0, zw_got ? sg.paylen : 0);
"""
    if fix:
        s += """    {   /* ZWPROBE --fix: the 50 bytes from iss1 + 101 with no gap, a probe byte counted.
         * A segment may start at or before what is covered (a repeated probe, or a
         * retransmission from snd_una), never after it, and never past the 50. */
        uint32_t covered = (zw_blocked_seen && zw_bseq == 101 && zw_blen == 1) ? 1 : 0;
        bool ok_run = zw_got;
        unsigned zw_segs = 0;
        for (unsigned k = 0; ok_run && covered < 50 && k < 8; k++) {
            uint32_t off = sg.seq - (iss1 + 101);
            if (off > covered || sg.paylen == 0 || off + sg.paylen > 50) {
                ok_run = false;
                break;
            }
            zw_segs++;
            if (off + sg.paylen > covered)
                covered = off + sg.paylen;
            if (covered < 50)
                ok_run = hin_recv(u, IPPROTO_TCP, 40001, &sg, HIN_TRIES);
        }
        kinfo("ZWPROBE: --fix read %u segments after the update, covered %u", zw_segs, covered);
        CHECK(ok_run && covered == 50);
    }"""
    else:
        s += """    CHECK(zw_got && sg.paylen == 50 && sg.seq == iss1 + 101);"""
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
    fix = '--fix' in args
    args = [a for a in args if a != '--fix']
    early = 0
    if args[:1] == ['--early']:
        if len(args) < 2 or not args[1].isdigit():
            sys.exit('usage: apply [--early MS] [--stretch MS] [--fix]')
        early = int(args[1])
        args = args[2:]
    stretch = 0
    if args[:1] == ['--stretch']:
        if len(args) != 2 or not args[1].isdigit():
            sys.exit('usage: apply [--early MS] [--stretch MS] [--fix]')
        stretch = int(args[1])
    elif args:
        sys.exit('usage: apply [--early MS] [--stretch MS] [--fix]')
    apply_files([(NETTEST, [(OLD, new_block(stretch, fix, early))])])
    print('applied: net-hostinput zero-window step instrumented'
          + (f'; {early} ms before the blocked check' if early else '')
          + (f'; {stretch} ms before the window update' if stretch else '')
          + ('; the candidate check' if fix else ''))


if __name__ == '__main__':
    {'apply': apply, 'revert': revert}.get(sys.argv[1] if len(sys.argv) > 1 else '',
                                           lambda: sys.exit(__doc__))()
