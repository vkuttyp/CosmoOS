/*
 * nice.h - Nice values: the rules of getpriority(2) and setpriority(2)
 * (docs/kernel/process/design.md, "Nice values").
 *
 * A process has one nice value, from NICE_MIN (the most favoured) to
 * NICE_MAX, 0 by default, kept in its `nice` under its `lock`. A fork and
 * a spawn copy the parent's; exec keeps it. The Linux personality's
 * getpriority and setpriority read and set it for a process, a process
 * group or a user's processes; /proc/<pid>/stat shows it.
 *
 * The rules are Linux's. A value out of range is clamped, not refused. A
 * caller may set the value of a process whose real or effective uid is
 * its own effective uid, or of any process with PRIV_PROC (Linux's
 * CAP_SYS_NICE); otherwise -EPERM. Lowering a value (raising the
 * priority) further needs PRIV_PROC or the target's RLIMIT_NICE: without
 * the privilege a value can go no lower than 20 - RLIMIT_NICE, so the
 * default limit, 0, allows none; otherwise -EACCES. Raising one is free.
 *
 * Each of the process's threads carries a copy (`thread.nice`), which the
 * scheduler's fair level weighs (kernel/include/kernel/fair.h): a
 * thread's share of a CPU at the default level follows Linux's weight
 * for it.
 *
 * The functions are CeeSafe's (kernel/process/nice.c, generated). They keep
 * no state; the caller holds the target's lock across the check and the
 * store.
 */

#ifndef KERNEL_NICE_H
#define KERNEL_NICE_H

#include <kernel/types.h>

#define NICE_MIN (-20)
#define NICE_MAX 19

/* What the rules look at in the caller of setpriority. */
struct nice_caller {
    uint32_t euid;
    bool privileged;     /* it holds PRIV_PROC (cred_has) */
};

/* What they look at in a process it names, read under that process's lock. */
struct nice_target {
    uint32_t ruid;
    uint32_t euid;
    int32_t nice;
    uint64_t rlimit;     /* its COSMO_RLIMIT_NICE */
};

/* `value` brought into NICE_MIN..NICE_MAX. */
int32_t nice_clamp(int32_t value);

/* Whether `caller` may give `target` the nice value `nice` (in range): 0,
 * -EPERM (not its process), or -EACCES (lower than it may go). */
int nice_may_set(const struct nice_caller *caller, const struct nice_target *target, int32_t nice);

/* setpriority's result over the processes it names, one at a time:
 * `result` starts at -ESRCH (none named), and `one` is a process's
 * nice_may_set. A refusal replaces the result; a success turns -ESRCH
 * into 0 and leaves an earlier refusal standing, as Linux reports. */
int nice_set_result(int result, int one);

/* getpriority's result over the processes it names: `result` starts at
 * -ESRCH, and each process's `nice` raises it to 20 - nice (1 to 40) if
 * that is higher, so a group or a user reports its lowest value. libc
 * turns 20 - nice back into the nice value. */
int nice_get_result(int result, int32_t nice);

#endif
