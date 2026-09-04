/*
 * fwcfg.c - QEMU fw_cfg through the traditional I/O ports: selector at
 * 0x510, data at 0x511. The file directory (selector 0x19) lists named
 * items; each is read by selecting its key and streaming bytes.
 */

#include <kernel/errno.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>

#include <arch/fwcfg.h>

#include <x86/io.h>

#define FW_CFG_PORT_SEL   0x510
#define FW_CFG_PORT_DATA  0x511
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

static void select_key(uint16_t key)
{
    outw(FW_CFG_PORT_SEL, key);
}

static void read_bytes(void *buf, size_t len)
{
    uint8_t *p = buf;
    for (size_t i = 0; i < len; i++)
        p[i] = inb(FW_CFG_PORT_DATA);
}

static void skip_bytes(size_t len)
{
    for (size_t i = 0; i < len; i++)
        (void)inb(FW_CFG_PORT_DATA);
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
