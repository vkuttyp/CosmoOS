#!/usr/bin/env python3
"""The self-test budgets come from the kernel's own line
(docs/audit/next-subsystem-watchdog-spent.md).

The kernel arms the hang watchdog before each test at that test's budget
and prints every budget in one `SELFTEST: budgets` line. The harness
judges durations by that line and by nothing of its own, so the two
cannot hold a test to different numbers. These are the host-side checks
on the parse: the defect they guard against lives entirely in what the
harness does with a list of strings.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from run_boot_test import selftest_budgets, budget_failures, selftest_timings  # noqa: E402

FAILURES = []
CHECKS = 0


def check(cond, what):
    global CHECKS
    CHECKS += 1
    print(f"{'ok  ' if cond else 'FAIL'} {what}")
    if not cond:
        FAILURES.append(what)


BUDGETS = "SELFTEST: budgets default=8000 cosmofs-replay=40000 process-user=20000"


def result(name, ms, ok=True):
    return f"SELFTEST: {name:<16} ... {'ok' if ok else 'FAIL: x'} ({ms} ms)"


# The line parses, default and named entries both.
b = selftest_budgets([BUDGETS])
check(b == {"default": 8000, "cosmofs-replay": 40000, "process-user": 20000},
      "the budgets line parses into the default and each named budget")

# A test under its own budget passes; the same duration fails an ordinary test.
lines = [BUDGETS, result("cosmofs-replay", 19000), result("net-bench", 9000)]
f = budget_failures(lines)
check(f == ["self-test net-bench took 9000 ms (budget 8000 ms)"],
      "a named budget covers its test and only its test")

# A named test over its own budget fails, at its own number.
f = budget_failures([BUDGETS, result("cosmofs-replay", 40001)])
check(f == ["self-test cosmofs-replay took 40001 ms (budget 40000 ms)"],
      "a named test over its budget fails at that budget")

# The boundary: equal to the budget is within it.
check(budget_failures([BUDGETS, result("pool", 8000)]) == [], "a test at exactly its budget passes")

# A failed test's duration is judged too.
check(budget_failures([BUDGETS, result("pool", 8500, ok=False)]) != [],
      "a failing test's duration is judged as well")

# No budgets line, tests ran: a failure, not the harness's own numbers.
f = budget_failures([result("pool", 5)])
check(f == ["missing budgets line (SELFTEST: budgets default=...)"],
      "tests without a budgets line fail the run")

# No budgets line and no tests (a boot without self-tests): nothing to judge.
check(budget_failures(["SELFTEST: PASS (0 tests)"]) == [], "no tests, no budgets line: no failure")

# A line without a numeric default is no line.
check(selftest_budgets(["SELFTEST: budgets cosmofs-replay=40000"]) is None,
      "a budgets line without a default is not a budgets line")
check(selftest_budgets(["SELFTEST: budgets default=eight"]) is None,
      "a non-numeric default is not a budget")

# The budgets line is not a result line.
check(selftest_timings([BUDGETS, result("pool", 3)]) == [(3, "pool")],
      "the budgets line is not counted as a test")

print(f"test_selftest_budgets: {CHECKS - len(FAILURES)}/{CHECKS} checks passed")
sys.exit(1 if FAILURES else 0)
