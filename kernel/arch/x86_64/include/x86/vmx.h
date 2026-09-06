/*
 * vmx.h - Intel VT-x: VMCS field encodings, control bits, exit reasons,
 * capability MSRs, and the pure helpers the host test covers
 * (docs/kernel-services/virtualization/design.md, "The VMX backend").
 *
 * Encodings are from the SDM volume 3, appendix B. Nothing here executes
 * a VMX instruction: vmx.c does that.
 */

#ifndef X86_VMX_H
#define X86_VMX_H

#include <kernel/types.h>

/* --- capability and control MSRs --- */

#define MSR_IA32_FEATURE_CONTROL 0x3Au
#define FEATURE_CONTROL_LOCK     (1ull << 0)
#define FEATURE_CONTROL_VMXON    (1ull << 2)   /* VMX outside SMX */

#define MSR_IA32_VMX_BASIC          0x480u
#define MSR_IA32_VMX_PINBASED_CTLS  0x481u
#define MSR_IA32_VMX_PROCBASED_CTLS 0x482u
#define MSR_IA32_VMX_EXIT_CTLS      0x483u
#define MSR_IA32_VMX_ENTRY_CTLS     0x484u
#define MSR_IA32_VMX_MISC           0x485u
#define MSR_IA32_VMX_CR0_FIXED0     0x486u
#define MSR_IA32_VMX_CR0_FIXED1     0x487u
#define MSR_IA32_VMX_CR4_FIXED0     0x488u
#define MSR_IA32_VMX_CR4_FIXED1     0x489u
#define MSR_IA32_VMX_PROCBASED_CTLS2 0x48Bu
#define MSR_IA32_VMX_EPT_VPID_CAP   0x48Cu
#define MSR_IA32_VMX_TRUE_PINBASED  0x48Du
#define MSR_IA32_VMX_TRUE_PROCBASED 0x48Eu
#define MSR_IA32_VMX_TRUE_EXIT      0x48Fu
#define MSR_IA32_VMX_TRUE_ENTRY     0x490u

/* IA32_VMX_BASIC */
#define VMX_BASIC_REVISION(x)   ((uint32_t)((x) & 0x7FFFFFFFu))
#define VMX_BASIC_SIZE(x)       ((unsigned)(((x) >> 32) & 0x1FFFu))
#define VMX_BASIC_TRUE_CTLS     (1ull << 55)
#define VMX_BASIC_MEMTYPE(x)    ((unsigned)(((x) >> 50) & 0xFu))   /* 6 = write-back */

/* IA32_VMX_EPT_VPID_CAP */
#define VMX_EPT_EXEC_ONLY       (1ull << 0)
#define VMX_EPT_PAGE_WALK_4     (1ull << 6)
#define VMX_EPT_MEMTYPE_WB      (1ull << 14)
#define VMX_EPT_2MB             (1ull << 16)
#define VMX_EPT_INVEPT          (1ull << 20)
#define VMX_EPT_AD              (1ull << 21)
#define VMX_EPT_INVEPT_SINGLE   (1ull << 25)
#define VMX_EPT_INVEPT_GLOBAL   (1ull << 26)
#define VMX_EPT_INVVPID         (1ull << 32)

/* --- execution controls --- */

#define PIN_EXT_INTR_EXITING (1u << 0)
#define PIN_NMI_EXITING      (1u << 3)
#define PIN_VIRTUAL_NMIS     (1u << 5)

#define CPU_INTR_WINDOW      (1u << 2)
#define CPU_HLT_EXITING      (1u << 7)
#define CPU_INVLPG_EXITING   (1u << 9)
#define CPU_MWAIT_EXITING    (1u << 10)
#define CPU_RDPMC_EXITING    (1u << 11)
#define CPU_RDTSC_EXITING    (1u << 12)
#define CPU_CR3_LOAD_EXITING (1u << 15)
#define CPU_CR3_STORE_EXITING (1u << 16)
#define CPU_MOV_DR_EXITING   (1u << 23)
#define CPU_UNCOND_IO_EXITING (1u << 24)
#define CPU_USE_IO_BITMAPS   (1u << 25)
#define CPU_USE_MSR_BITMAPS  (1u << 28)
#define CPU_MONITOR_EXITING  (1u << 29)
#define CPU_PAUSE_EXITING    (1u << 30)
#define CPU_SECONDARY_CTLS   (1u << 31)

