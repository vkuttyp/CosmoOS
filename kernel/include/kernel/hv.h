/*
 * hv.h - The VM manager: virtual machines, guest memory, virtual CPUs,
 * virtual interrupts and device backends
 * (docs/kernel-services/virtualization/).
 *
 * Generic over arch/hv.h. VMs and vCPUs are kobjects: the system calls
 * hand them out as handles; the self-tests use this API directly.
 */

#ifndef KERNEL_HV_H
#define KERNEL_HV_H

#include <kernel/list.h>
#include <kernel/mutex.h>
#include <kernel/object.h>
#include <kernel/spinlock.h>
#include <kernel/types.h>
#include <uapi/cosmo/syscall.h>

#include <arch/hv.h>

#define HV_VMS_MAX      COSMO_HV_VMS_MAX
#define HV_VCPUS_MAX    COSMO_HV_VCPUS_MAX
#define HV_VM_MEM_MAX   COSMO_HV_VM_MEM_MAX
#define HV_REGIONS_MAX  16u
#define HV_GPA_LIMIT    (1ull << 32)     /* stage 1: a 4 GiB guest-physical window */
#define HV_CONSOLE_SIZE 4096u
/* The guest's console UART: where QEMU's virt puts UART0 and a stock guest's
 * device tree names it. The hypervisor's constant, like the GIC's. */
#define VUART_BASE      0x09000000ull
#define VUART_SIZE      0x1000ull
#define VUART_INTID     33u

struct vm;
struct vcpu;
struct page;

struct guest_region {
    struct list_node link;
    uint64_t gpa, len;                   /* page aligned */
    struct page **pages;                 /* len / PAGE_SIZE order-0 pages */
};

/* A device backend: claims ports and/or a memory range of one VM. */
struct vm_device {
    struct list_node link;
    const char *name;
    uint16_t pio_base, pio_count;        /* count 0: no ports */
    uint64_t mmio_base, mmio_len;        /* len 0: no memory range */
    /* 0: handled (value in for OUT, out for IN); -ENODEV: hand the exit to the owner. */
    int (*pio)(struct vm_device *d, uint16_t port, bool write, unsigned size, uint32_t *value);
    /* 0: handled (a read's result in *value, up to `size` bytes); -ENODEV:
     * hand the exit to the owner. Called with the access the hardware
     * described; never for one it did not (size 0 goes to the owner). */
    int (*mmio)(struct vm_device *d, uint64_t gpa, bool write, unsigned size, uint64_t *value);
    /* A device with an interrupt line names it (an SPI; 0: none) and says
     * whether the line is up. Level is the source's: before every entry the
     * run loop raises the line of each device that says so, so a line still
     * up after the guest acknowledged is delivered again, and a device
     * lowers its own line (vm_lower_spi) when it drops. */
    unsigned irq;
    bool (*irq_asserted)(struct vm_device *d);
    void *priv;
};

struct vm {
    struct kobject obj;
    unsigned id;
    struct mutex lock;                   /* regions, devices, vcpus[], mem_bytes */
    struct arch_hv_vm *arch;
    struct list_node regions;            /* struct guest_region by gpa */
    unsigned nr_regions;
    uint64_t mem_limit;                  /* bytes of guest memory this VM may hold (the creator's rlimit) */
    uint64_t mem_bytes;
    struct vcpu *vcpus[HV_VCPUS_MAX];    /* weak: a vcpu references its vm */
    unsigned nr_vcpus;
    bool started;                        /* a vCPU has run: the device table is frozen */
    struct list_node devices;
    struct {
        spinlock_t lock;                 /* producer is the run loop, consumer the owner */
        uint8_t buf[HV_CONSOLE_SIZE];
        unsigned head, tail;             /* head: next write; tail: next read */
        uint64_t dropped;
    } console;
    struct vm_device debug_console;      /* the built-in port 0xE9 backend */
    struct vm_device uart_dev;           /* AArch64: the guest's PL011 at VUART_BASE */
    struct vuart *uart;
    uint32_t owner_uid;
    struct list_node link;               /* the manager's list */
};

