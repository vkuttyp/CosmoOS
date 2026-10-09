#!/usr/bin/env python3
"""Prove a successful AHCI reset leaves no pending error snapshot.

--old reverses the fix commit: after a recovery whose stop was refused,
the reset revived the port with the old err_slot/err_ci still pending.
"""
import sys
import fixprobe

sys.exit(fixprobe.run(
    "ahci-error-reset-probe", __doc__,
    "fix: clear a pending AHCI error when a reset fails every slot",
    ["drivers/storage/ahci.c"],
    ["ahci-error-reset"],
    ["missing successful AHCI reset error-snapshot proof"],
    ["AHCI-ERROR-RESET: FAIL", "pending_before=1 pending_after_reset=1"],
    ["AHCI-ERROR-RESET: PASS", "pending_before=1 pending_after_reset=0"]))