#define CPU2_ENABLE_EPT      (1u << 1)
#define CPU2_DESC_TABLE_EXITING (1u << 2)
#define CPU2_ENABLE_RDTSCP   (1u << 3)
#define CPU2_ENABLE_VPID     (1u << 5)
#define CPU2_WBINVD_EXITING  (1u << 6)
#define CPU2_UNRESTRICTED    (1u << 7)
#define CPU2_RDRAND_EXITING  (1u << 11)
#define CPU2_ENABLE_INVPCID  (1u << 12)
#define CPU2_RDSEED_EXITING  (1u << 16)
#define CPU2_ENABLE_XSAVES   (1u << 20)

#define EXIT_CTL_SAVE_DEBUG      (1u << 2)
#define EXIT_CTL_HOST_ADDR_SPACE (1u << 9)
#define EXIT_CTL_ACK_INTR        (1u << 15)
#define EXIT_CTL_SAVE_PAT        (1u << 18)
#define EXIT_CTL_LOAD_PAT        (1u << 19)
#define EXIT_CTL_SAVE_EFER       (1u << 20)
#define EXIT_CTL_LOAD_EFER       (1u << 21)

#define ENTRY_CTL_LOAD_DEBUG     (1u << 2)
#define ENTRY_CTL_IA32E_GUEST    (1u << 9)
#define ENTRY_CTL_LOAD_PAT       (1u << 14)
#define ENTRY_CTL_LOAD_EFER      (1u << 15)

/* --- VMCS field encodings --- */

/* 16-bit */
#define VMCS_VPID                 0x0000u
#define VMCS_GUEST_ES_SEL         0x0800u
#define VMCS_GUEST_CS_SEL         0x0802u
#define VMCS_GUEST_SS_SEL         0x0804u
#define VMCS_GUEST_DS_SEL         0x0806u
#define VMCS_GUEST_FS_SEL         0x0808u
#define VMCS_GUEST_GS_SEL         0x080Au
#define VMCS_GUEST_LDTR_SEL       0x080Cu
#define VMCS_GUEST_TR_SEL         0x080Eu
#define VMCS_HOST_ES_SEL          0x0C00u
#define VMCS_HOST_CS_SEL          0x0C02u
#define VMCS_HOST_SS_SEL          0x0C04u
#define VMCS_HOST_DS_SEL          0x0C06u
#define VMCS_HOST_FS_SEL          0x0C08u
#define VMCS_HOST_GS_SEL          0x0C0Au
#define VMCS_HOST_TR_SEL          0x0C0Cu

/* 64-bit */
#define VMCS_IO_BITMAP_A          0x2000u
#define VMCS_IO_BITMAP_B          0x2002u
#define VMCS_MSR_BITMAP           0x2004u
#define VMCS_EXIT_MSR_STORE_ADDR  0x2006u
#define VMCS_EXIT_MSR_LOAD_ADDR   0x2008u
#define VMCS_ENTRY_MSR_LOAD_ADDR  0x200Au
#define VMCS_TSC_OFFSET           0x2010u
#define VMCS_EPT_POINTER          0x201Au
#define VMCS_GUEST_PHYS_ADDR      0x2400u
#define VMCS_LINK_POINTER         0x2800u
#define VMCS_GUEST_DEBUGCTL       0x2802u
#define VMCS_GUEST_PAT            0x2804u
#define VMCS_GUEST_EFER           0x2806u
#define VMCS_HOST_PAT             0x2C00u
#define VMCS_HOST_EFER            0x2C02u

