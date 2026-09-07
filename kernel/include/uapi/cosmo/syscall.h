/*
 * uapi/cosmo/syscall.h - CosmoOS native system-call ABI.
 *
 * Shared verbatim between the kernel and user space. This is user ABI:
 * numbers and structures here are stable; add, never renumber.
 *
 * Convention (x86-64): number in rax, arguments in rdi rsi rdx r10 r8
 * r9, result in rax; negative results are -errno. rcx and r11 are
 * clobbered by the instruction.
 * Convention (AArch64): `svc #0` with the number in x8, arguments in
 * x0..x5, result in x0; negative results are -errno.
 */

#ifndef UAPI_COSMO_SYSCALL_H
#define UAPI_COSMO_SYSCALL_H

#include <stddef.h>
#include <stdint.h>

#define SYS_exit      0   /* (int status) -> never returns */
#define SYS_write     1   /* (int h, const void *buf, size_t len) -> bytes */
#define SYS_read      2   /* (int h, void *buf, size_t len) -> bytes */
#define SYS_getpid    3   /* () -> pid */
#define SYS_yield     4   /* () -> 0 */
#define SYS_sleep_ns  5   /* (uint64_t ns) -> 0 */
#define SYS_clock_ns  6   /* (unsigned clock) -> nanoseconds: COSMO_CLOCK_MONOTONIC (0, since boot) or COSMO_CLOCK_REALTIME (1, since 1970) */
#define SYS_mmap      7   /* (void *hint, size_t len, int prot, int flags) -> addr */
#define SYS_munmap    8   /* (void *addr, size_t len) -> 0 */
#define SYS_log       9   /* (const char *s, size_t len) -> 0 */
#define SYS_close     10  /* (int h) -> 0 */
/* Phase 7: the filesystem calls. */
#define SYS_open      11  /* (const char *path, int flags, uint32_t mode) -> handle */
#define SYS_stat      12  /* (const char *path, struct cosmo_stat *st) -> 0 */
#define SYS_fstat     13  /* (int h, struct cosmo_stat *st) -> 0 */
#define SYS_lseek     14  /* (int h, int64_t off, int whence) -> new position */
#define SYS_mkdir     15  /* (const char *path, uint32_t mode) -> 0 */
#define SYS_unlink    16  /* (const char *path) -> 0 */
#define SYS_rmdir     17  /* (const char *path) -> 0 */
#define SYS_rename    18  /* (const char *old, const char *new) -> 0 */
#define SYS_getdents  19  /* (int h, void *buf, size_t len) -> bytes, 0 at end */
#define SYS_sync      20  /* () -> 0 */
#define SYS_mount     21  /* (const char *source, const char *target, const char *fstype, unsigned flags) -> 0 */
#define SYS_umount    22  /* (const char *target, unsigned flags) -> 0 */
/* Phase 8: sockets. */
#define SYS_socket    23  /* (int family, int type, int proto) -> handle */
#define SYS_bind      24  /* (int h, const struct cosmo_sockaddr *, size_t len) -> 0 */
#define SYS_listen    25  /* (int h, int backlog) -> 0 */
#define SYS_accept    26  /* (int h, struct cosmo_sockaddr *peer, size_t *len) -> handle */
#define SYS_connect   27  /* (int h, const struct cosmo_sockaddr *, size_t len) -> 0 */
#define SYS_sendto    28  /* (int h, const void *buf, size_t len, const struct cosmo_sockaddr *to (NULL ok), size_t tolen) -> bytes */
#define SYS_recvfrom  29  /* (int h, void *buf, size_t len, struct cosmo_sockaddr *from (NULL ok), size_t *fromlen) -> bytes */
#define SYS_shutdown  30  /* (int h, int how) -> 0 */
#define SYS_getsockname 31 /* (int h, struct cosmo_sockaddr *, size_t *len) -> 0 */
/* Phase 9: processes, pipes, the working directory, introspection. */
#define SYS_spawn     32  /* (const struct cosmo_spawn *req) -> pid */
#define SYS_wait      33  /* (int pid, int *status, unsigned flags) -> pid, 0 with WNOHANG */
#define SYS_kill      34  /* (int pid, int sig) -> 0 */
#define SYS_pipe      35  /* (int h[2]) -> 0; h[0] reads, h[1] writes */
#define SYS_dup       36  /* (int h, int target, unsigned rights) -> handle; target -1 = lowest
                         free, rights COSMO_RIGHTS_SAME to keep them or a subset to drop some */
