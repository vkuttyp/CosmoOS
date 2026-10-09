#!/usr/bin/env python3
"""Prove a quarantined USB request stays quarantined on a repeated cancel.

--old reverses the fix commit: after a later acknowledged halt, a second
xhci_cancel returned 0 for a request whose buffers the controller kept.
"""
import sys
import fixprobe

sys.exit(fixprobe.run(
    "xhci-recancel-probe", __doc__,
    "fix: keep a quarantined USB request on a repeated cancel",
    ["drivers/usb/xhci.c"],
    ["xhci-cancel-ack"],
    ["missing successful xHCI cancel acknowledgement proof"],
    ["XHCI-CANCEL-ACK: outcome=quarantined FAIL", "recancel=0"],
    ["XHCI-CANCEL-ACK: outcome=quarantined PASS", "recancel=-5"]))
