/*
 * x86/svm.h - AMD-V (SVM) definitions: the VMCB, exit codes, intercept
 * bits, nested page tables and the pure decoders
 * (docs/kernel-services/virtualization/design.md, "The SVM backend").
 *
 * Also compiled by tests/host/test_hv.c: keep this header and svm_npt.c
 * free of anything but kernel/types.h, page.h and pmm.h.
 */

#ifndef X86_SVM_H
#define X86_SVM_H

#include <kernel/compiler.h>
#include <kernel/types.h>

/* CPUID */
#define CPUID_EXT_FEATURES     0x80000001u
#define CPUID_EXT_ECX_SVM      (1u << 2)
#define CPUID_SVM_FEATURES     0x8000000Au
#define CPUID_SVM_EDX_NP       (1u << 0)
#define CPUID_SVM_EDX_NRIP     (1u << 3)
#define CPUID_SVM_EDX_DECODE   (1u << 7)

/* MSRs */
#ifndef MSR_EFER
#define MSR_EFER 0xC0000080u
#endif
#ifndef EFER_SCE
#define EFER_SCE (1ull << 0)
#endif
#ifndef EFER_LME
#define EFER_LME (1ull << 8)
#endif
#ifndef EFER_LMA
#define EFER_LMA (1ull << 10)
#endif
#ifndef EFER_NXE
#define EFER_NXE (1ull << 11)
#endif
#ifndef EFER_SVME
#define EFER_SVME (1ull << 12)
#endif
#define MSR_STAR               0xC0000081u
#define MSR_LSTAR              0xC0000082u
#define MSR_CSTAR              0xC0000083u
#define MSR_SFMASK             0xC0000084u
#define MSR_FS_BASE_           0xC0000100u
#define MSR_GS_BASE_           0xC0000101u
#define MSR_KERNEL_GS_BASE     0xC0000102u
#define MSR_SYSENTER_CS        0x174u
#define MSR_SYSENTER_ESP       0x175u
#define MSR_SYSENTER_EIP       0x176u
#define MSR_PAT                0x277u
#define MSR_TSC                0x10u
#ifndef MSR_APIC_BASE
#define MSR_APIC_BASE 0x1Bu
#endif
#define MSR_MISC_ENABLE        0x1A0u
#define MSR_MTRRCAP            0xFEu
#define MSR_MTRR_DEF_TYPE      0x2FFu
#define MSR_VM_CR              0xC0010114u
#define VM_CR_SVMDIS           (1ull << 4)
#define MSR_VM_HSAVE_PA        0xC0010117u

#define PAT_DEFAULT            0x0007040600070406ull

/* Intercept vector 3 (offset 0x00C) */
#define SVM_INTERCEPT_INTR      (1u << 0)
#define SVM_INTERCEPT_NMI       (1u << 1)
#define SVM_INTERCEPT_SMI       (1u << 2)
#define SVM_INTERCEPT_INIT      (1u << 3)
#define SVM_INTERCEPT_VINTR     (1u << 4)
#define SVM_INTERCEPT_CPUID     (1u << 18)
#define SVM_INTERCEPT_INVD      (1u << 22)
#define SVM_INTERCEPT_HLT       (1u << 24)
#define SVM_INTERCEPT_INVLPGA   (1u << 26)
#define SVM_INTERCEPT_IOIO      (1u << 27)
#define SVM_INTERCEPT_MSR       (1u << 28)
#define SVM_INTERCEPT_FERR      (1u << 30)
#define SVM_INTERCEPT_SHUTDOWN  (1u << 31)
/* Intercept vector 4 (offset 0x010) */
#define SVM_INTERCEPT_VMRUN     (1u << 0)
#define SVM_INTERCEPT_VMMCALL   (1u << 1)
#define SVM_INTERCEPT_VMLOAD    (1u << 2)
#define SVM_INTERCEPT_VMSAVE    (1u << 3)
#define SVM_INTERCEPT_STGI      (1u << 4)
#define SVM_INTERCEPT_CLGI      (1u << 5)
#define SVM_INTERCEPT_SKINIT    (1u << 6)
#define SVM_INTERCEPT_RDTSCP    (1u << 7)
#define SVM_INTERCEPT_MONITOR   (1u << 10)
#define SVM_INTERCEPT_MWAIT     (1u << 11)
#define SVM_INTERCEPT_MWAIT_ARM (1u << 12)
#define SVM_INTERCEPT_XSETBV    (1u << 13)

