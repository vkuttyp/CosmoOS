# libc: design

## Layout

```text
libc/
  include/          the public headers (standard names) and cosmo/ (native)
  src/
    arch/<arch>/crt0.S   _start (x86_64/, aarch64/)
    errno.c         errno, __syscall_ret, strerror table
    string.c        mem*/str*
    ctype.c
    malloc.c        the allocator
    stdlib.c        __libc_start, exit/atexit/abort/__assert_fail, getenv/setenv/unsetenv
    conv.c          atoi/strtol/strtoul/strtoll/strtoull, abs/labs, qsort (also compiled on the host by test_libc)
    printf.c        vsnprintf core; printf/fprintf/dprintf/snprintf on top
    stdio.c         FILE, buffering, fopen/fread/fgets/..., puts/putchar
    unistd.c        open/read/write/close/lseek/stat/fstat/dup/dup2/pipe/chdir/getcwd/isatty/...
    dirent.c        opendir/readdir/closedir over getdents
    process.c       spawnve/spawnvp, waitpid/wait, kill, getpid/getppid, sleep/nanosleep
    socket.c        socket calls, inet_pton/inet_ntop
    cosmo.c         procinfo/klog/sysctl wrappers
  libc.mk
```

## Conventions

- Every system call goes through `static inline long cosmo_*()` in
  `cosmo/syscall.h`, then `__syscall_ret(long r)`: `if (r < 0) { errno = -r; return -1; } return r;`
  (pointer-returning calls such as `mmap` have their own check).
- No function in the library blocks on anything but a system call; no
  static buffers except the `FILE` objects and `strerror`'s table.
- `-mgeneral-regs-only`, `-fno-builtin` is **not** used: the compiler
  may turn loops into `memcpy`/`memset` calls, which the library
  provides; `string.c` is compiled with `-fno-builtin` itself so those
  functions are not turned into calls to themselves.
- Headers include only what they need and carry no kernel types; the
  UAPI structures (`struct cosmo_stat`, `struct cosmo_dirent`,
  `struct cosmo_sockaddr`, ...) are the ABI and are used directly with
  standard typedef names layered on (`struct stat` is
  `struct cosmo_stat` with `st_*` accessors as macros: `st_size`,
  `st_mode`, `st_ino`; `S_ISDIR`/`S_ISREG`/`S_ISCHR`/`S_ISFIFO`/
  `S_ISSOCK` test the `type` field, not mode bits).

## Program start

```asm
; x86-64 (libc/src/arch/x86_64/crt0.S)
_start: xor %ebp,%ebp; mov (%rsp),%rdi; lea 8(%rsp),%rsi; lea 16(%rsp,%rdi,8),%rdx
        mov %rdx, environ(%rip); and $-16,%rsp; call __libc_start   ; never returns
; AArch64 (libc/src/arch/aarch64/crt0.S)
_start: mov x29, xzr; mov x30, xzr; ldr x0, [sp]; add x1, sp, #8
        add x2, x1, x0, lsl #3; add x2, x2, #8; and sp, sp, #-16; bl __libc_start
```

Both emit the same `.note.cosmo` ABI note. The raw system-call wrapper in
`cosmo/syscall.h` is `syscall` (number in `rax`) on x86-64 and `svc #0`
(number in `x8`, arguments `x0`–`x5`, result `x0`) on AArch64; the
numbers and structures are identical.

`__libc_start(argc, argv, envp)` stores `environ`, initialises the three
`FILE`s, calls `main`, then `exit(main's return)`. `exit` runs `atexit`
handlers in reverse, flushes every open `FILE`, and calls `_exit` (the
`SYS_exit` wrapper).

## The allocator

Arenas of 64 KiB (or the request rounded up to a page for anything
larger than 16 KiB, which gets its own mapping, marked with a `BIG`
flag bit and keeping the mapping size in `prev_size`, and is unmapped on
`free`) from `mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_ANONYMOUS)`.
Inside an arena, blocks carry a 16-byte header `{ size_t size; size_t
prev_size; }` with the low bits of `size` marking "in use" and "big";
every arena ends in a zero-size in-use sentinel; a singly linked free list
threads through free blocks; `malloc` is first-fit with splitting (a
remainder smaller than 32 bytes is absorbed), `free` coalesces with the
physical neighbours using `prev_size`, `realloc` grows in place when
the next block is free, else copies. Alignment 16. Zero-size `malloc`
returns a unique pointer. Failures return NULL with `errno = ENOMEM`.
No thread safety (none needed).

## stdio

```c
struct _FILE { int fd; unsigned char *buf; size_t cap, len, pos; unsigned flags; /* READ WRITE EOF ERR LINEBUF UNBUF */
               int ungot; struct _FILE *next; };
```

