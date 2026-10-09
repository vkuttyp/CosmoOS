#!/usr/bin/env python3
"""Prove no driver is probed on a device whose DMA a driver retained.

--old reverses the fix commit: after an unacknowledged stop the AHCI and
e1000e functions were probed again on hardware that may still use the
retained rings (the device model clears drvdata after every remove).
"""
import sys
import fixprobe

sys.exit(fixprobe.run(
    "dma-retained-probe", __doc__,
    "fix: refuse to probe a device whose DMA a driver retained",
    ["kernel/device/device.c", "drivers/nvme/nvme.c", "drivers/storage/ahci.c", "drivers/network/e1000e.c",
     "drivers/usb/xhci.c"],
    ["ahci-stop-ack", "e1000e-stop-ack"],
    ["missing successful AHCI stop-acknowledgement proof",
     "missing successful e1000e RX/TX disable-acknowledgement proof"],
    ["rebind_refused=0"],
    ["AHCI-STOP-ACK: PASS", "E1000E-STOP-ACK-SWEEP: PASS", "not probed: a previous driver retained DMA"]))
