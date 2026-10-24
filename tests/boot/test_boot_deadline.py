#!/usr/bin/env python3
"""install_test.Boot's deadlines: the boot's own, and a command's
(Boot.per_command; docs/testing/flakes.md, "be-space ... 2026-10-24").

be-space missed its per-boot deadline twice on CI's slower x86-64 runners
with every command answered: the boot is forty commands, and its length
is the runner's speed times their number. per_command restates what the
deadline guarded -- a command that never answers -- for each command.
These checks drive Boot against a fake QEMU whose shell answers each
command after a set delay, or never; no guest is booted.
"""

import os
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import install_test  # noqa: E402
from install_test import Boot, Fail, PROMPT  # noqa: E402

FAILURES = []
CHECKS = 0


def check(cond, what):
    global CHECKS
    CHECKS += 1
    print(f"{'ok  ' if cond else 'FAIL'} {what}")
    if not cond:
        FAILURES.append(what)


class FakeShell:
    """A QEMU whose shell prints a prompt `delay` seconds after each line
    typed, or never for a line that starts with "hang"."""

    def __init__(self, boot, delay):
        self.boot = boot
        self.delay = delay
        self.stdin = self
        self.returncode = None

    def write(self, data):
        if data.startswith(b"hang"):
            return

        def answer():
            time.sleep(self.delay)
            with self.boot.lock:
                self.boot.buf += PROMPT

        threading.Thread(target=answer, daemon=True).start()

    def flush(self):
        pass

    def poll(self):
        return None


def fake_boot(timeout, delay):
    b = Boot.__new__(Boot)
    b.name = "fake"
    b.lock = threading.Lock()
    b.prompts_seen = 0
    b.command_budget = None
    b.buf = bytearray()
    b.t0 = time.monotonic()
    b.deadline = b.t0 + timeout
    b.last_line_s = 0.0
    b.proc = FakeShell(b, delay)
    return b


def run_all(b, cmds):
    try:
        for c in cmds:
            b.run(c)
    except Fail as e:
        return str(e)
    return None


def test_the_boot_deadline_bounds_the_total():
    """Without a command budget, ten commands of 0.15 s each cannot fit a
    one-second boot: the deadline is the boot's, as it always was."""
    b = fake_boot(1.0, 0.15)
    err = run_all(b, [f"cmd{i}" for i in range(10)])
    check(err is not None and "before the deadline" in err,
          f"ten 0.15 s commands miss a 1 s boot deadline ({err!r})")


def test_a_command_budget_lets_a_long_boot_finish():
    """With one, the same ten commands each answer inside their own 0.5 s,
    and the boot outlives its own second."""
    b = fake_boot(1.0, 0.15)
    b.per_command(0.5)
    t0 = time.monotonic()
    err = run_all(b, [f"cmd{i}" for i in range(10)])
    check(err is None and time.monotonic() - t0 > 1.0,
          f"ten 0.15 s commands pass with a 0.5 s budget each ({err!r}, {time.monotonic() - t0:.2f} s)")


def test_a_stuck_command_still_fails_on_its_budget():
    """What the deadline is for: a command that never answers fails, and
    within its budget of being typed -- not at some later total."""
    b = fake_boot(1.0, 0.15)
    b.per_command(0.5)
    err = run_all(b, [f"cmd{i}" for i in range(8)])
    t0 = time.monotonic()
    err = err or run_all(b, ["hang here"])
    took = time.monotonic() - t0
    check(err is not None and "'hang here'" in err and 0.4 <= took <= 0.8,
          f"a command that never answers fails {took:.2f} s after it was typed, budget 0.5 s ({err!r})")


def test_the_boot_deadline_still_applies_when_it_is_later():
    """A budget only ever extends the deadline: a command typed early in a
    long boot keeps the boot's own, later deadline."""
    b = fake_boot(5.0, 0.05)
    b.per_command(0.5)
    before = b.deadline
    run_all(b, ["cmd"])
    check(b.deadline == before, "a budget shorter than the boot's remaining time leaves its deadline alone")


if __name__ == "__main__":
    assert install_test.Boot.per_command
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn()
    print(f"{CHECKS - len(FAILURES)} of {CHECKS} checks passed")
    sys.exit(1 if FAILURES else 0)