`stdout` is line-buffered (`fflush` after a `\n` reaches the buffer),
`stderr` unbuffered, everything else fully buffered. A `FILE` list lets
`exit`/`fflush(NULL)` flush all. Reading and writing on the same `FILE`
are separated by `fflush`/`fseek` as in C (no automatic switching;
mixing sets `ERR`). `fgets` reads through the buffer one `read` at a
time (a `read` on the console returns one line, so an interactive
`fgets` needs one system call). `ungetc` holds one byte.

`vsnprintf` is the single formatting engine: a `struct out { char *p; size_t n, cap; }`
sink with a `put` function, so `printf` formats into a 256-byte stack
buffer in pieces (no allocation), `dprintf` writes to a handle directly,
`snprintf` writes to the caller's buffer and returns the full length.
Supported: `%[flags][width][.prec](l|ll|z|h)?[diuxXocsp%]`; `%s` with
NULL prints `(null)`; unknown conversions print the raw text.

## unistd and friends

Direct wrappers with `__syscall_ret`. Notes:

- `read` on a handle returns what the kernel returns (short reads are
  normal on the console and pipes; a single call moves at most 1024
  bytes).
- `dup2(old, new)` is `SYS_dup(old, new)`; `dup(old)` is `SYS_dup(old, -1)`.
- `isatty(fd)`: `fstat` and `type == COSMO_DT_CHR`.
- `getcwd(buf, size)`: `SYS_getcwd`; with `buf == NULL` allocates.
- `stat`/`fstat` fill `struct stat` (the UAPI structure).
- `opendir`: `open(path, O_RDONLY|O_DIRECTORY)`; `readdir` refills a
  4 KiB buffer with `getdents`, returns `struct dirent { ino_t d_ino; unsigned char d_type; char d_name[256]; }`
  copied out of the kernel record; `closedir` closes.
- `mount(source, target, fstype, flags)`, `umount(target)`,
  `umount2(target, flags)`.

## Processes

```c
struct spawn_handle { int child; int parent; };     /* = struct cosmo_spawn_handle */
pid_t spawnve(const char *path, const char *const argv[], const char *const envp[],
              const struct spawn_handle *h, size_t nh);
pid_t spawnvp(const char *file, const char *const argv[], const struct spawn_handle *h, size_t nh);   /* PATH search, environ */
pid_t waitpid(pid_t pid, int *status, int options);   /* WNOHANG */
pid_t wait(int *status);
int kill(pid_t pid, int sig);
```

`spawnvp` searches `PATH` (default `/bin:/sbin:/usr/bin:/usr/sbin` when unset) when `file`
has no `/`, trying `stat` on each candidate and spawning the first
regular file; it sets `errno` to the last error (`ENOENT` when none).
With `h == NULL, nh == 0` the child inherits handles 0, 1, 2 as they
are; the shell passes an explicit map for redirections and pipes. The
status from `waitpid` is the process's exit status as an int
(`0..255` for `exit`, `128 + sig` for a kill, 139 for a fault);
`WEXITSTATUS(s)` is `(s)` and `WIFSIGNALED(s)` is `(s) > 128`, provided
for reading comfort.

## Sockets

`socket`, `bind`, `listen`, `accept`, `connect`, `sendto`, `recvfrom`,
`send`, `recv`, `shutdown`, `getsockname` over the native calls;
`struct sockaddr` is the native family-tagged shape (`sa_family`,
`sa_port` in host order, `sa_flowinfo`, `sa_addr[16]`, `sa_scope`);
there is no `sockaddr_in`/`sockaddr_in6`. `htons` etc., `inet_pton`/
`inet_ntop` (pure C; the IPv6 form supports `::` compression). Nothing here is exercised by Phase 9's programs beyond
`init --selftest`; it exists so the network tools of a later phase have
their API.

## Error handling

Every failure sets `errno` and returns -1/NULL/EOF. The library never
prints or exits on its own except `abort` (writes `abort()` to stderr,
`_exit(134)`) and a failed `assert` (`file:line: assertion 'x' failed`,
then `abort`).

## Security

The library trusts its caller (same process) and the kernel; it copies
nothing from the kernel except through the system-call contracts. Buffers
it hands the kernel are sized by the caller. `getenv` returns pointers
into `environ`; `setenv` allocates.

## Performance

Irrelevant at this scale; the allocator is O(n) in free blocks per call,
`printf` is O(output). Recorded: a `memcpy` in C, no `rep movsb`.

## Future extensibility

- Threads: `errno` becomes TLS, the allocator and stdio take locks.
- Floating point once user FPU state is saved: `%f/%g/%e`, `strtod`,
  `<math.h>`.
- Linux compatibility lives in `compat/linux/` (Phase 11), not here
  (invariant 7): this library is the native one, and its `crt0.S`
  carries the note that marks a program as native.
- A wall clock (`time`, `gettimeofday`) when the kernel has one.