#define SYS_getppid   37  /* () -> parent pid, 0 for kernel-created processes */
#define SYS_chdir     38  /* (const char *path) -> 0 */
#define SYS_getcwd    39  /* (char *buf, size_t len) -> length written (without NUL) */
#define SYS_procinfo  40  /* (struct cosmo_procinfo *buf, size_t count) -> total processes */
#define SYS_klog      41  /* (char *buf, size_t len) -> bytes of the newest whole log lines */
#define SYS_sysctl    42  /* (const char *name, char *buf, size_t len) -> value length */
#define SYS_vm_create   43  /* (int vmm_handle) -> VM handle; needs /dev/vmm open for writing */
#define SYS_vm_mem      44  /* (int vm, uint64_t gpa, uint64_t len) -> 0: zeroed guest memory region */
#define SYS_vm_mem_rw   45  /* (int vm, uint64_t gpa, void *buf, size_t len, int write) -> bytes */
#define SYS_vcpu_create 46  /* (int vm, unsigned index) -> vCPU handle at the reset state */
#define SYS_vcpu_regs   47  /* (int vcpu, struct cosmo_vcpu_regs *regs, int set) -> 0 */
#define SYS_vcpu_run    48  /* (int vcpu, struct cosmo_vm_exit *exit) -> 0; runs until an exit */
#define SYS_vcpu_irq    49  /* (int vcpu, unsigned vector) -> 0: make a vector pending (>= 32) */
/* Credentials (POSIX real/effective/saved ids; -1 keeps an id). Unprivileged
 * callers may set an id only to one they already hold; euid 0 is privileged. */
#define SYS_setresuid   50  /* (int64_t ruid, int64_t euid, int64_t suid) -> 0 */
#define SYS_setresgid   51  /* (int64_t rgid, int64_t egid, int64_t sgid) -> 0 */
#define SYS_getresuid   52  /* (uint32_t *ruid, uint32_t *euid, uint32_t *suid) -> 0 */
#define SYS_getresgid   53  /* (uint32_t *rgid, uint32_t *egid, uint32_t *sgid) -> 0 */
#define SYS_setgroups   54  /* (const uint32_t *groups, size_t n) -> 0; privileged; n <= 16 */
#define SYS_getgroups   55  /* (uint32_t *groups, size_t n) -> count; n == 0 queries the count */
/* Resource limits (docs/kernel/security/design.md §2). */
#define SYS_getrlimit   56  /* (unsigned resource, uint64_t *value) -> 0 */
#define SYS_setrlimit   57  /* (unsigned resource, uint64_t value) -> 0; raising a limit needs privilege */
#define SYS_ioready     58  /* (int h) -> COSMO_IO_* mask of what would not block now */
#define SYS_setnonblock 59  /* (int h, int on) -> 0; -EOPNOTSUPP for objects that never block */
/* Milestone 9: the asynchronous I/O ring (docs/kernel/io/). */
#define SYS_aio_create  60  /* (unsigned entries 1..1024, unsigned flags 0) -> ring handle */
#define SYS_aio_submit  61  /* (int ring, const struct cosmo_sqe *sqes, unsigned n) -> entries accepted */
#define SYS_aio_wait    62  /* (int ring, struct cosmo_cqe *cqes, unsigned n, unsigned min, uint64_t timeout_ns) -> completions */
#define SYS_COUNT       63

#define COSMO_RLIMIT_AS     0   /* bytes of user address space mapped by regions */
#define COSMO_RLIMIT_MEM    1   /* bytes of anonymous memory populated (resident) */
#define COSMO_RLIMIT_NOFILE 2   /* open handles (at most the table size, 64) */
#define COSMO_RLIMIT_NPROC  3   /* processes with the caller's real uid, counting a new child */
#define COSMO_RLIMIT_VMEM   4   /* guest memory per VM the process creates */
#define COSMO_RLIMIT_COUNT  5
#define COSMO_RLIM_INFINITY (~0ull)

