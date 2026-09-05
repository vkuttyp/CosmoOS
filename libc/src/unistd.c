/* unistd.c - Files, handles, directories, memory, time: thin wrappers. */

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "libc.h"

int open(const char *path, int flags, ...)
{
    unsigned mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, unsigned);
        va_end(ap);
    }
    return (int)__syscall_ret(cosmo_open(path, flags, mode));
}

ssize_t read(int fd, void *buf, size_t n) { return __syscall_ret(cosmo_read(fd, buf, n)); }
ssize_t write(int fd, const void *buf, size_t n) { return __syscall_ret(cosmo_write(fd, buf, n)); }
int close(int fd) { return (int)__syscall_ret(cosmo_close(fd)); }
off_t lseek(int fd, off_t off, int whence) { return __syscall_ret(cosmo_lseek(fd, off, whence)); }
int stat(const char *path, struct stat *st) { return (int)__syscall_ret(cosmo_stat(path, (struct cosmo_stat *)st)); }
int fstat(int fd, struct stat *st) { return (int)__syscall_ret(cosmo_fstat(fd, (struct cosmo_stat *)st)); }
int mkdir(const char *path, mode_t mode) { return (int)__syscall_ret(cosmo_mkdir(path, mode)); }
int unlink(const char *path) { return (int)__syscall_ret(cosmo_unlink(path)); }
int rmdir(const char *path) { return (int)__syscall_ret(cosmo_rmdir(path)); }
int rename(const char *oldpath, const char *newpath) { return (int)__syscall_ret(cosmo_rename(oldpath, newpath)); }
void sync(void) { cosmo_sync(); }
pid_t getpid(void) { return (pid_t)cosmo_getpid(); }
pid_t getppid(void) { return (pid_t)cosmo_getppid(); }
int chdir(const char *path) { return (int)__syscall_ret(cosmo_chdir(path)); }
int dup(int fd) { return (int)__syscall_ret(cosmo_dup(fd, -1)); }
int dup2(int fd, int newfd) { return (int)__syscall_ret(cosmo_dup(fd, newfd)); }
int pipe(int fd[2]) { return (int)__syscall_ret(cosmo_pipe(fd)); }

int mount(const char *source, const char *target, const char *fstype, unsigned flags)
{
    return (int)__syscall_ret(cosmo_mount(source, target, fstype, flags));
}
int umount2(const char *target, unsigned flags) { return (int)__syscall_ret(cosmo_umount2(target, flags)); }
int umount(const char *target) { return umount2(target, 0); }

char *getcwd(char *buf, size_t size)
{
    if (buf == NULL) {
        size = size ? size : 1024;
        buf = malloc(size);
        if (buf == NULL)
            return NULL;
        if (__syscall_ret(cosmo_getcwd(buf, size)) < 0) {
            free(buf);
            return NULL;
        }
        return buf;
    }
    return __syscall_ret(cosmo_getcwd(buf, size)) < 0 ? NULL : buf;
}

int isatty(int fd)
{
    struct stat st;
    if (fstat(fd, &st) < 0)
        return 0;
    if (!S_ISCHR(st.st_type)) {
        errno = ENOTTY;
        return 0;
    }
    return 1;
}

int access(const char *path, int mode)
{
    struct stat st;
    if (stat(path, &st) < 0)
        return -1;
    if ((mode & X_OK) && !(st.st_mode & 0111)) {
        errno = EACCES;
        return -1;
    }
    return 0;
}

void *mmap(void *hint, size_t len, int prot, int flags, int fd, long off)
{
    (void)fd;
    (void)off;
    long r = cosmo_mmap(hint, len, prot, flags);
    if (r < 0 && r > -4096) {
        errno = (int)-r;
        return MAP_FAILED;
    }
    return (void *)r;
}

int munmap(void *addr, size_t len) { return (int)__syscall_ret(cosmo_munmap(addr, len)); }

int nanosleep(const struct timespec *req, struct timespec *rem)
{
    (void)rem;
    if (req->tv_sec < 0 || req->tv_nsec < 0 || req->tv_nsec >= 1000000000L) {
        errno = EINVAL;
        return -1;
    }
    uint64_t ns = (uint64_t)req->tv_sec * 1000000000ULL + (uint64_t)req->tv_nsec;
    return (int)__syscall_ret(cosmo_sleep_ns(ns));
}

unsigned sleep(unsigned seconds)
{
    struct timespec ts = { .tv_sec = (time_t)seconds, .tv_nsec = 0 };
    return nanosleep(&ts, NULL) < 0 ? seconds : 0;
}

int usleep(unsigned long usec)
{
    struct timespec ts = { .tv_sec = (time_t)(usec / 1000000UL), .tv_nsec = (long)(usec % 1000000UL) * 1000L };
    return nanosleep(&ts, NULL);
}

int clock_gettime(clockid_t clk, struct timespec *ts)
{
    if (clk != CLOCK_MONOTONIC) {
        errno = EINVAL;
        return -1;
    }
    uint64_t ns = cosmo_clock_ns();
    ts->tv_sec = (time_t)(ns / 1000000000ULL);
    ts->tv_nsec = (long)(ns % 1000000000ULL);
    return 0;
}
