# libc: API

The public headers under `libc/include/` are the interface. Names follow
POSIX and C11 where the kernel's semantics do; where they do not, the
native shape wins and is marked **native** below. **ABI stability**:
the header names and prototypes are meant to stay; the library is
static, so a rebuilt program picks up any change. Every failing call
sets `errno` and returns -1, NULL or EOF unless stated.

## Conventions (`libc/src/libc.h`, internal)

`long __syscall_ret(long r)`: a result in `[-4095, -1]` becomes
`errno = -r`, return -1; anything else is returned unchanged.
`__libc_start(argc, argv, envp)` (called by `crt0.S`) stores `environ`,
initialises stdio, calls `main`, then `exit`. `__stdio_init`,
`__stdio_flush_all` are internal.

## errno.h

`extern int errno` (a global; processes are single-threaded).
`E*` names are the `COSMO_E*` values of `uapi/cosmo/syscall.h`:
`EPERM` 1, `ENOENT` 2, `ESRCH` 3, `EINTR` 4, `EIO` 5, `E2BIG` 7,
`ENOEXEC` 8, `EBADF` 9, `ECHILD` 10, `EAGAIN` 11, `ENOMEM` 12,
`EACCES` 13, `EFAULT` 14, `EBUSY` 16, `EEXIST` 17, `EXDEV` 18,
`ENODEV` 19, `ENOTDIR` 20, `EISDIR` 21, `EINVAL` 22, `EMFILE` 24,
`ENOTTY` 25, `EFBIG` 27, `ENOSPC` 28, `ESPIPE` 29, `EROFS` 30,
`EPIPE` 32, `ERANGE` 34, `ENAMETOOLONG` 36, `ENOSYS` 38,
`ENOTEMPTY` 39, and the socket errors of Phase 8 (`EMSGSIZE`,
`EOPNOTSUPP`, `EAFNOSUPPORT`, `EADDRINUSE`, `EADDRNOTAVAIL`,
`ECONNRESET`, `EISCONN`, `ENOTCONN`, `ETIMEDOUT`, `ECONNREFUSED`,
`EHOSTUNREACH`). `char *strerror(int)` (static text; unknown numbers
give `Unknown error N` from a static buffer), `void perror(const char *)`.

## sys/types.h

`pid_t` (int), `ssize_t` (long), `off_t` (long), `mode_t` (unsigned),
`ino_t` (uint64_t), `uid_t`, `gid_t` (unsigned), `time_t` (long),
`socklen_t` (unsigned).

## string.h, ctype.h

`memcpy`, `memmove`, `memset`, `memcmp`, `memchr`, `strlen`, `strnlen`,
`strcpy`, `strncpy`, `strcat`, `strncat`, `strcmp`, `strncmp`, `strchr`,
`strrchr`, `strstr`, `strspn`, `strcspn`, `strpbrk`, `strtok_r`,
`strdup`, `strndup` (malloc), `strlcpy`, `strlcat` (BSD semantics),
`strerror`. Plain C, compiled with `-fno-builtin` so the compiler cannot
turn them into calls to themselves. `ctype.h`: `isalpha`, `isdigit`,
`isalnum`, `isspace`, `isupper`, `islower`, `isprint`, `isxdigit`,
`ispunct`, `toupper`, `tolower` as functions, ASCII only.

## stdlib.h

- `malloc`, `calloc`, `realloc`, `free`: the allocator of
  `libc/src/malloc.c` (design.md). `malloc(0)` returns a unique block;
  a request above `2^40` bytes is `ENOMEM`; `free` of a block already
  free calls `abort`. Alignment 16.
- `exit(status)`: `atexit` handlers in reverse order, flush every open
  `FILE`, `_exit`. `_exit(status)`: `SYS_exit`. `abort()`: writes
  `abort()` to handle 2 and exits with 134. `atexit` holds 32 handlers
  (-1 when full).
- `getenv`, `setenv(name, value, overwrite)` (`EINVAL` for an empty
  name or one containing `=`; may reallocate `environ`), `unsetenv`.
- `atoi`, `atol`, `strtol`, `strtoul`, `strtoll`, `strtoull` (base 0
  detects `0x` and a leading `0`; `ERANGE` on overflow with the
  saturated value; `*end` is `s` when no digits were read), `abs`,
  `labs`, `qsort` (quicksort with insertion sort below nine elements).
- `EXIT_SUCCESS` 0, `EXIT_FAILURE` 1.

## stdio.h

