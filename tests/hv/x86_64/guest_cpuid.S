/*
 * guest_cpuid.S - Real-mode guest: CPUID and MSR emulation, hypercall
 * (docs/kernel-services/virtualization/testing.md). Loaded at 0x1000.
 *
 *   cpuid 0x40000000 -> writes ebx, ecx, edx ("CosmoOSCosmo") to port 0xE9
 *   cpuid 1          -> writes bit 31 of ecx ('1' if the hypervisor bit is set) to 0xE9
 *   rdmsr EFER       -> writes '0' + (SVME bit) to 0xE9 (must be '0': hidden)
 *   wrmsr EFER(SCE)  -> then rdmsr again, writes '0' + SCE bit ('1')
 *   vmmcall          -> HYPERCALL exit with eax 7, ebx 0x11, ecx 0x22, edx 0x33, esi 0x44
 *   hlt
 */
    .code16
    .globl _start
_start:
    movl $0x40000000, %eax
    xorl %ecx, %ecx
    cpuid
    movl %ebx, %esi
    call out4
    movl %ecx, %esi
    call out4
    movl %edx, %esi
    call out4

    movl $1, %eax
    xorl %ecx, %ecx
    cpuid
    shrl $31, %ecx
    addb $'0', %cl
    movb %cl, %al
    outb %al, $0xE9

    movl $0xC0000080, %ecx
    rdmsr
    shrl $12, %eax
    andl $1, %eax
    addb $'0', %al
    outb %al, $0xE9

    movl $0xC0000080, %ecx
    movl $1, %eax              /* EFER.SCE */
    xorl %edx, %edx
    wrmsr
    rdmsr
    andl $1, %eax
    addb $'0', %al
    outb %al, $0xE9

    movl $7, %eax
    movl $0x11, %ebx
    movl $0x22, %ecx
    movl $0x33, %edx
    movl $0x44, %esi
    vmmcall
    hlt
1:  hlt
    jmp 1b

/* out4: the four bytes of esi to port 0xE9, low byte first */
out4:
    movl $4, %edi
2:  movl %esi, %eax
    outb %al, $0xE9
    shrl $8, %esi
    decl %edi
    jnz 2b
    ret