#define COSMO_NGROUPS_MAX 16

/* A handle's rights (docs/kernel/object/architecture.md, "Rights").
 * Bits 0..15 are generic; 16..31 belong to the object's type. */
#define COSMO_RIGHT_READ     (1u << 0)
#define COSMO_RIGHT_WRITE    (1u << 1)
#define COSMO_RIGHT_DUP      (1u << 2)
#define COSMO_RIGHT_TRANSFER (1u << 3)
#define COSMO_RIGHT_MANAGE   (1u << 4)
/* dup and spawn: keep what the caller holds. Zero, not all-ones,
 * deliberately -- a zeroed field or a register left over from a caller
 * that predates this argument then asks for the old behaviour, and
 * "a handle with no rights at all" is not a request worth being able
 * to make. */
#define COSMO_RIGHTS_SAME    0u

/* spawn: the child receives exactly the mapped handles.
 * handles == NULL with nr_handles == 0 means "0, 1, 2 as they are". */
/* With COSMO_SPAWN_HANDLE_RIGHTS. Without it the map is an array of
 * just the first two fields, which is what it was before rights. */
struct cosmo_spawn_handle {
    int child;      /* slot in the child */
    int parent;     /* handle in the caller */
    /* What the child gets: COSMO_RIGHTS_SAME (zero) for the caller's
     * own rights, or a subset of them. Rights only ever shrink, so a
     * process can give away a read-only view of what it can write. */
    unsigned rights;
    unsigned pad;
};
struct cosmo_spawn {
    const char *path;
    const char *const *argv;                   /* NULL-terminated, argv[0] required */
    const char *const *envp;                   /* NULL-terminated or NULL */
    const struct cosmo_spawn_handle *handles;
    size_t nr_handles;
    const char *cwd;                           /* NULL: inherit */
    unsigned flags;                            /* COSMO_SPAWN_* or 0 */
    uint32_t uid, gid;                         /* with COSMO_SPAWN_SETCRED: the child's ids */
    /* Added after the fields above, and read only when
     * COSMO_SPAWN_SETROOT is set, so a caller built against the older
     * header passes the older struct and the kernel never reads past
     * what it gave. */
    const char *root;                          /* with COSMO_SPAWN_SETROOT */
};

/* What a caller that predates `root` passes: everything up to it. */
#define COSMO_SPAWN_SIZE_V1 __builtin_offsetof(struct cosmo_spawn, root)
/* The child starts with real, effective and saved ids `uid`/`gid` and no
 * supplementary groups. A privileged caller names any ids; an unprivileged
 * one only ids it holds (docs/kernel/security/design.md §1). */
#define COSMO_SPAWN_SETCRED (1u << 0)
/* The handle map is an array of struct cosmo_spawn_handle as declared
 * above -- child, parent, rights, pad. Without this flag the kernel
 * reads the two-int elements that were the whole of the map before
 * rights existed, and gives the child the caller's own rights. A
 * program built against the older header therefore keeps working
 * unchanged; libc sets the flag. */
#define COSMO_SPAWN_HANDLE_RIGHTS (1u << 1)
/* The child's root is `root`, resolved in the caller's own namespace.
 * The child then cannot name anything outside it: absolute paths start
 * there and ".." stops there. Privileged, like SETCRED -- a process
 * that could give itself a root could also give itself one it does not
 * own the contents of. Privilege flows down and never up. */
#define COSMO_SPAWN_SETROOT (1u << 2)
#define COSMO_ARG_MAX   2048   /* argv + envp string bytes; at most 128 entries in all */
#define COSMO_ARG_ENTRIES 128
#define COSMO_PATH_MAX  1024   /* = VFS_PATH_MAX */

#define COSMO_WNOHANG 1u
/* Signals are numbers only in this phase: every one terminates the target
 * with status 128 + sig; there are no handlers. */
#define COSMO_SIGHUP  1
#define COSMO_SIGINT  2
#define COSMO_SIGKILL 9
#define COSMO_SIGSEGV 11
#define COSMO_SIGTERM 15
#define COSMO_NSIG    32

