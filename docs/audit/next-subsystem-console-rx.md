# NEXT SUBSYSTEM — the console's receive interrupt is cleared after the drain

> Constitution §68 report. Takes up the aarch64 console stall recorded
> in `docs/testing/flakes.md` ("An aarch64 release boot whose console
> stopped mid-line after an interrupt"): three sightings, the third on
> `main` itself (run 35942625646, 2026-09-24), each an interactive shell
> that stopped taking input for good. The entry's own advice after the
> second was "instrument before theorising"; this report does, finds the
> cause in four lines of the PL011 driver, and proposes the fix and a
> test that does not wait for luck.
>
> **Built (PR #241).** As designed, with these differences:
> - **The test checks the latch, not the tty.** `console-rx-clear` takes
>   the PL011's receive interrupt away from the GIC for its window, so
>   nothing reads the byte. It checks what the old order destroyed: after
>   the hook's byte arrives, the raw receive interrupt (`RIS.RXRIS`) must
>   still be pending. It then drains the byte itself, because a test byte
>   is not input.
> - **The byte is `'\r'`.** QEMU's PL011 in loopback also sends the byte
>   out on the line, so it shows up in the serial log. A carriage return
>   leaves no visible mark there.
> - **The test lives in `pl011.c`**, which owns the registers and the
>   shared `rx_service(tty, after_drain)` that both `rx_irq` and the test
>   call. On x86-64, `serial.c` has a stub that logs a skip.
> - **Holding the console off needed a new core API**:
>   `console_hold()` and `console_release()` take and release the console
>   spinlock (`kernel/core/console.c`).
> - **The harness burst is six cycles** of `sleep 1 &`, a pause of
>   0.85-1.15 s, then a typed line, before the harness's `exit 0`.
>
> | mutation | result |
> | --- | --- |
> | the old order restored (drain, then clear) | `console-rx-clear` FAIL (`ris & RIS_RXRIS`) on the first aarch64 debug boot; the only failing test |
> | the old order restored, harness burst only | stall in 1 of 5 aarch64 release boots, so the burst is a regression check, not the proof |
>
> This machine's QEMU (11.1.1) implements loopback, so the test runs
> here rather than skipping. CI's log line for the test says which it
> did there. Both architectures pass in debug and release,
> `gmake host-test` passes, and `gmake analyze` is clean.

## Problem

In the aarch64 release boot, right after a job event -- a `sleep` killed
by `^C`, a background `sleep` finishing -- the echo of the next line the
boot harness types stops partway (`cosmo$ echo after-interru`), and
nothing more comes for the rest of the boot: no prompt, no output, no
echo of anything typed after. The harness then reports every later
command as never sent. The flakes entry could not say whether the guest
had stopped or only its console input had.

### Measured

`tools/console-stall-probe.py` injects into the shell harness 25 cycles
of the two shapes seen -- `sleep 1` interrupted by `^C` then a typed
line, and `sleep 1 &` then a line typed as the job exits -- and, when a
prompt does not come, looks at the guest from outside before giving up:
it types Enter and `^C` and waits three seconds for any output, then
asks QEMU over QMP for every vCPU's registers and for the console
PL011's status registers (never its data register, which a read would
consume).

**It reproduces, on this machine, quiet or loaded: 13 stalls in 20
aarch64 release boots** with the probe applied -- none in the first
three, six of the next eight (quiet and under host load alike), and
seven of the nine run while the stall dump itself was being fixed. The
shape is the CI shape exactly:

```
cosmo$ sleep 1 &
[7] 38
cosmo$ [ INFO] process: pid 38 'sleep' exited with status 0 (3 syscalls)
echo csprobe-after
```

**The guest is alive and idle.** In the stall, Enter and `^C` produce
nothing -- no output at all, +0 bytes, so not even the `^C` echo (the
probe as first run counted any log growth as an answer; review asked for
the echo itself, and every recorded stall had none) -- and the registers show all four vCPUs in
`arch_cpu_wait_for_interrupt`, with CPU 0 in the exception vectors half a
second later -- taking its tick. Nothing is runnable; nothing is stuck.

**The input is sitting in the UART.** The PL011 (`virt`'s UART0 at
`0x0900_0000`) in the stall:

| register | value | meaning |
| --- | --- | --- |
| FR | `0xc0` | receive FIFO **full** (RXFF), transmit FIFO empty |
| IMSC | `0x50` | receive and receive-timeout interrupts **enabled** |
| RIS | `0x20` | raw status: transmit only -- **no receive interrupt pending** |
| MIS | `0x00` | nothing asserted to the interrupt controller |

A full receive FIFO, its interrupt enabled, and no interrupt raised: the
guest will never read it, because nothing tells it to.

**The cause is the order of two operations.** `rx_irq`
(kernel/arch/aarch64/pl011.c):

```c
while ((rd(UART_FR) & FR_RXFE) == 0) {       /* drain until empty */
    uint8_t c = (uint8_t)rd(UART_DR);
    tty_input(t, &c, 1);
}
wr(UART_ICR, IMSC_RXIM | IMSC_RTIM);         /* then clear the interrupt */
```

A character that arrives after the loop's last "empty" read and before
the write to ICR raises the receive interrupt -- and the write clears
it. That character stays in the FIFO. QEMU's PL011 raises the receive
interrupt when the FIFO count *reaches* its trigger level (one, with the
FIFO enabled), so every later character, arriving into a FIFO that
already holds one, raises nothing; the FIFO fills, and input stops for
good. (On hardware the receive-timeout interrupt would eventually fire
for a non-empty FIFO; QEMU's model does not raise it on its own, which is
why the stall is permanent here rather than a pause.) Why the window is
hit after a job event is not established -- the process-exit log line
and the echo share the console, and anything that slows the handler
moves where the loop ends relative to the next character -- and nothing
below depends on it.

**The fix, measured.** Clearing the interrupt *before* draining closes the
window: a character that arrives during the drain is read by it, and one
that arrives after the drain's last read raises an interrupt nothing
clears. The same probe, with only those two statements swapped: **no
stall in six of six boots**, every one running all 25 cycles (128
prompts), against 13 stalls in 20 boots without it.

### Why it matters

- **An interactive shell that stops taking input is a hang**, from the
  user's side, of the whole machine -- and it happened on `main`.
- **It is the console**: the one input path every aarch64 user has.
- **Three sightings over four days were recorded and re-run**, because
  the failure looked like a harness flake. It reproduces 13 times in 20
  once provoked.

## Current implementation

`arch_console_input_init` requests the PL011's receive interrupt (level
triggered), drains anything already in the FIFO, enables the receive and
receive-timeout interrupts (`IMSC_RXIM | IMSC_RTIM`), and enables the
interrupt at the GIC. `rx_irq` drains the FIFO into the console tty
(`tty_input`, which echoes) and then clears both interrupts through ICR.

