# libc

Native C library. Applications reach the kernel only through libc and
the syscall layer. Today: `include/cosmo/syscall.h`, inline wrappers for
the 23 native system calls (`cosmo_write` … `cosmo_umount`) over the
`SYSCALL` instruction, plus the shared UAPI header
`kernel/include/uapi/cosmo/syscall.h` (numbers, flags, `struct
cosmo_stat`, `struct cosmo_dirent`, errno values). No `errno` variable
yet: wrappers return the kernel's negative errno.