/* Exit codes */
#define SVM_EXIT_EXCP_BASE   0x40u
#define SVM_EXIT_INTR        0x60u
#define SVM_EXIT_NMI         0x61u
#define SVM_EXIT_SMI         0x62u
#define SVM_EXIT_INIT        0x63u
#define SVM_EXIT_VINTR       0x64u
#define SVM_EXIT_CPUID       0x72u
#define SVM_EXIT_INVD        0x76u
#define SVM_EXIT_HLT         0x78u
#define SVM_EXIT_INVLPGA     0x7Au
#define SVM_EXIT_IOIO        0x7Bu
#define SVM_EXIT_MSR         0x7Cu
#define SVM_EXIT_FERR        0x7Eu
#define SVM_EXIT_SHUTDOWN    0x7Fu
#define SVM_EXIT_VMRUN       0x80u
#define SVM_EXIT_VMMCALL     0x81u
#define SVM_EXIT_VMLOAD      0x82u
#define SVM_EXIT_VMSAVE      0x83u
#define SVM_EXIT_STGI        0x84u
#define SVM_EXIT_CLGI        0x85u
#define SVM_EXIT_SKINIT      0x86u
#define SVM_EXIT_RDTSCP      0x87u
#define SVM_EXIT_MONITOR     0x8Au
#define SVM_EXIT_MWAIT       0x8Bu
#define SVM_EXIT_MWAIT_COND  0x8Cu
#define SVM_EXIT_XSETBV      0x8Du
#define SVM_EXIT_NPF         0x400u
#define SVM_EXIT_INVALID     (~0ull)

/* EVENTINJ */
#define SVM_EVTINJ_TYPE_INTR  (0ull << 8)
#define SVM_EVTINJ_TYPE_NMI   (2ull << 8)
#define SVM_EVTINJ_TYPE_EXCP  (3ull << 8)
#define SVM_EVTINJ_TYPE_SOFT  (4ull << 8)
#define SVM_EVTINJ_EV         (1ull << 11)
#define SVM_EVTINJ_VALID      (1ull << 31)

/* IOIO EXITINFO1 */
#define SVM_IOIO_IN      (1u << 0)
#define SVM_IOIO_STR     (1u << 2)
#define SVM_IOIO_REP     (1u << 3)
#define SVM_IOIO_SZ8     (1u << 4)
#define SVM_IOIO_SZ16    (1u << 5)
#define SVM_IOIO_SZ32    (1u << 6)
#define SVM_IOIO_PORT_SHIFT 16

/* NPF EXITINFO1 */
#define SVM_NPF_WRITE    (1ull << 1)

/* TLB_CONTROL */
#define SVM_TLB_FLUSH_ALL 1u

/* Segment attribute word: type(4) S DPL(2) P | AVL L DB G (bits 12-15). */
#define SVM_ATTR_CODE_RM   0x009Bu   /* real-mode code: execute/read, accessed */
#define SVM_ATTR_DATA_RM   0x0093u   /* real-mode data: read/write, accessed */
#define SVM_ATTR_LDT       0x0082u
#define SVM_ATTR_TSS_BUSY  0x008Bu

/* --- the VMCB: one 4 KiB page, control area then the save area at 0x400.
 * Every field sits at its natural alignment, so no packing is needed; the
 * STATIC_ASSERTs below pin the architectural offsets. --- */

struct svm_seg {
    uint16_t selector;
    uint16_t attrib;
    uint32_t limit;
    uint64_t base;
};

