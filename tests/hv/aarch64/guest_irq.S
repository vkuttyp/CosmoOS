/*
 * guest_irq.S - A guest that takes an interrupt
 * (docs/kernel-services/virtualization/testing.md).
 *
 * Nothing here knows it is virtualised. The guest enables its CPU
 * interface with the same four writes the host's own GICv3 driver
 * makes -- `ICC_SRE_EL1`, `ICC_PMR_EL1`, `ICC_IGRPEN1_EL1` -- and
 * acknowledges with `ICC_IAR1_EL1`; with `HCR_EL2.IMO` set those
 * accesses are redirected by hardware to the *virtual* interface, so no
 * distributor is emulated and none is needed.
 *
 *   hvc #0 with x0 = 1        "ready": the host injects after this
 *   loop: hvc #0 with x0 = 2  a heartbeat, so every run ends in an exit
 *   IRQ -> handler:
 *       mrs x0, ICC_IAR1_EL1  acknowledge (the list register goes Active)
 *       hvc #0                the host sees the INTID, and sees Active
 *       msr ICC_EOIR1_EL1, x0 complete (the list register goes Invalid)
 *       eret                  back into the heartbeat
 *
 * The hypercall between acknowledge and completion is deliberate: it is
 * the window in which a list register is Active, and a hypervisor that
 * calls that "not taken" injects the same interrupt twice.
 *
 * The heartbeat is a hypercall rather than a WFI, and that is not
 * decoration. `HCR_EL2.TWI` traps a WFI only when it would actually
 * wait: with an interrupt already pending -- the masked case, where the
 * guest cannot take it -- WFI completes as a no-op and traps nothing, so
 * a guest built around WFI spins forever and the run never returns.
 * Measured, after this guest did exactly that.
 */
    .text
    .globl _start
_start:
    adr  x0, vectors
    msr  vbar_el1, x0
    mov  x0, #1
    msr  icc_sre_el1, x0           /* the system-register interface */
    isb
    mov  x0, #0xF0
    msr  icc_pmr_el1, x0           /* admit any priority below 0xF0 */
    mov  x0, #1
    msr  icc_igrpen1_el1, x0       /* Group 1 interrupts on */
    isb
    msr  daifclr, #2               /* and unmask IRQ */
    mov  x0, #1
    hvc  #0                        /* ready */
1:  mov  x0, #2
    hvc  #0                        /* heartbeat */
    b    1b

/* The handler leaves the acknowledged INTID in x0 on purpose: the owner
 * reads it back as the guest's evidence that the interrupt arrived with
 * the number it was given. */
irq_spx:
    mrs  x0, icc_iar1_el1
    hvc  #0
    msr  icc_eoir1_el1, x0
    eret

.macro ventry label
    .balign 0x80
    b \label
.endm

    .balign 2048
vectors:
    ventry .                       /* 0x000 current EL, SP0: sync */
    ventry .                       /* 0x080 IRQ */
    ventry .                       /* 0x100 FIQ */
    ventry .                       /* 0x180 SError */
    ventry .                       /* 0x200 current EL, SPx: sync */
    ventry irq_spx                 /* 0x280 IRQ -- the guest runs at EL1h */
    ventry .                       /* 0x300 FIQ */
    ventry .                       /* 0x380 SError */
    ventry .                       /* 0x400 lower EL, AArch64: sync */
    ventry .                       /* 0x480 IRQ */
    ventry .                       /* 0x500 FIQ */
    ventry .                       /* 0x580 SError */
    ventry .                       /* 0x600 lower EL, AArch32: sync */
    ventry .                       /* 0x680 IRQ */
    ventry .                       /* 0x700 FIQ */
    ventry .                       /* 0x780 SError */
