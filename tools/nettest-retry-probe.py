#!/usr/bin/env python3
"""
nettest-retry-probe.py -- does a second connection through the same
slirp succeed when the first one is reset?

`net-harness` failed seven CI jobs on 2026-09-22 alone and reproduces on
about one local boot in twenty (docs/testing/flakes.md, "The count"). The defect is localised
and is not in this kernel: QEMU's user-mode networking resets the
guest's half of one connection while keeping its own half open, and
answers a probe through the same instance milliseconds later
(docs/audit/2026-09-deferred-work-inventory.md, the `net-harness` row,
sighting thirty).

That makes a retry the obvious repair, and this measures the one thing a
retry depends on: that a *fresh* connection works when the first has
been reset. The probe adds a second, diagnostic-only exchange on the
failure path -- it does not change the verdict, so the boot still fails
and the log says what the retry would have done:

    NETTEST: retry probe: connect 0 in 2 ms, sent 12, recv 12 -> WOULD HAVE PASSED

Three outcomes, and the third is why the deadline is reported separately:
the reply came back (a retry repairs this), the connection was reset
again (it does not), or slirp accepted it and forwarded nothing for five
seconds (it does not, and that is a different defect from a reset).

The read is non-blocking with a five-second deadline. The failure this
probe runs after is slirp accepting a connection and never forwarding
it, so a blocking read is the one thing that could turn a diagnosed
failure into a hung boot with no answer at all.

Usage:

    python3 tools/nettest-retry-probe.py apply
    while ! grep -q 'retry probe' out/x86_64-debug/boot-test.log; do gmake test; done
    python3 tools/nettest-retry-probe.py revert

`apply` refuses a file with uncommitted changes and `revert` restores
its own snapshot, so it can only undo what it did.
"""

import os
import shutil
import subprocess
import sys

NETTEST = 'kernel-services/network/nettest.c'
BACKUP = '.nettest-retry-probe.orig'

PROBE = r'''
    /* --- RETRY PROBE (tools/nettest-retry-probe.py; not for merge) ---
     * The failure has just been recorded. Try the same exchange again on
     * a fresh socket, and say what happened. Diagnostic only: the
     * verdict above is untouched, so the boot still fails and the log
     * carries the answer. The harness keeps accepting for BACK_GRACE_S
     * (10 s) after a connection settles, so this lands inside its
     * window. */
    if (!client_ok) {
        struct socket *c2 = NULL;
        if (ksock_create(COSMO_AF_INET, COSMO_SOCK_STREAM, 0, &c2) == 0) {
            uint64_t rns = clock_now_ns();
            int rrc = ksock_connect(c2, &host);
            uint64_t rms = (clock_now_ns() - rns) / 1000000ull;
            int64_t rsent = -1, rgot = -1;
            char rbuf[32];
            if (rrc == 0) {
                rsent = ksock_sendto(c2, "cosmo hello\n", 12, NULL);
                if (rsent == 12) {
                    /* Bounded, and the bound matters more here than in
                     * the test it imitates: the failure this probe runs
                     * after is *slirp accepting a connection and never
                     * forwarding it*, so a blocking read is exactly the
                     * thing that would hang. Non-blocking plus a
                     * five-second deadline, which is half the harness's
                     * own receive budget (found in review of this
                     * report). */
                    ksock_set_nonblock(c2, true);
                    uint64_t rdl = clock_deadline_ns(5000ull * 1000000ull);
                    for (;;) {
                        rgot = ksock_recvfrom(c2, rbuf, sizeof(rbuf), NULL);
                        if (rgot != -EAGAIN || clock_deadline_passed(rdl))
                            break;
                        thread_sleep_ms(5);
                    }
                }
            }
            /*
             * Three outcomes, and they are the whole measurement:
             *
             *   the reply came back   a retry repairs this flake
             *   reset again           slirp kills a fresh connection too
             *   accepted and silent   slirp took it and forwarded nothing,
             *                         which a retry does NOT repair
             *
             * The third is the deadline case (`-EAGAIN` out of the
             * non-blocking read) and it is the one that would change the
             * unit's answer, so it must not be printed as a reset
             * (found in review of this report).
             */
            bool again_ok = rgot == 12 && memcmp(rbuf, "cosmo world\n", 12) == 0;
            const char *verdict = again_ok ? "WOULD HAVE PASSED"
                                : rgot == -EAGAIN ? "accepted and silent for 5 s (a retry would NOT have helped)"
                                : rrc != 0 ? "the retry's connect failed too"
                                : "reset again";
            kprintf("NETTEST: retry probe: connect %d in %llu ms, sent %lld, recv %lld -> %s\n",
                    rrc, (unsigned long long)rms, (long long)rsent, (long long)rgot, verdict);
            ksock_put(c2);
        } else {
            kprintf("NETTEST: retry probe: could not create a socket\n");
        }
    }
    /* --- end retry probe --- */
'''


def apply_():
    if os.path.exists(NETTEST + BACKUP):
        sys.exit("%s%s exists: a previous run was not reverted. Revert first." % (NETTEST, BACKUP))
    dirty = subprocess.run(['git', 'status', '--porcelain', '--', NETTEST],
                           capture_output=True, text=True, check=True).stdout.strip()
    if dirty:
        sys.exit("uncommitted changes in %s; commit or stash them first" % NETTEST)
    s = open(NETTEST).read()
    # After the failure has been reported and before the echo service loop.
    anchor = "    /* Serve echo until the harness sends QUIT (60 s budget). */"
    if s.count(anchor) != 1:
        sys.exit("anchor appears %d times, expected once" % s.count(anchor))
    shutil.copyfile(NETTEST, NETTEST + BACKUP)
    open(NETTEST, 'w').write(s.replace(anchor, PROBE + "\n" + anchor, 1))
    print("applied: boot until 'NETTEST: retry probe' appears in the boot log")


def revert():
    if not os.path.exists(NETTEST + BACKUP):
        sys.exit("no snapshot found: nothing to revert")
    shutil.move(NETTEST + BACKUP, NETTEST)
    print("reverted")


if __name__ == '__main__':
    if len(sys.argv) != 2 or sys.argv[1] not in ('apply', 'revert'):
        sys.exit(__doc__)
    (apply_ if sys.argv[1] == 'apply' else revert)()
