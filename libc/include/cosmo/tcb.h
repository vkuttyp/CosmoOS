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
 * the raw syscall as well: **a thread that calls libc must have a block
 * whose prefix is this one.** A raw SYS_thread_create with `tls = 0` has no
 * block; a raw create with a thread pointer of the caller's own design has
 * one libc will misread, overwriting whatever the caller keeps where
 * `errno` and the tid live. Before `errno` moved behind the thread pointer
 * the second case was harmless, which is exactly why it is written down
 * here. `cosmo_tcb_install` is the way for either to use libc anyway.
 *
 * **The block's fields are libc's, and the thread pointer's own offset is
 * not a program's to choose.** A program that installs its own storage
 * hands over `COSMO_TCB_STORAGE` bytes and leaves their contents to libc.
 * On x86-64 the first word must point at the block itself: the
 * architecture cannot read the FS base without `rdfsbase`, so `%fs:0` is
 * how the block is found, which is the only reason `self` exists.
 *
 * `reserved[]` is **libc's**, not the program's. An earlier version of
 * this header offered it as a program's own per-thread storage and called
 * the prefix permanent; on AArch64 those bytes are where the ELF ABI puts
 * `__thread` variables, so the offer could not be kept. A program that
 * wants per-thread storage uses `__thread`, which is what that keyword is
 * for (docs/audit/next-subsystem-pt-tls.md).
 */

#ifndef COSMO_TCB_H
#define COSMO_TCB_H

#include <stddef.h>

#define COSMO_TCB_SIZE 128u

/*
 * Where the thread pointer sits inside a thread's storage, and how much
 * storage a thread therefore needs. The two architectures differ because
 * their ELF TLS ABIs do, and getting this wrong is silent:
 *
 *  - **x86-64** (psABI variant II) puts thread-local variables at
 *    *negative* offsets from the thread pointer and requires the word at
 *    `%fs:0` to point at itself. So the thread pointer is the block, the
 *    offset is 0, and a future TLS image grows downward into space nothing
 *    else uses.
 *  - **AArch64** (variant I) puts them at *positive* offsets and reserves
 *    16 bytes at the thread pointer for the ABI's own use, with the first
 *    variable at `TP + 16`. Those bytes are exactly where this block's
 *    `reserved[]` would be, so the block sits **below** the thread pointer
 *    instead, and the storage carries the 16-byte ABI head above it.
 *
 * A caller allocating storage for a thread (`cosmo_tcb_install`) provides
 * `COSMO_TCB_STORAGE` bytes, 16-byte aligned, and the thread pointer ends
 * up at `storage + COSMO_TCB_TP_OFFSET`.
 */
#if defined(__aarch64__)
#define COSMO_TCB_TP_OFFSET COSMO_TCB_SIZE
#define COSMO_TCB_ABI_HEAD  16u          /* variant I reserves this at the thread pointer */
#else
#define COSMO_TCB_TP_OFFSET 0u
#define COSMO_TCB_ABI_HEAD  0u
#endif
#define COSMO_TCB_STORAGE (COSMO_TCB_SIZE + COSMO_TCB_ABI_HEAD)

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
    unsigned tid;               /* the cached id; 0 means "not asked yet" */
    char reserved[112];         /* to COSMO_TCB_SIZE: 8 + 4 + 4 + 112 */
} __attribute__((aligned(16)));

/*
 * How much storage one thread needs: the block, the ABI's reserved head
 * where the architecture has one, and a copy of this program's own
 * thread-local template. It is a property of the *program*, not a
 * constant, because the template is: a program with a large `__thread`
 * array needs more per thread than one with none.
 *
 * `COSMO_TCB_STORAGE` is the floor -- what a program with no `__thread`
 * variables needs -- and a caller that allocates storage for a thread must
 * ask this rather than assume it.
 */
size_t cosmo_tcb_storage(void);

/*
 * Take over the calling thread's block, from storage the caller owns.
 * `len` must be at least `cosmo_tcb_storage()` and `block` 16-byte aligned;
 * returns 0, -EINVAL, or whatever SYS_set_tls refused. The block's
 * lifetime is the caller's problem and must outlast the thread.
 *
 * This is for a thread made outside libc's wrapper. A thread from
 * `cosmo_thread_start` already has one, and installing another leaks
 * nothing but loses the tid cache until it is set again.
 */
int cosmo_tcb_install(void *block, size_t len);

#endif /* COSMO_TCB_H */
