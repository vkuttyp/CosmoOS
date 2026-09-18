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
from nettest import (NetTest, BACK_PREVIEW, BACK_BACKLOG,  # noqa: E402
                     BACK_GRACE_S)

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
    check(isinstance(nt.results.get("back_done_s"), float),
          "and the harness records when it gave up, not just that it did")


def test_a_silent_peer_is_recorded_too():
    """The path that raises nothing and still fails.

    A peer that connects and closes without sending leaves no exception
    behind. An instrument that records a time only in the `except`
    prints "gave up nan s" here and discards what did arrive -- so it
    cannot tell silence from a partial write from a wrong answer.

    **What changed with the accept unit**: a silent peer no longer *ends*
    the exchange. Ending on it is the defect -- it is how a connection
    that was never the guest's came to be reported as the guest's
    failure. The harness now records it and keeps waiting for a
    connection that delivers the request, so this test runs to its
    deadline on purpose, and the deadline is short for that reason
    (docs/audit/next-subsystem-nettest-accept.md).
    """
    nt = NetTest()
    try:
        t = threading.Thread(target=nt._back_server,
                             args=(time.monotonic() + 1.5,), daemon=True)
        t.start()
        time.sleep(0.05)
        s = socket.create_connection(("127.0.0.1", nt.back_port), timeout=5)
        s.close()               # connect, say nothing, go away
        t.join(15)

        check(nt.results.get("back_request") is False,
              "a silent peer is a failed request")
        check(isinstance(nt.results.get("back_done_s"), float),
              "and it still records when the exchange ended")
        check(nt.results.get("back_bytes") == 0,
              f"and how much arrived (got {nt.results.get('back_bytes')!r})")
        check(isinstance(nt.results.get("back_accept_s"), float),
              "and that the connection had been accepted")
        check("1 connection(s)" in (nt.results.get("back_roster") or ""),
              f"and it is named in the roster (got {nt.results.get('back_roster')!r})")
    finally:
        try:
            nt.listener.close()
        except OSError:
            pass


def _serve(nt, budget):
    t = threading.Thread(target=nt._back_server,
                         args=(time.monotonic() + budget,), daemon=True)
    t.start()
    time.sleep(0.05)
    return t


def test_a_stale_connection_does_not_win_the_accept():
    """The reproduction, as a test that runs in a second.

    Occupying the single backlog slot before the guest connects
    reproduced `net-harness` on the first boot: the harness accepted the
    intruder, read nothing from it for ten seconds, and reported
    `TimeoutError` while the guest's own connection was reset
    (docs/audit/next-subsystem-nettest-accept.md). The guest's
    connection is the one that delivers the request, so an intruder
    ahead of it must not win.
    """
    nt = NetTest()
    stale = socket.socket()
    try:
        stale.connect(("127.0.0.1", nt.back_port))   # occupies the queue
        t = _serve(nt, 10.0)
        g = socket.create_connection(("127.0.0.1", nt.back_port), timeout=5)
        g.sendall(b"cosmo hello\n")
        reply = g.recv(64)
        g.close()
        t.join(15)

        check(nt.results.get("back_request") is True,
              "a stale connection ahead of the guest does not win the accept")
        check(reply == b"cosmo world\n",
              f"and the reply goes to the guest's connection (got {reply!r})")
    finally:
        for sk in (stale,):
            try:
                sk.close()
            except OSError:
                pass
        try:
            nt.listener.close()
        except OSError:
            pass


def test_the_intruder_is_named():
    """And it is recorded, which is the sentence 22 sightings needed.

    The old failure line said "connection accepted at 90.9s" -- a time
    without an identity. Whatever else happens, the harness must be able
    to say *which* connections reached the port and what each sent.
    """
    nt = NetTest()
    stale = socket.socket()
    try:
        stale.connect(("127.0.0.1", nt.back_port))
        t = _serve(nt, 10.0)
        g = socket.create_connection(("127.0.0.1", nt.back_port), timeout=5)
        g.sendall(b"cosmo hello\n")
        g.recv(64)
        g.close()
        t.join(15)

        roster = nt.results.get("back_roster") or ""
        check(len(nt.back_conns) == 2,
              f"both connections are recorded (got {len(nt.back_conns)})")
        check("2 connection(s)" in roster,
              f"and the roster counts them (got {roster!r})")
        check("[the request]" in roster,
              "and marks which one carried the request")
        check(sum(1 for c in nt.back_conns if c["delivered"]) == 1,
              "exactly one connection is the guest's")
    finally:
        try:
            stale.close()
        except OSError:
            pass
        try:
            nt.listener.close()
        except OSError:
            pass


