#!/usr/bin/env python3
"""Prove a lowered irqpoll worker is raised even when switched out at its drop.

irqpoll-boost failed eleven times (nine on CI) with the same 290 ms gap: the
worker dropped to the default priority, was switched out at the drop's
own unlock (a preemption point) before it armed the timer that raises it
again, and the spinner above the default then held it off for its whole
run. irqpoll-drop-preempt makes that switch deterministic through a
one-shot seam at the drop: with --old, which reverses the fix commit's
irqpoll.c, the poll never runs while the spinner does and the test fails
on every boot; fixed, the raise brings the worker back within its hold.
Both architectures.
"""
import sys
import fixprobe

sys.exit(fixprobe.run(
    "irqpoll-drop-probe", __doc__,
    "irqpoll: lower the worker and arm its raise as one step",
    ["kernel/core/irqpoll.c"],
    ["irqpoll-drop-preempt"],
    [],
    ["calls of the poll, 0 while it spun", "switched out at the drop"],
    ["switched out at the drop"]))
