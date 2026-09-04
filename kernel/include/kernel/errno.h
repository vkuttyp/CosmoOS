/*
 * errno.h - Kernel-internal error codes.
 *
 * Kernel APIs return 0 on success and a negative errno value on failure.
 * Values match the traditional Unix numbering so the user ABI can pass
 * them through unchanged later. Only codes the kernel actually uses are
 * defined; add more as subsystems need them.
 */

#ifndef KERNEL_ERRNO_H
#define KERNEL_ERRNO_H

#define EPERM   1   /* operation not permitted */
#define ENOENT  2   /* no such entry */
#define EIO     5   /* I/O error */
#define ENOMEM  12  /* out of memory */
#define EBUSY   16  /* resource busy */
#define EEXIST  17  /* already exists */
#define EINVAL  22  /* invalid argument */
#define ENOSPC  28  /* no space left */
#define ERANGE  34  /* result out of range */
#define ENOSYS  38  /* not implemented */

#endif /* KERNEL_ERRNO_H */
