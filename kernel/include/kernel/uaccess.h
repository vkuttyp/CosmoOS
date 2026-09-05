/*
 * uaccess.h - The only way kernel code touches user memory.
 *
 * Every helper checks the range against the user address window, then
 * copies inside an architecture user-access window (STAC/CLAC with SMAP,
 * PAN on AArch64) with a primitive whose faulting instructions carry
 * exception fixups (kernel/extable.h). Demand-zero faults on the
 * process's anonymous regions are serviced during the copy; any other
 * fault, including an allocation failure, makes the helper return
 * -EFAULT (docs/kernel/memory/design.md §6.1).
 *
 * All helpers may block (a demand-zero fault allocates). From a thread
 * without a process every user address is -EFAULT.
 */

#ifndef KERNEL_UACCESS_H
#define KERNEL_UACCESS_H

#include <kernel/types.h>

#include <arch/mmu.h>

/* True if [addr, addr+len) lies inside the user window without overflow. */
bool user_range_ok(uint64_t addr, size_t len);

/* Return 0 or -EFAULT. len 0 is always fine. */
int copy_from_user(void *dst, uint64_t user_src, size_t len);
int copy_to_user(uint64_t user_dst, const void *src, size_t len);

/* Copy a NUL-terminated string of at most max-1 bytes; always terminates
 * dst. Returns the length or -EFAULT / -ENAMETOOLONG. */
int strncpy_from_user(char *dst, uint64_t user_src, size_t max);

#endif /* KERNEL_UACCESS_H */