struct cosmo_procinfo {
    uint32_t pid, ppid, uid, gid;
    uint32_t state;         /* 0 running, 1 exiting, 2 exited (zombie) */
    uint32_t nr_threads;
    uint64_t syscalls;
    uint64_t run_ns;        /* CPU time of its threads */
    char name[32];
};

#define COSMO_AF_INET  2
#define COSMO_AF_INET6 10
#define COSMO_SOCK_STREAM 1
#define COSMO_SOCK_DGRAM  2
#define COSMO_SOCK_NONBLOCK 0x800   /* ORed into the type: the socket starts non-blocking */

/* Clocks (SYS_clock_ns). */
#define COSMO_CLOCK_MONOTONIC 0
#define COSMO_CLOCK_REALTIME  1

/* Readiness bits (SYS_ioready). */
#define COSMO_IO_READABLE 1
#define COSMO_IO_WRITABLE 2
#define COSMO_IO_HANGUP   4
#define COSMO_IO_ERROR    8

/* The asynchronous I/O ring (docs/kernel/io/design.md). */
#define COSMO_AIO_MAX_ENTRIES 1024
#define COSMO_AIO_NOP    0
#define COSMO_AIO_READ   1   /* the object's read at its position: result = bytes, 0 at end of file */
#define COSMO_AIO_WRITE  2   /* the object's write: result = bytes */
#define COSMO_AIO_PREAD  3   /* a file, at `offset` */
#define COSMO_AIO_PWRITE 4
#define COSMO_AIO_FSYNC  5   /* a file: result 0 */
#define COSMO_AIO_POLL   6   /* result = the COSMO_IO_* bits set among `events` */
#define COSMO_AIO_F_NOWAIT 1 /* complete with -EAGAIN instead of waiting for readiness */
#define COSMO_AIO_WAIT_FOREVER (~0ull)

struct cosmo_sqe {
    uint8_t op;          /* COSMO_AIO_* */
    uint8_t flags;       /* COSMO_AIO_F_* */
    uint16_t events;     /* POLL: bits of interest */
    int32_t handle;
    uint64_t addr;       /* user buffer */
    uint64_t len;
    uint64_t offset;     /* PREAD/PWRITE */
    uint64_t user_data;  /* returned in the completion */
};                       /* 40 bytes */

struct cosmo_cqe {
    uint64_t user_data;
    int64_t result;      /* bytes, a mask, 0, or -COSMO_E* */
};                       /* 16 bytes */
#define COSMO_SHUT_RD   0
#define COSMO_SHUT_WR   1
#define COSMO_SHUT_RDWR 2

/* One address shape for both families: addr holds 4 (AF_INET) or 16
 * (AF_INET6) bytes in network byte order; port is in host byte order. */
struct cosmo_sockaddr {
    uint16_t family;
    uint16_t port;
    uint32_t flowinfo;
    uint8_t  addr[16];
    uint32_t scope;
};

/* open() flags. */
#define COSMO_O_RDONLY    0x0000
#define COSMO_O_WRONLY    0x0001
#define COSMO_O_RDWR      0x0002
#define COSMO_O_ACCMODE   0x0003
#define COSMO_O_CREAT     0x0040
#define COSMO_O_EXCL      0x0080
#define COSMO_O_TRUNC     0x0200
#define COSMO_O_APPEND    0x0400
#define COSMO_O_DIRECTORY 0x10000

/* lseek() whence. */
#define COSMO_SEEK_SET 0
#define COSMO_SEEK_CUR 1
#define COSMO_SEEK_END 2

/* Directory entry and stat types. */
#define COSMO_DT_UNKNOWN 0
#define COSMO_DT_REG     1
#define COSMO_DT_DIR     2
#define COSMO_DT_CHR     3
#define COSMO_DT_FIFO    4
#define COSMO_DT_SOCK    5

struct cosmo_stat {
    uint64_t ino;
    uint32_t type;      /* COSMO_DT_* */
    uint32_t mode;      /* permission bits */
    uint32_t nlink;
    uint32_t uid;
    uint32_t gid;
    uint32_t pad;
    uint64_t size;
    uint64_t mtime_ns;
    uint64_t ctime_ns;
};

/* getdents() record: reclen bytes, name NUL terminated, 8-byte aligned. */
struct cosmo_dirent {
    uint64_t ino;
    uint16_t reclen;
    uint8_t  type;      /* COSMO_DT_* */
    uint8_t  namelen;
    char     name[];
};