`FILE` (opaque `struct _FILE`), `stdin`, `stdout`, `stderr`, `EOF`,
`BUFSIZ` (1024), `SEEK_SET/CUR/END`. `fopen(path, mode)` with `r`, `w`,
`a` and `+` (`EINVAL` otherwise; files are created with mode 0644),
`fdopen`, `fclose` (flushes, closes the handle, frees), `fflush(f)`
(NULL flushes every stream), `fread`, `fwrite`, `fgetc`, `getc`,
`getchar`, `fgets`, `ungetc` (one byte), `fputc`, `putc`, `putchar`,
`fputs`, `puts`, `feof`, `ferror`, `clearerr`, `fileno`, `fseek`,
`ftell`, `rewind`, `remove` (unlink, then rmdir for a directory),
`rename`. `printf`, `fprintf`, `dprintf` (to a handle), `sprintf`,
`snprintf` (returns the full length, terminates when `n > 0`),
`vprintf`, `vfprintf`, `vdprintf`, `vsnprintf`.

Buffering: `stdout` line-buffered (flushed when a `\n` enters the
buffer), `stderr` unbuffered, other streams fully buffered (1 KiB). A
stream is either reading or writing; switching flushes or discards the
buffer (`fflush` on a reading stream drops the read-ahead). `fgets` on
the console returns one typed line per system call.

Formats: `%[flags][width][.prec][h|hh|l|ll|z|j|t][d i u x X o c s p %]`
with flags `-`, `0`, `+`, space, `#`, and `*` for width and precision.
`%s` with NULL prints `(null)`. `%f`, `%e` and `%g` (and their capital
forms) take a `double`, with `inf` and `nan` spelled out and the sign
before any zero padding; precision defaults to 6 and is capped at 17,
the digits a `double` carries; `#` keeps the decimal point that a
precision of zero would drop and `%g`'s trailing zeros. No `long
double`, no `%a`, no libm: the
conversion scales and divides, which is exact enough for diagnostics and
does not claim an exactly rounded last digit. A value whose integer part
will not fit 64 bits is printed in exponent form whatever was asked for.
An unknown conversion prints the `%` and the character.

## unistd.h, fcntl.h, sys/stat.h, dirent.h, sys/mount.h, sys/mman.h, time.h

- `open(path, flags, [mode])` with `O_RDONLY`, `O_WRONLY`, `O_RDWR`,
  `O_CREAT`, `O_EXCL`, `O_TRUNC`, `O_APPEND`, `O_DIRECTORY` (the native
  `COSMO_O_*` values); `read`, `write`, `close`, `lseek`, `unlink`,
  `rmdir`, `rename`, `mkdir(path, mode)`, `sync`, `access(path, mode)`
  (`stat` plus a mode-bit check for `X_OK`), `isatty(fd)` (`fstat` and
  `S_ISCHR`; sets `ENOTTY`), `getpid`, `getppid`, `chdir`, `getcwd(buf,
  size)` (NULL `buf` allocates), `dup`, `dup2`, `pipe`, `sleep`
  (seconds), `usleep`, `nanosleep` (through `SYS_sleep_ns`; at most one
  hour), `clock_gettime(CLOCK_MONOTONIC, ts)`.
- **native** `struct stat`: the layout of `struct cosmo_stat` with the
  fields `st_ino`, `st_type` (`DT_*`), `st_mode`, `st_nlink`, `st_uid`,
  `st_gid`, `st_size`, `st_mtime_ns`, `st_ctime_ns`. `S_ISREG`, `S_ISDIR`,
  `S_ISCHR`, `S_ISFIFO`, `S_ISSOCK` take `st_type`, not `st_mode`.
  `stat`, `fstat` (works on files, the console, pipe ends).
- `DIR`, `struct dirent { d_ino, d_type, d_name[256] }`, `opendir`
  (`O_RDONLY | O_DIRECTORY`), `readdir` (4 KiB `getdents` refills; a
  malformed record sets `EIO`), `closedir`. `DT_UNKNOWN/REG/DIR/CHR/
  FIFO/SOCK`.
- `mount(source, target, fstype, flags)`, `umount`, `umount2(target,
  flags)`, `MS_RDONLY`, `MNT_FORCE`.
- `mmap(hint, len, prot, flags, fd, off)` (anonymous only: `fd` and `off`
  are ignored, `MAP_FAILED` on error), `munmap`, `PROT_*`, `MAP_ANONYMOUS`,
  `MAP_FIXED`, `MAP_PRIVATE` (0).

## spawn.h, sys/wait.h, signal.h (**native**)

```c
struct spawn_handle { int child; int parent; };
pid_t spawnve(const char *path, const char *const argv[], const char *const envp[],
              const struct spawn_handle *h, size_t nh);
