/*
 * guest_spin.S - A guest that never yields
 * (docs/kernel-services/virtualization/testing.md).
 *
 * Every host timer tick is taken to EL2 (HCR_EL2.IMO), so the run loop
 * sees INTR exits and stays preemptible; the owner's bound on those is
 * what ends the run.
 */
    .text
    .globl _start
_start:
1:  b 1b
