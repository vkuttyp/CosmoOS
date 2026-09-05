/*
 * uaccess.c - Kernel access to user memory (docs/kernel/memory/design.md §6.1).
 *
 * The range is checked against the user window; the copy itself is the
 * architecture primitive whose faulting instructions carry exception
 * fixups. A page that is unmapped, PROT_NONE, or lacks the permission,
 * or a demand-zero fault that finds no memory, ends the copy with
 * -EFAULT instead of a kernel fault. There is no walk of the region list
 * and therefore no check-then-copy window: a concurrent munmap makes
 * the copy fail, never the kernel.
 */

#include <kernel/errno.h>
#include <kernel/panic.h>
#include <kernel/process.h>
#include <kernel/string.h>
#include <kernel/lockdep.h>
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

/* One access window around the raw copy; the result is bytes not copied. */
static size_t raw_copy(void *dst, const void *src, size_t len)
{
    arch_user_access_begin();
    size_t left = arch_copy_user_raw(dst, src, len);
    arch_user_access_end();
    return left;
}

int copy_from_user(void *dst, uint64_t user_src, size_t len)
{
    might_sleep();   /* a demand fault allocates: never under a spinlock */
    if (len == 0)
        return 0;
    if (!user_range_ok(user_src, len))
        return -EFAULT;
    return raw_copy(dst, (const void *)(uintptr_t)user_src, len) ? -EFAULT : 0;
}

int copy_to_user(uint64_t user_dst, const void *src, size_t len)
{
    might_sleep();   /* a demand fault allocates: never under a spinlock */
    if (len == 0)
        return 0;
    if (!user_range_ok(user_dst, len))
        return -EFAULT;
    return raw_copy((void *)(uintptr_t)user_dst, src, len) ? -EFAULT : 0;
}

int strncpy_from_user(char *dst, uint64_t user_src, size_t max)
{
    might_sleep();   /* a demand fault allocates: never under a spinlock */
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
        if (!user_range_ok(addr, want)) {
            dst[done] = '\0';
            return -EFAULT;
        }
        size_t left = raw_copy(dst + done, (const void *)(uintptr_t)addr, want);
        size_t got = want - left;
        for (size_t i = 0; i < got; i++) {
            if (dst[done + i] == '\0')
                return (int)(done + i);
        }
        if (left) {
            dst[done] = '\0';   /* the string ran into an inaccessible page */
            return -EFAULT;
        }
        done += want;
    }
    dst[done] = '\0';
    return -ENAMETOOLONG;
}
