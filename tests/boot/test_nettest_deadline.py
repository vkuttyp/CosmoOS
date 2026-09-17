#!/usr/bin/env python3
"""The network harness's deadlines (docs/audit/next-subsystem-nettest-deadline.md).

`net-harness` failed seven times in a fortnight, on both architectures,
on CI and locally, including on branches that add one Markdown file. The
harness could not say why: it reported `TimeoutError('timed out')` and
nothing about when the guest connected or how much budget was left.

These are host-side checks, and deliberately so. The defect is entirely
in the harness, and a proof that boots a guest would take three minutes
to demonstrate what a socket and a clock show in under one.
"""

import os
import socket
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nettest import NetTest  # noqa: E402

FAILURES = []
CHECKS = 0


def check(cond, what):
    global CHECKS
    CHECKS += 1
    print(f"{'ok  ' if cond else 'FAIL'} {what}")
    if not cond:
        FAILURES.append(what)


def test_no_deadline_before_the_guest_exists():
    """The defect, stated as a property that costs nothing to check.

    `NetTest()` is constructed before QEMU is launched
    (run_boot_test.py:536 against :589). Any deadline armed here is
    counting through a boot it cannot see, and when it expired the
    handler closed the listener underneath a guest that connected after
    it. There must be no timeout on the listener until something is
    waiting for a guest that exists.
    """
    nt = NetTest()
    try:
        check(nt.listener.gettimeout() is None,
              "no deadline is armed on the listener at construction "
              f"(got {nt.listener.gettimeout()!r})")
    finally:
        nt.listener.close()


def test_a_late_connection_is_still_accepted():
    """And the behaviour that property buys.

    A guest that takes its time still finds somewhere to connect: the
    listener is open, and the backlog holds the connection until the
    accept runs.
    """
    nt = NetTest()
    try:
        # Stand in for a boot. Any wait would do; this one keeps the
        # test under a second.
        time.sleep(0.5)

        # What run_when_ready does once the guest reports ready: accept
        # with what is left of the run's budget.
        t = threading.Thread(target=nt._back_server,
                             args=(time.monotonic() + 10.0,), daemon=True)
        t.start()
        time.sleep(0.05)

        s = socket.create_connection(("127.0.0.1", nt.back_port), timeout=5)
        s.sendall(b"cosmo hello\n")
        reply = s.recv(64)
        s.close()
        t.join(5)

        check(nt.results.get("back_request") is True,
              "a connection made after a wait is accepted and read")
        check(reply == b"cosmo world\n",
              f"and answered (got {reply!r})")
        check(isinstance(nt.results.get("back_accept_s"), float),
              "and the time it was accepted is recorded")
    finally:
        try:
            nt.listener.close()
        except OSError:
            pass


def test_the_budget_is_derived_not_fixed():
    """The second half: one budget for the exchange, from the run.

    The harness had two deadlines for the same event -- 150 s derived
    from --timeout for noticing the guest, and a hard-coded 120 s for
    accepting its connection, started earlier. A caller that gives the
    run more time must give the exchange more time with it.
    """
    nt = NetTest()
    try:
        t = threading.Thread(target=nt._back_server,
                             args=(time.monotonic() + 7.0,), daemon=True)
        t.start()
        time.sleep(0.2)
        waited = nt.results.get("back_wait_s")
        check(isinstance(waited, float) and 5.0 < waited <= 7.0,
              f"the accept takes its deadline from the caller (got {waited!r})")
        # Let the thread finish rather than leaving it on the port.
        s = socket.create_connection(("127.0.0.1", nt.back_port), timeout=5)
        s.sendall(b"cosmo hello\n")
        s.recv(64)
        s.close()
        t.join(5)
    finally:
        try:
            nt.listener.close()
        except OSError:
            pass


def test_an_expired_deadline_leaves_nothing_listening():
    """Why an expired deadline was fatal rather than merely late.

    The handler closes the listener on its way out, which is right: the
    exchange is over. It is also why a guest that connected after the
    old deadline expired saw its connection completed by QEMU's user
    networking and then nothing at all -- `ksock_connect` returning 0
    with no echo, which is what every sighting recorded.

    Simulated here with a deadline that expires immediately, because the
    shape is the point and 120 seconds is not.
    """
    nt = NetTest()
    port = nt.back_port
    t = threading.Thread(target=nt._back_server,
                         args=(time.monotonic() + 0.3,), daemon=True)
    t.start()
    t.join(5)
    try:
        socket.create_connection(("127.0.0.1", port), timeout=2).close()
        gone = False
    except OSError:
        gone = True
    check(gone, "an expired deadline leaves nothing listening on the port")
    check(isinstance(nt.results.get("back_gaveup_s"), float),
          "and the harness records when it gave up, not just that it did")


def main():
    for fn in (test_no_deadline_before_the_guest_exists,
               test_a_late_connection_is_still_accepted,
               test_the_budget_is_derived_not_fixed,
               test_an_expired_deadline_leaves_nothing_listening):
        fn()
    if FAILURES:
        print(f"nettest-deadline: FAIL ({len(FAILURES)} of {CHECKS})")
        return 1
    print("nettest-deadline: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