/* Wide enough for the widest architecture's range (arch/hv.h). */
#define VINTR_WORDS ((HV_VINTR_MAX + 64) / 64)

struct vcpu {
    struct kobject obj;
    struct vm *vm;                       /* referenced */
    unsigned index;
    struct mutex run_lock;               /* run and regs */
    struct arch_hv_vcpu *arch;
    spinlock_t irq_lock;
    uint64_t pending[VINTR_WORDS];        /* VirtualInterrupt: one bit per injectable number */
    int offered;                         /* vector offered to the backend for this entry, -1 none */
    bool in_completion;                  /* an IN waits for its value */
    uint8_t in_size;
    struct {                             /* an MMIO read the owner is answering */
        bool pending;
        uint8_t size, reg, insn_len;
        bool sse, sf;
    } mmio_completion;
    bool dead;
    uint64_t exits, entries;
    unsigned msr_gp;                     /* #GP injected for unmodelled MSRs */
};

/* Boot: probe the backend, create /dev/vmm. After vfs_init and the ramfs population. */
void hv_init(void);
const struct hv_caps *hv_caps(void);
unsigned hv_vm_count(void);
void hv_stats(uint64_t *exits, uint64_t *entries, unsigned *vcpus);
/* sysctl hv.<name>: value text into out, -ENOENT for an unknown name. */
int hv_sysctl(const char *name, char *out, size_t n);

/* VM lifetime: the returned reference belongs to the caller. */
int vm_create(uint32_t owner_uid, uint64_t mem_limit, struct vm **out);   /* mem_limit: COSMO_RLIMIT_VMEM of the creator */
struct vm *vm_from_kobject(struct kobject *obj);

/* GuestMemory. */
int vm_mem_add(struct vm *vm, uint64_t gpa, uint64_t len);
int vm_mem_read(struct vm *vm, uint64_t gpa, void *buf, size_t len);
int vm_mem_write(struct vm *vm, uint64_t gpa, const void *buf, size_t len);
bool vm_mem_lookup(struct vm *vm, uint64_t gpa, struct page **page, size_t *offset);

/* Device backends: only before the first run. */
int vm_device_register(struct vm *vm, struct vm_device *dev);
/* The debug console ring (what the guest wrote to port 0xE9). */
size_t vm_console_read(struct vm *vm, void *buf, size_t len);
/* The owner's input to the guest: bytes into the console UART's receive
 * FIFO, which interrupts the guest if it asked to be. -ENOTSUP where the
 * VM has no such device (x86-64). */
int64_t vm_console_write(struct vm *vm, const void *buf, size_t len);
/* A device's shared interrupt line, up or down (arch_hv_vm_raise_spi). */
int vm_raise_spi(struct vm *vm, unsigned intid);
int vm_lower_spi(struct vm *vm, unsigned intid);
size_t vm_console_pending(struct vm *vm);

/* VirtualCPU. */
int vcpu_create(struct vm *vm, unsigned index, struct vcpu **out);
struct vcpu *vcpu_from_kobject(struct kobject *obj);
int vcpu_get_regs(struct vcpu *v, struct cosmo_vcpu_regs *out);
int vcpu_set_regs(struct vcpu *v, const struct cosmo_vcpu_regs *in);
/* Run until an exit; `exit` carries an IN completion in and the exit out.
 * -EIO dead, -EINTR the caller is being killed, -ENOTSUP no backend. */
int vcpu_run(struct vcpu *v, struct cosmo_vm_exit *exit);
int vcpu_run_limited(struct vcpu *v, struct cosmo_vm_exit *exit, unsigned max_intr);
/* VirtualInterrupt: make a vector (32..255) pending. */
int vcpu_inject(struct vcpu *v, unsigned vector);
int vcpu_lowest_pending(struct vcpu *v);   /* -1 none */

/* Character device ops for /dev/vmm and the vnode the manager created. */
struct vnode;
bool hv_is_vmm_vnode(const struct vnode *vn);

#endif /* KERNEL_HV_H */
