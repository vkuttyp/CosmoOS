# libc

The native C library (docs/libc/). Applications reach the kernel only
through it: `include/` holds the standard headers (`stdio.h`,
`stdlib.h`, `string.h`, `unistd.h`, `fcntl.h`, `dirent.h`, `spawn.h`,
`sys/wait.h`, `signal.h`, `sys/socket.h`, ...) plus `cosmo/` for the
native extras (`procinfo`, `klog_read`, `sysctl_get`) and the raw
system-call wrappers (`cosmo/syscall.h`, internal); `src/` holds
`crt0.S`, `errno`, strings, the allocator, stdio and `printf`, the
system-call wrappers, directories, `spawnve`/`spawnvp`/`waitpid`/`kill`,
sockets with `inet_pton`/`inet_ntop`. `libc.mk` builds `libc.a` and
`crt0.o`; every program links against them. Single-threaded, no
`fork`. Floating point is there since the FP/SIMD unit: `%f`, `%e` and
`%g` over `double`, converted by scaling and integer division -- no
`long double`, no `%a`, no libm, and no claim of an exactly rounded last
digit. Host test:
`tests/host/test_libc.c`.
