/*
 * guest_irq_hb.S - guest_irq.S whose heartbeat always comes before the
 * next pending interrupt (docs/audit/next-subsystem-irq-order.md).
 *
 * guest_irq_exit.S makes the pending interrupt come first, always; this
 * guest makes the other order certain, so a test run on both runs in
 * both orders on every boot rather than in whichever the host's timing
 * gives. After its EOI the handler forces the same emulated exit
 * (a GICD_CTLR read, so the next entry places whatever is pending) and
 * then sends the heartbeat itself, *before* `eret`: the handler runs with
 * PSTATE.I set, so an interrupt placed by that exit cannot be taken until
 * the handler returns, and the heartbeat is first however the host
 * interleaves. The forced exit is what makes the order provable: without
 * the in-handler heartbeat this guest is guest_irq_exit, and the
 * interrupt comes first.
 *
 * The read goes to x28, which nothing reads. Everything else is
 * guest_irq.S's, and its header describes it.
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
    movz x28, #0x0800, lsl #16     /* GICD_CTLR: an emulated exit, the next entry offers what is pending */
    ldr  w28, [x28]
    mov  x0, #2
    hvc  #0                        /* the heartbeat, while IRQs are still masked: always first */
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