The x86-64 console (the 16550) has no such clear: its receive interrupt
is the line status's "data ready", level while the FIFO holds anything,
so there is no window of this kind there, which matches every sighting
being aarch64.

## Design

### 1. Clear, then drain

`rx_irq` writes ICR first and drains after. The invariant it keeps: **the
receive interrupt is never cleared while the FIFO may hold a character
the handler has not read.** After the write, every character either is
read by the drain or arrives after it into an empty FIFO, raising an
interrupt that nothing clears.

### 2. A test that makes the race happen

The PL011 has a loopback mode (CR bit 7, LBE): a transmitted character is
received by the same UART. A kernel self-test, `console-rx-clear`, uses
it to put a character into the receive FIFO at the one moment that
matters:

- with the console's own output held off for the test's few
  milliseconds (so no log line is looped back), set LBE;
- drive the receive path through a test hook at the point between the
  drain and the clear: the hook transmits one byte, which the loopback
  delivers into the FIFO -- exactly the character the old order lost;
- then require that the byte's receive interrupt is still pending (as
  built -- the design said "reaches the tty"; see the banner).

With the old order the byte's interrupt is cleared and it is never read;
with the new one it is. If QEMU's PL011 does not implement loopback (it
was added in the 9 series), the test says so and skips rather than
passing vacuously; the build checks what CI's QEMU does.

The hook is a per-call argument of a test entry point, not global state
the handler reads -- the rule the lockup-bound unit's review arrived at.

### 3. The harness keeps a burst

The shell harness gains a few of the probe's cycles (a background job
exiting as the next line arrives), in every boot that runs it: not the
proof -- the loopback test is -- but the regression the user would see,
kept where the user would see it.

### 4. The invariant

The console's invariants gain: a receive interrupt is cleared only before
the drain that follows it. Checked by `console-rx-clear`.

### 5. The §70 gate

**Correctness.** Two statements reordered; the lost-wakeup window closed.

**Concurrency.** The handler runs with the interrupt masked at the GIC;
the order change affects only the device's own latch, which is the
point.

**Ownership and lifetime.** Nothing new is owned.

**Security.** None.

**Failure.** None added: the drain already reads until empty.

**Performance.** None: the same two operations.

## Affected files

| file | change |
| --- | --- |
| kernel/arch/aarch64/pl011.c | clear before draining (`rx_service`); the `console-rx-clear` test |
| kernel/arch/x86_64/serial.c | a `console-rx-clear` stub that logs a skip |
| kernel/core/console.c, kernel/include/kernel/console.h | `console_hold` / `console_release` |
| kernel/core/selftest.c, kernel/include/kernel/selftest.h | the test's registration |
| tests/boot/shelltest.py | six burst cycles and a `PAUSE` step |
| docs | the console/tty invariants and testing docs; `docs/testing/flakes.md` (the entry marked fixed); `docs/kernel/diagnostics/api.md` (`console_hold`); README Status |

## APIs

None for userspace. In the kernel: `console_hold()` and
`console_release()` (as built; see the banner). The test's hook is an
argument to `rx_service`, not global state the handler reads.

## Migration plan

One PR: the reorder, the test, the harness cycles, the documents.

## Tests

| test | checks | mutation it must catch |
| --- | --- | --- |
| `console-rx-clear` (new, aarch64) | a byte looped into the FIFO after the drain still has its receive interrupt pending | the old order (drain, then clear): fails on the first boot |
| shell harness burst cycles | a line typed as a background job exits echoes and runs | the old order: 1 in 5 release boots, so not the proof |

## Benchmarks

None.

## Risks

- **Loopback may be absent in CI's QEMU.** The test then skips with a
  reason, and the harness cycles carry the regression alone; the build
  records which it was.
- **The console's output during the test** goes to the loopback, not the
  line: the test holds console output off for its window and restores it
  on every exit.

## Alternatives considered

- **Drain again after the clear** (read, clear, read): also correct, and
  one loop more than the reorder.
- **Rely on the receive-timeout interrupt**: QEMU's model does not raise
  it on its own for a non-empty FIFO, which is why the stall is permanent
  here; hardware that does would pause, not stop -- and the order would
  still be wrong.
- **Poll the FIFO from the tick**: a workaround for a latch the driver
  itself clears.