/* mount() flags. */
#define COSMO_MOUNT_RDONLY (1u << 0)

/* umount() flags. FORCE skips the final commit and drops the open
 * transaction (recovery when a commit keeps failing). */
#define COSMO_UMOUNT_FORCE (1u << 0)

/* mmap protection and flags. */
#define COSMO_PROT_NONE  0
#define COSMO_PROT_READ  (1 << 0)
#define COSMO_PROT_WRITE (1 << 1)
#define COSMO_PROT_EXEC  (1 << 2)

#define COSMO_MAP_ANONYMOUS (1 << 0)
#define COSMO_MAP_FIXED     (1 << 1)  /* hint is a requirement */

/* Error numbers (subset, values as in kernel/errno.h). */
#define COSMO_EPERM   1
#define COSMO_ENOENT  2
#define COSMO_ESRCH   3
#define COSMO_EINTR   4
#define COSMO_E2BIG   7
#define COSMO_ENOEXEC 8
#define COSMO_ECHILD  10
#define COSMO_EACCES  13
#define COSMO_ENOTTY  25
#define COSMO_ESPIPE  29
#define COSMO_ERANGE  34
#define COSMO_EIO     5
#define COSMO_EBADF   9
#define COSMO_EAGAIN  11
#define COSMO_ENOMEM  12
#define COSMO_EFAULT  14
#define COSMO_EBUSY   16
#define COSMO_EEXIST  17
#define COSMO_EINVAL  22
#define COSMO_EMFILE  24
#define COSMO_ENOSPC  28
#define COSMO_ENOSYS  38
#define COSMO_EXDEV   18
#define COSMO_ENODEV  19
#define COSMO_ENOTDIR 20
#define COSMO_EISDIR  21
#define COSMO_EFBIG   27
#define COSMO_EROFS   30
#define COSMO_ENOTEMPTY 39
#define COSMO_ENAMETOOLONG 36
#define COSMO_EPIPE   32
#define COSMO_EMSGSIZE 90
#define COSMO_EOPNOTSUPP 95
#define COSMO_EAFNOSUPPORT 97
#define COSMO_EADDRINUSE 98
#define COSMO_EADDRNOTAVAIL 99
#define COSMO_ECONNRESET 104
#define COSMO_EISCONN 106
#define COSMO_ENOTCONN 107
#define COSMO_ETIMEDOUT 110
#define COSMO_EALREADY 114
#define COSMO_EINPROGRESS 115
#define COSMO_ECONNREFUSED 111
#define COSMO_EHOSTUNREACH 113

/* Exit status reported for a process terminated by a fatal fault. */
#define COSMO_EXIT_FAULT 139

/* Standard handles of a new process. */
#define COSMO_STDIN  0
#define COSMO_STDOUT 1
#define COSMO_STDERR 2

/* Auxiliary vector entries on the initial stack. */
#define COSMO_AT_NULL   0
#define COSMO_AT_PAGESZ 6
#define COSMO_AT_ENTRY  9


/* --- virtualization (docs/kernel-services/virtualization/) --- */

/* Segment attributes in the architectural descriptor layout, which is
 * neither vendor's control-block packing: a backend translates on the
 * way in and out (docs/kernel-services/virtualization/design.md). Bits
 * 9-11 are reserved and must be zero. */
#define COSMO_SEG_TYPE     0x000Fu   /* descriptor type */
#define COSMO_SEG_S        0x0010u   /* code/data, 0 = system */
#define COSMO_SEG_DPL      0x0060u
#define COSMO_SEG_DPL_SHIFT 5u
#define COSMO_SEG_P        0x0080u   /* present */
#define COSMO_SEG_UNUSABLE 0x0100u   /* no segment loaded */
#define COSMO_SEG_AVL      0x1000u
#define COSMO_SEG_L        0x2000u   /* 64-bit code segment */
#define COSMO_SEG_DB       0x4000u
#define COSMO_SEG_G        0x8000u   /* limit counts 4 KiB pages */
#define COSMO_SEG_RESERVED 0x0E00u

