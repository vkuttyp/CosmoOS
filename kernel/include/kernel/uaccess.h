/*
 * uaccess.h - The only way kernel code touches user memory.
 *
 * Every helper checks the range against the user address window and
 * against the current process's regions with the required protection
 * before copying inside an architecture user-access window (STAC/CLAC
 * when SMAP exists). Demand-zero faults on the process's own anonymous
 * regions may occur during the copy and are serviced by the fault
 * handler; anything else is a kernel bug.
 *
 * All helpers may block (a demand-zero fault allocates). They must be
 * called from a thread that belongs to a process.
 */

#ifndef KERNEL_UACCESS_H
#define KERNEL_UACCESS_H

#include <kernel/types.h>

#include <arch/mmu.h>

/* True if [addr, addr+len) lies inside the user window without overflow. */
bool user_range_ok(uint64_t addr, size_t len);

/* True if every page of the range is inside a region of the current
 * process with all of `prot` (VM_PROT_READ / VM_PROT_WRITE). */
bool user_range_mapped(uint64_t addr, size_t len, vm_prot_t prot);

/* Return 0 or -EFAULT. len 0 is always fine. */
int copy_from_user(void *dst, uint64_t user_src, size_t len);
int copy_to_user(uint64_t user_dst, const void *src, size_t len);

/* Copy a NUL-terminated string of at most max-1 bytes; always terminates
 * dst. Returns the length or -EFAULT / -ENAMETOOLONG. */
int strncpy_from_user(char *dst, uint64_t user_src, size_t max);

#endif /* KERNEL_UACCESS_H */
