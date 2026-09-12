/*
 * guest_offspin.S - a machine-mode guest whose second CPU never yields
 * (docs/kernel-services/virtualization/testing.md).
 *
 * The first CPU brings the second up at a spin loop and asks PSCI to power
 * the machine off immediately after. Machine mode holds a power-off for a
 * sibling that has not yet had a turn end on its own terms, so this guest is
 * the case that hold has to survive: the sibling never yields, and the
 * machine must still stop -- the hold is bounded in turns, and every turn
 * must therefore end. An owner that ran the last runnable vCPU untimed here
 * would never come back, and `vmctl` would hang instead of powering off.
 *
 * No device tree is read and nothing is printed: the observable is that
 * `vmctl` prints "guest powered off" and exits, which `rc.test` turns into
 * "HVTEST: offspin ok". Loaded flat at the start of guest RAM, so the
 * secondary's entry is taken with `adr` -- its own address is its GPA.
 */
    .text
    .globl _start
_start:
    adr     x2, secondary            /* entry_point_address */
    mov     x1, #1                   /* target_cpu: affinity 0.0.0.1 */
    mov     x3, #0                   /* context_id */
    mov     x0, #0x0003
    movk    x0, #0xC400, lsl #16     /* PSCI_CPU_ON (0xC4000003) */
    hvc     #0

    mov     x0, #0x0008
    movk    x0, #0x8400, lsl #16     /* PSCI_SYSTEM_OFF (0x84000008) */
    hvc     #0
1:  b       1b                       /* the owner stops the machine here */

secondary:
2:  b       2b                       /* never a voluntary exit */
