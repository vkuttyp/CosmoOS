"""The firmware-handover stall and its one retry (docs/testing/flakes.md,
"firmware handover"; five sightings by 2026-10-11).

Now and then QEMU runs, OVMF clears the screen, and then nothing: no loader
banner, no kernel output, for the whole boot. The kernel never ran, so it
is not a kernel failure; but it fails whatever boot it lands in. The rule:

- A boot whose log shows our loader's banner (``cosmoboot-uefi``) within
  HANDOVER_S, or whose QEMU exits within it, has handed over: it is never
  retried, so a kernel that hangs or dies -- after printing anything at
  all -- still fails.
- One that shows neither is killed and started once more, and the result
  says "firmware stall, retried".
- A second stall fails the boot (FirmwareStall): twice in a row is not the
  sporadic stall this exists for.

Shared by tests/boot/run_boot_test.py and the install_test.Boot boots
(test-install, test-busybox, test-net-login).
"""

import time

BANNER = b"cosmoboot-uefi"
HANDOVER_S = 30.0
RETRIED_NOTE = "firmware stall, retried"


class FirmwareStall(Exception):
    pass


def handed_over(read_log, proc, deadline, clock=time.monotonic, sleep=time.sleep, poll_s=0.5):
    """True once `read_log()` holds the loader banner or `proc` has exited;
    False if neither happened by `deadline` (a `clock()` value). The last
    look is at the deadline itself, so a banner printed during the final
    sleep still counts."""
    while True:
        if proc.poll() is not None:
            return True   # it exited: whatever happened is in the log, not a stall
        if BANNER in read_log():
            return True
        now = clock()
        if now >= deadline:
            return False
        sleep(min(poll_s, deadline - now))


def launch_with_retry(launch, read_log, kill, on_retry, handover_s=HANDOVER_S, clock=time.monotonic,
                      sleep=time.sleep):
    """Start a boot with `launch(attempt)` (attempt 0, then 1), watching
    `read_log(attempt)`; a stalled first attempt is killed (`kill(proc)`),
    reported (`on_retry()`), and started once more. Returns (proc, retried).
    Raises FirmwareStall after a second stall, having killed it. A caller
    whose whole boot may take less than HANDOVER_S passes its own bound."""
    for attempt in (0, 1):
        proc = launch(attempt)
        if handed_over(lambda: read_log(attempt), proc, clock() + handover_s, clock=clock, sleep=sleep):
            return proc, attempt == 1
        kill(proc)
        if attempt == 0:
            on_retry()
    raise FirmwareStall(f"the firmware did not hand over within {handover_s:.0f}s, twice (no loader banner)")
