/*
 * uaccess.c - Validated kernel access to user memory.
 */

#include <kernel/errno.h>
#include <kernel/panic.h>
#include <kernel/process.h>
#include <kernel/string.h>
#include <kernel/uaccess.h>
#include <kernel/vmm.h>

#include <arch/user.h>

bool user_range_ok(uint64_t addr, size_t len)
{
    if (len == 0)
        return addr >= USER_LO && addr <= USER_HI;
    if (addr < USER_LO || addr >= USER_HI)
        return false;
    if (addr + len < addr)
        return false;
    return addr + len <= USER_HI;
}

bool user_range_mapped(uint64_t addr, size_t len, vm_prot_t prot)
{
    struct process *p = process_current();
    if (p == NULL)
        return false;
    return vm_user_range_mapped(p->space, addr, len, prot);
}

int copy_from_user(void *dst, uint64_t user_src, size_t len)
{
    if (len == 0)
        return 0;
    if (!user_range_ok(user_src, len) || !user_range_mapped(user_src, len, VM_PROT_READ))
        return -EFAULT;

    arch_user_access_begin();
    memcpy(dst, (const void *)(uintptr_t)user_src, len);
    arch_user_access_end();
    return 0;
}

int copy_to_user(uint64_t user_dst, const void *src, size_t len)
{
    if (len == 0)
        return 0;
    if (!user_range_ok(user_dst, len) || !user_range_mapped(user_dst, len, VM_PROT_WRITE))
        return -EFAULT;

    arch_user_access_begin();
    memcpy((void *)(uintptr_t)user_dst, src, len);
    arch_user_access_end();
    return 0;
}

int strncpy_from_user(char *dst, uint64_t user_src, size_t max)
{
    if (max == 0)
        return -EINVAL;

    /* Copy in page-bounded pieces so a string that ends before an
     * unmapped page is accepted while one that runs into it is not. */
    size_t done = 0;
    while (done + 1 < max) {
        uint64_t addr = user_src + done;
        size_t in_page = (size_t)(4096 - (addr & 4095));
        size_t want = max - 1 - done;
        if (want > in_page)
            want = in_page;
        if (!user_range_ok(addr, want) || !user_range_mapped(addr, want, VM_PROT_READ)) {
            dst[done] = '\0';
            return -EFAULT;
        }
        arch_user_access_begin();
        for (size_t i = 0; i < want; i++) {
            char c = ((const char *)(uintptr_t)addr)[i];
            dst[done + i] = c;
            if (c == '\0') {
                arch_user_access_end();
                return (int)(done + i);
            }
        }
        arch_user_access_end();
        done += want;
    }
    dst[done] = '\0';
    return -ENAMETOOLONG;
}
