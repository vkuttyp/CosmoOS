/*
 * guest_ptimer.S - A guest that reaches for the host's clock
 * (docs/kernel-services/virtualization/testing.md).
 *
 * The physical counter is the host's uptime and the physical timer is
 * the host's tick. A guest at EL1 could read one and arm the other until
 * CNTHCTL_EL2 was cleared for guests; both accesses now trap, and this
 * guest makes each in turn so the owner can see the trap and step over.
 *
 *   mrs x3, CNTPCT_EL0        traps (SYSREG exit, read, CRn 14)
 *   msr CNTP_CTL_EL0, x4      traps (SYSREG exit, write, CRn 14)
 *   hvc  x0 = 9               done, with x3 = whatever the owner answered
 */
    .text
    .globl _start
_start:
    mov  x3, #0
    mrs  x3, cntpct_el0            /* the host's uptime: not this guest's business */
    mov  x4, #1
    msr  cntp_ctl_el0, x4          /* the host's tick: even less so */
    mov  x0, #9
    hvc  #0
1:  b    1b