struct vmcb_control {
    uint32_t intercept_cr;          /* 0x000: reads bits 0-15, writes 16-31 */
    uint32_t intercept_dr;          /* 0x004 */
    uint32_t intercept_exceptions;  /* 0x008 */
    uint32_t intercept_misc1;       /* 0x00C */
    uint32_t intercept_misc2;       /* 0x010 */
    uint32_t intercept_misc3;       /* 0x014 */
    uint8_t reserved0[0x3C - 0x18];
    uint16_t pause_filter_threshold;/* 0x03C */
    uint16_t pause_filter_count;    /* 0x03E */
    uint64_t iopm_base_pa;          /* 0x040 */
    uint64_t msrpm_base_pa;         /* 0x048 */
    uint64_t tsc_offset;            /* 0x050 */
    uint32_t asid;                  /* 0x058 */
    uint8_t tlb_control;            /* 0x05C */
    uint8_t reserved1[3];
    uint8_t v_tpr;                  /* 0x060 */
    uint8_t v_irq;                  /* 0x061: bit 0 V_IRQ, bit 1 VGIF */
    uint8_t v_intr_prio;            /* 0x062: bits 0-3 priority, bit 4 V_IGN_TPR */
    uint8_t v_intr_masking;         /* 0x063: bit 0 */
    uint8_t v_intr_vector;          /* 0x064 */
    uint8_t reserved2[3];
    uint64_t interrupt_shadow;      /* 0x068 */
    uint64_t exitcode;              /* 0x070 */
    uint64_t exitinfo1;             /* 0x078 */
    uint64_t exitinfo2;             /* 0x080 */
    uint64_t exitintinfo;           /* 0x088 */
    uint64_t np_enable;             /* 0x090 */
    uint64_t avic_apic_bar;         /* 0x098 */
    uint64_t ghcb_pa;               /* 0x0A0 */
    uint64_t eventinj;              /* 0x0A8 */
    uint64_t n_cr3;                 /* 0x0B0 */
    uint64_t lbr_virt;              /* 0x0B8 */
    uint32_t clean_bits;            /* 0x0C0 */
    uint32_t reserved3;
    uint64_t nrip;                  /* 0x0C8 */
    uint8_t insn_len;               /* 0x0D0 */
    uint8_t insn_bytes[15];
    uint8_t reserved4[0x400 - 0x0E0];
};

struct vmcb_save {
    struct svm_seg es, cs, ss, ds, fs, gs;   /* 0x400.. */
    struct svm_seg gdtr, ldtr, idtr, tr;     /* 0x460.. */
    uint8_t reserved0[0x4CB - 0x4A0];
    uint8_t cpl;                    /* 0x4CB */
    uint32_t reserved1;
    uint64_t efer;                  /* 0x4D0 */
    uint8_t reserved2[0x548 - 0x4D8];
    uint64_t cr4;                   /* 0x548 */
    uint64_t cr3;                   /* 0x550 */
    uint64_t cr0;                   /* 0x558 */
    uint64_t dr7;                   /* 0x560 */
    uint64_t dr6;                   /* 0x568 */
    uint64_t rflags;                /* 0x570 */
    uint64_t rip;                   /* 0x578 */
    uint8_t reserved3[0x5D8 - 0x580];
    uint64_t rsp;                   /* 0x5D8 */
    uint64_t s_cet;                 /* 0x5E0 */
    uint64_t ssp;                   /* 0x5E8 */
    uint64_t isst_addr;             /* 0x5F0 */
    uint64_t rax;                   /* 0x5F8 */
    uint64_t star;                  /* 0x600 */
    uint64_t lstar;                 /* 0x608 */
    uint64_t cstar;                 /* 0x610 */
    uint64_t sfmask;                /* 0x618 */
    uint64_t kernel_gs_base;        /* 0x620 */
    uint64_t sysenter_cs;           /* 0x628 */
    uint64_t sysenter_esp;          /* 0x630 */
    uint64_t sysenter_eip;          /* 0x638 */
    uint64_t cr2;                   /* 0x640 */
    uint8_t reserved4[0x668 - 0x648];
    uint64_t g_pat;                 /* 0x668 */
    uint64_t dbgctl;                /* 0x670 */
    uint64_t br_from;               /* 0x678 */
    uint64_t br_to;                 /* 0x680 */
    uint64_t last_excp_from;        /* 0x688 */
    uint64_t last_excp_to;          /* 0x690 */
    uint8_t reserved5[0x1000 - 0x400 - 0x298];
};

struct vmcb {
    struct vmcb_control control;
    struct vmcb_save save;
};

