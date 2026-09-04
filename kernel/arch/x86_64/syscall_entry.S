/*
 * syscall_entry.S - SYSCALL entry and SYSRET exit.
 *
 * On entry (from ring 3): rcx = user rip, r11 = user rflags, rsp = user
 * stack, IF cleared by SFMASK, GS still the user's. We swap GS, park the
 * user rsp in the per-CPU block, load the kernel stack, and build a
 * frame that mirrors an interrupt frame so diagnostics can treat both
 * alike. Arguments: rdi rsi rdx r10 r8 r9; number: rax.
 *
 * Frame layout (struct syscall_frame, top of stack first):
 *   r15 r14 r13 r12 rbp rbx   callee-saved (diagnostics)
 *   r9 r8 r10 rdx rsi rdi     arguments 6..1
 *   rax                       number in / result out
 *   rip cs rflags rsp ss      user return state
 */

#define PERCPU_KERNEL_STACK_TOP 8
#define PERCPU_USER_RSP_SCRATCH 16
#define USER_CS 0x23
#define USER_SS 0x1b

    .section .text, "ax", @progbits
    .globl x86_syscall_entry
    .type x86_syscall_entry, @function
x86_syscall_entry:
    swapgs
    movq %rsp, %gs:PERCPU_USER_RSP_SCRATCH
    movq %gs:PERCPU_KERNEL_STACK_TOP, %rsp

    /* iret-style return state */
    pushq $USER_SS
    pushq %gs:PERCPU_USER_RSP_SCRATCH
    pushq %r11                  /* rflags */
    pushq $USER_CS
    pushq %rcx                  /* rip */

    pushq %rax                  /* number */
    pushq %rdi
    pushq %rsi
    pushq %rdx
    pushq %r10
    pushq %r8
    pushq %r9
    pushq %rbx
    pushq %rbp
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    cld
    sti

    /* syscall_dispatch(nr, args[6], frame): pass a pointer to the six
     * argument slots (r9 r8 r10 rdx rsi rdi are on the stack in reverse
     * order; the C side reads them from the frame through x86_syscall_args). */
    movq %rsp, %rdi
    call x86_syscall_c

    cli

    /* x86_syscall_c stored the result in the frame's rax slot. */
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbp
    popq %rbx
    popq %r9
    popq %r8
    popq %r10
    popq %rdx
    popq %rsi
    popq %rdi
    popq %rax                   /* result */

    popq %rcx                   /* rip */
    addq $8, %rsp               /* cs */
    popq %r11                   /* rflags */
    popq %rsp                   /* user rsp; ss discarded implicitly */

    swapgs
    sysretq
    .size x86_syscall_entry, . - x86_syscall_entry

    .section .note.GNU-stack, "", @progbits
