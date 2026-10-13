#!/usr/bin/env python3
"""Prove a mutex waiter overtaken once gets the mutex at the next unlock (S33).

sched-migrate-stress's second mutex worker failed on CI three times on
2026-10-13: on a ready list and being run, its round count still for 2 s,
because its partner unlocked and locked again before it ran, every time.
The fix commit makes a waiter that was woken and found the mutex taken
again the owner at the next unlock. mutex-handoff makes the overtaking
deterministic (one CPU, the owner yielding while it holds the mutex): with
--old, which reverses the fix commit's mutex.c and mutex.h, the waiter gets
the mutex only after the owner's 20th round and the test fails on every
boot; fixed, after its 2nd. Both architectures.
"""
import sys
import fixprobe

sys.exit(fixprobe.run(
    "mutex-handoff-probe", __doc__,
    "mutex: hand the mutex to a waiter that was overtaken",
    ["kernel/scheduler/mutex.c", "kernel/include/kernel/mutex.h"],
    ["mutex-handoff"],
    [],
    ["mutex-handoff ... FAIL"],
    ["selftest: mutex-handoff: the waiter got the mutex after the owner's round 2 of 20"]))
