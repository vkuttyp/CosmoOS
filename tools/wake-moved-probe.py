#!/usr/bin/env python3
"""Prove a wake queues a thread on the CPU it is on, not the CPU the waker read (S36).

--old reverses the fix commit: sched_wake read the thread's CPU and locked
that CPU's queue without reading it again, so a second waker, held between
the two while the first wake landed and the thread moved and armed its next
wait, queued the thread on the CPU it had left. Two CPUs.
"""
import sys
import fixprobe

sys.exit(fixprobe.run(
    "wake-moved-probe", __doc__,
    "sched_wake re-reads the thread's CPU under the queue's lock (S36)",
    ["kernel/scheduler/sched.c"],
    ["sched-wake-moved"],
    [],
    ["queued the thread on CPU", "a wake queued a thread on the CPU it had left (S36)"],
    ["for a thread since moved", "queued it on CPU"],
    smp=2))
