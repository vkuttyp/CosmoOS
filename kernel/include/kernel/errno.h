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

#define EPERM     1   /* operation not permitted */
#define ENOENT    2   /* no such entry */
#define EIO       5   /* I/O error */
#define ENOEXEC   8   /* executable format error */
#define EBADF     9   /* bad handle */
#define EAGAIN    11  /* try again */
#define ENOMEM    12  /* out of memory */
#define EFAULT    14  /* bad address */
#define EBUSY     16  /* resource busy */
#define EEXIST    17  /* already exists */
#define EXDEV     18  /* cross-device link */
#define ENODEV    19  /* no such device */
#define ENOTDIR   20  /* not a directory */
#define EISDIR    21  /* is a directory */
#define EINVAL    22  /* invalid argument */
#define EMFILE    24  /* too many open handles */
#define EFBIG     27  /* file too large */
#define ENOSPC    28  /* no space left */
#define ESPIPE    29  /* not seekable */
#define EROFS     30  /* read-only device */
#define ERANGE    34  /* result out of range */
#define ENAMETOOLONG 36 /* string too long */
#define ENOSYS    38  /* not implemented */
#define ENOTEMPTY 39  /* directory not empty */
#define ELOOP     40  /* too many levels */
#define ENOTSUP   95  /* operation not supported */
#define ETIMEDOUT 110 /* operation timed out */
#define ENOKEY    126 /* required key not available */
#define EKEYREJECTED 129 /* key or signature rejected */

#endif /* KERNEL_ERRNO_H */
