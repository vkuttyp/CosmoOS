/*
 * guest_psci_race.S - a machine-mode guest that asks for CPUs while the
 * machine is powering off (docs/audit/next-subsystem-vcpu-threads.md).
 *
 * With a thread per vCPU, `SYSTEM_OFF` tells every thread to quit and kicks
 * the ones inside a guest. A `CPU_ON` running concurrently -- from a vCPU
 * thread that has not been kicked yet, which is the normal state of affairs
 * during a shutdown -- must not be able to undo that. If it could store
 * "running" over "quit", the revived thread would run a guest nobody is
 * waiting to stop, the owner's live count would never reach zero, and
 * `vmctl` would hang rather than power off.
 *
 * So: the first CPU brings the second up and powers the machine off two
 * instructions later, while the second and third CPUs ask for a CPU in a
 * loop for as long as they are allowed to run. Whatever the interleaving,
 * the machine must stop.
 *
 * Nothing is read and nothing is printed: the observable is that `vmctl`
 * prints "guest powered off" and exits, which `rc.test` turns into
 * "HVTEST: psci-race ok". A hang is the failure, and the boot test's
 * deadline is what detects it -- the same shape as `guest_offspin`.
 *
 * **What this does not prove**, twice over. Replacing the owner's
 * compare-and-swap with a plain store -- the bug it is named for -- does not
 * fail this test on demand. After the first `CPU_ON` for a target succeeds, every later
 * one is answered ALREADY_ON and returns before reaching the store, so a
 * guest that asks in a loop still gets only one attempt per target at a
 * window a few instructions wide. What this test does is exercise the
 * shutdown path with PSCI calls arriving concurrently from other vCPUs,
 * which is worth having and is not the same claim.
 *
 * Nor does it catch a `CPU_ON` that writes its target's registers *before*
 * asking whether the target is already running -- which a review found in
 * the first threaded build. An instrumented version of this guest tried and
 * could not: the kernel's `vcpu_set_regs` takes the target's `run_lock`, so
 * a stray write cannot land inside a run at all. It lands *between* two
 * runs and resets the target's PC to this entry point, which a guest
 * looping through that entry cannot tell from looping -- `get_regs` then
 * `set_regs` preserves every other register. Detecting it needs a counter
 * in guest *memory*, incremented at the entry and checked for a second
 * arrival; that is a test worth writing and is not this one.
 *
 * Both fixes are correct by construction and cost nothing; the arguments
 * are in docs/audit/next-subsystem-vcpu-threads.md, not here.
 *
 * Loaded flat at the start of guest RAM, so an entry point is taken with
 * `adr`: its own address is its GPA.
 */
    .text
    .globl _start
_start:
    adr     x2, racer                /* entry_point_address */
    mov     x1, #1                   /* target_cpu: affinity 0.0.0.1 */
    mov     x3, #0                   /* context_id */
    mov     x0, #0x0003
    movk    x0, #0xC400, lsl #16     /* PSCI_CPU_ON (0xC4000003) */
    hvc     #0

    mov     x0, #0x0008
    movk    x0, #0x8400, lsl #16     /* PSCI_SYSTEM_OFF (0x84000008) */
    hvc     #0
1:  b       1b                       /* the owner stops the machine here */

/*
 * Every CPU brought up here asks for CPU 2, over and over. The first such
 * call is the one that races the power-off; the rest are answered
 * ALREADY_ON or DENIED, and asking anyway is the point -- a guest that
 * keeps asking is what makes the race easy to hit rather than a
 * once-per-boot coincidence.
 */
racer:
    adr     x19, racer               /* the entry to ask for, kept across calls */
2:  mov     x2, x19
    mov     x1, #2                   /* target_cpu: affinity 0.0.0.2 */
    mov     x3, #0
    mov     x0, #0x0003
    movk    x0, #0xC400, lsl #16     /* PSCI_CPU_ON */
    hvc     #0
    b       2b
