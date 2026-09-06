/*
 * guest_wfi.S - The simplest EL2 guest
 * (docs/kernel-services/virtualization/testing.md).
 * Loaded at guest-physical 0x1000, entered at EL1 with the MMU off.
 *
 *   wfi (WFI exit; the manager sees the guest with nothing to do)
 *   wfi (a second one, so a re-entry can be checked)
 *   then spin, which nothing should reach
 */
    .text
    .globl _start
_start:
    wfi
    wfi
1:  b 1b