STATIC_ASSERT(sizeof(struct vmcb_control) == 0x400, "VMCB control area is 1 KiB");
STATIC_ASSERT(sizeof(struct vmcb) == 4096, "VMCB is one page");
STATIC_ASSERT(offsetof(struct vmcb_control, iopm_base_pa) == 0x40, "IOPM_BASE_PA at 0x40");
STATIC_ASSERT(offsetof(struct vmcb_control, asid) == 0x58, "ASID at 0x58");
STATIC_ASSERT(offsetof(struct vmcb_control, v_intr_vector) == 0x64, "V_INTR_VECTOR at 0x64");
STATIC_ASSERT(offsetof(struct vmcb_control, exitcode) == 0x70, "EXITCODE at 0x70");
STATIC_ASSERT(offsetof(struct vmcb_control, np_enable) == 0x90, "NP_ENABLE at 0x90");
STATIC_ASSERT(offsetof(struct vmcb_control, eventinj) == 0xA8, "EVENTINJ at 0xA8");
STATIC_ASSERT(offsetof(struct vmcb_control, n_cr3) == 0xB0, "N_CR3 at 0xB0");
STATIC_ASSERT(offsetof(struct vmcb_control, nrip) == 0xC8, "nRIP at 0xC8");
STATIC_ASSERT(offsetof(struct vmcb, save.es) == 0x400, "save area at 0x400");
STATIC_ASSERT(offsetof(struct vmcb, save.cpl) == 0x4CB, "CPL at 0x4CB");
STATIC_ASSERT(offsetof(struct vmcb, save.efer) == 0x4D0, "EFER at 0x4D0");
STATIC_ASSERT(offsetof(struct vmcb, save.cr4) == 0x548, "CR4 at 0x548");
STATIC_ASSERT(offsetof(struct vmcb, save.rip) == 0x578, "RIP at 0x578");
STATIC_ASSERT(offsetof(struct vmcb, save.rsp) == 0x5D8, "RSP at 0x5D8");
STATIC_ASSERT(offsetof(struct vmcb, save.rax) == 0x5F8, "RAX at 0x5F8");
STATIC_ASSERT(offsetof(struct vmcb, save.star) == 0x600, "STAR at 0x600");
STATIC_ASSERT(offsetof(struct vmcb, save.cr2) == 0x640, "CR2 at 0x640");
STATIC_ASSERT(offsetof(struct vmcb, save.g_pat) == 0x668, "G_PAT at 0x668");

/* Guest GPRs the VMCB does not hold; svm_run.S loads and spills them. */
struct svm_gprs {
    uint64_t rbx, rcx, rdx, rsi, rdi, rbp;    /* 0x00 .. 0x28 */
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;   /* 0x30 .. 0x68 */
};
STATIC_ASSERT(sizeof(struct svm_gprs) == 14 * 8, "svm_gprs layout is assembly ABI");

/* svm_run.S: CLGI, VMSAVE host, load GPRs, VMLOAD/VMRUN/VMSAVE guest, spill GPRs, VMLOAD host, STGI. */
void svm_run(uint64_t vmcb_pa, uint64_t host_vmcb_pa, struct svm_gprs *gprs);

/* --- pure decoders (host-tested) --- */

struct svm_ioio {
    uint16_t port;
    uint8_t size;      /* 1, 2, 4 */
    bool in, string, rep;
};

static inline struct svm_ioio svm_decode_ioio(uint64_t exitinfo1)
{
    struct svm_ioio io;
    io.port = (uint16_t)(exitinfo1 >> SVM_IOIO_PORT_SHIFT);
    io.in = (exitinfo1 & SVM_IOIO_IN) != 0;
    io.string = (exitinfo1 & SVM_IOIO_STR) != 0;
    io.rep = (exitinfo1 & SVM_IOIO_REP) != 0;
    io.size = (exitinfo1 & SVM_IOIO_SZ32) ? 4 : (exitinfo1 & SVM_IOIO_SZ16) ? 2 : 1;
    return io;
}

/* --- nested page tables (svm_npt.c) --- */

/* Root table; every present entry is P|RW|US (nested walks need the User bit). */
paddr_t npt_create(void);
void npt_destroy(paddr_t root);
/* 4 KiB granular with 2 MiB leaves where gpa, hpa and len allow;
 * `prot` is HV_MAP_* and must be nonzero. -EINVAL/-ENOMEM/-EEXIST. */
int npt_map(paddr_t root, uint64_t gpa, paddr_t hpa, size_t len, unsigned prot);
int npt_unmap(paddr_t root, uint64_t gpa, size_t len);
bool npt_query(paddr_t root, uint64_t gpa, paddr_t *hpa);
/* Number of table pages (root included) currently allocated: accounting for tests. */
unsigned npt_table_pages(paddr_t root);

#endif /* X86_SVM_H */