pid_t spawnvp(const char *file, const char *const argv[], const struct spawn_handle *h, size_t nh);
pid_t spawnve_as(const char *path, const char *const argv[], const char *const envp[],
                 const struct spawn_handle *h, size_t nh, uid_t uid, gid_t gid);   /* COSMO_SPAWN_SETCRED */
pid_t waitpid(pid_t pid, int *status, int options);   /* WNOHANG */
pid_t wait(int *status);
int kill(pid_t pid, int sig);
```

`spawnve` fills `struct cosmo_spawn` (`cwd` NULL, `flags` 0) and calls
`SYS_spawn`; `spawnve_as` sets `COSMO_SPAWN_SETCRED` with the uid and
gid the child starts as (`docs/kernel/security/design.md` §1; `EPERM`
for ids the caller may not grant). `cosmo_getrlimit`/`cosmo_setrlimit`
in `cosmo/syscall.h` are the raw resource-limit calls; the child receives exactly the handles in `h` (`h == NULL,
nh == 0`: handles 0, 1, 2 as they are). `spawnvp` uses `environ`; a
`file` containing `/` is used as is, otherwise each `PATH` directory
(default `/bin:/sbin:/usr/bin:/usr/sbin`) is tried with `stat` and the first regular file
is spawned; `errno` is the last error, `ENOENT` when nothing matched.
The `argv` arrays are `const char *const *` so string literals can be
passed under `-Wwrite-strings`.

The wait status is the process's exit status itself: `0..255` from
`exit`, `128 + sig` from a kill, 139 for a fault. `WEXITSTATUS(s)` is
`s`; `WIFEXITED(s)` is `s < 128`; `WIFSIGNALED(s)` is `s > 128 && s !=
139`; `WTERMSIG(s)` is `s - 128`. `waitpid(-1, ...)` waits for any
child; `WNOHANG` returns 0 when none has exited; `ECHILD` when there is
no such child.

```c
pid_t spawnve_pgrp(const char *path, const char *const argv[], const char *const envp[],
                   const struct spawn_handle *h, size_t nh, pid_t pgid);
pid_t spawnvp_pgrp(const char *file, const char *const argv[], const struct spawn_handle *h,
                   size_t nh, pid_t pgid);          /* COSMO_SPAWN_SETPGID */
int setpgid(pid_t pid, pid_t pgid);  pid_t getpgid(pid_t pid);  pid_t getpgrp(void);
pid_t setsid(void);                  pid_t getsid(pid_t pid);
pid_t tcgetpgrp(int fd);             int tcsetpgrp(int fd, pid_t pgid);   /* unistd.h */
```

The `_pgrp` spawns place the child in `pgid` -- or in a group of its
own, when that is 0 -- before its first instruction, which a `setpgid`
after the spawn cannot do: the window between them is one in which a
signal to the group misses the child. The session calls are the
kernel's, unchanged (`docs/kernel/process/design.md`, "Sessions and
process groups"); `tcsetpgrp` on an unclaimed terminal claims it for the
caller's session, and `tcgetpgrp` is `ENOTTY` outside the terminal's
session.

```c
typedef unsigned long sigset_t;
typedef struct { int si_signo, si_code; pid_t si_pid; unsigned si_detail; void *si_addr; } siginfo_t;
struct sigaction { union { void (*sa_handler)(int);
                           void (*sa_sigaction)(int, siginfo_t *, void *); };
                   sigset_t sa_mask; unsigned sa_flags; };