struct cosmo_vcpu_seg {          /* 16 bytes */
    uint16_t selector;
    uint16_t attrib;             /* COSMO_SEG_* */
    uint32_t limit;
    uint64_t base;
};

/* VMState: the whole architectural register file of a virtual CPU.
 *
 * Per architecture, because a register file is: `rax` and `cs` mean
 * nothing on AArch64 and `x0`/`sctlr_el1` mean nothing on x86-64. Both
 * blocks are 448 bytes so the system call, the copies and the tests do
 * not vary with the architecture
 * (docs/kernel-services/virtualization/design.md). */
#if defined(__aarch64__)
struct cosmo_vcpu_regs {         /* 448 bytes */
    uint64_t x[31];              /* x0..x30 (x30 = LR) */
    uint64_t sp_el1, sp_el0;
    uint64_t pc, pstate;         /* ELR_EL1 and SPSR of the guest */
    /* The EL1 system state a guest owns. */
    uint64_t sctlr_el1, ttbr0_el1, ttbr1_el1, tcr_el1, mair_el1, amair_el1;
    uint64_t vbar_el1, esr_el1, far_el1, elr_el1, spsr_el1;
    uint64_t tpidr_el0, tpidrro_el0, tpidr_el1, contextidr_el1, cpacr_el1;
    uint64_t par_el1, mdscr_el1;
    uint64_t pending_irq;        /* get: lowest pending vector, or ~0 when none; set: ignored */
    uint64_t reserved[8];
};
#else
struct cosmo_vcpu_regs {         /* 448 bytes */
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp, rsp, r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rip, rflags;
    struct cosmo_vcpu_seg cs, ds, es, fs, gs, ss, ldtr, tr;
    struct cosmo_vcpu_seg gdtr, idtr;   /* selector and attrib unused */
    uint64_t cr0, cr2, cr3, cr4, cr8;
    uint64_t efer;
    uint64_t dr6, dr7;
    uint64_t pending_irq;        /* get: lowest pending vector, or ~0 when none; set: ignored */
    uint64_t reserved[9];
};
#endif

/* VMExit: why vcpu_run returned. */
struct cosmo_vm_exit {           /* 64 bytes */
    uint32_t kind;               /* COSMO_VM_EXIT_* */
    uint32_t flags;              /* COSMO_VM_EXIT_F_* */
    uint64_t rip;                /* guest rip at the exit (after IN/OUT: the next instruction) */
    union {
        struct { uint16_t port; uint8_t size; uint8_t write; uint8_t string; uint8_t rep; uint16_t pad;
                 uint32_t value; uint32_t pad2; } io;
        struct { uint64_t gpa; uint32_t write; uint32_t pad; } mmio;
        struct { uint32_t iss; uint8_t reg; uint8_t write; uint16_t pad; uint32_t pad2; } sysreg;
        struct { uint64_t nr, a0, a1, a2, a3; } hypercall;
        struct { uint32_t code; uint32_t pad; uint64_t info1, info2; } fail;
        uint64_t raw[6];
    };
};

#define COSMO_VM_EXIT_HLT       1u  /* halted; inject a vector and run again */
#define COSMO_VM_EXIT_IO        2u  /* port I/O no device claimed; a read completes on the next run */
#define COSMO_VM_EXIT_MMIO      3u  /* access to guest-physical memory with no region */
#define COSMO_VM_EXIT_HYPERCALL 4u  /* VMMCALL: nr = rax, args rbx rcx rdx rsi */
#define COSMO_VM_EXIT_SHUTDOWN  5u  /* triple fault: the vCPU is dead */
#define COSMO_VM_EXIT_FAIL      6u  /* the hardware refused the state, or an unknown exit */
#define COSMO_VM_EXIT_WFI       7u  /* AArch64: the guest waited for an interrupt (x86: HLT) */
#define COSMO_VM_EXIT_SYSREG    8u  /* AArch64: a trapped system register (x86: CPUID/MSR) */

#define COSMO_VM_EXIT_F_IRQ_PENDING 1u  /* a pending vector was not delivered yet */

#define COSMO_HV_VMS_MAX     8u
#define COSMO_HV_VCPUS_MAX   4u
#define COSMO_HV_VM_MEM_MAX  (64u << 20)

#endif /* UAPI_COSMO_SYSCALL_H */