/* 32-bit control */
#define VMCS_PIN_CTLS             0x4000u
#define VMCS_CPU_CTLS             0x4002u
#define VMCS_EXCEPTION_BITMAP     0x4004u
#define VMCS_PF_ERROR_MASK        0x4006u
#define VMCS_PF_ERROR_MATCH       0x4008u
#define VMCS_CR3_TARGET_COUNT     0x400Au
#define VMCS_EXIT_CTLS            0x400Cu
#define VMCS_EXIT_MSR_STORE_COUNT 0x400Eu
#define VMCS_EXIT_MSR_LOAD_COUNT  0x4010u
#define VMCS_ENTRY_CTLS           0x4012u
#define VMCS_ENTRY_MSR_LOAD_COUNT 0x4014u
#define VMCS_ENTRY_INTR_INFO      0x4016u
#define VMCS_ENTRY_EXCEPTION_ERR  0x4018u
#define VMCS_ENTRY_INSTR_LEN      0x401Au
#define VMCS_CPU_CTLS2            0x401Eu

/* 32-bit read-only */
#define VMCS_INSTRUCTION_ERROR    0x4400u
#define VMCS_EXIT_REASON          0x4402u
#define VMCS_EXIT_INTR_INFO       0x4404u
#define VMCS_EXIT_INTR_ERROR      0x4406u
#define VMCS_IDT_VECTORING_INFO   0x4408u
#define VMCS_IDT_VECTORING_ERROR  0x440Au
#define VMCS_EXIT_INSTR_LEN       0x440Cu
#define VMCS_EXIT_INSTR_INFO      0x440Eu

/* 32-bit guest */
#define VMCS_GUEST_ES_LIMIT       0x4800u
#define VMCS_GUEST_CS_LIMIT       0x4802u
#define VMCS_GUEST_SS_LIMIT       0x4804u
#define VMCS_GUEST_DS_LIMIT       0x4806u
#define VMCS_GUEST_FS_LIMIT       0x4808u
#define VMCS_GUEST_GS_LIMIT       0x480Au
#define VMCS_GUEST_LDTR_LIMIT     0x480Cu
#define VMCS_GUEST_TR_LIMIT       0x480Eu
#define VMCS_GUEST_GDTR_LIMIT     0x4810u
#define VMCS_GUEST_IDTR_LIMIT     0x4812u
#define VMCS_GUEST_ES_AR          0x4814u
#define VMCS_GUEST_CS_AR          0x4816u
#define VMCS_GUEST_SS_AR          0x4818u
#define VMCS_GUEST_DS_AR          0x481Au
#define VMCS_GUEST_FS_AR          0x481Cu
#define VMCS_GUEST_GS_AR          0x481Eu
#define VMCS_GUEST_LDTR_AR        0x4820u
#define VMCS_GUEST_TR_AR          0x4822u
#define VMCS_GUEST_INTERRUPTIBILITY 0x4824u
#define VMCS_GUEST_ACTIVITY_STATE 0x4826u
#define VMCS_GUEST_SYSENTER_CS    0x482Au
#define VMCS_HOST_SYSENTER_CS     0x4C00u

