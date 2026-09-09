/*
 * guest_ctimer.S - A guest that only uses its clock and timer
 * (docs/kernel-services/virtualization/testing.md).
 *
 * No GIC, no vector table, no interrupts: this guest arms CNTV and
 * heartbeats through HVC, and never waits for the timer to fire. It is
 * what the isolation and offset tests use, because those test the timer
 * *state* -- that a guest's CNTV does not outlive it, and that its clock
 * is its VM's -- and need nothing of the interrupt path. It therefore
 * runs on a GICv2 host too, where a guest has no system-register CPU
 * interface to set up (guest_timer.S, which does, is for the delivery
 * tests and those are GICv3-only).
 *
 *   hvc  x0 = 1, x1 = CNTVCT      "ready", before arming (offset reads x1)
 *   (arm CNTV ~15 ms out)
 *   loop: hvc x0 = 2, x1 = CNTVCT  heartbeat
 */
    .text
    .globl _start
_start:
    mrs  x1, cntvct_el0
    mov  x0, #1
    hvc  #0                        /* ready */

    mrs  x0, cntfrq_el0
    lsr  x0, x0, #6                /* about 15 ms */
    msr  cntv_tval_el0, x0
    mov  x0, #1
    msr  cntv_ctl_el0, x0
    isb
1:  mrs  x1, cntvct_el0
    mov  x0, #2
    hvc  #0                        /* heartbeat, with the time */
    b    1b
