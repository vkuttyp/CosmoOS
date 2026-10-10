#!/usr/bin/env python3
"""Prove the TCP sender avoids the silly window (RFC 1122 4.2.3.4).

--old reverses the fix commit's tcp.c and tcp.h: with a segment in flight
and the world's window opened by 200 bytes, the old sender sends a 200-byte
segment at once, and net-tcp-sws finds it where a full segment belongs
(docs/testing/flakes.md, 2026-10-10: net-bench's loopback stream at 1 MiB/s
in 300-byte segments on slow CI boots).
"""
import sys
import fixprobe

sys.exit(fixprobe.run(
    "tcp-sws-probe", __doc__,
    "tcp: sender silly-window avoidance (RFC 1122 4.2.3.4)",
    ["kernel-services/network/tcp.c", "kernel/include/kernel/net/tcp.h"],
    ["net-tcp-sws"],
    [],
    ["net-tcp-sws: next data at +1460, 200 bytes (mss 1460)"],
    ["selftest: net-tcp-sws: a 200-byte opening with a segment in flight waited"]))