def test_wrong_data_is_not_the_guest():
    """A connection that answers, but not with the request.

    Silence is not the only way to be the wrong connection.
    """
    nt = NetTest()
    try:
        t = _serve(nt, 1.5)
        s = socket.create_connection(("127.0.0.1", nt.back_port), timeout=5)
        s.sendall(b"not the request\n")
        t.join(15)
        s.close()

        check(nt.results.get("back_request") is False,
              "a connection sending the wrong thing is not the guest's")
        check(any(c["bytes"] > 0 and not c["delivered"] for c in nt.back_conns),
              "and it is recorded with what it sent")
        check("not the request" in (nt.results.get("back_roster") or ""),
              "and the roster shows the payload")
    finally:
        try:
            nt.listener.close()
        except OSError:
            pass


def test_the_request_may_arrive_on_the_second_connection():
    """The guest need not be first, and the reply must follow the request."""
    nt = NetTest()
    junk = socket.socket()
    try:
        t = _serve(nt, 10.0)
        junk.connect(("127.0.0.1", nt.back_port))
        junk.sendall(b"junk\n")
        time.sleep(0.1)
        g = socket.create_connection(("127.0.0.1", nt.back_port), timeout=5)
        g.sendall(b"cosmo hello\n")
        reply = g.recv(64)
        g.close()
        t.join(15)

        check(nt.results.get("back_request") is True,
              "the request is found on the second connection")
        check(reply == b"cosmo world\n",
              f"and the reply goes out on that one (got {reply!r})")
        check(nt.results.get("back_bytes") == len(b"cosmo hello\n"),
              f"and the recorded bytes are the guest's "
              f"(got {nt.results.get('back_bytes')!r})")
    finally:
        try:
            junk.close()
        except OSError:
            pass
        try:
            nt.listener.close()
        except OSError:
            pass


def test_each_connection_gets_its_own_receive_budget():
    """Design point 5: the guest is not charged for the intruder's time.

    The accept budget and a connection's receive budget are different
    questions. A guest accepted near the end of the accept budget still
    has its own BACK_RECV_S to deliver, measured from *its* accept --
    otherwise an intruder early in the run silently shortens the
    guest's window.

    Kept fast deliberately: the accept budget is one second and the
    guest sends after it has expired, which is the property, without
    waiting out the ten-second receive budget.
    """
    nt = NetTest()
    idler = socket.socket()
    try:
        idler.connect(("127.0.0.1", nt.back_port))   # burns the run's budget
        t = _serve(nt, 1.0)
        g = socket.create_connection(("127.0.0.1", nt.back_port), timeout=5)
        time.sleep(1.3)          # the accept budget is gone by now
        g.sendall(b"cosmo hello\n")
        reply = g.recv(64)
        g.close()
        t.join(20)

        check(nt.results.get("back_request") is True,
              "a connection accepted inside the budget may deliver after it")
        check(reply == b"cosmo world\n",
              f"and is still answered (got {reply!r})")
    finally:
        try:
            idler.close()
        except OSError:
            pass
        try:
            nt.listener.close()
        except OSError:
            pass


def test_the_preview_is_bounded():
    """A foreign connection may send megabytes; the line must not.

    The full count of what was read is recorded, but the preview is
    capped at BACK_PREVIEW so the roster stays a line. The cap matches
    what the harness already kept for the guest's own connection.
    """
    nt = NetTest()
    payload = bytes(range(65, 91)) * 4 + b"\n"       # 105 bytes, not the request
    try:
        t = _serve(nt, 1.5)
        s = socket.create_connection(("127.0.0.1", nt.back_port), timeout=5)
        s.sendall(payload)
        t.join(15)
        s.close()

        rec = nt.back_conns[0] if nt.back_conns else None
        check(rec is not None, "the connection is recorded")
        if rec is not None:
            check(len(rec["preview"]) == BACK_PREVIEW,
                  f"the preview is exactly {BACK_PREVIEW} bytes "
                  f"(got {len(rec['preview'])})")
            check(rec["preview"] == payload[:BACK_PREVIEW],
                  "and it is the first bytes, not a tail or a hash")
            check(rec["bytes"] >= BACK_PREVIEW,
                  f"and the byte count is what was read, not the preview "
                  f"length (got {rec['bytes']})")
            roster = nt.results.get("back_roster") or ""
            check(len(roster) < 400,
                  f"and the roster stays a line (got {len(roster)} chars)")
    finally:
        try:
            nt.listener.close()
        except OSError:
            pass


