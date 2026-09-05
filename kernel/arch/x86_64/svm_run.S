/*
 * svm_run.S - The VMRUN sequence
 * (docs/kernel-services/virtualization/design.md, "The entry sequence").
 *
 * void svm_run(uint64_t vmcb_pa (rdi), uint64_t host_vmcb_pa (rsi), struct svm_gprs *gprs (rdx))
 *
 * The caller has local interrupts disabled. CLGI holds physical
 * interrupts (and NMIs) until STGI, so an interrupt that arrives while
 * the guest runs exits the guest (INTR is intercepted) and is taken on
 * the host after this function re-enables GIF. VMRUN saves the host's
 * rsp/rip/rflags/CR0/CR3/CR4/EFER and segment selectors in the host
 * save area and restores them at the exit; VMSAVE/VMLOAD carry the
 * host's FS/GS/TR/LDTR bases, KernelGSBase and the SYSCALL MSRs, which
 * VMRUN does not. Guest GPRs other than rax/rsp/rip live in *gprs
 * (struct svm_gprs: rbx rcx rdx rsi rdi rbp r8-r15, 8 bytes each).
 */

    .text
    .globl svm_run
    .type svm_run, @function
svm_run:
    push %rbp
    push %rbx
    push %r12
    push %r13
    push %r14
    push %r15

    mov %rdi, %r12              /* guest VMCB */
    mov %rsi, %r13              /* host VMCB */
    mov %rdx, %r14              /* gprs */

    clgi
    sti                         /* held by GIF; VMRUN records host IF=1 so V_INTR_MASKING lets
                                   physical interrupts exit the guest whatever its own IF */

    mov %r13, %rax
    vmsave %rax                 /* host FS/GS/TR/LDTR/KernelGSBase/STAR... */

    push %r13                   /* [rsp+8] host VMCB pa */
    push %r14                   /* [rsp]   gprs */

    mov %r12, %rax              /* VMCB pa in rax for vmload/vmrun/vmsave */
    mov 0x00(%r14), %rbx
    mov 0x08(%r14), %rcx
    mov 0x10(%r14), %rdx
    mov 0x18(%r14), %rsi
    mov 0x20(%r14), %rdi
    mov 0x28(%r14), %rbp
    mov 0x30(%r14), %r8
    mov 0x38(%r14), %r9
    mov 0x40(%r14), %r10
    mov 0x48(%r14), %r11
    mov 0x50(%r14), %r12
    mov 0x58(%r14), %r13
    mov 0x68(%r14), %r15
    mov 0x60(%r14), %r14

    vmload %rax
    vmrun %rax
    vmsave %rax                 /* guest FS/GS/TR/LDTR bases and MSRs into its VMCB */

    /* rax = VMCB pa, rsp = host stack; everything else is the guest's. */
    push %rbx                   /* guest rbx, temporarily */
    mov 8(%rsp), %rbx           /* gprs */
    mov %rcx, 0x08(%rbx)
    mov %rdx, 0x10(%rbx)
    mov %rsi, 0x18(%rbx)
    mov %rdi, 0x20(%rbx)
    mov %rbp, 0x28(%rbx)
    mov %r8,  0x30(%rbx)
    mov %r9,  0x38(%rbx)
    mov %r10, 0x40(%rbx)
    mov %r11, 0x48(%rbx)
    mov %r12, 0x50(%rbx)
    mov %r13, 0x58(%rbx)
    mov %r14, 0x60(%rbx)
    mov %r15, 0x68(%rbx)
    pop %rcx                    /* guest rbx */
    mov %rcx, 0x00(%rbx)

    pop %r14                    /* gprs (unused now) */
    pop %rax                    /* host VMCB pa */
    vmload %rax
    cli                         /* the caller restores its interrupt state */
    stgi

    cld
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbx
    pop %rbp
    ret
    .size svm_run, . - svm_run

    .section .note.GNU-stack, "", @progbits
