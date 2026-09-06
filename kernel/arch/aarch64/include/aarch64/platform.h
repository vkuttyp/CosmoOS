/*
 * aarch64/platform.h - Private declarations shared by the AArch64 backend
 * files (docs/kernel/arch/aarch64/): CPU identification, the GIC, the
 * PL011, fw_cfg, PSCI and the QEMU `virt` defaults used when ACPI does
 * not describe a device.
 */

#ifndef AARCH64_PLATFORM_H
#define AARCH64_PLATFORM_H

#include <kernel/types.h>

struct arch_trap_frame;

/* --- cpu.c --- */
struct aarch64_cpu_info {
    uint64_t midr, mpidr;
    bool has_pan;
    unsigned parange;        /* ID_AA64MMFR0.PARange encoding */
    unsigned gic_sysreg;     /* ID_AA64PFR0.GIC: 0 = memory-mapped only */
    char brand[48];
};
void aarch64_cpu_init(void);
const struct aarch64_cpu_info *aarch64_cpu_info(void);

/* --- start.c / smp.c --- */
void aarch64_start(const void *info) __attribute__((noreturn));
void aarch64_ap_entry(unsigned cpu) __attribute__((noreturn));

/* --- gic.c --- */
#define GIC_INTID_COUNT      1020u
#define GIC_SGI_COUNT        16u
#define GIC_PPI_BASE         16u
#define GIC_SPI_BASE         32u
#define GIC_INTID_SPURIOUS   1023u
#define VEC_SPURIOUS         1020u
#define VEC_SYNC_BASE        1024u    /* + enum arch_trap_kind */
#define VEC_DYNAMIC_BASE     1056u
#define VEC_DYNAMIC_COUNT    256u
#define VEC_COUNT            (VEC_DYNAMIC_BASE + VEC_DYNAMIC_COUNT)

void gic_irq_dispatch(struct arch_trap_frame *frame);
/* INTID of the interrupt this CPU is handling (for EOI). */
unsigned gic_current_intid(void);
/* PPIs are banked per CPU: bind once, enable on each CPU. */
void gic_bind_ppi(unsigned intid, unsigned vector);
void gic_enable_local(unsigned intid);
void gic_disable_local(unsigned intid);

/* --- start.c: the direct-map base before the PMM publishes its own --- */
extern uint64_t aarch64_hhdm_base;

/* --- timer.c --- */
void aarch64_timer_init_cpu(void);
void aarch64_timer_ack(unsigned intid);   /* gic.c: before dispatching a PPI */

/* --- pl011.c --- */
void pl011_early_putc(char c);

/* --- QEMU virt defaults, used only when the ACPI tables lack the device --- */
#define VIRT_GICD_BASE     0x08000000ull
#define VIRT_GICC_BASE     0x08010000ull
#define VIRT_GICV2M_BASE   0x08020000ull
#define VIRT_PL011_BASE    0x09000000ull
#define VIRT_PL011_INTID   33u
#define VIRT_PL031_BASE    0x09010000ull   /* the real-time clock: RTCDR (seconds since 1970) at offset 0 */
#define VIRT_FWCFG_BASE    0x09020000ull
#define VIRT_TIMER_EL1_PHYS_INTID 30u
#define VIRT_TIMER_EL1_VIRT_INTID 27u

/* --- PSCI (SMC calling convention, 32-bit function ids) --- */
#define PSCI_FN_VERSION        0x84000000u
#define PSCI_FN_CPU_ON         0xC4000003u
#define PSCI_FN_AFFINITY_INFO  0xC4000004u
#define PSCI_FN_SYSTEM_OFF     0x84000008u
#define PSCI_RET_SUCCESS       0
#define PSCI_RET_NOT_SUPPORTED (-1)
#define PSCI_RET_INVALID       (-2)
#define PSCI_RET_DENIED        (-3)
#define PSCI_RET_ALREADY_ON    (-4)
#define PSCI_RET_ON_PENDING    (-5)

/* ACPI FADT ARM boot flags (offset 129, 2 bytes). */
#define FADT_ARM_PSCI_COMPLIANT (1u << 0)
#define FADT_ARM_PSCI_USE_HVC   (1u << 1)

#endif /* AARCH64_PLATFORM_H */
