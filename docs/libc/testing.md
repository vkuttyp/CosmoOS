# libc: testing

## Overview

| Layer | Mechanism | Command |
|---|---|---|
| Host | `tests/host/test_libc.c`: the pure parts compiled with the host clang under ASan and UBSan, functions renamed with a `c_` prefix so they do not clash with the host's libc | `make host-test` |
| Target, user mode | `init --selftest` (`userland/init/init.c`): every system-call-backed function through the library, plus `malloc`/`realloc`, `snprintf`, `strtol`, `setenv`/`getenv`, stdio on a file, `opendir`/`readdir`, `inet_pton`/`inet_ntop` | `make test` (self-test builds) |
| Integration | The shell and the utilities are built on the library and exercised by `/etc/rc.test` and the interactive harness | `make test` |

## Host test (`tests/host/test_libc.c`, `make host-test`)

Includes `libc/src/printf.c`, `libc/src/malloc.c` and `libc/src/conv.c`
directly after `#define`-renaming the functions they define (`printf`
becomes `c_printf`, `malloc` becomes `c_malloc`, and so on) and after
supplying the few externals they need: `c_write`/`c_fwrite` sinks that
count bytes, `c_strlen`/`c_strnlen`, an `mmap` macro that carves a
static 1 MiB arena and counts calls, a `munmap` macro that counts calls,
and a `c_abort` that records a failure. The host's own headers are
included first so the renames never touch host declarations.

| Area | Checks |
|---|---|
| `snprintf` | `%d %i %u`; width, `-`, `0`, `+`, space flags; `%x %X %o %#x %#o`; `%s` with precision and width both ways; `%c`, `%%`; `%ld %lld %zu %hd %hhd`; `%p`; `%s` of NULL is `(null)`; truncation to 4 bytes returns the full length 9; NULL buffer with size 0 returns 5; `%*d %-*d %.*d`; `%.0d` of 0 prints nothing; `LLONG_MIN`; `%f` prints `?`; `%q` prints `%q` |
| conversions | `strtol` with leading blanks, sign and trailing text (`end` set); `strtoul` base 0 with `0x`, leading `0` and decimal; base 36; no digits leaves `end == s`; overflow both ways saturates with `ERANGE`; `atoi("+77")`, `atol("-9")` |
| `qsort` | 200 pseudo-random integers sorted; `n` of 0 and 1 |
| allocator | 64 blocks of `i*37+1` bytes, 16-byte aligned, written and read back; even blocks freed, odd blocks grown with `realloc` (contents kept), all freed; a 16000-byte block then fits an existing arena (no new mapping); a 100000-byte block maps once and its `free` unmaps once; `calloc` zeroes; `malloc(1 << 50)` is NULL; `malloc(0)` is a unique block |

Prints `libc  ok` or `libc  FAIL (n)` with one line per failed check.
Result: 6 host binaries pass (`test_buddy`, `test_slab`, `test_crypto`,
`test_modelf`, `test_cosmofs`, `test_libc`).

## Target (`init --selftest`)

`fs_selftest` uses `fopen`/`fprintf`/`fgets`/`feof`/`fclose` on
`/tmp/stdio.txt` and `opendir`/`readdir`/`closedir` on `/tmp/d`;
`net_selftest` uses the socket functions with `struct sockaddr`,
`inet_pton` for `127.0.0.1` and `fe80::1`, `inet_ntop` back;
`proc_selftest` uses `pipe`, `dup`, `dup2`, `fstat`, `isatty`,
`spawnvp`, `spawnve`, `waitpid` (with and without `WNOHANG`), `kill`,
`chdir`, `getcwd` (including `ERANGE` for a 4-byte buffer), `mkdir`,
`rmdir`, `getppid`, `procinfo`, `klog_read`, `sysctl_get`, `malloc`,
`realloc`, `free`, `snprintf`, `strtol`, `strtoul`, `setenv`, `getenv`.
The full list is in `docs/kernel/process/testing.md`. Every failure
prints `USERTEST: check failed: <expression> (errno N)`; the run ends
with `USERTEST: PASS` or `USERTEST: FAIL (n checks)`.

## Gaps and planned tests

- `string.c`, `stdio.c`, `dirent.c`, `unistd.c` have no host test; the
  string functions are exercised only through everything else.
- No fuzzing of `vsnprintf` or `strtol`.
- No test of `atexit` ordering or of `fflush(NULL)` beyond exit.
- No leak or fragmentation measurement of the allocator under a
  long-running program (nothing runs long yet).
