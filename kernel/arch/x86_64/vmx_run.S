/*
 * vmx_run.S - The VMLAUNCH/VMRESUME sequence
 * (docs/kernel-services/virtualization/design.md, "The VMX backend").
 *
 * uint32_t vmx_run(struct vmx_gprs *gprs (rdi), uint32_t launched (esi))
 *
 * The caller has local interrupts disabled. Unlike SVM there is no GIF
 * and no host save area: the VMCS host-state fields describe where the
 * CPU lands after an exit, and this stub writes the two that depend on
 * the call itself (HOST_RSP and HOST_RIP) just before entering. A
 * physical interrupt while the guest runs causes a VM exit (pin-based
 * external-interrupt exiting); the host handler runs when the caller
 * restores its interrupt flag, not here.
 *
 * Returns 0 after a VM exit, 1 when VMLAUNCH/VMRESUME failed to enter
 * (the caller reads VM-instruction error). Guest RSP and RIP are VMCS
 * fields; every other GPR is carried in *gprs, whose layout is fixed:
 *   0 rax   8 rbx  16 rcx  24 rdx  32 rsi  40 rdi  48 rbp
 *  56 r8   64 r9   72 r10  80 r11  88 r12  96 r13 104 r14 112 r15
 */

#define VMCS_HOST_RSP 0x6C14
#define VMCS_HOST_RIP 0x6C16

    .text
    .globl vmx_run
    .type vmx_run, @function
vmx_run:
    push %rbp
    push %rbx
    push %r12
    push %r13
    push %r14
    push %r15
    push %rdi                   /* the gprs pointer: the exit path finds it at (%rsp) */

    /* Where the CPU resumes on a VM exit, and with which stack. */
    mov $VMCS_HOST_RSP, %rax
    vmwrite %rsp, %rax
    mov $VMCS_HOST_RIP, %rax
    lea 2f(%rip), %rbx
    vmwrite %rbx, %rax

    /* Guest registers. rdi is loaded last because it is the pointer. */
    mov  %esi, %ebx             /* `launched` survives in ebx until the test below */
    mov   0(%rdi), %rax
    mov  16(%rdi), %rcx
    mov  24(%rdi), %rdx
    mov  32(%rdi), %rsi
    mov  48(%rdi), %rbp
    mov  56(%rdi), %r8
    mov  64(%rdi), %r9
    mov  72(%rdi), %r10
    mov  80(%rdi), %r11
    mov  88(%rdi), %r12
    mov  96(%rdi), %r13
    mov 104(%rdi), %r14
    mov 112(%rdi), %r15
    test %ebx, %ebx
    mov   8(%rdi), %rbx         /* guest rbx, after the test that used it */
    mov  40(%rdi), %rdi         /* guest rdi, last */
    jnz 1f
    vmlaunch
    jmp 3f                      /* only reached when the entry failed */
1:  vmresume
    jmp 3f

    /* A VM exit lands here with the host state of the VMCS loaded: rsp
     * is what was written above, so (%rsp) is still the gprs pointer. */
2:  push %rdi                   /* free one register to work with */
    mov  8(%rsp), %rdi          /* the saved gprs pointer */
    mov %rax,   0(%rdi)
    mov %rbx,   8(%rdi)
    mov %rcx,  16(%rdi)
    mov %rdx,  24(%rdi)
    mov %rsi,  32(%rdi)
    mov %rbp,  48(%rdi)
    mov %r8,   56(%rdi)
    mov %r9,   64(%rdi)
    mov %r10,  72(%rdi)
    mov %r11,  80(%rdi)
    mov %r12,  88(%rdi)
    mov %r13,  96(%rdi)
    mov %r14, 104(%rdi)
    mov %r15, 112(%rdi)
    pop %rax                    /* the guest's rdi, pushed on arrival */
    mov %rax,  40(%rdi)
    xor %eax, %eax              /* 0: a normal exit */
    jmp 4f

3:  mov $1, %eax                /* the entry itself failed */
4:  pop %rdi                    /* the gprs pointer */
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbx
    pop %rbp
    ret
    .size vmx_run, . - vmx_run

    .section .note.GNU-stack, "", @progbits