/* natural width */
#define VMCS_CR0_MASK             0x6000u
#define VMCS_CR4_MASK             0x6002u
#define VMCS_CR0_READ_SHADOW      0x6004u
#define VMCS_CR4_READ_SHADOW      0x6006u
#define VMCS_EXIT_QUALIFICATION   0x6400u
#define VMCS_GUEST_LINEAR_ADDR    0x640Au
#define VMCS_GUEST_CR0            0x6800u
#define VMCS_GUEST_CR3            0x6802u
#define VMCS_GUEST_CR4            0x6804u
#define VMCS_GUEST_ES_BASE        0x6806u
#define VMCS_GUEST_CS_BASE        0x6808u
#define VMCS_GUEST_SS_BASE        0x680Au
#define VMCS_GUEST_DS_BASE        0x680Cu
#define VMCS_GUEST_FS_BASE        0x680Eu
#define VMCS_GUEST_GS_BASE        0x6810u
#define VMCS_GUEST_LDTR_BASE      0x6812u
#define VMCS_GUEST_TR_BASE        0x6814u
#define VMCS_GUEST_GDTR_BASE      0x6816u
#define VMCS_GUEST_IDTR_BASE      0x6818u
#define VMCS_GUEST_DR7            0x681Au
#define VMCS_GUEST_RSP            0x681Cu
#define VMCS_GUEST_RIP            0x681Eu
#define VMCS_GUEST_RFLAGS         0x6820u
#define VMCS_GUEST_PENDING_DBG    0x6822u
#define VMCS_GUEST_SYSENTER_ESP   0x6824u
#define VMCS_GUEST_SYSENTER_EIP   0x6826u
#define VMCS_HOST_CR0             0x6C00u
#define VMCS_HOST_CR3             0x6C02u
#define VMCS_HOST_CR4             0x6C04u
#define VMCS_HOST_FS_BASE         0x6C06u
#define VMCS_HOST_GS_BASE         0x6C08u
#define VMCS_HOST_TR_BASE         0x6C0Au
#define VMCS_HOST_GDTR_BASE       0x6C0Cu
#define VMCS_HOST_IDTR_BASE       0x6C0Eu
#define VMCS_HOST_SYSENTER_ESP    0x6C10u
#define VMCS_HOST_SYSENTER_EIP    0x6C12u
#define VMCS_HOST_RSP             0x6C14u
#define VMCS_HOST_RIP             0x6C16u

/* --- exit reasons (SDM appendix C) --- */

#define VMX_EXIT_EXCEPTION_NMI    0u
#define VMX_EXIT_EXT_INTERRUPT    1u
#define VMX_EXIT_TRIPLE_FAULT     2u
#define VMX_EXIT_INIT             3u
#define VMX_EXIT_SIPI             4u
#define VMX_EXIT_INTR_WINDOW      7u
#define VMX_EXIT_NMI_WINDOW       8u
#define VMX_EXIT_TASK_SWITCH      9u
#define VMX_EXIT_CPUID            10u
#define VMX_EXIT_HLT              12u
#define VMX_EXIT_INVD             13u
#define VMX_EXIT_INVLPG           14u
#define VMX_EXIT_RDPMC            15u
#define VMX_EXIT_RDTSC            16u
#define VMX_EXIT_VMCALL           18u
#define VMX_EXIT_VMCLEAR          19u
#define VMX_EXIT_VMLAUNCH         20u
#define VMX_EXIT_VMPTRLD          21u
#define VMX_EXIT_VMPTRST          22u
#define VMX_EXIT_VMREAD           23u
#define VMX_EXIT_VMRESUME         24u
#define VMX_EXIT_VMWRITE          25u
#define VMX_EXIT_VMXOFF           26u
#define VMX_EXIT_VMXON            27u
#define VMX_EXIT_CR_ACCESS        28u
#define VMX_EXIT_DR_ACCESS        29u
#define VMX_EXIT_IO               30u
#define VMX_EXIT_RDMSR            31u
#define VMX_EXIT_WRMSR            32u
#define VMX_EXIT_ENTRY_INVALID    33u
#define VMX_EXIT_ENTRY_MSR_LOAD   34u
#define VMX_EXIT_MWAIT            36u
#define VMX_EXIT_MONITOR          39u
#define VMX_EXIT_PAUSE            40u
#define VMX_EXIT_ENTRY_MCE        41u
#define VMX_EXIT_APIC_ACCESS      44u
#define VMX_EXIT_GDTR_IDTR        46u
#define VMX_EXIT_LDTR_TR          47u
#define VMX_EXIT_EPT_VIOLATION    48u
#define VMX_EXIT_EPT_MISCONFIG    49u
#define VMX_EXIT_INVEPT           50u
#define VMX_EXIT_RDTSCP           51u
#define VMX_EXIT_WBINVD           54u
#define VMX_EXIT_XSETBV           55u
#define VMX_EXIT_RDRAND           57u
#define VMX_EXIT_INVPCID          58u
#define VMX_EXIT_RDSEED           61u
#define VMX_EXIT_XSAVES           63u
#define VMX_EXIT_XRSTORS          64u

