/* errno.c - errno, the return-value translation, strerror. */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "libc.h"

int errno;

long __syscall_ret(long r)
{
    if (r < 0 && r > -4096) {
        errno = (int)-r;
        return -1;
    }
    return r;
}

static const char *const messages[] = {
    [0] = "Success",
    [EPERM] = "Operation not permitted",
    [ENOENT] = "No such file or directory",
    [ESRCH] = "No such process",
    [EINTR] = "Interrupted system call",
    [EIO] = "Input/output error",
    [E2BIG] = "Argument list too long",
    [ENOEXEC] = "Exec format error",
    [EBADF] = "Bad file descriptor",
    [ECHILD] = "No child processes",
    [EAGAIN] = "Resource temporarily unavailable",
    [ENOMEM] = "Cannot allocate memory",
    [EACCES] = "Permission denied",
    [EFAULT] = "Bad address",
    [EBUSY] = "Device or resource busy",
    [EEXIST] = "File exists",
    [EXDEV] = "Invalid cross-device link",
    [ENODEV] = "No such device",
    [ENOTDIR] = "Not a directory",
    [EISDIR] = "Is a directory",
    [EINVAL] = "Invalid argument",
    [EMFILE] = "Too many open files",
    [ENOTTY] = "Not a terminal",
    [EFBIG] = "File too large",
    [ENOSPC] = "No space left on device",
    [ESPIPE] = "Illegal seek",
    [EROFS] = "Read-only file system",
    [EPIPE] = "Broken pipe",
    [ERANGE] = "Result out of range",
    [ENAMETOOLONG] = "File name too long",
    [ENOSYS] = "Function not implemented",
    [ENOTEMPTY] = "Directory not empty",
    [EMSGSIZE] = "Message too long",
    [EOPNOTSUPP] = "Operation not supported",
    [EAFNOSUPPORT] = "Address family not supported",
    [EADDRINUSE] = "Address already in use",
    [EADDRNOTAVAIL] = "Address not available",
    [ECONNRESET] = "Connection reset by peer",
    [EISCONN] = "Socket is already connected",
    [ENOTCONN] = "Socket is not connected",
    [ETIMEDOUT] = "Connection timed out",
    [ECONNREFUSED] = "Connection refused",
    [EHOSTUNREACH] = "No route to host",
};

char *strerror(int err)
{
    static char unknown[32];
    if (err >= 0 && (size_t)err < sizeof(messages) / sizeof(messages[0]) && messages[err])
        return (char *)(uintptr_t)messages[err];
    snprintf(unknown, sizeof(unknown), "Unknown error %d", err);
    return unknown;
}

void perror(const char *s)
{
    if (s && *s)
        fprintf(stderr, "%s: %s\n", s, strerror(errno));
    else
        fprintf(stderr, "%s\n", strerror(errno));
}