def test_arrivals_without_the_request_still_fail():
    """Design point 6: a deeper backlog must not turn a fault into a pass.

    The pass condition is that the guest's request arrived -- not that
    some connection did.
    """
    nt = NetTest()
    a, b = socket.socket(), socket.socket()
    try:
        t = _serve(nt, 1.5)
        a.connect(("127.0.0.1", nt.back_port))
        b.connect(("127.0.0.1", nt.back_port))
        b.sendall(b"nope\n")
        t.join(15)

        check(nt.results.get("back_request") is False,
              "connections that never deliver the request are still a failure")
        nt.results["ready"] = True
        nt.results["tcp_echo"] = True
        nt.results["udp_ok"] = True
        nt.results["quit_sent"] = True
        msgs = nt.failures()
        check(len(msgs) == 1, f"and it is reported once (got {len(msgs)})")
        check("connection(s)" in msgs[0],
              f"and the failure line carries the roster (got {msgs[0]!r})")
        check("per connection from its own accept" in msgs[0],
              "and says the receive budget is per connection")
    finally:
        for sk in (a, b):
            try:
                sk.close()
            except OSError:
                pass
        try:
            nt.listener.close()
        except OSError:
            pass


def test_the_backlog_is_deeper_than_one():
    """The measured precondition, pinned.

    A backlog of one made the *next* connect stall silently -- the SYN
    dropped, no refusal -- so a stale connection both won the accept and
    blocked the guest's. The depth is the fix's floor, and a regression
    to one would restore the defect without failing anything else.
    """
    nt = NetTest()
    socks = []
    try:
        check(BACK_BACKLOG > 1, f"the backlog is deeper than one (got {BACK_BACKLOG})")
        # Two connections queue without the harness accepting anything.
        for _ in range(2):
            sk = socket.socket()
            sk.settimeout(2.0)
            sk.connect(("127.0.0.1", nt.back_port))
            socks.append(sk)
        check(True, "two connections queue with nothing accepting them")
    except OSError as e:
        check(False, f"two connections queue with nothing accepting them ({e!r})")
    finally:
        for sk in socks:
            try:
                sk.close()
            except OSError:
                pass
        try:
            nt.listener.close()
        except OSError:
            pass


def test_foreign_connections_are_closed_not_leaked():
    """A connection the harness rejects must not be left open.

    The loop drops a connection from its watch set on three paths: the
    peer closed, it sent something that is not the request, or its own
    receive budget expired. Each path now closes the socket explicitly.

    **This is not a bug-proof, and the distinction is worth stating.**
    Review asked for these closes on the grounds that descriptors would
    accumulate until `select` failed and truncated the roster. Measured
    against the version without them, that does not happen: nothing
    retains a rejected socket -- `back_conns` holds records, not sockets
    -- so CPython's refcounting closes it as soon as it leaves `live`,
    and this test passes either way. The closes are kept because
    resource lifetime should not depend on interpreter internals, not
    because a leak was observed.

    What the test does guard is the regression that would make the
    claimed failure real: someone retaining rejected sockets in a list
    -- for the roster, say -- without closing them. Asserted from the
    peer's side, because "the harness closed it" is the property and a
    descriptor count is only a proxy for it.
    """
    nt = NetTest()
    peers = []
    try:
        t = _serve(nt, 1.5)
        for i in range(5):
            sk = socket.create_connection(("127.0.0.1", nt.back_port), timeout=5)
            sk.sendall(b"junk %d\n" % i)
            peers.append(sk)
        t.join(15)

        closed = 0
        for sk in peers:
            sk.settimeout(2.0)
            try:
                if sk.recv(16) == b"":
                    closed += 1
            except OSError:
                closed += 1     # reset counts as closed too
        check(closed == len(peers),
              f"every rejected connection is closed by the harness "
              f"({closed} of {len(peers)})")
        check(len(nt.back_conns) == len(peers),
              f"and all of them are still in the roster "
              f"(got {len(nt.back_conns)})")
    finally:
        for sk in peers:
            try:
                sk.close()
            except OSError:
                pass
        try:
            nt.listener.close()
        except OSError:
            pass


