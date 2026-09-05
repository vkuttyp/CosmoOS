# libc: architecture

Phase 9 of the roadmap. Constitution section 46 (applications reach the
kernel only through libc; kernel internals are not exposed), section 45
(a traditional Unix userland), section 61 (ABI testing), invariant 2
(userland never sees kernel pointers), invariant 7 (the native ABI is
not contaminated by Linux compatibility).

## Where it sits

```text
   programs (userland/)      #include <stdio.h> <unistd.h> <spawn.h> ...
        │
        ▼
   libc/include/             standard headers: the names programs know
   libc/src/                 libc.a: string, memory, stdio, stdlib, errno, unistd, dirent, spawn, wait, ...
   libc/crt0.S               program entry: _start → main → exit
        │
        ▼
   libc/include/cosmo/syscall.h    raw wrappers over the SYSCALL instruction (Phase 4; kept, internal)
   kernel/include/uapi/cosmo/      the ABI: numbers, structs, flags, errno values (shared with the kernel)
        │
        ▼
   kernel/syscall/native.c
```

Programs see only the standard headers. The `cosmo/` and `uapi/cosmo/`
headers stay available (the shell's `spawn` and the tools' `procinfo`
are native calls without a POSIX name), but a program that uses them
knows it is native.

## Purpose

Give the shell and the utilities a C library that is small enough to
read in an afternoon and complete enough that the programs read like
ordinary Unix C: `printf`, `fopen`, `strtol`, `malloc`, `open`, `read`,
`opendir`, `spawn`, `waitpid`, `errno`.

## Responsibilities

- **Program start**: `crt0.S` (`_start`: argc/argv/envp from the System
  V stack, `environ`, `main`, `exit`). One copy for every program
  (replaces the copy under `userland/init/`).
- **Errors**: `errno` (a global; programs are single-threaded), the
  negative-return convention translated once in `__syscall_ret`,
  `strerror`, `perror`.
- **Memory** (`string.h`): `mem*`, `str*`, `strl*`, `strtok_r`,
  `strdup`. Written in plain C; no inline assembly in the library.
- **Allocation** (`stdlib.h`): `malloc`, `calloc`, `realloc`, `free`
  over `mmap` (anonymous, 64 KiB arenas; a first-fit free list with
  headers and coalescing of neighbours), `abort`, `exit`, `atexit` (32
  handlers), `getenv`, `setenv`, `atoi`, `strtol`, `strtoul`, `qsort`.
- **stdio** (`stdio.h`): `FILE` with a 1 KiB buffer, `stdin`/`stdout`/
  `stderr`, `fopen`/`fdopen`/`fclose`, `fread`/`fwrite`/`fgets`/`fgetc`/
  `fputs`/`fputc`/`puts`/`putchar`, `fflush`, `printf` family including
  `snprintf`/`vsnprintf`/`dprintf`. Formats: `d i u x X o c s p %`, length
  `l ll z`, flags `- 0 +`, width and precision (also `*`). No floating
  point (programs are built with `-mgeneral-regs-only`: the kernel does
  not yet save FPU state for user threads; `%f` prints `?`).
  `stdout` is line-buffered, `stderr` unbuffered, files fully buffered;
  `exit` flushes.
- **Files and handles** (`unistd.h`, `fcntl.h`, `sys/stat.h`,
  `dirent.h`): `open`, `close`, `read`, `write`, `lseek`, `stat`, `fstat`,
  `mkdir`, `rmdir`, `unlink`, `rename`, `chdir`, `getcwd`, `dup`, `dup2`,
  `pipe`, `isatty` (fstat type), `sync`, `opendir`/`readdir`/`closedir`
  over `getdents`, `mount`/`umount` (`sys/mount.h`, native flags).
- **Processes** (`spawn.h`, `sys/wait.h`, `signal.h`, `unistd.h`):
  `cosmo_spawn` exposed as `spawnve(path, argv, envp, handles, n)` with
  `spawnvp` doing the `PATH` search; `waitpid`, `wait`, `WNOHANG`;
  `kill`, `SIGKILL`/`SIGTERM`/`SIGINT` numbers; `getpid`, `getppid`,
  `_exit`, `sleep`, `usleep`, `nanosleep`.
- **Native extras** (`cosmo/procinfo.h`, `cosmo/klog.h`, `cosmo/sysctl.h`):
  thin wrappers with the kernel's structures, for `ps`, `dmesg`, `sysctl`.
- **Character classes** (`ctype.h`), `assert.h`, `limits.h`
  (`PATH_MAX` = `VFS_PATH_MAX` 1024, `ARG_MAX` 2048), `stdbool.h`/
  `stdint.h`/`stddef.h`/`stdarg.h` from the compiler.
- **Sockets** (`sys/socket.h`, `netinet/in.h`, `arpa/inet.h`): the
  socket system calls with the native address structure and
  `inet_pton`/`inet_ntop` for IPv4 and IPv6. No name resolution.

## Non-responsibilities

- Threads, TLS, locking inside the library (single-threaded processes).
- Floating point, `<math.h>`, locales, wide characters, `time.h`
  calendar functions (there is no wall clock; `clock_gettime` gives the
  monotonic clock only).
- `fork`, `exec*`, `system`, `popen` (no `fork`), signal handlers
  (`signal`, `sigaction`), `setjmp` (nothing needs it yet), `termios`,
  `getpwnam` (no user database), dynamic linking, `dlopen`.
- POSIX conformance as a goal: the names match POSIX where the kernel's
  semantics do; where they do not (`spawn` instead of `fork`,
  handle-based `stat` types, wait status without encoding macros) the
  native shape wins and `api.md` says so.

## Interfaces at a glance

| Header | Contents | Backed by |
|---|---|---|
| `errno.h` | `errno`, `E*` (values = `COSMO_E*`) | `__syscall_ret` |
| `string.h`, `ctype.h`, `stdlib.h`, `stdio.h`, `assert.h`, `limits.h` | as above | pure C, `mmap`, `write`/`read` |
| `unistd.h`, `fcntl.h`, `sys/stat.h`, `dirent.h`, `sys/mount.h` | files, handles, directories | system calls 1–22, 35–39 |
| `spawn.h`, `sys/wait.h`, `signal.h` | processes | system calls 32–34, 37 |
| `sys/socket.h`, `netinet/in.h`, `arpa/inet.h` | sockets | system calls 23–31 |
| `cosmo/procinfo.h`, `cosmo/klog.h`, `cosmo/sysctl.h` | native introspection | system calls 40–42 |
| `cosmo/syscall.h` | raw wrappers (internal) | `SYSCALL` (x86-64) or `SVC #0` (AArch64) |

Build: `libc/libc.mk` compiles `libc/src/*.c` and
`libc/src/arch/$(ARCH)/crt0.S` with the user flags into
`$(OUT)/libc/libc.a` and `crt0.o`; `userland.mk` links every program as
`crt0.o program.o ... libc.a` with `user.ld`.

Tests (`testing.md`): `make host-test` compiles the pure parts
(`string`, `stdlib` conversions and `qsort`, `vsnprintf`, the allocator
over a fake `mmap`) with ASan/UBSan against the host and checks them
(`test_libc`); `init --selftest` exercises the system-call-backed parts
on the target (`USERTEST: PASS`); the shell and utilities are the
integration test.