int sigaction(int sig, const struct sigaction *act, struct sigaction *old);
void (*signal(int sig, void (*handler)(int)))(int);
int sigprocmask(int how, const sigset_t *set, sigset_t *old);
int sigpending(sigset_t *set);
int raise(int sig);
int sigemptyset/sigfillset/sigaddset/sigdelset/sigismember(...);
```

`WUNTRACED` and `WCONTINUED` report a stopped or continued child without
reaping it; `WIFSTOPPED`, `WSTOPSIG` and `WIFCONTINUED` read those
statuses, and `WIFEXITED`/`WIFSIGNALED` gained an upper bound so they
cannot claim one.

Numbers: `SIGHUP` 1 … `SIGCHLD` 17, `SIGCONT` 18, `SIGSTOP` 19,
`SIGTSTP` 20, `SIGTTIN` 21, `SIGTTOU` 22,
`SIGSYS` 31, `NSIG` 32. Flags: `SA_NODEFER`, `SA_RESETHAND`,
`SA_RESTART`, and `SA_SIGINFO`, which is accepted and stripped -- the
kernel hands every handler all three arguments, so it distinguishes
nothing here and exists so that POSIX code compiles. `sigaction` fills
in the library's restorer (`__cosmo_sigreturn`, two instructions of
assembly, because the kernel finds the frame from the stack pointer and
a restorer must not touch it); `SIG_DFL` and `SIG_IGN` get none, and the
kernel refuses a real handler without one. `siginfo_t` is `struct
cosmo_siginfo` under POSIX names, with the layout asserted field by
field, so a handler reads the kernel's own record. `signal()` sets
`SA_RESTART`. There is no `sigaltstack`, no real-time signal, no queued
siginfo and no `sigsuspend`. Job control is here: `SIGSTOP` and friends
stop a process, `WUNTRACED`/`WCONTINUED` report it, and the shell has
`jobs`/`fg`/`bg`.

## sys/socket.h, netinet/in.h, arpa/inet.h

**native** `struct sockaddr { sa_family, sa_port (host order),
sa_flowinfo, sa_addr[16] (network order, 4 bytes used for AF_INET),
sa_scope }`, the layout of `struct cosmo_sockaddr`; `socklen_t` must
be at least its size (`EINVAL` otherwise). `socket`, `bind`, `listen`,
`accept`, `connect`, `sendto`, `recvfrom`, `send`, `recv`, `shutdown`,
`getsockname` (flags arguments are ignored). `AF_INET`, `AF_INET6`,
`SOCK_STREAM`, `SOCK_DGRAM`, `SHUT_*`. `htons`, `ntohs`, `htonl`,
`ntohl`. `inet_pton(family, text, out)` (1, 0 for bad text,
-1/`EAFNOSUPPORT`), `inet_ntop(family, addr, buf, size)` (IPv6 with the
longest zero run compressed; `ENOSPC` when the buffer is short).

## cosmo/procinfo.h, cosmo/klog.h, cosmo/sysctl.h (**native**)

`int procinfo(struct cosmo_procinfo *buf, size_t count)` returns the
total number of processes (fill `min(total, count)` records).
`ssize_t klog_read(char *buf, size_t len)` copies the newest whole
kernel log lines that fit. `int sysctl_get(const char *name, char *buf,
size_t len)` returns the value's length, writes it NUL-terminated when
it fits (truncated without NUL otherwise), `ENOENT` for an unknown name.

## cosmo/hv.h (**native**, Phase 12)

Raw wrappers for virtual machines (`docs/kernel-services/virtualization/api.md`):
`cosmo_vm_create(vmm_handle)`, `cosmo_vm_mem(vm, gpa, len)`,
`cosmo_vm_mem_read/write(vm, gpa, buf, len)`, `cosmo_vcpu_create(vm,
index)`, `cosmo_vcpu_get_regs/set_regs(vcpu, regs)`, `cosmo_vcpu_run(vcpu,
exit)`, `cosmo_vcpu_irq(vcpu, vector)`. They return the kernel's result
(negative errno) unchanged; `struct cosmo_vcpu_regs` and `struct
cosmo_vm_exit` come from the UAPI header. Used by `vmctl`.

## cosmo/syscall.h (raw wrappers, internal)

`cosmo_syscall0..6`, and one typed inline wrapper per system call
(`cosmo_exit` .. `cosmo_sysctl`), returning the raw kernel result. Used
by the library and by `init --selftest`, which checks kernel error
codes exactly.

## Program start (`libc/src/crt0.S`)

Since Phase 11 `crt0.S` also emits the CosmoOS ABI note (section
`.note.cosmo`: `namesz` 8, `descsz` 4, type 1, name `CosmoOS`, desc =
ABI version 1), which `user.ld` places in a `PT_NOTE`; the kernel runs
an executable without it under the Linux personality
(`docs/compat/linux/api.md`). Every program linked with `crt0.o` is
therefore native.

`_start`: `rbp = 0`, `rdi = argc`, `rsi = argv`, `rdx = envp`, stack
aligned to 16, `call __libc_start`. Every program is linked as
`crt0.o objects libc.a` with `userland/user.ld` (`libc/libc.mk`,
`userland/userland.mk`).
