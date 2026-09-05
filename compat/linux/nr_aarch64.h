/*
 * nr_aarch64.h - Linux AArch64 system call numbers (the asm-generic table,
 * include/uapi/asm-generic/unistd.h). Selected by linux_abi.h. The x86-64
 * only calls (open, stat, poll, pipe, dup2, fork, access, rename, mkdir,
 * rmdir, creat, unlink, readlink, select, getpgrp, arch_prctl, time,
 * pause, ...) have no number here: the *at forms and ppoll replace them,
 * and the personality's table leaves those rows out (#ifdef).
 */

#ifndef COMPAT_LINUX_NR_AARCH64_H
#define COMPAT_LINUX_NR_AARCH64_H

#define LX_read 63
#define LX_write 64
#define LX_close 57
#define LX_fstat 80
#define LX_ppoll 73
#define LX_lseek 62
#define LX_mmap 222
#define LX_mprotect 226
#define LX_munmap 215
#define LX_brk 214
#define LX_rt_sigaction 134
#define LX_rt_sigprocmask 135
#define LX_rt_sigreturn 139
#define LX_ioctl 29
#define LX_pread64 67
#define LX_pwrite64 68
#define LX_readv 65
#define LX_writev 66
#define LX_sched_yield 124
#define LX_mremap 216
#define LX_msync 227
#define LX_madvise 233
#define LX_dup 23
#define LX_nanosleep 101
#define LX_getpid 172
#define LX_socket 198
#define LX_connect 203
#define LX_accept 202
#define LX_sendto 206
#define LX_recvfrom 207
#define LX_sendmsg 211
#define LX_recvmsg 212
#define LX_shutdown 210
#define LX_bind 200
#define LX_listen 201
#define LX_getsockname 204
#define LX_getpeername 205
#define LX_setsockopt 208
#define LX_getsockopt 209
#define LX_clone 220
#define LX_execve 221
#define LX_exit 93
#define LX_wait4 260
#define LX_kill 129
#define LX_uname 160
#define LX_fcntl 25
#define LX_fsync 82
#define LX_fdatasync 83
#define LX_getcwd 17
#define LX_chdir 49
#define LX_umask 166
#define LX_gettimeofday 169
#define LX_getrlimit 163
#define LX_sysinfo 179
#define LX_getuid 174
#define LX_getgid 176
#define LX_setuid 146
#define LX_setgid 144
#define LX_geteuid 175
#define LX_getegid 177
#define LX_setreuid 145
#define LX_setregid 143
#define LX_getgroups 158
#define LX_setgroups 159
#define LX_setresuid 147
#define LX_getresuid 148
#define LX_setresgid 149
#define LX_getresgid 150
#define LX_setpgid 154
#define LX_getppid 173
#define LX_setsid 157
#define LX_sigaltstack 132
#define LX_setrlimit 164
#define LX_sync 81
#define LX_gettid 178
#define LX_futex 98
#define LX_sched_setaffinity 122
#define LX_sched_getaffinity 123
#define LX_getdents64 61
#define LX_set_tid_address 96
#define LX_clock_gettime 113
#define LX_clock_nanosleep 115
#define LX_exit_group 94
#define LX_tgkill 131
#define LX_openat 56
#define LX_mkdirat 34
#define LX_newfstatat 79
#define LX_unlinkat 35
#define LX_renameat 38
#define LX_readlinkat 78
#define LX_faccessat 48
#define LX_set_robust_list 99
#define LX_accept4 242
#define LX_dup3 24
#define LX_pipe2 59
#define LX_prlimit64 261
#define LX_getrandom 278
#define LX_rseq 293
#define LX_clone3 435
#define LX_rt_sigpending 136
#define LX_rt_sigsuspend 133
#define LX_tkill 130

#endif /* COMPAT_LINUX_NR_AARCH64_H */
