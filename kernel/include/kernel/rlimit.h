/*
 * rlimit.h - Per-process resource limits (docs/kernel/security/design.md §2).
 *
 * One 64-bit value per COSMO_RLIMIT_* resource, inherited by copy at
 * spawn, lowered freely and raised only with privilege. The limits that
 * other subsystems enforce are copied into their structures when set
 * (vm_space, handle_table, vm) so those layers stay ignorant of processes.
 */

#ifndef KERNEL_RLIMIT_H
#define KERNEL_RLIMIT_H

#include <kernel/types.h>
#include <uapi/cosmo/syscall.h>

struct rlimits {
    uint64_t v[COSMO_RLIMIT_COUNT];
};

/* The limits a process created by the kernel (init) starts with. */
extern const struct rlimits rlimits_default;

#endif /* KERNEL_RLIMIT_H */