def test_a_failed_exchange_does_not_eat_the_run():
    """A harness failure must not become a run-wide timeout.

    Regression from this unit's own CI. Making the loop wait for a
    connection that delivers the request -- rather than ending on the
    first one, which is the defect -- also made it wait out the *whole*
    remaining budget when none ever did. On PR #177's aarch64 job that
    meant giving up at 157.0s where the previous harness gave up at
    100.9s, which left no budget for the rest of the boot: the run
    exceeded its 180 s timeout and every later marker went missing, so
    one harness failure hid the entire tail of the boot.

    The guest makes exactly one back-connection attempt and never
    retries, so once a connection has arrived and resolved without the
    request, more waiting cannot help. The bound is BACK_GRACE_S after
    that, not the rest of the run.
    """
    nt = NetTest()
    budget = 60.0                      # a generous run budget
    try:
        started = time.monotonic()
        t = _serve(nt, budget)
        s = socket.create_connection(("127.0.0.1", nt.back_port), timeout=5)
        s.close()                      # arrive, say nothing, go away
        t.join(budget + 15)
        elapsed = time.monotonic() - started

        check(nt.results.get("back_request") is False,
              "the exchange still fails")
        check(elapsed < budget / 2,
              f"and gives up on the grace, not the run's budget "
              f"({elapsed:.1f}s of {budget:.0f}s)")
        check(elapsed >= BACK_GRACE_S - 1.0,
              f"but does wait the grace, so a second connection could "
              f"still arrive (got {elapsed:.1f}s)")
    finally:
        try:
            nt.listener.close()
        except OSError:
            pass


def test_no_connection_at_all_still_waits_the_budget():
    """And the bound must not undo the deadline unit.

    The grace applies only once a connection has *arrived*. While none
    has, a guest that connects late must still be found -- which is the
    property `test_a_late_connection_is_still_accepted` buys and the
    whole point of the earlier unit.
    """
    nt = NetTest()
    try:
        started = time.monotonic()
        t = _serve(nt, 3.0)
        # Nothing connects until well past the grace would have expired
        # had one arrived at t0.
        time.sleep(2.0)
        s = socket.create_connection(("127.0.0.1", nt.back_port), timeout=5)
        s.sendall(b"cosmo hello\n")
        reply = s.recv(64)
        s.close()
        t.join(20)

        check(nt.results.get("back_request") is True,
              "a guest that connects late is still found")
        check(reply == b"cosmo world\n",
              f"and answered (got {reply!r})")
        check(time.monotonic() - started >= 2.0,
              "and the harness really did wait for it")
    finally:
        try:
            nt.listener.close()
        except OSError:
            pass


def main():
    for fn in (test_no_deadline_before_the_guest_exists,
               test_a_late_connection_is_still_accepted,
               test_the_budget_is_derived_not_fixed,
               test_an_expired_deadline_leaves_nothing_listening,
               test_a_silent_peer_is_recorded_too,
               test_a_stale_connection_does_not_win_the_accept,
               test_the_intruder_is_named,
               test_wrong_data_is_not_the_guest,
               test_the_request_may_arrive_on_the_second_connection,
               test_each_connection_gets_its_own_receive_budget,
               test_the_preview_is_bounded,
               test_arrivals_without_the_request_still_fail,
               test_the_backlog_is_deeper_than_one,
               test_foreign_connections_are_closed_not_leaked,
               test_a_failed_exchange_does_not_eat_the_run,
               test_no_connection_at_all_still_waits_the_budget):
        fn()
    if FAILURES:
        print(f"nettest-deadline: FAIL ({len(FAILURES)} of {CHECKS})")
        return 1
    print("nettest-deadline: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
