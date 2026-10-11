#!/usr/bin/env python3
"""Unit test of tests/boot/firmware_stall.py (make host-test).

A fake clock, fake QEMU processes and fake logs drive the rule:
- the banner within the bound: one launch, no retry;
- QEMU exiting within the bound: one launch, no retry (whatever it printed
  is the boot's to judge);
- any kernel output (after the banner) disables the retry -- the banner
  ends the watch, even if nothing more comes;
- nothing at all: killed, "firmware stall, retried" reported, launched
  again, and the second attempt's banner accepted (retried = True);
- nothing at all twice: both killed and FirmwareStall raised;
- the banner arriving only after the bound is too late: a stall.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import firmware_stall as fs  # noqa: E402


class Clock:
    def __init__(self):
        self.t = 0.0

    def __call__(self):
        return self.t

    def sleep(self, s):
        self.t += s


class Proc:
    def __init__(self, exit_at=None, clock=None):
        self.exit_at, self.clock, self.killed = exit_at, clock, False

    def poll(self):
        return 0 if self.exit_at is not None and self.clock() >= self.exit_at else None


def run(script, handover=30.0):
    """`script`: per attempt, (banner_at or None, exit_at or None)."""
    clock = Clock()
    procs, retries = [], []

    def launch(attempt):
        procs.append(Proc(script[attempt][1], clock))
        return procs[-1]

    def read_log(attempt):
        at = script[attempt][0]
        return b"firmware noise " + (fs.BANNER + b" v1\nkernel..." if at is not None and clock() >= at else b"")

    def kill(p):
        p.killed = True

    try:
        _, retried = fs.launch_with_retry(launch, read_log, kill, lambda: retries.append(1), handover_s=handover,
                                          clock=clock, sleep=clock.sleep)
        return ("ok", retried, len(procs), [p.killed for p in procs], len(retries))
    except fs.FirmwareStall:
        return ("stall", None, len(procs), [p.killed for p in procs], len(retries))


def check(cond, what):
    if not cond:
        print(f"FAIL: {what}")
        sys.exit(1)
    print(f"ok: {what}")


def main():
    check(run([(3.0, None)]) == ("ok", False, 1, [False], 0), "the banner in time: one launch, no retry")
    check(run([(None, 5.0)]) == ("ok", False, 1, [False], 0), "QEMU exiting in time: no retry")
    check(run([(1.0, None)])[1] is False, "kernel output ends the watch: a later hang is never retried")
    check(run([(None, None), (2.0, None)]) == ("ok", True, 2, [True, False], 1),
          "a stall: killed, reported, launched again, the second banner accepted")
    check(run([(None, None), (None, None)]) == ("stall", None, 2, [True, True], 1),
          "two stalls: both killed, FirmwareStall raised")
    check(run([(45.0, None), (None, None)])[0] == "stall", "a banner after the bound is a stall")
    print("PASS: firmware-stall")


if __name__ == "__main__":
    main()
