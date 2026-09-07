#ifndef _UNISTD_H
#define _UNISTD_H
#include <cosmo/syscall.h>
#include <stddef.h>
#include <sys/types.h>
#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
#define F_OK 0
#define R_OK 4
#define W_OK 2
#define X_OK 1
extern char **environ;
ssize_t read(int fd, void *buf, size_t n);
ssize_t write(int fd, const void *buf, size_t n);
int close(int fd);
off_t lseek(int fd, off_t off, int whence);
pid_t getpid(void);
pid_t getppid(void);
int chdir(const char *path);
char *getcwd(char *buf, size_t size);
/* The machine's name, from the caller's uts namespace. Setting it is
 * privileged; HOST_NAME_MAX includes the terminator. */
#define HOST_NAME_MAX COSMO_HOST_NAME_MAX
int gethostname(char *buf, size_t size);
int sethostname(const char *name, size_t len);
/* Narrow the calls this process may make: bit N of `mask` allows number
 * N, `words` 64-bit words of it, the rest denied. Intersects with what
 * is in force, is inherited by children, and needs no privilege -- it
 * only ever takes authority away. A denied call kills the process with
 * SIGSYS (docs/kernel/security/design.md §1f). */
int syscall_filter(const uint64_t *mask, size_t words);
#define SYSCALL_ALLOW(mask, nr) ((mask)[(nr) / 64] |= 1ull << ((nr) % 64))
int dup(int fd);
int dup2(int fd, int newfd);
/* dup with fewer rights than the original: `rights` is a subset of
 * COSMO_RIGHT_* that the caller already holds (0 keeps them all).
 * EPERM when it asks for anything the original does not carry. */
int dup_rights(int fd, int newfd, unsigned rights);
int pipe(int fd[2]);
int isatty(int fd);
unsigned sleep(unsigned seconds);
int usleep(unsigned long usec);
int unlink(const char *path);
int rmdir(const char *path);
int access(const char *path, int mode);
void sync(void);
uid_t getuid(void);
uid_t geteuid(void);
gid_t getgid(void);
gid_t getegid(void);
int setuid(uid_t uid);
int setgid(gid_t gid);
int setresuid(uid_t ruid, uid_t euid, uid_t suid);   /* (uid_t)-1 keeps an id */
int setresgid(gid_t rgid, gid_t egid, gid_t sgid);
int getresuid(uid_t *ruid, uid_t *euid, uid_t *suid);
int getresgid(gid_t *rgid, gid_t *egid, gid_t *sgid);
int getgroups(int size, gid_t list[]);
int setgroups(size_t size, const gid_t *list);
void _exit(int status) __attribute__((noreturn));
#endif
