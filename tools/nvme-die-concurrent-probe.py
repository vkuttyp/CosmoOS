#!/usr/bin/env python3
"""Prove a second NVMe controller_die gets the first caller's outcome.

--old reverses the fix commit: a second caller read the acknowledgement
flag before the first had written it and reported the disable refused.
"""
import sys
import fixprobe

sys.exit(fixprobe.run(
    "nvme-die-concurrent-probe", __doc__,
    "fix: give every NVMe controller_die caller the first caller's outcome",
    ["drivers/nvme/nvme.c"],
    ["nvme-die-concurrent"],
    ["missing successful NVMe concurrent controller-death proof"],
    ["NVME-DIE-CONCURRENT: FAIL parked=1 first=1 second=0"],
    ["NVME-DIE-CONCURRENT: PASS parked=1 first=1 second=1 second_answered_before_release=0"]))
