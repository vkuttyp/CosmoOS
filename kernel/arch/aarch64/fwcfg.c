/*
 * fwcfg.c - QEMU fw_cfg over MMIO (docs/kernel/arch/aarch64/design.md).
 *
 * The `virt` machine places the register block at 0x09020000: the data
 * register at +0 (one byte per byte-wide read, in stream order), the
 * 16-bit big-endian selector at +8. The same file-directory walk as the
 * x86 port-based version.
 */

#include <kernel/errno.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>
#include <arch/fwcfg.h>
#include <aarch64/platform.h>

extern uint64_t aarch64_hhdm_base;

#define FW_CFG_SIGNATURE  0x0000
#define FW_CFG_FILE_DIR   0x0019
#define FW_CFG_MAX_FILES  64

struct fw_cfg_file {
    uint32_t size;      /* big endian */
    uint16_t select;    /* big endian */
    uint16_t reserved;
    char name[56];
};

static spinlock_t g_lock = SPINLOCK_INIT("fwcfg");

static volatile uint8_t *regs(void)
{
    return (volatile uint8_t *)(uintptr_t)(aarch64_hhdm_base + VIRT_FWCFG_BASE);
}

static void select_key(uint16_t key)
{
    volatile uint16_t *sel = (volatile uint16_t *)(regs() + 8);
    *sel = (uint16_t)((key << 8) | (key >> 8));
    __asm__ volatile("dsb sy" ::: "memory");
}

static void read_bytes(void *buf, size_t len)
{
    uint8_t *p = buf;
    volatile uint8_t *data = regs();
    for (size_t i = 0; i < len; i++)
        p[i] = *data;
}

static void skip_bytes(size_t len)
{
    volatile uint8_t *data = regs();
    for (size_t i = 0; i < len; i++)
        (void)*data;
}

static bool present(void)
{
    char sig[4];
    select_key(FW_CFG_SIGNATURE);
    read_bytes(sig, 4);
    return memcmp(sig, "QEMU", 4) == 0;
}

static uint32_t be32(uint32_t v)
{
    return ((v & 0xff) << 24) | ((v & 0xff00) << 8) | ((v >> 8) & 0xff00) | (v >> 24);
}

static uint16_t be16(uint16_t v)
{
    return (uint16_t)((v << 8) | (v >> 8));
}

int arch_fwcfg_read(const char *name, void *buf, size_t len)
{
    arch_irq_state_t s = spin_lock_irqsave(&g_lock);
    if (!present()) {
        spin_unlock_irqrestore(&g_lock, s);
        return -ENODEV;
    }
    select_key(FW_CFG_FILE_DIR);
    uint32_t count;
    read_bytes(&count, 4);
    count = be32(count);
    if (count > FW_CFG_MAX_FILES)
        count = FW_CFG_MAX_FILES;
    int rc = -ENOENT;
    for (uint32_t i = 0; i < count; i++) {
        struct fw_cfg_file f;
        read_bytes(&f, sizeof(f));
        f.name[sizeof(f.name) - 1] = '\0';
        if (strcmp(f.name, name) == 0) {
            uint32_t size = be32(f.size);
            select_key(be16(f.select));
            size_t take = size < len ? size : len;
            read_bytes(buf, take);
            skip_bytes(size - take);
            rc = (int)size;
            break;
        }
    }
    spin_unlock_irqrestore(&g_lock, s);
    return rc;
}