#define VMX_EXIT_REASON_MASK      0xFFFFu   /* bit 31 = VM-entry failure */
#define VMX_EXIT_ENTRY_FAILURE    (1u << 31)

/* Interruptibility state (VMCS_GUEST_INTERRUPTIBILITY) */
#define VMX_INTR_SHADOW_STI       (1u << 0)
#define VMX_INTR_SHADOW_MOVSS     (1u << 1)

/* VM-entry interruption information */
#define VMX_ENTRY_INTR_VALID      (1u << 31)
#define VMX_ENTRY_INTR_TYPE_EXT   (0u << 8)
#define VMX_ENTRY_INTR_TYPE_HW    (3u << 8)   /* hardware exception */
#define VMX_ENTRY_INTR_ERR_VALID  (1u << 11)

/* Exit qualification of an I/O exit (SDM table 27-5) */
struct vmx_io {
    uint16_t port;
    uint8_t size;      /* 1, 2, 4 */
    bool in;
    bool string;
    bool rep;
};

/* --- pure helpers (host-tested) --- */

/* A control word the CPU will accept: bits the capability MSR requires
 * (low half, allowed-0) are forced on, bits it forbids (high half,
 * allowed-1) are dropped. Returns the fixed value. */
static inline uint32_t vmx_fix_ctls(uint64_t cap, uint32_t want)
{
    uint32_t allowed0 = (uint32_t)cap;            /* must be 1 */
    uint32_t allowed1 = (uint32_t)(cap >> 32);    /* may be 1 */
    return (want | allowed0) & allowed1;
}

/* Whether every bit of `want` survived the fixing: a control the CPU
 * does not support cannot simply be dropped. */
static inline bool vmx_ctls_ok(uint64_t cap, uint32_t want)
{
    return (vmx_fix_ctls(cap, want) & want) == want;
}

static inline struct vmx_io vmx_decode_io(uint64_t qual)
{
    struct vmx_io io;
    io.size = (uint8_t)((qual & 7u) + 1u);        /* 0 = 1 byte, 1 = 2, 3 = 4 */
    io.in = (qual & (1u << 3)) != 0;
    io.string = (qual & (1u << 4)) != 0;
    io.rep = (qual & (1u << 5)) != 0;
    io.port = (uint16_t)((qual >> 16) & 0xFFFFu);
    return io;
}

/* EPT pointer: the root, write-back memory type, four levels. */
static inline uint64_t vmx_eptp(paddr_t root, bool ad_bits)
{
    return (uint64_t)root | (6ull << 0) | (3ull << 3) | (ad_bits ? (1ull << 6) : 0);
}

/* --- the EPT builder (vmx_ept.c; the same shape as npt_*) --- */

paddr_t ept_create(void);
void ept_destroy(paddr_t root);
int ept_map(paddr_t root, uint64_t gpa, paddr_t hpa, size_t len, unsigned prot);
int ept_unmap(paddr_t root, uint64_t gpa, size_t len);
bool ept_query(paddr_t root, uint64_t gpa, paddr_t *hpa);
unsigned ept_table_pages(paddr_t root);

/* EPT entry bits, exposed for the host test. */
#define EPT_READ   (1ull << 0)
#define EPT_WRITE  (1ull << 1)
#define EPT_EXEC   (1ull << 2)
#define EPT_MEMTYPE_WB (6ull << 3)
#define EPT_LARGE  (1ull << 7)
#define EPT_ADDR_MASK 0x000FFFFFFFFFF000ull

#endif /* X86_VMX_H */
