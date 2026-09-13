/*
 * tcb.h - the per-thread block libc keeps behind the thread pointer
 * (docs/audit/next-subsystem-errno-tls.md).
 *
 * `errno` is per-thread, and this is where it lives. The kernel keeps a
 * thread pointer for every thread and SYS_set_tls sets it; the layout
 * behind that pointer is libc's, not the kernel's, and this header is it.
 *
 * Every thread libc knows about has a block before its first instruction:
 * the first thread's is a static object installed by `__libc_start` before
 * anything else runs, and a thread created by `cosmo_thread_start` has one
 * in the mapping that carries its stack. The accessor is therefore
 * unconditional -- one load of the thread pointer and an offset, with no
 * case for "no block" -- because on x86-64 no such case can exist: reading
 * `%fs:0` with a zero base dereferences address zero and faults before any
 * check could run.
 *
 * The consequence is a contract, and it is stated in cosmo/thread.h beside
 * the raw syscall as well: **a thread created by a raw SYS_thread_create
 * with `tls = 0` must not call libc.** `cosmo_tcb_install` is the way for a
 * program that wants such a thread to use libc anyway.
 *
 * **The prefix is permanent.** A program that installs its own block must
 * leave libc's fields intact and put its own storage at offset
 * COSMO_TCB_SIZE or beyond. On x86-64 the first word must point at the
 * block itself: the architecture cannot read the FS base without
 * `rdfsbase`, so `%fs:0` is how the block is found, which is the only
 * reason `self` exists. Changing either would break every compiled binary.
 */

#ifndef COSMO_TCB_H
#define COSMO_TCB_H

#include <stddef.h>

#define COSMO_TCB_SIZE 128u

/*
 * Aligned to 16 by the type, not by each definition: the thread pointer
 * must be 16-byte aligned (SYS_set_tls refuses otherwise), and the struct's
 * natural alignment is only 8 because it leads with a pointer. Leaving that
 * to whoever defines a block means it holds until a linker happens to place
 * one at an 8-mod-16 address -- which is exactly what happened, to one
 * program out of the suite, the first time this was built.
 */
struct __cosmo_tcb {
    struct __cosmo_tcb *self;   /* x86-64 reads this at %fs:0 */
    int err;                    /* errno */
    unsigned tid;               /* cached, so cosmo_thread_id() costs no syscall */
    char reserved[112];         /* to COSMO_TCB_SIZE: 8 + 4 + 4 + 112 */
} __attribute__((aligned(16)));

/*
 * Take over the calling thread's block, from storage the caller owns.
 * `len` must be at least COSMO_TCB_SIZE and `block` 16-byte aligned;
 * returns 0, -EINVAL, or whatever SYS_set_tls refused. The block's
 * lifetime is the caller's problem and must outlast the thread.
 *
 * This is for a thread made outside libc's wrapper. A thread from
 * `cosmo_thread_start` already has one, and installing another leaks
 * nothing but loses the tid cache until it is set again.
 */
int cosmo_tcb_install(void *block, size_t len);

#endif /* COSMO_TCB_H */
